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
#include "MemoryUtils.h"
#include "ee/VuBasicBlock.h"
#include "ee/VuExecutor.h"
#include "string_format.h"

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

		class CTracingEeExecutor : public CEeExecutor
		{
		public:
			using CEeExecutor::CEeExecutor;

			BasicBlockPtr BlockFactory(CMIPS& context, uint32 start, uint32 end) override
			{
				g_blocks.push_back({start, end, g_frame.load(std::memory_order_relaxed)});
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

		class CTracingVuExecutor : public CVuExecutor
		{
		public:
			using CVuExecutor::CVuExecutor;

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

	std::string TraceInstall(CPS2VM& vm, const fs::path& dir, bool countBlocks, const fs::path& liftList,
	                         const fs::path& elf, bool runTranslated)
	{
		std::error_code ec;
		fs::create_directories(dir, ec);
		g_dir = dir;
		g_countBlocks = countBlocks;
		if(!liftList.empty())
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

		SaveBytes(g_dir / L"vu1-micro.bin", vm.m_ee->m_microMem1, PS2::MICROMEM1SIZE);
		SaveBytes(g_dir / L"ee-ram.bin", vm.m_ee->m_ram, PS2::EE_RAM_SIZE);
		SaveBytes(g_dir / L"ee-bios.bin", vm.m_ee->m_bios, PS2::EE_BIOS_SIZE);
	}
}
