// Development-only measurements of the reference core (launcher switch
// --trace <dir>). Used by stage R1 (ADR 0006) to learn, from a real session:
//   - which game code the core executed (every block it compiled),
//   - where the time of each emulated refresh went,
//   - what main memory looked like at the end.
// Nothing here changes what the game does. Output is derived from the
// owner's dump and therefore only ever written to git-ignored folders.
#pragma once

#include <cstdint>
#include <filesystem>

class CPS2VM;

namespace hp2ref
{
	// Call once, with the virtual machine created and paused, before it is reset.
	// Replaces the core's EE executor by a subclass that records every block it
	// compiles (the same seam the recompiled code will use).
	void TraceInstall(CPS2VM& vm, const std::filesystem::path& dir);
	bool TraceEnabled();

	// VM thread, in the new-frame handler: before and after the pacing wait.
	void TraceVmFrameWorkDone(uint32_t frame);
	void TraceVmFrameWaitDone();

	// GS thread, once per presented image.
	void TracePresent();

	// With the virtual machine paused, before it is destroyed. Writes
	// blocks.tsv (EE code executed), vu1-blocks.tsv (vector unit microcode
	// executed, with contents), profile.csv, and copies of main memory, the
	// vector unit's program memory and the core's BIOS stand-in.
	void TraceFinish(CPS2VM& vm);
}
