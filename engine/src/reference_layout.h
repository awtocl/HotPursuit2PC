// Where the translated game code finds things in the reference core: its
// processor-state layout and VU1 constants (hp2::Hp2CodeLayout, ADR 0009).
// Computed from the reference core's own headers, so the converter (which
// translates for it) and the launcher (which checks a package against it)
// agree by construction.
#pragma once

#include "hp2/code_package.h"

namespace hp2ref
{
	hp2::Hp2CodeLayout ReferenceCodeLayout();
}
