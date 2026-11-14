// The translated game code at run time (ADR 0009): loads build/hp2code.dll
// (written once by the converter's "code" stage) and runs the game program
// and the VU1 microcode on it, inside the reference core's machine model.
// Anything the package does not cover - an untranslated function, microcode
// that differs from what was translated - keeps running on the reference core.
#pragma once

#include <filesystem>
#include <string>

class CPS2VM;

namespace hp2rt
{
	// Call with the virtual machine created and paused, before it is reset.
	// `dll`: the code package; `elf`: the game executable it must have been
	// made from. Returns one line for the log: what was installed, or why
	// the package is not used (missing, made for another executable or
	// launcher, CPU without the instructions it was compiled for).
	std::string InstallTranslatedCode(CPS2VM& vm, const std::filesystem::path& dll, const std::filesystem::path& elf);

	// Counters for the log at exit (empty when nothing was installed).
	std::string TranslatedCodeReport();
}
