// The processor state of the native runtime (native/, ADR 0012): our own
// structure - no emulator involved. The translated code is built for this
// layout by `hp2recomp package --native` (later: the converter).
#pragma once

#include <cstddef>
#include <cstdint>

#include "hp2/code_package.h"

namespace hp2 {

struct alignas(16) NativeState {
  std::uint32_t pc = 0;
  std::uint32_t delayed_jump = 1;
  std::uint32_t has_exception = 0;
  std::int32_t cycle_quota = 0;
  std::uint64_t gpr[32][2] = {};  // 128-bit registers, low half first
  std::uint64_t hi = 0, lo = 0, hi1 = 0, lo1 = 0;
  std::uint32_t fpr[32] = {};
  std::uint32_t fpacc = 0, fcsr = 0;
  std::uint32_t cop0[32] = {};
  alignas(16) std::uint32_t vf[33][4] = {};
  std::uint32_t vacc[4] = {};
  std::uint32_t vi[16] = {};
  std::uint32_t vq = 0, vi_i = 0, vr = 0, vcf = 0, vmf = 0, vdf = 0, vt = 0;
  std::uint32_t pipe_q[2] = {};
  std::uint32_t pipe_mac[17] = {}, pipe_sticky[17] = {}, pipe_clip[17] = {};
  std::uint32_t pipe_time = 0;
  alignas(16) std::uint32_t fmac_write[12] = {};
  std::uint32_t saved_next_index = 0, saved_next_value = 0, xgkick_address = 0;
};

inline constexpr std::uint32_t kNativeRamSize = 32u << 20;
// has_exception values used by VU1 microcode in the native runtime.
inline constexpr std::uint32_t kNativeVuDbit = 8, kNativeVuTbit = 9, kNativeVuEbit = 10, kNativeQuotaDone = 0x80;

// Game functions the native runtime provides itself (ADR 0011): they are left
// out of the code package, so calls to them return to the runtime.
inline constexpr std::uint32_t kHookSifBindRpc = 0x00285788, kHookSifCallRpc = 0x00285958,
                               kHookSifCheckStatRpc = 0x00285B48;
inline constexpr std::uint32_t kNativeHooks[] = {kHookSifBindRpc, kHookSifCallRpc, kHookSifCheckStatRpc};

inline Hp2CodeLayout native_code_layout() {
  Hp2CodeLayout l = {};
  auto at = [](std::size_t o) { return static_cast<std::uint32_t>(o); };
  l.pc = at(offsetof(NativeState, pc));
  l.gpr = at(offsetof(NativeState, gpr));
  l.gpr_stride = 16;
  l.hi = at(offsetof(NativeState, hi));
  l.lo = at(offsetof(NativeState, lo));
  l.hi1 = at(offsetof(NativeState, hi1));
  l.lo1 = at(offsetof(NativeState, lo1));
  l.fpr = at(offsetof(NativeState, fpr));
  l.fcsr = at(offsetof(NativeState, fcsr));
  l.fpacc = at(offsetof(NativeState, fpacc));
  l.vf = at(offsetof(NativeState, vf));
  l.cycle_quota = at(offsetof(NativeState, cycle_quota));
  l.has_exception = at(offsetof(NativeState, has_exception));
  l.vacc = at(offsetof(NativeState, vacc));
  l.vi = at(offsetof(NativeState, vi));
  l.vq = at(offsetof(NativeState, vq));
  l.vi_i = at(offsetof(NativeState, vi_i));
  l.vr = at(offsetof(NativeState, vr));
  l.vcf = at(offsetof(NativeState, vcf));
  l.vmf = at(offsetof(NativeState, vmf));
  l.vdf = at(offsetof(NativeState, vdf));
  l.vt = at(offsetof(NativeState, vt));
  l.pipe_mac = at(offsetof(NativeState, pipe_mac));
  l.pipe_sticky = at(offsetof(NativeState, pipe_sticky));
  l.pipe_clip = at(offsetof(NativeState, pipe_clip));
  l.pipe_time = at(offsetof(NativeState, pipe_time));
  l.pipe_q = at(offsetof(NativeState, pipe_q));
  l.delayed_jump = at(offsetof(NativeState, delayed_jump));
  l.fmac_write = at(offsetof(NativeState, fmac_write));
  l.saved_next_index = at(offsetof(NativeState, saved_next_index));
  l.saved_next_value = at(offsetof(NativeState, saved_next_value));
  l.xgkick_address = at(offsetof(NativeState, xgkick_address));
  l.micro_size = 0x4000;
  l.data_mask = 0x3FFF;
  l.addr_top = 0x8400;
  l.addr_xgkick = 0x8410;
  l.addr_itop = 0x8420;
  l.exception_dbit = kNativeVuDbit;
  l.exception_tbit = kNativeVuTbit;
  l.exception_ebit = kNativeVuEbit;
  l.quota_done = kNativeQuotaDone;
  l.invalid_pc = 1;
  l.ram_size = kNativeRamSize;
  return l;
}

}  // namespace hp2
