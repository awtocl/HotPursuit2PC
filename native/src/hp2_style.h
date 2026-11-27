// The port's own UI in Hot Pursuit 2's front-end style (ADR 0013, ADR 0016), shared by the launcher and the in-game
// settings menu. Taken from the game's menus by eye: a navy blueprint grid on near-black, brushed-steel strips (the
// title bar and the ticker), a heavy italic silver title with a black outline, a thin red rule under the title strip,
// menu rows on dark translucent bands with condensed bold outlined lettering, the selection in orange with an orange
// underline, small list cells (like the music list). Everything is drawn; no image or font of the game is used.
// GDI+ (a Windows component). Coordinates are whatever the caller's Graphics transform says. Every string drawn here
// goes through i18n::T (English in, the chosen language out; anything not in its table is drawn as it is).
#pragma once
#include <windows.h>

#include <objidl.h>

#include <algorithm>
#include <memory>
#include <string>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace hp2style {

namespace col {
constexpr Gdiplus::ARGB ink = 0xFF05080B;      // background
constexpr Gdiplus::ARGB grid = 0xFF203D58;     // blueprint lines
constexpr Gdiplus::ARGB gridHi = 0xFF2C5476;   // every fourth line
constexpr Gdiplus::ARGB steelHi = 0xFF6A7884, steel = 0xFF46535F, steelLo = 0xFF131B23;
constexpr Gdiplus::ARGB silver = 0xFFD2D4D6, silverLo = 0xFF9EA2A6;
constexpr Gdiplus::ARGB text = 0xFFC9CBCD, dim = 0xFF8F959B, faint = 0xFF5D6670;
constexpr Gdiplus::ARGB orange = 0xFFE47C08, orangeHi = 0xFFFF9A2A, orangeLo = 0xFFB05A00;
constexpr Gdiplus::ARGB red = 0xFFB3141C, white = 0xFFD8D8D8;
constexpr Gdiplus::ARGB ok = 0xFF7BC86C, bad = 0xFFE0453A;
}  // namespace col

enum class Align { Left, Center, Right };

// Starts GDI+ once per process (both programs call it before drawing; safe to call again).
void Startup();

// Fonts, created on first use: title (heavy, drawn leaning), item (condensed bold), body (regular), small (regular).
Gdiplus::FontFamily* TitleFamily();
Gdiplus::FontFamily* ItemFamily();
Gdiplus::FontFamily* BodyFamily();
std::wstring FamilyNames();  // for diagnostics: the families that were found

// Near-black background with the blueprint grid (cell in caller units), lines aligned to `origin`.
void Grid(Gdiplus::Graphics& g, Gdiplus::RectF area, float cell, float alpha = 1.0f);
// Brushed-steel strip: light at the left, dark at the right, with a highlight on top.
void SteelStrip(Gdiplus::Graphics& g, Gdiplus::RectF r, float alpha = 1.0f);
// The page title: heavy italic silver letters with a black outline; `size` is the em height.
void Title(Gdiplus::Graphics& g, const std::wstring& s, float x, float baselineTop, float size);
// The rules under the title strip: a red line over the whole width, a thin white one from `whiteFrom`.
void Rules(Gdiplus::Graphics& g, float x0, float x1, float y, float whiteFrom);
// A dark translucent panel (the game's video/menu panel).
void Panel(Gdiplus::Graphics& g, Gdiplus::RectF r, float alpha = 1.0f);
// A menu row: dark band fading to the right; selected: a lighter band and an orange underline.
void Row(Gdiplus::Graphics& g, Gdiplus::RectF r, bool selected);
// A small list cell (the music list): dark gradient with a separator line.
void Cell(Gdiplus::Graphics& g, Gdiplus::RectF r, bool selected);

// Outlined lettering (the game's menu text): `family` at `size`, fill colour, black outline, optional lean.
void Outlined(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::RectF r, Gdiplus::FontFamily* family, int style, float size,
              Gdiplus::ARGB fill, Align a = Align::Left, float lean = 0.0f, float outline = -1.0f);
// Menu item text (condensed bold, outlined): grey, or orange when selected.
void Item(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::RectF r, float size, bool selected, Align a = Align::Left);
// Plain text (no outline) for small print and paragraphs; `wrap` breaks lines inside r.
void Plain(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::RectF r, float size, Gdiplus::ARGB c, Align a = Align::Left,
           bool wrap = false, bool bold = false);
float PlainWidth(Gdiplus::Graphics& g, const std::wstring& s, float size, bool bold = false);
// A round button glyph (controller) or a key cap (keyboard) with its label inside.
void Glyph(Gdiplus::Graphics& g, Gdiplus::RectF r, const std::wstring& label, bool round, Gdiplus::ARGB ring);
// A tick or a cross in a small circle.
void Mark(Gdiplus::Graphics& g, float x, float y, float size, bool ok);
// A progress bar in the strip style.
void Progress(Gdiplus::Graphics& g, Gdiplus::RectF r, double fraction);

}  // namespace hp2style
