// Development-only: the recompiler's translator inside the reference build
// (launcher switches --lift-check <list> and --run-translated <list>,
// measurement builds with LLVM only).
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
		uint32_t pc, gpr, gprStride, hi, lo, hi1, lo1, fpr, fcsr, fpacc, vf, cycleQuota, hasException;
		uint32_t vacc, vi, vq, viI, vr, vcf, vmf, vdf, vt, pipeMac, pipeSticky, pipeClip, pipeTime, pipeQ;
	};

	// Slow path for addresses that are neither main memory nor the scratchpad (same layout as hp2::Runtime).
	struct LiftRuntime
	{
		uint64_t (*read)(uint8_t* state, uint32_t address, uint32_t size);
		void (*write)(uint8_t* state, uint32_t address, uint64_t value, uint32_t size);
	};

	using LiftedCode = void (*)(uint8_t* state, uint8_t* ram, uint8_t* spr, const LiftRuntime* rt);

	// Translates the functions listed in `list` (one hexadecimal address per
	// line; '#' starts a comment) from the executable `elf`. Returns false and
	// sets `error` when the translator is not part of this build or fails.
	// `report` receives a summary and one line per listed function that could not be translated.
	bool LiftCheckPrepare(const std::filesystem::path& elf, const std::filesystem::path& list, const LiftLayout& layout,
	                      uint32_t ramSize, std::string& error, std::string& report);

	// After a successful prepare: the translated code for a function start, or nullptr.
	LiftedCode LiftCheckLookup(uint32_t address);
	// ... for any place where translated code can be entered (Translator::lookup_entry), or nullptr.
	LiftedCode LiftCheckLookupEntry(uint32_t address);
}
