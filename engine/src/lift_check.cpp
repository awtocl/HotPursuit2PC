// See lift_check.h. Compiled only in builds with HP2_LIFT_CHECK (LLVM present).
#include "lift_check.h"

#include <fstream>
#include <memory>
#include <sstream>

#include "hp2/io.h"
#include "hp2/lift.h"

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
			auto elf = hp2::ElfImage::parse(hp2::read_whole_file(elfPath, 64ull << 20));
			auto discovery = hp2::discover(elf);
			g_prepared = std::make_unique<Prepared>(Prepared{std::move(elf), std::move(discovery), nullptr});
			hp2::StateLayout l;
			l.pc = layout.pc;
			l.gpr = layout.gpr;
			l.gpr_stride = layout.gprStride;
			l.hi = layout.hi;
			l.lo = layout.lo;
			l.fpr = layout.fpr;
			l.fcsr = layout.fcsr;
			l.fpacc = layout.fpacc;
			g_prepared->translator =
			    std::make_unique<hp2::Translator>(g_prepared->elf, g_prepared->discovery, l, ramSize);

			std::ifstream in(list);
			if(!in)
			{
				error = "cannot open the function list " + hp2::to_utf8(list);
				return false;
			}
			std::string line;
			while(std::getline(in, line))
			{
				if(const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
				std::istringstream fields(line);
				std::string word;
				if(!(fields >> word)) continue;
				const uint32_t address = static_cast<uint32_t>(std::stoul(word, nullptr, 16));
				if(auto why = g_prepared->translator->add_function(address))
					report += std::format("{:08X}\tnot translated: {}\n", address, *why);
			}
			g_prepared->translator->compile();
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
}
