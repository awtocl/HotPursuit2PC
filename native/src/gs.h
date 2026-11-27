// The graphics synthesizer of the native runtime (ADR 0012): our own implementation, written for this project.
// Local memory, the host-to-local image transfer, a software rasteriser for the GS primitives and a window that
// shows the display buffer. (The hardware layout tables of the GS memory are facts of the console; the code is ours.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
void Shutdown();
// Diagnostic: the last GS event-register activity (F FINISH write, W CSR write, R CSR read, M IMR), oldest first.
std::string CsrTraceText();
// Diagnostic: the frame buffers that received the most pixels since the last call (by FBP), PMODE and DISPFB1/2.
std::string TargetsText();
// Diagnostic: log every large primitive (rectangle >= 2048 px) while on; the text collected so far.
void SetPrimLog(bool on);
// Diagnostic: a buffer of GS memory (base page = FBP, width in 64-pixel units) written as a BMP file.
bool DumpPage(std::uint32_t page, std::uint32_t fbw, std::uint32_t w, std::uint32_t h, std::uint32_t psm, const char* path);
std::string PrimLogText();  // before leaving main: closes the window, joins its thread
bool WindowFocused();
// Settings overlay (ADR 0013): while it is open the game gets no pad input. `pressed` = buttons held this vblank in libpad's bit
// order (0x0001 select, 0x0008 start, 0x0010 up, 0x0020 right, 0x0040 down, 0x0080 left, 0x2000 circle, 0x4000 cross).
bool OverlayVisible();
void OverlayPad(std::uint16_t pressed);
// Opens the graphics settings (from the game's Options > Display menu); no effect if already open.
void OpenSettings();
// The Aspect ratio setting is 16:9: the runtime keeps the game's widescreen patches written (ADR 0015). Any thread.
bool Widescreen();
// The game draws its picture for 16:9 (the widescreen patches are in place, decided at start): the window shows it at 16:9.
void SetWidePicture(bool wide);
// Development: writes the last presented frame as a BMP file (kept out of git: .cache).
bool DumpFrame(const char* path);
// Development: the next picture the window shows (post-processing and settings menu included) as a BMP file.
void RequestWindowShot(const char* path);

struct Stats {
  std::uint64_t triangles = 0, sprites = 0, lines = 0, points = 0, pixels = 0, textured = 0, transfers = 0, transferBytes = 0, frames = 0;
  std::uint64_t unsupported = 0;
  // time spent (QPC ticks) drawing primitives, decoding textures (and how many), reading out the display, transfers
  std::uint64_t drawTicks = 0, textureTicks = 0, textureDecodes = 0, presentTicks = 0, transferTicks = 0;
  std::uint64_t waitTicks = 0;  // the GS thread waiting for the band workers (display read-out, render-to-texture, uploads)
  // ... by reason: texture (render to texture), upload, palette, display read-out, full barrier; time and number of waits
  std::uint64_t waitByReason[5] = {}, waitsByReason[5] = {};
  std::uint64_t shaded = 0;  // pixels shaded (written or rejected by the depth/alpha tests)
  std::uint64_t directTextures = 0;  // buffer copies that read GS memory directly (no wait, no decode)
  std::uint64_t skippedDepthFog = 0;  // 1 once the race's depth-fog pass has been skipped
  std::uint64_t flips = 0;            // changes of DISPFB1: frames the game finished
  std::uint64_t textureStale = 0, textureSame = 0;  // cached textures whose pages changed; of those, same bytes (not decoded)
};
const Stats& GetStats();
// Prefer this window size (client area); 0 = the game's own display size.
void SetWindowScale(int percent);
// Development (HP2_GS_FILLTEST=1): full-screen textured triangles through the whole GS path; million pixels per second.
double FillTest(int quads, bool linear, bool blend);
// Development (HP2_LOG_WAITS=1): texture read-backs that waited long for the band workers.
std::string WaitNotes();
// Development (HP2_SAMPLE profiler): the GS's own threads, by group ("gs", "workers"), as thread handles.
std::vector<std::pair<std::string, void*>> ProfiledThreads();

}  // namespace gs
