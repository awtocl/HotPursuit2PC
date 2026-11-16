// See ref_trace.h.
#include "ref_trace.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "PS2VM.h"
#include "Profiler.h"
#include "Ps2Const.h"
#include "ee/EeBasicBlock.h"
#include "ee/EeExecutor.h"
#include "ee/MA_VU.h"
#include "ee/Vif.h"
#include "ee/Vpu.h"
#include "MemoryUtils.h"
#include "ee/VuBasicBlock.h"
#include "ee/VuExecutor.h"
#include "string_format.h"
#include "xxhash.h"

#include "lift_check.h"

namespace hp2ref
{
	namespace
	{
		namespace fs = std::filesystem;

		bool g_enabled = false;
		fs::path g_dir;
		std::atomic<uint32_t> g_frame{0};

		// ---- executed code --------------------------------------------------------------
		struct BlockEvent
		{
			uint32_t begin, end, frame;
		};
		std::vector<BlockEvent> g_blocks;  // VM thread only: blocks are compiled there

		// The core compiles a block the first time execution reaches an address
		// that has none (and again after the block was invalidated). Recording
		// here therefore lists exactly the code that ran, complete even when
		// blocks are thrown away later.
		// Execution counters (--trace-counts): how often each block runs, to find
		// the code that matters. Each block gets a call to CountBlock in its
		// epilogue (the prologue is skipped when a block loops on itself). This
		// run mode builds the blocks itself instead of through the core's block
		// cache, so the core's write protection of code pages and its per-game
		// block hints are not active: use it for measuring only.
		bool g_countBlocks = false;
		std::vector<uint64_t> g_blockCounts;  // same indexing as g_blocks

		void CountBlock(uint32 index)
		{
			++g_blockCounts[index];
		}

		// ---- translated code (--lift-check, --run-translated) --------------------------------
		constexpr uint32_t kRamSize = PS2::EE_RAM_SIZE;  // main memory as the translator sees it
		constexpr uint32_t kSprSize = PS2::EE_SPR_SIZE;
		constexpr int kVerifyCalls = 6;      // compared calls per function
		constexpr int kTimeCalls = 400;      // timed calls of the reference per function
		constexpr int kLiftedRepeats = 64;   // the translated code is timed by re-running it

		bool g_liftCheck = false;      // compare translated functions with the reference core
		bool g_runTranslated = false;  // run translated functions instead of the reference core
		std::vector<uint8_t> g_liftEntry;  // per block index: begins a translated function
		CMIPS* g_ee = nullptr;
		uint8_t* g_ram = nullptr;
		uint8_t* g_spr = nullptr;
		std::vector<uint8_t> g_ramEntry, g_ramLifted, g_sprEntry, g_sprLifted;
		uint64_t g_translatedCalls = 0;

		// The real slow path: the reference core's own memory map (hardware registers, VU memory, ...).
		uint64_t RealRead(uint8_t*, uint32_t address, uint32_t size)
		{
			switch(size)
			{
			case 1: return MemoryUtils_GetByteProxy(g_ee, address);
			case 2: return MemoryUtils_GetHalfProxy(g_ee, address);
			case 4: return MemoryUtils_GetWordProxy(g_ee, address);
			default: return MemoryUtils_GetDoubleProxy(g_ee, address);
			}
		}
		void RealWrite(uint8_t*, uint32_t address, uint64_t value, uint32_t size)
		{
			switch(size)
			{
			case 1: MemoryUtils_SetByteProxy(g_ee, static_cast<uint32>(value), address); break;
			case 2: MemoryUtils_SetHalfProxy(g_ee, static_cast<uint32>(value), address); break;
			case 4: MemoryUtils_SetWordProxy(g_ee, static_cast<uint32>(value), address); break;
			default: MemoryUtils_SetDoubleProxy(g_ee, value, address); break;
			}
		}
		const LiftRuntime g_realRuntime{&RealRead, &RealWrite};

		// The verification slow path: the translated code runs on copies, so it
		// must not touch real hardware. Such a call is not compared.
		bool g_touchedHardware = false;
		uint64_t StubRead(uint8_t*, uint32_t, uint32_t)
		{
			g_touchedHardware = true;
			return 0;
		}
		void StubWrite(uint8_t*, uint32_t, uint64_t, uint32_t)
		{
			g_touchedHardware = true;
		}
		const LiftRuntime g_stubRuntime{&StubRead, &StubWrite};

		// ---- comparison (--lift-check) ----
		// At the entry of a translated function, the translated code runs on a
		// copy of the processor state, main memory and scratchpad; the reference
		// core then runs the same call; when it comes back (first block at the
		// return address with the same stack pointer) both results are compared.
		// Later calls of the reference are only timed, entry to return.
		struct LiftStats
		{
			int verified = 0, stateMismatch = 0, liftedOnlyWrites = 0, referenceOnlyChanges = 0;
			int stoppedEarly = 0, touchedHardware = 0;
			int timed = 0;
			std::vector<uint64_t> referenceTicks, liftedTicks;
			std::string firstMismatch;
		};
		std::map<uint32_t, LiftStats> g_liftStats;

		struct Pending
		{
			bool active = false, verify = false;
			uint32_t function = 0, returnAddress = 0, stack = 0;
			uint64_t start = 0;
			MIPSSTATE lifted;
		};
		Pending g_pending;

