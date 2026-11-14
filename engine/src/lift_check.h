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
#include <vector>

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

	// ---- VU1 microcode (ADR 0008) ----
	// State fields used only by microcode, beyond LiftLayout (same structure).
	struct VuLiftLayout
	{
		uint32_t delayedJump, fmacWrite, savedNextIndex, savedNextValue, xgkickAddress;
	};
	struct VuLiftConfig
	{
		uint32_t microSize, dataMask, addrTop, addrXgkick, addrItop;
		uint32_t exceptionDbit, exceptionTbit, exceptionEbit, quotaDone, invalidPc;
	};
	// The VU1 memory map's I/O ports (same layout as hp2::VuRuntime).
	struct VuLiftRuntime
	{
		uint32_t (*read)(uint8_t* state, uint32_t address);
		void (*write)(uint8_t* state, uint32_t address, uint32_t value);
	};
	using VuLiftedCode = void (*)(uint8_t* state, uint8_t* vuMem, const VuLiftRuntime* rt);
	struct VuLiftImage
	{
		uint32_t address;
		std::vector<uint32_t> words;
		VuLiftedCode code;
	};
	// Translates the microcode images of `elf` (the game's uploads applied in order).
	bool VuLiftPrepare(const std::filesystem::path& elf, const LiftLayout& layout, const VuLiftLayout& vuLayout,
	                   const VuLiftConfig& config, std::vector<VuLiftImage>& images, std::string& error, std::string& report);
	bool VuLiftIsBlockStart(size_t image, uint32_t address);
	// Code entering `image` at `address` when it is not a block start (translated on first use), or nullptr.
	VuLiftedCode VuLiftEntry(size_t image, uint32_t address);
	// Entries translated so far and the time spent; the report lines of refused block starts.
	void VuLiftEntryStats(size_t& entries, double& seconds, std::string& refused);
	// The translator's view of a word's operands (hp2::vu::Operands), in the order of the reference core's
	// OPERANDSET: writeF, readF0, readElemF0, readF1, readElemF1, writeI, writeILsu, readI0, readI1,
	// syncQ, readQ, syncP, readP, readMACflags, writeMACflags, branchValue.
	void VuLiftOperands(uint32_t lower, uint32_t upper, bool upperWord, uint32_t out[16]);

	// After a successful prepare: the translated code for a function start, or nullptr.
	LiftedCode LiftCheckLookup(uint32_t address);
	// ... for any place where translated code can be entered (Translator::lookup_entry), or nullptr.
	LiftedCode LiftCheckLookupEntry(uint32_t address);
}
