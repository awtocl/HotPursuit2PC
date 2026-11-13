// lift_check.h for builds without the translator (the normal case: LLVM is
// linked only into measurement builds, preset "profile").
#include "lift_check.h"

namespace hp2ref
{
	bool LiftCheckPrepare(const std::filesystem::path&, const std::filesystem::path&, const LiftLayout&, uint32_t,
	                      std::string& error, std::string&)
	{
		error = "this launcher was built without the translator (use the 'profile' build with LLVM present)";
		return false;
	}

	LiftedCode LiftCheckLookup(uint32_t)
	{
		return nullptr;
	}
}