		std::string DescribeStateDifference(const MIPSSTATE& ref, const MIPSSTATE& lifted)
		{
			if(ref.nPC != lifted.nPC) return string_format("pc %08X vs %08X", ref.nPC, lifted.nPC);
			for(int r = 0; r < 32; r++)
				for(int w = 0; w < 4; w++)
					if(ref.nGPR[r].nV[w] != lifted.nGPR[r].nV[w])
						return string_format("r%d word %d: %08X vs %08X", r, w, ref.nGPR[r].nV[w], lifted.nGPR[r].nV[w]);
			for(int w = 0; w < 2; w++)
			{
				if(ref.nHI[w] != lifted.nHI[w]) return string_format("hi word %d: %08X vs %08X", w, ref.nHI[w], lifted.nHI[w]);
				if(ref.nLO[w] != lifted.nLO[w]) return string_format("lo word %d: %08X vs %08X", w, ref.nLO[w], lifted.nLO[w]);
				if(ref.nHI1[w] != lifted.nHI1[w]) return string_format("hi1 word %d: %08X vs %08X", w, ref.nHI1[w], lifted.nHI1[w]);
				if(ref.nLO1[w] != lifted.nLO1[w]) return string_format("lo1 word %d: %08X vs %08X", w, ref.nLO1[w], lifted.nLO1[w]);
			}
			for(int r = 0; r < 32; r++)
				if(ref.nCOP1[r] != lifted.nCOP1[r]) return string_format("f%d: %08X vs %08X", r, ref.nCOP1[r], lifted.nCOP1[r]);
			if(ref.nCOP1A != lifted.nCOP1A) return string_format("fpu acc: %08X vs %08X", ref.nCOP1A, lifted.nCOP1A);
			if(ref.nFCSR != lifted.nFCSR) return string_format("fcsr: %08X vs %08X", ref.nFCSR, lifted.nFCSR);
			for(int r = 0; r < 32; r++)
				for(int w = 0; w < 4; w++)
					if(ref.nCOP2[r].nV[w] != lifted.nCOP2[r].nV[w])
						return string_format("vf%d word %d: %08X vs %08X", r, w, ref.nCOP2[r].nV[w], lifted.nCOP2[r].nV[w]);
			for(int w = 0; w < 4; w++)
				if(ref.nCOP2A.nV[w] != lifted.nCOP2A.nV[w])
					return string_format("vu acc word %d: %08X vs %08X", w, ref.nCOP2A.nV[w], lifted.nCOP2A.nV[w]);
			for(int r = 0; r < 16; r++)
				if(ref.nCOP2VI[r] != lifted.nCOP2VI[r]) return string_format("vi%d: %08X vs %08X", r, ref.nCOP2VI[r], lifted.nCOP2VI[r]);
			if(ref.nCOP2Q != lifted.nCOP2Q) return string_format("Q: %08X vs %08X", ref.nCOP2Q, lifted.nCOP2Q);
			if(ref.nCOP2I != lifted.nCOP2I) return string_format("I: %08X vs %08X", ref.nCOP2I, lifted.nCOP2I);
			if(ref.nCOP2CF != lifted.nCOP2CF) return string_format("clip flags: %08X vs %08X", ref.nCOP2CF, lifted.nCOP2CF);
			if(ref.nCOP2MF != lifted.nCOP2MF) return string_format("MAC flags: %08X vs %08X", ref.nCOP2MF, lifted.nCOP2MF);
			if(ref.nCOP2DF != lifted.nCOP2DF) return string_format("division flag: %08X vs %08X", ref.nCOP2DF, lifted.nCOP2DF);
			if(ref.pipeQ.counter != lifted.pipeQ.counter || ref.pipeQ.heldValue != lifted.pipeQ.heldValue)
				return string_format("Q pipeline: %08X/%08X vs %08X/%08X", ref.pipeQ.counter, ref.pipeQ.heldValue,
				                     lifted.pipeQ.counter, lifted.pipeQ.heldValue);
			const FLAG_PIPELINE* pipes[3][2] = {{&ref.pipeMac, &lifted.pipeMac}, {&ref.pipeSticky, &lifted.pipeSticky},
			                                    {&ref.pipeClip, &lifted.pipeClip}};
			const char* names[3] = {"MAC", "sticky", "clip"};
			for(int p = 0; p < 3; p++)
				if(memcmp(pipes[p][0], pipes[p][1], sizeof(FLAG_PIPELINE)) != 0)
					return string_format("%s flag pipeline differs (index %u vs %u)", names[p], pipes[p][0]->index, pipes[p][1]->index);
			return {};
		}

		// Bytes the translated code wrote differently from the reference
		// (`liftedOnly`), and bytes the reference changed that it did not
		// (`referenceOnly`; these can also come from elsewhere, DMA or an
		// interrupt, while the reference ran the call).
		void CompareMemory(const uint8_t* real, const uint8_t* lifted, const uint8_t* entry, uint32_t size,
		                   bool& liftedOnly, bool& referenceOnly)
		{
			for(uint32_t page = 0; page < size; page += 4096)
			{
				if(memcmp(real + page, lifted + page, 4096) == 0) continue;
				for(uint32_t i = page; i < page + 4096; i++)
				{
					if(real[i] == lifted[i]) continue;
					if(lifted[i] != entry[i])
						liftedOnly = true;
					else
						referenceOnly = true;
				}
			}
		}

		void FinishPending(CMIPS* context)
		{
			const uint64_t ticks = __rdtsc() - g_pending.start;
			LiftStats& stats = g_liftStats[g_pending.function];
			g_pending.active = false;
			if(!g_pending.verify)
			{
				stats.referenceTicks.push_back(ticks);
				return;
			}
			stats.verified++;
			// The reference core's own bookkeeping is not part of the comparison.
			g_pending.lifted.cycleQuota = context->m_State.cycleQuota;
			const std::string difference = DescribeStateDifference(context->m_State, g_pending.lifted);
			if(!difference.empty())
			{
				stats.stateMismatch++;
				if(stats.firstMismatch.empty()) stats.firstMismatch = difference;
			}
			bool liftedOnly = false, referenceOnly = false;
			CompareMemory(g_ram, g_ramLifted.data(), g_ramEntry.data(), kRamSize, liftedOnly, referenceOnly);
			CompareMemory(g_spr, g_sprLifted.data(), g_sprEntry.data(), kSprSize, liftedOnly, referenceOnly);
			if(liftedOnly)
			{
				stats.liftedOnlyWrites++;
				if(stats.firstMismatch.empty()) stats.firstMismatch = "memory written differently";
			}
			if(referenceOnly) stats.referenceOnlyChanges++;
		}

		void RunLifted(LiftedCode code, MIPSSTATE& state, uint32_t function)
		{
			state.nPC = function;
			state.cycleQuota = 1 << 30;  // no time slice: the call runs to its end
			state.nHasException = 0;
			code(reinterpret_cast<uint8_t*>(&state), g_ramLifted.data(), g_sprLifted.data(), &g_stubRuntime);
		}

		void StartPending(CMIPS* context, uint32_t function)
		{
			LiftStats& stats = g_liftStats[function];
			auto code = LiftCheckLookup(function);
			if(!code) return;
			Pending& p = g_pending;
			p.function = function;
			p.returnAddress = context->m_State.nGPR[CMIPS::RA].nV0;
			p.stack = context->m_State.nGPR[CMIPS::SP].nV0;
			if(stats.verified + stats.stoppedEarly + stats.touchedHardware < kVerifyCalls)
			{
				memcpy(g_ramEntry.data(), g_ram, kRamSize);
				memcpy(g_ramLifted.data(), g_ram, kRamSize);
				memcpy(g_sprEntry.data(), g_spr, kSprSize);
				memcpy(g_sprLifted.data(), g_spr, kSprSize);
				p.lifted = context->m_State;
				g_touchedHardware = false;
				RunLifted(code, p.lifted, function);
				if(g_touchedHardware)
				{
					stats.touchedHardware++;
					return;
				}
				if(p.lifted.nPC != p.returnAddress)
				{
					stats.stoppedEarly++;
					return;
				}
				// Time the translated code on this call's inputs (memory as left by the
				// first run): one timed burst of calls, each on its own copy of the
				// entry state, because the time-stamp counter is too coarse for one
				// short call. Stored in thousandths of a tick per call.
				{
					static std::vector<MIPSSTATE> scratch(kLiftedRepeats);
					for(auto& s : scratch)
					{
						s = context->m_State;
						s.nPC = function;
						s.cycleQuota = 1 << 30;
					}
					const uint64_t t0 = __rdtsc();
					for(auto& s : scratch) code(reinterpret_cast<uint8_t*>(&s), g_ramLifted.data(), g_sprLifted.data(), &g_stubRuntime);
					stats.liftedTicks.push_back((__rdtsc() - t0) * 1000 / kLiftedRepeats);
				}
				// Run once more from the entry state, so that the copies hold the result of one call.
				memcpy(g_ramLifted.data(), g_ramEntry.data(), kRamSize);
				memcpy(g_sprLifted.data(), g_sprEntry.data(), kSprSize);
				p.lifted = context->m_State;
				RunLifted(code, p.lifted, function);
				p.verify = true;
			}
			else if(stats.timed < kTimeCalls)
			{
				stats.timed++;
				p.verify = false;
			}
			else
			{
				return;
			}
			p.active = true;
			p.start = __rdtsc();
		}

		// Prologue of every block in --lift-check mode.
		void BlockEntry(CMIPS* context, uint32 index)
		{
			if(g_pending.active)
			{
				const uint32_t begin = g_blocks[index].begin;
				if(begin == g_pending.returnAddress && context->m_State.nGPR[CMIPS::SP].nV0 == g_pending.stack)
					FinishPending(context);
				return;
			}
			if(g_liftEntry[index]) StartPending(context, g_blocks[index].begin);
		}

