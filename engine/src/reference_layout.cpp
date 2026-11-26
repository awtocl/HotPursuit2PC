// See reference_layout.h.
#include "reference_layout.h"

#include <cstddef>

#include "MIPS.h"
#include "Ps2Const.h"
#include "ee/Vpu.h"

namespace hp2ref
{
	hp2::Hp2CodeLayout ReferenceCodeLayout()
	{
		static_assert(offsetof(FLAG_PIPELINE, index) == 0 && offsetof(FLAG_PIPELINE, values) == 4 &&
		                  offsetof(FLAG_PIPELINE, pipeTimes) == 36,
		              "flag pipeline layout assumed by the translator");
		static_assert(offsetof(REGISTER_PIPELINE, counter) == 0 && offsetof(REGISTER_PIPELINE, heldValue) == 4,
		              "register pipeline layout assumed by the translator");
		hp2::Hp2CodeLayout l = {};
		auto at = [](size_t offset) { return static_cast<uint32_t>(offset); };
		l.pc = at(offsetof(MIPSSTATE, nPC));
		l.gpr = at(offsetof(MIPSSTATE, nGPR));
		l.gpr_stride = sizeof(uint128);
		l.hi = at(offsetof(MIPSSTATE, nHI));
		l.lo = at(offsetof(MIPSSTATE, nLO));
		l.hi1 = at(offsetof(MIPSSTATE, nHI1));
		l.lo1 = at(offsetof(MIPSSTATE, nLO1));
		l.fpr = at(offsetof(MIPSSTATE, nCOP1));
		l.fcsr = at(offsetof(MIPSSTATE, nFCSR));
		l.fpacc = at(offsetof(MIPSSTATE, nCOP1A));
		l.vf = at(offsetof(MIPSSTATE, nCOP2));
		l.cycle_quota = at(offsetof(MIPSSTATE, cycleQuota));
		l.has_exception = at(offsetof(MIPSSTATE, nHasException));
		l.vacc = at(offsetof(MIPSSTATE, nCOP2A));
		l.vi = at(offsetof(MIPSSTATE, nCOP2VI));
		l.sa = 0;  // the reference core does not keep SA in MIPSSTATE for its own use: leave mtsa/qfsrv to it
		l.vq = at(offsetof(MIPSSTATE, nCOP2Q));
		l.vi_i = at(offsetof(MIPSSTATE, nCOP2I));
		l.vr = at(offsetof(MIPSSTATE, nCOP2R));
		l.vcf = at(offsetof(MIPSSTATE, nCOP2CF));
		l.vmf = at(offsetof(MIPSSTATE, nCOP2MF));
		l.vdf = at(offsetof(MIPSSTATE, nCOP2DF));
		l.vt = at(offsetof(MIPSSTATE, nCOP2T));
		l.pipe_mac = at(offsetof(MIPSSTATE, pipeMac));
		l.pipe_sticky = at(offsetof(MIPSSTATE, pipeSticky));
		l.pipe_clip = at(offsetof(MIPSSTATE, pipeClip));
		l.pipe_time = at(offsetof(MIPSSTATE, pipeTime));
		l.pipe_q = at(offsetof(MIPSSTATE, pipeQ));
		l.delayed_jump = at(offsetof(MIPSSTATE, nDelayedJumpAddr));
		l.fmac_write = at(offsetof(MIPSSTATE, pipeFmacWrite));
		l.saved_next_index = at(offsetof(MIPSSTATE, savedNextBlockIntRegIdx));
		l.saved_next_value = at(offsetof(MIPSSTATE, savedNextBlockIntRegVal));
		l.xgkick_address = at(offsetof(MIPSSTATE, xgkickAddress));
		l.micro_size = PS2::MICROMEM1SIZE;
		l.data_mask = PS2::VUMEM1SIZE - 1;
		l.addr_top = CVpu::VU_ADDR_TOP;
		l.addr_xgkick = CVpu::VU_ADDR_XGKICK;
		l.addr_itop = CVpu::VU_ADDR_ITOP;
		l.exception_dbit = MIPS_EXCEPTION_VU_DBIT;
		l.exception_tbit = MIPS_EXCEPTION_VU_TBIT;
		l.exception_ebit = MIPS_EXCEPTION_VU_EBIT;
		l.quota_done = MIPS_EXCEPTION_STATUS_QUOTADONE;
		l.invalid_pc = MIPS_INVALID_PC;
		l.ram_size = PS2::EE_RAM_SIZE;
		return l;
	}
}
