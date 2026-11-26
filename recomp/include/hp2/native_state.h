// The processor state of the native runtime (native/, ADR 0012): our own
// structure - no emulator involved. The translated code is built for this
// layout by `hp2recomp package --native` (later: the converter).
#pragma once

#include <cstddef>
#include <cstdint>

#include "hp2/code_package.h"

namespace hp2 {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded because of alignas: intended, the vector registers are 16-byte aligned
#endif
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
  std::uint32_t sa = 0;  // SA register (qfsrv funnel shift, in bits)
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline constexpr std::uint32_t kNativeRamSize = 32u << 20;
// has_exception values used by VU1 microcode in the native runtime.
inline constexpr std::uint32_t kNativeVuDbit = 8, kNativeVuTbit = 9, kNativeVuEbit = 10, kNativeQuotaDone = 0x80;

// Game functions the native runtime provides itself (ADR 0011): they are left
// out of the code package, so calls to them return to the runtime.
inline constexpr std::uint32_t kHookSifBindRpc = 0x00285788, kHookSifCallRpc = 0x00285958,
                               kHookSifCheckStatRpc = 0x00285B48;
// sceSifInitRpc: starts the SIF command layer and waits for the I/O processor's answer. The runtime has no
// I/O processor: it marks the layer as started (the game's own "already initialised" word, 0x00327F7C).
// (0x0028511C is where discovery places the function start: both are left out.)
inline constexpr std::uint32_t kHookSifInitRpc = 0x00285118;
// The game's file API (found by its use of the RPC server 80000001): C signatures, served by the runtime from the disc.
inline constexpr std::uint32_t kHookOpen = 0x00286980, kHookClose = 0x00286C08, kHookLseek = 0x00286D88,
                               kHookRead = 0x00286FC0;
// Event handler of the "Calibrate display offset" screen of Options > Display (registered by name at 001F5968). Screen
// centring has no meaning on PC: the runtime serves this handler; when the screen opens (event 1) it shows its graphics
// settings, and closing them confirms the screen (event 19), which returns to Display (ADR 0013, settings in the game's menus).
inline constexpr std::uint32_t kHookCalibrateScreen = 0x001F4B20;
// Library function `ctc2 a0, CMSAR1; jr ra`: the EE starts a VU1 microprogram at a0 (8-byte units). The translated CTC2
// to CMSAR1 is ignored, so the runtime serves this function and runs VU1 there.
inline constexpr std::uint32_t kHookVu1Start = 0x002818D0;
inline constexpr std::uint32_t kNativeHooks[] = {kHookSifBindRpc, kHookSifCallRpc, kHookSifCheckStatRpc, kHookSifInitRpc,
                                                  0x0028511C,      kHookOpen,       kHookClose,          kHookLseek,
                                                  kHookRead,       kHookCalibrateScreen, kHookVu1Start};

// Code the game copies into kernel memory at start-up (found at 0x0028B73C-0x0028B748: it passes the
// source 0x00328E58, 1856 bytes, and the destination 0x80076000 to its own kernel-copy system call).
// It is EA's kernel-mode helper layer; translated like the rest, at its load address.
// A second blob goes to 0x80075000 (found at 0x0028A8B4-0x0028A8C4: source 0x00328020, 816 bytes).
inline constexpr std::uint32_t kNativeOverlays[][3] = {  // source, size, load
    {0x00328E58, 0x740, 0x80076000},
    {0x00328020, 0x330, 0x80075000},
};

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
  l.sa = at(offsetof(NativeState, sa));
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