		class CCountingEeBasicBlock : public CEeBasicBlock
		{
		public:
			CCountingEeBasicBlock(CMIPS& context, uint32 begin, uint32 end, BLOCK_CATEGORY category, uint32 index)
			    : CEeBasicBlock(context, begin, end, category)
			    , m_index(index)
			{
			}

		protected:
			void CompileProlog(CMipsJitter* jitter) override
			{
				if(g_liftCheck)
				{
					jitter->PushCtx();
					jitter->PushCst(m_index);
					jitter->Call(reinterpret_cast<void*>(&BlockEntry), 2, Jitter::CJitter::RETURN_VALUE_NONE);
				}
				CEeBasicBlock::CompileProlog(jitter);
			}
			void CompileEpilog(CMipsJitter* jitter, bool loopsOnItself) override
			{
				if(g_countBlocks)
				{
					jitter->PushCst(m_index);
					jitter->Call(reinterpret_cast<void*>(&CountBlock), 1, Jitter::CJitter::RETURN_VALUE_NONE);
				}
				CEeBasicBlock::CompileEpilog(jitter, loopsOnItself);
			}

		private:
			uint32 m_index;
		};

		// ---- running translated code (--run-translated) ----
		// A block at the start of a translated function: its code calls the
		// translated function, which runs (with everything it calls that was
		// translated) until it returns or hands over; the block's epilogue then
		// continues wherever the translated code left the program counter.
		struct TranslatedEntry
		{
			uint32_t begin;
			LiftedCode code;
		};
		std::vector<TranslatedEntry> g_translatedEntries;  // indexed by the slot compiled into each block

		void RunTranslated(CMIPS* context, uint32 slot)
		{
			const TranslatedEntry& entry = g_translatedEntries[slot];
			auto& state = context->m_State;
			state.nPC = entry.begin;
			++g_translatedCalls;
			entry.code(reinterpret_cast<uint8_t*>(&state), g_ram, g_spr, &g_realRuntime);
			state.nDelayedJumpAddr = state.nPC;
			state.nPC = entry.begin;
		}

		class CTranslatedBlock : public CBasicBlock
		{
		public:
			CTranslatedBlock(CMIPS& context, uint32 begin, uint32 end, BLOCK_CATEGORY category, uint32 slot)
			    : CBasicBlock(context, begin, end, category)
			    , m_slot(slot)
			{
			}

			void CompileRange(CMipsJitter* jitter) override
			{
				jitter->PushCtx();
				jitter->PushCst(m_slot);
				jitter->Call(reinterpret_cast<void*>(&RunTranslated), 2, Jitter::CJitter::RETURN_VALUE_NONE);
				CBasicBlock::CompileEpilog(jitter, false);
			}

		private:
			uint32 m_slot;
		};

		// ---- watched game functions (--trace-calls a,b,...): every entry, with arguments ----
		// calls.tsv: refresh, milliseconds since start, function, a0..a3. Works on the
		// reference core only (translated code calls functions directly).
		std::vector<uint32_t> g_watched;
		FILE* g_callsFile = nullptr;
		int64_t Qpc();
		int64_t g_callsOrigin = 0;
		double g_qpcToMs = 0;

		void RecordCall(CMIPS* context, uint32 function)
		{
			if(!g_callsFile) return;
			const auto& s = context->m_State;
			fprintf(g_callsFile, "%u\t%.3f\t%08X\t%08X\t%08X\t%08X\t%08X\n", g_frame.load(std::memory_order_relaxed),
			        (Qpc() - g_callsOrigin) * g_qpcToMs, function, s.nGPR[CMIPS::A0].nV0, s.nGPR[CMIPS::A1].nV0,
			        s.nGPR[CMIPS::A2].nV0, s.nGPR[CMIPS::A3].nV0);
		}

		class CWatchedEeBasicBlock : public CEeBasicBlock
		{
		public:
			using CEeBasicBlock::CEeBasicBlock;

		protected:
			void CompileProlog(CMipsJitter* jitter) override
			{
				jitter->PushCtx();
				jitter->PushCst(m_begin);
				jitter->Call(reinterpret_cast<void*>(&RecordCall), 2, Jitter::CJitter::RETURN_VALUE_NONE);
				CEeBasicBlock::CompileProlog(jitter);
			}
		};

		class CTracingEeExecutor : public CEeExecutor
		{
		public:
			using CEeExecutor::CEeExecutor;

			BasicBlockPtr BlockFactory(CMIPS& context, uint32 start, uint32 end) override
			{
				g_blocks.push_back({start, end, g_frame.load(std::memory_order_relaxed)});
				if(std::find(g_watched.begin(), g_watched.end(), start) != g_watched.end())
				{
					auto block = std::make_shared<CWatchedEeBasicBlock>(context, start, end, m_blockCategory);
					block->Compile();
					return block;
				}
				if(g_runTranslated)
					if(const LiftedCode code = LiftCheckLookupEntry(start))
				{
					g_translatedEntries.push_back({start, code});
					auto block = std::make_shared<CTranslatedBlock>(context, start, end, m_blockCategory,
					                                                static_cast<uint32>(g_translatedEntries.size() - 1));
					block->Compile();
					// Not linked to its successors: where execution continues is only known at run time.
					block->SetRecycleCount(RECYCLE_NOLINK_THRESHOLD);
					return block;
				}
				if(!g_countBlocks && !g_liftCheck) return CEeExecutor::BlockFactory(context, start, end);
				g_blockCounts.push_back(0);
				g_liftEntry.push_back(g_liftCheck && LiftCheckLookup(start) != nullptr);
				auto block = std::make_shared<CCountingEeBasicBlock>(context, start, end, m_blockCategory,
				                                                     static_cast<uint32>(g_blocks.size() - 1));
				block->Compile();
				return block;
			}
		};

		// ---- vector unit 1 microcode -------------------------------------------------------
		// The game uploads small programs to the vector unit at run time. Each
		// block the core compiles is recorded with its contents, so that they can
		// be looked up in the game's files afterwards (can they be translated
		// ahead of time?).
		struct VuBlock
		{
			uint32_t begin, end, firstFrame, created;
			std::vector<uint32_t> words;
			std::vector<uint32_t> slots;  // counters of this block's compiled copies (--trace-counts)
		};
		std::map<std::pair<uint32_t, uint64_t>, VuBlock> g_vuBlocks;  // by (begin, content hash); VM thread only
		uint64_t g_vuBlocksCreated = 0;

		// --trace-counts: how often each VU1 block runs (counted in its epilogue, like the EE's).
		std::vector<uint64_t> g_vuCounts;
		void CountVuBlock(uint32 slot)
		{
			++g_vuCounts[slot];
		}
		class CCountingVuBasicBlock : public CVuBasicBlock
		{
		public:
			CCountingVuBasicBlock(CMIPS& context, uint32 begin, uint32 end, BLOCK_CATEGORY category, uint32 slot)
			    : CVuBasicBlock(context, begin, end, category)
			    , m_slot(slot)
			{
			}

		protected:
			void CompileEpilog(CMipsJitter* jitter, bool loopsOnItself) override
			{
				jitter->PushCst(m_slot);
				jitter->Call(reinterpret_cast<void*>(&CountVuBlock), 1, Jitter::CJitter::RETURN_VALUE_NONE);
				CVuBasicBlock::CompileEpilog(jitter, loopsOnItself);
			}

		private:
			uint32 m_slot;
		};

