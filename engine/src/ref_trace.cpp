// See ref_trace.h.
#include "ref_trace.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "PS2VM.h"
#include "Profiler.h"
#include "Ps2Const.h"
#include "ee/EeExecutor.h"
#include "ee/VuExecutor.h"

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
		class CTracingEeExecutor : public CEeExecutor
		{
		public:
			using CEeExecutor::CEeExecutor;

			BasicBlockPtr BlockFactory(CMIPS& context, uint32 start, uint32 end) override
			{
				g_blocks.push_back({start, end, g_frame.load(std::memory_order_relaxed)});
				return CEeExecutor::BlockFactory(context, start, end);
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
		};
		std::map<std::pair<uint32_t, uint64_t>, VuBlock> g_vuBlocks;  // by (begin, content hash); VM thread only
		uint64_t g_vuBlocksCreated = 0;

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
				if(isNew) it->second = {begin, end, g_frame.load(std::memory_order_relaxed), 0, std::move(words)};
				++it->second.created;
				return CVuExecutor::BlockFactory(context, begin, end);
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

	void TraceInstall(CPS2VM& vm, const fs::path& dir)
	{
		std::error_code ec;
		fs::create_directories(dir, ec);
		g_dir = dir;
		g_blocks.reserve(1 << 18);
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
			fprintf(file, "# every EE block the reference core compiled: begin, end (address of the last instruction), frame\n");
			for(const auto& block : g_blocks) fprintf(file, "%08X\t%08X\t%u\n", block.begin, block.end, block.frame);
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
			              "# begin, end, first frame, times created, contents (32-bit words, lower/upper pairs)\n",
			        static_cast<unsigned long long>(g_vuBlocksCreated));
			for(const auto& [key, block] : g_vuBlocks)
			{
				fprintf(file, "%04X\t%04X\t%u\t%u\t", block.begin, block.end, block.firstFrame, block.created);
				for(uint32_t word : block.words) fprintf(file, "%08X", word);
				fputc('\n', file);
			}
			fclose(file);
		}

		SaveBytes(g_dir / L"vu1-micro.bin", vm.m_ee->m_microMem1, PS2::MICROMEM1SIZE);
		SaveBytes(g_dir / L"ee-ram.bin", vm.m_ee->m_ram, PS2::EE_RAM_SIZE);
		SaveBytes(g_dir / L"ee-bios.bin", vm.m_ee->m_bios, PS2::EE_BIOS_SIZE);
	}
}
