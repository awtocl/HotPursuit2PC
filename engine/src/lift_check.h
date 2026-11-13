// Development-only: the translator spike inside the reference build
// (launcher switch --lift-check <list>, measurement builds with LLVM only).
//
// This header is included by C++17 code (ref_trace.cpp); the implementation
// (lift_check.cpp) is C++20 and uses the recompiler library.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace hp2ref
{
	// Where the pieces of processor state are inside the reference core's state structure.
	struct LiftLayout
	{
		uint32_t pc, gpr, gprStride, hi, lo, fpr, fcsr, fpacc;
	};

	using LiftedCode = void (*)(uint8_t* state, uint8_t* ram);

	// Translates the functions listed in `list` (one hexadecimal address per
	// line; '#' starts a comment) from the executable `elf`. Returns false and
	// sets `error` when the translator is not part of this build or fails.
	// `report` receives one line per listed function that could not be translated.
	bool LiftCheckPrepare(const std::filesystem::path& elf, const std::filesystem::path& list, const LiftLayout& layout,
	                      uint32_t ramSize, std::string& error, std::string& report);

	// After a successful prepare: the translated code for a function start, or nullptr.
	LiftedCode LiftCheckLookup(uint32_t address);
}