		// ---- translated VU1 microcode (--vu1 check|run, ADR 0008) ----
		// The translated images are valid while microcode memory holds exactly
		// their words: the core invalidates a range before an upload changes it,
		// and the next run compares the memory again.
		int g_vuMode = 0;  // 0 off, 1 compare every translated run with the reference core, 2 run translated
		std::vector<VuLiftImage> g_vuImages;
		enum class ImageState : uint8_t
		{
			Unknown,
			Valid,
			Invalid
		};
		std::vector<ImageState> g_vuImageState;
		CMIPS* g_vu1 = nullptr;
		uint8_t* g_microMem1 = nullptr;
		uint8_t* g_vuMem1 = nullptr;
		std::vector<uint8_t> g_vuMemLifted;

		struct VuKick
		{
			uint32_t address;
			uint64_t memoryHash;  // all of VU1 data memory when the packet is handed to the GIF
			bool operator==(const VuKick& o) const { return address == o.address && memoryHash == o.memoryHash; }
		};
		std::vector<VuKick> g_kicksReference, g_kicksLifted;
		bool g_recordReferenceKicks = false;

		struct VuStats
		{
			uint64_t verified = 0, stateMismatch = 0, memoryMismatch = 0, kickMismatch = 0, kicks = 0;
			uint64_t handedOver = 0, notTranslated = 0;
			uint64_t referenceTicks = 0;
			std::string firstMismatch;
			std::map<uint32_t, uint64_t> handOverAt;
			uint64_t runCalls = 0, runHandOvers = 0, runReferenceBlocks = 0;
			uint32_t operandWords = 0, operandMismatches = 0;
			std::string firstOperandMismatch;
		};
		VuStats g_vuStats;
		bool g_vuOperandsChecked = false;

		uint64_t HashVuMemory(const uint8_t* memory)
		{
			return XXH3_64bits(memory, PS2::VUMEM1SIZE);
		}
		uint32_t VuPortRead(uint8_t*, uint32_t address)
		{
			return MemoryUtils_GetWordProxy(g_vu1, address);
		}
		void VuPortWrite(uint8_t*, uint32_t address, uint32_t value)
		{
			MemoryUtils_SetWordProxy(g_vu1, value, address);
		}
		const VuLiftRuntime g_vuRealRuntime{&VuPortRead, &VuPortWrite};
		// Verification: the translated code runs on copies; its xgkicks are recorded, not sent.
		void VuRecordKick(uint8_t*, uint32_t, uint32_t value)
		{
			g_kicksLifted.push_back({value, HashVuMemory(g_vuMemLifted.data())});
		}
		const VuLiftRuntime g_vuCheckRuntime{&VuPortRead, &VuRecordKick};

		// The translator's operand tables against the reference core's, on the real microcode memory.
		void CheckVuOperands(const VuLiftImage& image)
		{
			g_vuOperandsChecked = true;
			auto arch = static_cast<CMA_VU*>(g_vu1->m_pArch);
			for(uint32_t a = image.address; a + 8 <= image.address + image.words.size() * 4; a += 8)
			{
				const uint32_t lower = image.words[(a - image.address) / 4], upper = image.words[(a - image.address) / 4 + 1];
				for(int word = 0; word < 2; word++)
				{
					const auto ref = arch->GetAffectedOperands(g_vu1, a + word * 4, word ? upper : lower);
					const uint32_t expected[16] = {ref.writeF, ref.readF0, ref.readElemF0, ref.readF1, ref.readElemF1,
					                               ref.writeI, ref.writeILsu, ref.readI0, ref.readI1, ref.syncQ, ref.readQ,
					                               ref.syncP, ref.readP, ref.readMACflags, ref.writeMACflags, ref.branchValue};
					uint32_t mine[16];
					VuLiftOperands(lower, upper, word == 1, mine);
					g_vuStats.operandWords++;
					if(memcmp(expected, mine, sizeof mine) != 0)
					{
						g_vuStats.operandMismatches++;
						if(g_vuStats.firstOperandMismatch.empty())
						{
							int field = 0;
							while(expected[field] == mine[field]) field++;
							g_vuStats.firstOperandMismatch = string_format("%s word at %04X (%08X): field %d is %u, translator says %u",
							                                               word ? "upper" : "lower", a + word * 4,
							                                               word ? upper : lower, field, expected[field], mine[field]);
						}
					}
				}
			}
		}

		// The code to run from `pc` in a valid image: the image itself at one of its
		// block starts, else an entry translated on first use; nullptr if none.
		VuLiftedCode VuCodeFor(int image, uint32_t pc)
		{
			if(VuLiftIsBlockStart(image, pc)) return g_vuImages[image].code;
			return VuLiftEntry(image, pc);
		}

		int FindVuImage(uint32_t pc)
		{
			for(size_t i = 0; i < g_vuImages.size(); i++)
			{
				const auto& image = g_vuImages[i];
				if(pc < image.address || pc >= image.address + image.words.size() * 4) continue;
				if(g_vuImageState[i] == ImageState::Unknown)
				{
					const bool same = memcmp(g_microMem1 + image.address, image.words.data(), image.words.size() * 4) == 0;
					g_vuImageState[i] = same ? ImageState::Valid : ImageState::Invalid;
					if(same && !g_vuOperandsChecked) CheckVuOperands(image);
				}
				if(g_vuImageState[i] == ImageState::Valid) return static_cast<int>(i);
			}
			return -1;
		}

		std::string DescribeVuDifference(const MIPSSTATE& r, const MIPSSTATE& l)
		{
			if(r.nPC != l.nPC) return string_format("pc %04X vs %04X", r.nPC, l.nPC);
			if(r.nDelayedJumpAddr != l.nDelayedJumpAddr) return string_format("delayed jump %08X vs %08X", r.nDelayedJumpAddr, l.nDelayedJumpAddr);
			if(r.nHasException != l.nHasException) return string_format("exception %08X vs %08X", r.nHasException, l.nHasException);
			if(r.cycleQuota != l.cycleQuota) return string_format("cycle quota %d vs %d", r.cycleQuota, l.cycleQuota);
			for(int i = 1; i < 32; i++)
				for(int w = 0; w < 4; w++)
					if(r.nCOP2[i].nV[w] != l.nCOP2[i].nV[w])
						return string_format("vf%d word %d: %08X vs %08X", i, w, r.nCOP2[i].nV[w], l.nCOP2[i].nV[w]);
			for(int w = 0; w < 4; w++)
				if(r.nCOP2A.nV[w] != l.nCOP2A.nV[w]) return string_format("acc word %d: %08X vs %08X", w, r.nCOP2A.nV[w], l.nCOP2A.nV[w]);
			for(int i = 0; i < 16; i++)
				if(r.nCOP2VI[i] != l.nCOP2VI[i]) return string_format("vi%d: %08X vs %08X", i, r.nCOP2VI[i], l.nCOP2VI[i]);
			if(r.nCOP2Q != l.nCOP2Q) return string_format("Q: %08X vs %08X", r.nCOP2Q, l.nCOP2Q);
			if(r.nCOP2I != l.nCOP2I) return string_format("I: %08X vs %08X", r.nCOP2I, l.nCOP2I);
			if(r.nCOP2R != l.nCOP2R) return string_format("R: %08X vs %08X", r.nCOP2R, l.nCOP2R);
			if(r.nCOP2CF != l.nCOP2CF) return string_format("clip flags: %08X vs %08X", r.nCOP2CF, l.nCOP2CF);
			if(r.nCOP2DF != l.nCOP2DF) return string_format("division flag: %08X vs %08X", r.nCOP2DF, l.nCOP2DF);
			if(r.nCOP2T != l.nCOP2T) return string_format("T: %08X vs %08X", r.nCOP2T, l.nCOP2T);
			if(r.pipeQ.counter != l.pipeQ.counter || r.pipeQ.heldValue != l.pipeQ.heldValue)
				return string_format("Q pipeline: %08X/%08X vs %08X/%08X", r.pipeQ.counter, r.pipeQ.heldValue, l.pipeQ.counter, l.pipeQ.heldValue);
			if(memcmp(&r.pipeClip, &l.pipeClip, sizeof(FLAG_PIPELINE)) != 0)
				return string_format("clip flag pipeline (index %u vs %u)", r.pipeClip.index, l.pipeClip.index);
			for(int i = 0; i < 3; i++)
				for(int w = 0; w < 4; w++)
					if(r.pipeFmacWrite[i].nV[w] != l.pipeFmacWrite[i].nV[w])
						return string_format("FMAC writes %d word %d: %08X vs %08X", i, w, r.pipeFmacWrite[i].nV[w], l.pipeFmacWrite[i].nV[w]);
			if(r.pipeTime != l.pipeTime) return string_format("pipeline time %u vs %u", r.pipeTime, l.pipeTime);
			if(r.savedNextBlockIntRegIdx != l.savedNextBlockIntRegIdx || r.savedNextBlockIntRegVal != l.savedNextBlockIntRegVal)
				return string_format("trailing integer register %u=%08X vs %u=%08X", r.savedNextBlockIntRegIdx, r.savedNextBlockIntRegVal,
				                     l.savedNextBlockIntRegIdx, l.savedNextBlockIntRegVal);
			if(r.xgkickAddress != l.xgkickAddress) return string_format("xgkick address %08X vs %08X", r.xgkickAddress, l.xgkickAddress);
			return {};
		}

