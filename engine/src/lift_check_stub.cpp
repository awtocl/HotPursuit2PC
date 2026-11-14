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

	bool VuLiftPrepare(const std::filesystem::path&, const LiftLayout&, const VuLiftLayout&, const VuLiftConfig&,
	                   std::vector<VuLiftImage>&, std::string& error, std::string&)
	{
		error = "this launcher was built without the translator (use the 'profile' build with LLVM present)";
		return false;
	}

	bool VuLiftIsBlockStart(size_t, uint32_t)
	{
		return false;
	}

	VuLiftedCode VuLiftEntry(size_t, uint32_t)
	{
		return nullptr;
	}

	void VuLiftEntryStats(size_t& entries, double& seconds, std::string& refused)
	{
		entries = 0;
		seconds = 0;
		refused.clear();
	}

	void VuLiftOperands(uint32_t, uint32_t, bool, uint32_t out[16])
	{
		for(int i = 0; i < 16; i++) out[i] = 0;
	}

	LiftedCode LiftCheckLookup(uint32_t)
	{
		return nullptr;
	}

	LiftedCode LiftCheckLookupEntry(uint32_t)
	{
		return nullptr;
	}
}
