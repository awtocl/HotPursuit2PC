// See lift_check.h. Compiled only in builds with HP2_LIFT_CHECK (LLVM present).
#include "lift_check.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>

#include "hp2/io.h"
#include "hp2/lift.h"
#include "hp2/vu.h"
#include "hp2/vulift.h"

static_assert(sizeof(hp2ref::LiftRuntime) == sizeof(hp2::Runtime));
static_assert(sizeof(hp2ref::VuLiftRuntime) == sizeof(hp2::VuRuntime));

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
		std::unique_ptr<hp2::VuTranslator> g_vu;

		hp2::StateLayout ToStateLayout(const LiftLayout& layout)
		{
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
			return l;
		}
	}

	bool VuLiftPrepare(const std::filesystem::path& elfPath, const LiftLayout& layout, const VuLiftLayout& vuLayout,
	                   const VuLiftConfig& config, std::vector<VuLiftImage>& images, std::string& error, std::string& report)
	{
		try
		{
			const auto t0 = std::chrono::steady_clock::now();
			const auto elf = hp2::ElfImage::parse(hp2::read_whole_file(elfPath, 64ull << 20));
			hp2::VuLayout v;
			v.delayed_jump = vuLayout.delayedJump;
			v.fmac_write = vuLayout.fmacWrite;
			v.saved_next_index = vuLayout.savedNextIndex;
			v.saved_next_value = vuLayout.savedNextValue;
			v.xgkick_address = vuLayout.xgkickAddress;
			hp2::VuConfig c;
			c.micro_size = config.microSize;
			c.data_mask = config.dataMask;
			c.addr_top = config.addrTop;
			c.addr_xgkick = config.addrXgkick;
			c.addr_itop = config.addrItop;
			c.exception_dbit = config.exceptionDbit;
			c.exception_tbit = config.exceptionTbit;
			c.exception_ebit = config.exceptionEbit;
			c.quota_done = config.quotaDone;
			c.invalid_pc = config.invalidPc;
			g_vu = std::make_unique<hp2::VuTranslator>(ToStateLayout(layout), v, c);
			for(hp2::vu::MicroImage& im : hp2::vu::microcode_images(elf, config.microSize))
				g_vu->add_image(im.vu_address, std::move(im.words));
			const auto t1 = std::chrono::steady_clock::now();
			g_vu->compile();
			const auto t2 = std::chrono::steady_clock::now();
			images.clear();
			for(size_t i = 0; i < g_vu->images(); i++)
				images.push_back({g_vu->image_address(i), g_vu->image_words(i), reinterpret_cast<VuLiftedCode>(g_vu->code(i))});
			report = std::format("# VU1: {} microcode images, {} block starts ({} pairs); translation {:.2f} s, "
			                     "optimisation and compilation {:.2f} s\n",
			                     g_vu->images(), g_vu->stats().blocks, g_vu->stats().pairs,
			                     std::chrono::duration<double>(t1 - t0).count(),
			                     std::chrono::duration<double>(t2 - t1).count());
			for(const auto& [start, why] : g_vu->refused_starts())
				report += std::format("{:04X}\tVU1 block not translated: {}\n", start, why);
			return true;
		}
		catch(const std::exception& e)
		{
			error = e.what();
			g_vu.reset();
			return false;
		}
	}

	bool VuLiftIsBlockStart(size_t image, uint32_t address)
	{
		return g_vu && image < g_vu->images() && g_vu->is_block_start(image, address);
	}

	VuLiftedCode VuLiftEntry(size_t image, uint32_t address)
	{
		if(!g_vu) return nullptr;
		return reinterpret_cast<VuLiftedCode>(g_vu->entry(image, address));
	}

	void VuLiftEntryStats(size_t& entries, double& seconds, std::string& refused)
	{
		entries = g_vu ? g_vu->entry_functions() : 0;
		seconds = g_vu ? g_vu->entry_seconds() : 0;
		refused.clear();
		if(g_vu)
			for(const auto& [start, why] : g_vu->refused_starts()) refused += std::format("{:04X} {}; ", start, why);
	}

	void VuLiftOperands(uint32_t lower, uint32_t upper, bool upperWord, uint32_t out[16])
	{
		const hp2::vu::Operands o = upperWord ? hp2::vu::upper_operands(upper) : hp2::vu::lower_operands(lower, upper);
		const uint32_t values[16] = {o.write_f, o.read_f0, o.read_elem_f0, o.read_f1, o.read_elem_f1, o.write_i,
		                             o.write_i_lsu, o.read_i0, o.read_i1, o.sync_q, o.read_q, o.sync_p, o.read_p,
		                             o.read_mac, o.write_mac, o.branch_value};
		for(int i = 0; i < 16; i++) out[i] = values[i];
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