		class CTracingVuExecutor : public CVuExecutor
		{
		public:
			using CVuExecutor::CVuExecutor;

			int Execute(int quota) override
			{
				if(g_vuMode == 0) return CVuExecutor::Execute(quota);
				auto& s = m_context.m_State;
				if(g_vuMode == 1)
				{
					const int image = s.nHasException == 0 ? FindVuImage(s.nPC) : -1;
					if(image < 0)
					{
						g_vuStats.notTranslated++;
						return CVuExecutor::Execute(quota);
					}
					return CheckedExecute(image, quota);
				}
				// Run mode: CGenericMipsExecutor::Execute, with translated images where they are valid.
				s.cycleQuota = quota;
				while(s.nHasException == 0)
				{
					const int image = FindVuImage(s.nPC);
					if(image >= 0)
						if(const VuLiftedCode code = VuCodeFor(image, s.nPC))
						{
							code(reinterpret_cast<uint8_t*>(&s), g_vuMem1, &g_vuRealRuntime);
							g_vuStats.runCalls++;
							continue;  // each call runs at least one block: until an exception, or a place without code
						}
					g_vuStats.runHandOvers++;
					g_vuStats.handOverAt[s.nPC]++;
					g_vuStats.runReferenceBlocks++;
					m_blockLookup.FindBlockAt(s.nPC & m_addressMask)->Execute();
				}
				s.nHasException &= ~MIPS_EXCEPTION_STATUS_QUOTADONE;
				return s.cycleQuota;
			}

			void Reset() override
			{
				CVuExecutor::Reset();
				std::fill(g_vuImageState.begin(), g_vuImageState.end(), ImageState::Unknown);
			}

			void ClearActiveBlocksInRange(uint32 start, uint32 end, bool executing) override
			{
				CVuExecutor::ClearActiveBlocksInRange(start, end, executing);
				for(size_t i = 0; i < g_vuImages.size(); i++)
				{
					const auto& image = g_vuImages[i];
					if(start < image.address + image.words.size() * 4 && end > image.address) g_vuImageState[i] = ImageState::Unknown;
				}
			}

		private:
			// The translated code runs first, on copies of the state and data memory;
			// then the reference core runs the same time slice for real; then both are compared.
			int CheckedExecute(int image, int quota)
			{
				auto& s = m_context.m_State;
				MIPSSTATE lifted = s;
				memcpy(g_vuMemLifted.data(), g_vuMem1, PS2::VUMEM1SIZE);
				g_kicksLifted.clear();
				lifted.cycleQuota = quota;
				bool handedOver = false;
				while(lifted.nHasException == 0)
				{
					const VuLiftedCode code = VuCodeFor(image, lifted.nPC);
					if(!code)
					{
						handedOver = true;
						break;
					}
					code(reinterpret_cast<uint8_t*>(&lifted), g_vuMemLifted.data(), &g_vuCheckRuntime);
				}
				lifted.nHasException &= ~MIPS_EXCEPTION_STATUS_QUOTADONE;

				g_kicksReference.clear();
				g_recordReferenceKicks = true;
				const uint64_t t0 = __rdtsc();
				const int result = CVuExecutor::Execute(quota);
				g_vuStats.referenceTicks += __rdtsc() - t0;
				g_recordReferenceKicks = false;

				if(handedOver)
				{
					g_vuStats.handedOver++;
					g_vuStats.handOverAt[lifted.nPC]++;
					return result;
				}
				g_vuStats.verified++;
				g_vuStats.kicks += g_kicksReference.size();
				const std::string difference = DescribeVuDifference(s, lifted);
				if(!difference.empty())
				{
					g_vuStats.stateMismatch++;
					if(g_vuStats.firstMismatch.empty()) g_vuStats.firstMismatch = string_format("run %llu: ", g_vuStats.verified) + difference;
				}
				if(memcmp(g_vuMem1, g_vuMemLifted.data(), PS2::VUMEM1SIZE) != 0)
				{
					g_vuStats.memoryMismatch++;
					if(g_vuStats.firstMismatch.empty())
					{
						uint32_t at = 0;
						while(g_vuMem1[at] == g_vuMemLifted[at]) at++;
						g_vuStats.firstMismatch = string_format("run %llu: data memory differs from byte %04X", g_vuStats.verified, at);
					}
				}
				if(g_kicksReference != g_kicksLifted)
				{
					g_vuStats.kickMismatch++;
					if(g_vuStats.firstMismatch.empty())
						g_vuStats.firstMismatch = string_format("run %llu: %zu xgkicks vs %zu (or different packets)", g_vuStats.verified,
						                                        g_kicksReference.size(), g_kicksLifted.size());
				}
				return result;
			}

		protected:
			BasicBlockPtr BlockFactory(CMIPS& context, uint32 begin, uint32 end) override
			{
				std::vector<uint32_t> words;
				uint64_t hash = 1469598103934665603ull;  // FNV-1a
				for(uint32 address = begin; address <= end; address += 4)
				{
					const uint32 word = context.m_pMemoryMap->GetInstruction(address);
					words.push_back(word);
					for(int i = 0; i < 4; i++) hash = (hash ^ ((word >> (8 * i)) & 0xFF)) * 1099511628211ull;
				}
				++g_vuBlocksCreated;
				auto [it, isNew] = g_vuBlocks.try_emplace({begin, hash});
				if(isNew) it->second = {begin, end, g_frame.load(std::memory_order_relaxed), 0, std::move(words), {}};
				++it->second.created;
				if(!g_countBlocks) return CVuExecutor::BlockFactory(context, begin, end);
				const uint32 slot = static_cast<uint32>(g_vuCounts.size());
				g_vuCounts.push_back(0);
				it->second.slots.push_back(slot);
				auto block = std::make_shared<CCountingVuBasicBlock>(context, begin, end, m_blockCategory, slot);
				block->Compile();
				return block;
			}
		};

