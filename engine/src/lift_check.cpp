// See lift_check.h. Compiled only in builds with HP2_LIFT_CHECK (LLVM present).
#include "lift_check.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>

#include "hp2/io.h"
#include "hp2/lift.h"

static_assert(sizeof(hp2ref::LiftRuntime) == sizeof(hp2::Runtime));

namespace hp2ref
{
	namespace
	{
		struct Prepared
		{
			hp2::ElfImage elf;
			hp2::Discovery discovery;
			std::unique_ptr<hp2::Translator> translator;
		};
		std::unique_ptr<Prepared> g_prepared;
	}

	bool LiftCheckPrepare(const std::filesystem::path& elfPath, const std::filesystem::path& list,
	                      const LiftLayout& layout, uint32_t ramSize, std::string& error, std::string& report)
	{
		try
		{
			const auto t0 = std::chrono::steady_clock::now();
			auto elf = hp2::ElfImage::parse(hp2::read_whole_file(elfPath, 64ull << 20));
			auto discovery = hp2::discover(elf);
			g_prepared = std::make_unique<Prepared>(Prepared{std::move(elf), std::move(discovery), nullptr});
			hp2::StateLayout l;
			l.pc = layout.pc;
			l.gpr = layout.gpr;
			l.gpr_stride = layout.gprStride;
			l.hi = layout.hi;
			l.lo = layout.lo;
			l.hi1 = layout.hi1;
			l.lo1 = layout.lo1;
			l.fpr = layout.fpr;
			l.fcsr = layout.fcsr;
			l.fpacc = layout.fpacc;
			l.vf = layout.vf;
			l.cycle_quota = layout.cycleQuota;
			l.has_exception = layout.hasException;
			l.vacc = layout.vacc;
			l.vi = layout.vi;
			l.vq = layout.vq;
			l.vi_i = layout.viI;
			l.vr = layout.vr;
			l.vcf = layout.vcf;
			l.vmf = layout.vmf;
			l.vdf = layout.vdf;
			l.vt = layout.vt;
			l.pipe_mac = layout.pipeMac;
			l.pipe_sticky = layout.pipeSticky;
			l.pipe_clip = layout.pipeClip;
			l.pipe_time = layout.pipeTime;
			l.pipe_q = layout.pipeQ;
			g_prepared->translator =
			    std::make_unique<hp2::Translator>(g_prepared->elf, g_prepared->discovery, l, ramSize);

			std::ifstream in(list);
			if(!in)
			{
				error = "cannot open the function list " + hp2::to_utf8(list);
				return false;
			}
			std::string line, refused;
			size_t listed = 0;
			while(std::getline(in, line))
			{
				if(const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
				std::istringstream fields(line);
				std::string word;
				if(!(fields >> word)) continue;
				++listed;
				const uint32_t address = static_cast<uint32_t>(std::stoul(word, nullptr, 16));
				if(auto why = g_prepared->translator->add_function(address))
					refused += std::format("{:08X}\tnot translated: {}\n", address, *why);
			}
			const auto t1 = std::chrono::steady_clock::now();
			g_prepared->translator->compile();
			const auto t2 = std::chrono::steady_clock::now();
			const auto& stats = g_prepared->translator->stats();
			report = std::format("# {} functions listed, {} translated ({} instructions, {} direct calls, {} hand-over "
			                     "places, {} re-entry places); translation {:.1f} s, optimisation and compilation {:.1f} s\n",
			                     listed, g_prepared->translator->size(), stats.instructions, stats.calls, stats.stops,
			                     g_prepared->translator->entries(),
			                     std::chrono::duration<double>(t1 - t0).count(),
			                     std::chrono::duration<double>(t2 - t1).count()) +
			         refused;
			return true;
		}
		catch(const std::exception& e)
		{
			error = e.what();
			g_prepared.reset();
			return false;
		}
	}

	LiftedCode LiftCheckLookup(uint32_t address)
	{
		if(!g_prepared || !g_prepared->translator) return nullptr;
		return reinterpret_cast<LiftedCode>(g_prepared->translator->lookup(address));
	}

	LiftedCode LiftCheckLookupEntry(uint32_t address)
	{
		if(!g_prepared || !g_prepared->translator) return nullptr;
		return reinterpret_cast<LiftedCode>(g_prepared->translator->lookup_entry(address));
	}
}
