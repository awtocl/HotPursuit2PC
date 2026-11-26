// The graphics synthesizer of the native runtime (ADR 0012): our own implementation, written for this project.
// Local memory, the host-to-local image transfer, a software rasteriser for the GS primitives and a window that
// shows the display buffer. (The hardware layout tables of the GS memory are facts of the console; the code is ours.)
#pragma once

#include <cstddef>
#include <cstdint>

namespace gs {

void Reset();
// A GS register write (GIF A+D, REGLIST, or a PACKED register converted to its register number).
void RegWrite(std::uint32_t reg, std::uint64_t value);
// PACKED ST: S, T and Q as raw float bits.
void StQ(std::uint32_t s, std::uint32_t t, std::uint32_t q);
// Image data of a host-to-local transfer (GIF IMAGE tag or HWREG): whole quadwords.
void ImageData(const std::uint8_t* data, std::size_t bytes);
// Privileged registers (PMODE, SMODE2, DISPFB1/2, DISPLAY1/2, BGCOLOR...), address 0x12000000 + offset.
// Privileged registers: `size` is the access width in bytes (4 or 8); a 32-bit write changes only its half of the register.
void PrivWrite(std::uint32_t address, std::uint64_t value, std::uint32_t size);
// GS_CSR (0x12001000) and GS_IMR (0x12001010) reads; `field` = current field bit (0/1).
std::uint64_t PrivRead(std::uint32_t address, std::uint32_t field);
// Vertical blank start: sets CSR.VSINT.
void VBlankStart();
// Called when CSR.SIGNAL or CSR.FINISH is raised and not masked in GS_IMR (the host raises INTC cause 0, GS).
void SetInterruptCallback(void (*callback)());
// Vertical blank: show the display buffer.
void Present();
bool WindowClosed();
bool WindowFocused();
// Development: writes the last presented frame as a BMP file (kept out of git: .cache).
bool DumpFrame(const char* path);

struct Stats {
  std::uint64_t triangles = 0, sprites = 0, lines = 0, points = 0, pixels = 0, textured = 0, transfers = 0, transferBytes = 0, frames = 0;
  std::uint64_t unsupported = 0;
};
const Stats& GetStats();
// Prefer this window size (client area); 0 = the game's own display size.
void SetWindowScale(int percent);

}  // namespace gs