		// ---- VIF1 command stream (--trace-vif <from>-<to>): what the game sends to VU1 ----
		// Every DMA transfer the VIF1 consumes during the frame window is written to
		// vif1-stream.bin: a header {frame, consumed bytes, tag included, source address}
		// followed by the consumed bytes. tools/recon/vifstream.py parses it.
		FILE* g_vifFile = nullptr;
		uint32_t g_vifFrom = 0, g_vifTo = 0;
		CVif* g_vif1 = nullptr;
		uint8_t* g_vifRam = nullptr;
		uint8_t* g_vifSpr = nullptr;

		uint32 TraceVifDma(uint32 address, uint32 qwc, uint32 unused, bool tagIncluded)
		{
			const uint32 consumed = g_vif1->ReceiveDMA(address, qwc, unused, tagIncluded);
			const uint32_t frame = g_frame.load(std::memory_order_relaxed);
			if(g_vifFile && consumed != 0 && frame >= g_vifFrom && frame < g_vifTo)
			{
				const uint32_t header[4] = {frame, consumed * 16, tagIncluded ? 1u : 0u, address};
				fwrite(header, sizeof header, 1, g_vifFile);
				const bool spr = (address & 0x80000000) != 0;
				const uint8_t* source = spr ? g_vifSpr : g_vifRam;
				const uint32_t mask = spr ? (PS2::EE_SPR_SIZE - 1) : (PS2::EE_RAM_SIZE - 1);
				for(uint32_t i = 0; i < consumed * 16; i += 16) fwrite(source + ((address + i) & mask), 16, 1, g_vifFile);
			}
			return consumed;
		}

		// ---- GS privileged registers (--trace-gs): when the game flips, in emulated refreshes ----
		// Every EE write to 0x12000000-0x1200108C is listed in gs-priv.tsv with the refresh
		// (vertical blank) it happened in: the game's own frame changes (DISPFB1/2), counted
		// in emulated time, independent of how fast the host renders.
		FILE* g_gsFile = nullptr;

		// ---- time per emulated refresh ------------------------------------------------------
		struct FrameSample
		{
			uint32_t frame;
			int64_t qpc;          // when the refresh's work ended
			uint64_t vmCycles;    // CPU cycles the VM thread used for this refresh
			uint64_t gsCycles;    // CPU cycles the GS thread used since the previous sample
			int64_t waitTicks;    // QPC ticks spent in the pacer afterwards
			uint32_t firstZone;   // index into g_zoneTimes
		};
		std::vector<FrameSample> g_samples;
		std::vector<uint64_t> g_zoneTimes;      // nanoseconds, zoneCount per sample
		std::vector<std::string> g_zoneNames;   // fixed at the first sample
		std::atomic<uint64_t> g_gsThreadCycles{0};
		uint64_t g_lastVmCycles = 0, g_lastGsCycles = 0;
		int64_t g_waitStart = 0;
		int64_t g_startQpc = 0;
		uint64_t g_startTsc = 0;

		int64_t Qpc()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		bool SaveBytes(const fs::path& path, const void* data, size_t size)
		{
			FILE* file = _wfopen(path.c_str(), L"wb");
			if(!file) return false;
			const bool ok = fwrite(data, 1, size, file) == size;
			fclose(file);
			return ok;
		}
	}

	bool TraceEnabled()
	{
		return g_enabled;
	}

	namespace
	{
		LiftLayout ReferenceLayout()
		{
			LiftLayout layout;
			layout.pc = offsetof(MIPSSTATE, nPC);
			layout.gpr = offsetof(MIPSSTATE, nGPR);
			layout.gprStride = sizeof(uint128);
			layout.hi = offsetof(MIPSSTATE, nHI);
			layout.lo = offsetof(MIPSSTATE, nLO);
			layout.hi1 = offsetof(MIPSSTATE, nHI1);
			layout.lo1 = offsetof(MIPSSTATE, nLO1);
			layout.fpr = offsetof(MIPSSTATE, nCOP1);
			layout.fcsr = offsetof(MIPSSTATE, nFCSR);
			layout.fpacc = offsetof(MIPSSTATE, nCOP1A);
			layout.vf = offsetof(MIPSSTATE, nCOP2);
			layout.cycleQuota = offsetof(MIPSSTATE, cycleQuota);
			layout.hasException = offsetof(MIPSSTATE, nHasException);
			layout.vacc = offsetof(MIPSSTATE, nCOP2A);
			layout.vi = offsetof(MIPSSTATE, nCOP2VI);
			layout.vq = offsetof(MIPSSTATE, nCOP2Q);
			layout.viI = offsetof(MIPSSTATE, nCOP2I);
			layout.vr = offsetof(MIPSSTATE, nCOP2R);
			layout.vcf = offsetof(MIPSSTATE, nCOP2CF);
			layout.vmf = offsetof(MIPSSTATE, nCOP2MF);
			layout.vdf = offsetof(MIPSSTATE, nCOP2DF);
			layout.vt = offsetof(MIPSSTATE, nCOP2T);
			layout.pipeMac = offsetof(MIPSSTATE, pipeMac);
			layout.pipeSticky = offsetof(MIPSSTATE, pipeSticky);
			layout.pipeClip = offsetof(MIPSSTATE, pipeClip);
			layout.pipeTime = offsetof(MIPSSTATE, pipeTime);
			layout.pipeQ = offsetof(MIPSSTATE, pipeQ);
			static_assert(offsetof(FLAG_PIPELINE, index) == 0 && offsetof(FLAG_PIPELINE, values) == 4 &&
			              offsetof(FLAG_PIPELINE, pipeTimes) == 36, "flag pipeline layout assumed by the translator");
			static_assert(offsetof(REGISTER_PIPELINE, counter) == 0 && offsetof(REGISTER_PIPELINE, heldValue) == 4,
			              "register pipeline layout assumed by the translator");
			return layout;
		}
	}

	std::string TraceInstall(CPS2VM& vm, const fs::path& dir, bool countBlocks, const fs::path& liftList,
	                         const fs::path& elf, bool runTranslated, int vu1Mode, uint32_t vifFrom, uint32_t vifTo,
	                         bool traceGs, const std::vector<uint32_t>& watched)
	{
		std::error_code ec;
		fs::create_directories(dir, ec);
		g_dir = dir;
		g_countBlocks = countBlocks;
		if(!liftList.empty())
		{
			const LiftLayout layout = ReferenceLayout();
			std::string error, report;
			if(!LiftCheckPrepare(elf, liftList, layout, kRamSize, error, report)) return error;
			if(FILE* file = _wfopen((dir / L"translation.txt").c_str(), L"w"))
			{
				fputs(report.c_str(), file);
				fclose(file);
			}
			g_ee = &vm.m_ee->m_EE;
			g_ram = vm.m_ee->m_ram;
			g_spr = vm.m_ee->m_spr;
			if(runTranslated)
			{
				g_runTranslated = true;
			}
			else
			{
				g_liftCheck = true;
				g_ramEntry.resize(kRamSize);
				g_ramLifted.resize(kRamSize);
				g_sprEntry.resize(kSprSize);
				g_sprLifted.resize(kSprSize);
			}
		}
		g_blocks.reserve(1 << 18);
		g_blockCounts.reserve(1 << 18);  // never reallocated while compiled code holds indices into it
		g_translatedEntries.reserve(1 << 16);
		g_vuCounts.reserve(1 << 16);  // never reallocated while compiled code holds indices into it
		g_samples.reserve(1 << 16);
		g_zoneTimes.reserve(1 << 20);

		// Swap the executor. The core's page-fault handler keeps a pointer to the
		// executor it was registered for, so unregister the old one first.
		auto& ee = vm.m_ee->m_EE;
		static_cast<CEeExecutor*>(ee.m_executor.get())->RemoveExceptionHandler();
		ee.m_emptyBlockHandler = nullptr;  // the new executor's constructor installs its own
		ee.m_executor = std::make_unique<CTracingEeExecutor>(ee, vm.m_ee->m_ram);
		static_cast<CEeExecutor*>(ee.m_executor.get())->AddExceptionHandler();

		auto& vu1 = vm.m_ee->m_VU1;
		vu1.m_emptyBlockHandler = nullptr;
		vu1.m_executor = std::make_unique<CTracingVuExecutor>(vu1, PS2::MICROMEM1SIZE);

		if(vu1Mode != 0)
		{
			const VuLiftLayout vuLayout{static_cast<uint32_t>(offsetof(MIPSSTATE, nDelayedJumpAddr)),
			                            static_cast<uint32_t>(offsetof(MIPSSTATE, pipeFmacWrite)),
			                            static_cast<uint32_t>(offsetof(MIPSSTATE, savedNextBlockIntRegIdx)),
			                            static_cast<uint32_t>(offsetof(MIPSSTATE, savedNextBlockIntRegVal)),
			                            static_cast<uint32_t>(offsetof(MIPSSTATE, xgkickAddress))};
			const VuLiftConfig config{PS2::MICROMEM1SIZE, PS2::VUMEM1SIZE - 1, CVpu::VU_ADDR_TOP, CVpu::VU_ADDR_XGKICK,
			                          CVpu::VU_ADDR_ITOP, MIPS_EXCEPTION_VU_DBIT, MIPS_EXCEPTION_VU_TBIT, MIPS_EXCEPTION_VU_EBIT,
			                          MIPS_EXCEPTION_STATUS_QUOTADONE, MIPS_INVALID_PC};
			std::string error, report;
			if(!VuLiftPrepare(elf, ReferenceLayout(), vuLayout, config, g_vuImages, error, report)) return error;
			if(FILE* file = _wfopen((dir / L"vu1-translation.txt").c_str(), L"w"))
			{
				fputs(report.c_str(), file);
				fclose(file);
			}
			g_vuImageState.assign(g_vuImages.size(), ImageState::Unknown);
			g_vu1 = &vu1;
			g_microMem1 = vm.m_ee->m_microMem1;
			g_vuMem1 = vm.m_ee->m_vuMem1;
			g_vuMemLifted.resize(PS2::VUMEM1SIZE);
			g_vuMode = vu1Mode;
			if(vu1Mode == 1)
			{
				// Record what the reference core hands to the GIF (xgkick), for the comparison.
				auto* port = const_cast<CMemoryMap::MEMORYMAPELEMENT*>(vu1.m_pMemoryMap->GetWriteMap(CVpu::VU_ADDR_XGKICK));
				if(!port) return "the VU1 memory map has no xgkick port";
				auto original = port->handler;
				port->handler = [original](uint32 address, uint32 value) -> uint32 {
					if(g_recordReferenceKicks && address == CVpu::VU_ADDR_XGKICK)
						g_kicksReference.push_back({value, HashVuMemory(g_vuMem1)});
					return original(address, value);
				};
			}
		}

		if(!watched.empty())
		{
			g_callsFile = _wfopen((dir / L"calls.tsv").c_str(), L"w");
			if(!g_callsFile) return "cannot create calls.tsv";
			fputs("# refresh\tms\tfunction\ta0\ta1\ta2\ta3\n", g_callsFile);
			g_watched = watched;
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			g_qpcToMs = 1000.0 / f.QuadPart;
			g_callsOrigin = Qpc();
		}
		if(traceGs)
		{
			g_gsFile = _wfopen((dir / L"gs-priv.tsv").c_str(), L"w");
			if(!g_gsFile) return "cannot create gs-priv.tsv";
			fputs("# refresh\taddress\tvalue (EE writes to the GS privileged registers)\n", g_gsFile);
			auto* port = const_cast<CMemoryMap::MEMORYMAPELEMENT*>(vm.m_ee->m_EE.m_pMemoryMap->GetWriteMap(0x12000000));
			if(!port) return "the EE memory map has no GS port";
			auto original = port->handler;
			port->handler = [original](uint32 address, uint32 value) -> uint32 {
				if(g_gsFile && address >= 0x12000000 && address <= 0x1200108C)
					fprintf(g_gsFile, "%u\t%08X\t%08X\n", g_frame.load(std::memory_order_relaxed), address, value);
				return original(address, value);
			};
		}
		if(vifTo > vifFrom)
		{
			g_vifFile = _wfopen((dir / L"vif1-stream.bin").c_str(), L"wb");
			if(!g_vifFile) return "cannot create vif1-stream.bin";
			g_vifFrom = vifFrom;
			g_vifTo = vifTo;
			g_vif1 = &vm.m_ee->m_vpu1->GetVif();
			g_vifRam = vm.m_ee->m_ram;
			g_vifSpr = vm.m_ee->m_spr;
			vm.m_ee->m_dmac.SetChannelTransferFunction(CDMAC::CHANNEL_ID_VIF1, &TraceVifDma);
		}

		g_startQpc = Qpc();
		g_startTsc = __rdtsc();
		g_enabled = true;
		return {};
	}

	void TraceVmFrameWorkDone(uint32_t frame)
	{
		if(!g_enabled) return;
		// Blocks compiled from now on belong to the next refresh.
		g_frame.store(frame + 1, std::memory_order_relaxed);

		FrameSample sample = {};
		sample.frame = frame;
		sample.qpc = Qpc();
		ULONG64 cycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		sample.vmCycles = cycles - g_lastVmCycles;
		g_lastVmCycles = cycles;
		const uint64_t gs = g_gsThreadCycles.load(std::memory_order_relaxed);
		sample.gsCycles = gs - g_lastGsCycles;
		g_lastGsCycles = gs;

		// The core's own profiler (only when built with HP2_PLAY_PROFILE): time
		// per zone since the previous refresh. It is reset right after this handler.
		const auto zones = CProfiler::GetInstance().GetStats();
		if(g_zoneNames.empty())
			for(const auto& zone : zones) g_zoneNames.push_back(zone.name);
		sample.firstZone = static_cast<uint32_t>(g_zoneTimes.size());
		for(size_t i = 0; i < g_zoneNames.size(); i++)
			g_zoneTimes.push_back(i < zones.size() ? zones[i].totalTime : 0);

		g_samples.push_back(sample);
		g_waitStart = Qpc();
	}

	void TraceVmFrameWaitDone()
	{
		if(!g_enabled || g_samples.empty()) return;
		g_samples.back().waitTicks = Qpc() - g_waitStart;
		// The wait must not count as CPU work of the next refresh.
		ULONG64 cycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		g_lastVmCycles = cycles;
	}

	void TracePresent()
	{
		if(!g_enabled) return;
		ULONG64 cycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		g_gsThreadCycles.store(cycles, std::memory_order_relaxed);
	}

	void TraceFinish(CPS2VM& vm)
	{
		if(!g_enabled) return;
		g_enabled = false;

		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const double qpcToMs = 1000.0 / frequency.QuadPart;
		const int64_t endQpc = Qpc();
		const uint64_t endTsc = __rdtsc();
		const double cyclesPerMs = (endTsc - g_startTsc) / ((endQpc - g_startQpc) * qpcToMs);

		if(FILE* file = _wfopen((g_dir / L"blocks.tsv").c_str(), L"w"))
		{
			fprintf(file, "# every EE block the reference core compiled: begin, end (address of the last instruction), frame%s\n",
			        g_countBlocks ? ", times executed" : "");
			for(size_t i = 0; i < g_blocks.size(); i++)
			{
				const auto& block = g_blocks[i];
				if(g_countBlocks)
					fprintf(file, "%08X\t%08X\t%u\t%llu\n", block.begin, block.end, block.frame,
					        static_cast<unsigned long long>(g_blockCounts[i]));
				else
					fprintf(file, "%08X\t%08X\t%u\n", block.begin, block.end, block.frame);
			}
			fclose(file);
		}

		if(FILE* file = _wfopen((g_dir / L"profile.csv").c_str(), L"w"))
		{
			fprintf(file, "frame,t_ms,vm_cpu_ms,gs_cpu_ms,wait_ms");
			for(const auto& name : g_zoneNames) fprintf(file, ",%s", name.c_str());
			fputc('\n', file);
			for(const auto& sample : g_samples)
			{
				fprintf(file, "%u,%.3f,%.3f,%.3f,%.3f", sample.frame, (sample.qpc - g_startQpc) * qpcToMs,
				        sample.vmCycles / cyclesPerMs, sample.gsCycles / cyclesPerMs, sample.waitTicks * qpcToMs);
				for(size_t i = 0; i < g_zoneNames.size(); i++)
					fprintf(file, ",%.3f", g_zoneTimes[sample.firstZone + i] / 1e6);
				fputc('\n', file);
			}
			fclose(file);
		}

		if(FILE* file = _wfopen((g_dir / L"vu1-blocks.tsv").c_str(), L"w"))
		{
			fprintf(file, "# distinct VU1 microcode blocks the reference core compiled (%llu block creations in total):\n"
			              "# begin, end, first frame, times created, times executed (with --trace-counts), contents (32-bit words, lower/upper pairs)\n",
			        static_cast<unsigned long long>(g_vuBlocksCreated));
			for(const auto& [key, block] : g_vuBlocks)
			{
				uint64_t executed = 0;
				for(uint32_t slot : block.slots) executed += g_vuCounts[slot];
				fprintf(file, "%04X\t%04X\t%u\t%u\t%llu\t", block.begin, block.end, block.firstFrame, block.created,
				        static_cast<unsigned long long>(executed));
				for(uint32_t word : block.words) fprintf(file, "%08X", word);
				fputc('\n', file);
			}
			fclose(file);
		}

		if(g_liftCheck)
		{
			auto median = [](std::vector<uint64_t> v) -> double {
				if(v.empty()) return 0;
				std::sort(v.begin(), v.end());
				return static_cast<double>(v[v.size() / 2]);
			};
			const double cyclesPerNs = cyclesPerMs / 1e6;
			if(FILE* file = _wfopen((g_dir / L"lift-check.tsv").c_str(), L"w"))
			{
				fprintf(file, "function\tverified\tstate_mismatch\tmemory_mismatch\treference_only_memory_changes"
				              "\tstopped_early\ttouched_hardware\treference_calls_timed\treference_median_ns"
				              "\ttranslated_median_ns\tfirst_mismatch\n");
				for(const auto& [function, stats] : g_liftStats)
					fprintf(file, "%08X\t%d\t%d\t%d\t%d\t%d\t%d\t%zu\t%.1f\t%.1f\t%s\n", function, stats.verified,
					        stats.stateMismatch, stats.liftedOnlyWrites, stats.referenceOnlyChanges, stats.stoppedEarly,
					        stats.touchedHardware, stats.referenceTicks.size(), median(stats.referenceTicks) / cyclesPerNs,
					        median(stats.liftedTicks) / 1000.0 / cyclesPerNs, stats.firstMismatch.c_str());
				fclose(file);
			}
		}
		if(g_runTranslated)
		{
			if(FILE* file = _wfopen((g_dir / L"translation.txt").c_str(), L"a"))
			{
				fprintf(file, "# calls into translated code during the session: %llu\n",
				        static_cast<unsigned long long>(g_translatedCalls));
				fclose(file);
			}
		}

		if(g_vuMode != 0)
		{
			if(FILE* file = _wfopen((g_dir / L"vu1-check.txt").c_str(), L"w"))
			{
				const auto& v = g_vuStats;
				const double cyclesPerNs = cyclesPerMs / 1e6;
				fprintf(file, "mode: %s\n", g_vuMode == 1 ? "check (translated runs compared with the reference core)" : "run");
				fprintf(file, "operand tables: %u words compared with the reference core, %u differ%s%s\n", v.operandWords,
				        v.operandMismatches, v.firstOperandMismatch.empty() ? "" : "; first: ", v.firstOperandMismatch.c_str());
				if(g_vuMode == 1)
				{
					fprintf(file, "time slices verified: %llu (with %llu xgkicks); state mismatches %llu, data memory mismatches %llu, "
					              "xgkick mismatches %llu\n",
					        static_cast<unsigned long long>(v.verified), static_cast<unsigned long long>(v.kicks),
					        static_cast<unsigned long long>(v.stateMismatch), static_cast<unsigned long long>(v.memoryMismatch),
					        static_cast<unsigned long long>(v.kickMismatch));
					fprintf(file, "first mismatch: %s\n", v.firstMismatch.empty() ? "none" : v.firstMismatch.c_str());
					fprintf(file, "not compared: %llu slices handed over to the reference core, %llu outside the translated images\n",
					        static_cast<unsigned long long>(v.handedOver), static_cast<unsigned long long>(v.notTranslated));
					fprintf(file, "reference core time in the verified slices: %.1f ms\n", v.referenceTicks / cyclesPerNs / 1e6);
				}
				else
				{
					fprintf(file, "translated calls %llu, hand-overs to the reference core %llu, reference blocks run %llu\n",
					        static_cast<unsigned long long>(v.runCalls), static_cast<unsigned long long>(v.runHandOvers),
					        static_cast<unsigned long long>(v.runReferenceBlocks));
				}
				size_t entries = 0;
				double entrySeconds = 0;
				std::string refused;
				VuLiftEntryStats(entries, entrySeconds, refused);
				fprintf(file, "entries translated on first use: %zu, %.3f s in total; not translated: %s\n", entries, entrySeconds,
				        refused.empty() ? "none" : refused.c_str());
				for(const auto& [pc, n] : v.handOverAt) fprintf(file, "hand-over at %04X: %llu\n", pc, static_cast<unsigned long long>(n));
				fclose(file);
			}
		}

		if(g_callsFile)
		{
			fclose(g_callsFile);
			g_callsFile = nullptr;
		}
		if(g_gsFile)
		{
			fclose(g_gsFile);
			g_gsFile = nullptr;
		}
		if(g_vifFile)
		{
			fclose(g_vifFile);
			g_vifFile = nullptr;
		}
		SaveBytes(g_dir / L"vu1-micro.bin", vm.m_ee->m_microMem1, PS2::MICROMEM1SIZE);
		SaveBytes(g_dir / L"ee-ram.bin", vm.m_ee->m_ram, PS2::EE_RAM_SIZE);
		SaveBytes(g_dir / L"ee-bios.bin", vm.m_ee->m_bios, PS2::EE_BIOS_SIZE);
	}
}
