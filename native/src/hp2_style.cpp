// Hot Pursuit 2's front-end style for the port's own UI. See hp2_style.h.
#include "hp2_style.h"

#include <cmath>
#include <mutex>

using namespace Gdiplus;

namespace hp2style {
namespace {

std::once_flag g_started;
ULONG_PTR g_token = 0;
std::unique_ptr<FontFamily> g_title, g_item, g_body;
std::wstring g_names;
std::once_flag g_fonts;

std::unique_ptr<FontFamily> Pick(std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* n : names) {
    auto f = std::make_unique<FontFamily>(n);
    if (f->GetLastStatus() == Ok && f->IsAvailable()) {
      g_names += std::wstring(n) + L"; ";
      return f;
    }
  }
  g_names += L"(generic sans); ";
  return std::unique_ptr<FontFamily>(FontFamily::GenericSansSerif()->Clone());
}

void MakeFonts() {
  std::call_once(g_fonts, [] {
    g_title = Pick({L"Arial Black", L"Bahnschrift SemiBold", L"Segoe UI Black"});
    g_item = Pick({L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed", L"Arial Narrow", L"Bahnschrift SemiBold",
                   L"Segoe UI Semibold"});
    g_body = Pick({L"Bahnschrift", L"Segoe UI"});
  });
}

BYTE Scale(ARGB c, float alpha) { return static_cast<BYTE>(std::clamp(static_cast<float>((c >> 24) & 0xFF) * alpha, 0.0f, 255.0f)); }
Color With(ARGB c, float alpha) {
  return Color(Scale(c, alpha), static_cast<BYTE>(c >> 16), static_cast<BYTE>(c >> 8), static_cast<BYTE>(c));
}

StringFormat* Format(Align a, bool wrap) {
  static StringFormat* formats[6] = {};
  const int i = static_cast<int>(a) + (wrap ? 3 : 0);
  if (!formats[i]) {
    formats[i] = new StringFormat(StringFormat::GenericTypographic());  // lives as long as the process
    formats[i]->SetAlignment(a == Align::Left ? StringAlignmentNear : a == Align::Center ? StringAlignmentCenter : StringAlignmentFar);
    formats[i]->SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
    formats[i]->SetFormatFlags(wrap ? StringFormatFlagsMeasureTrailingSpaces
                                    : StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    if (!wrap) formats[i]->SetTrimming(StringTrimmingEllipsisCharacter);
  }
  return formats[i];
}

}  // namespace

void Startup() {
  std::call_once(g_started, [] {
    GdiplusStartupInput in;
    GdiplusStartup(&g_token, &in, nullptr);  // shut down by the process's exit
  });
}

FontFamily* TitleFamily() { MakeFonts(); return g_title.get(); }
FontFamily* ItemFamily() { MakeFonts(); return g_item.get(); }
FontFamily* BodyFamily() { MakeFonts(); return g_body.get(); }
std::wstring FamilyNames() { MakeFonts(); return g_names; }

void Grid(Graphics& g, RectF area, float cell, float alpha) {
  SolidBrush bg(With(col::ink, alpha));
  g.FillRectangle(&bg, area);
  Pen line(With(col::grid, alpha), 1.2f), hi(With(col::gridHi, alpha), 1.4f);
  int i = 0;
  for (float x = area.X + cell * 0.5f; x < area.GetRight(); x += cell, ++i) g.DrawLine(i % 4 == 0 ? &hi : &line, x, area.Y, x, area.GetBottom());
  i = 0;
  for (float y = area.Y + cell * 0.5f; y < area.GetBottom(); y += cell, ++i) g.DrawLine(i % 4 == 0 ? &hi : &line, area.X, y, area.GetRight(), y);
  // the vignette of the game's backdrop: darker towards the edges
  GraphicsPath p;
  p.AddEllipse(area.X - area.Width * 0.25f, area.Y - area.Height * 0.35f, area.Width * 1.5f, area.Height * 1.7f);
  PathGradientBrush v(&p);
  v.SetCenterColor(Color(0, 0, 0, 0));
  Color edge(static_cast<BYTE>(150 * alpha), 0, 0, 0);
  int n = 1;
  v.SetSurroundColors(&edge, &n);
  g.FillRectangle(&v, area);
}

void SteelStrip(Graphics& g, RectF r, float alpha) {
  LinearGradientBrush b(PointF(r.X, r.Y), PointF(r.GetRight(), r.Y), With(col::steelHi, alpha), With(col::steelLo, alpha));
  Color stops[4] = {With(col::steelHi, alpha), With(col::steel, alpha), With(0xFF2A3540, alpha), With(col::steelLo, alpha)};
  REAL pos[4] = {0, 0.3f, 0.65f, 1};
  b.SetInterpolationColors(stops, pos, 4);
  g.FillRectangle(&b, r);
  LinearGradientBrush shine(PointF(0, r.Y), PointF(0, r.GetBottom()), Color(static_cast<BYTE>(70 * alpha), 255, 255, 255),
                            Color(0, 255, 255, 255));
  g.FillRectangle(&shine, RectF(r.X, r.Y, r.Width, r.Height * 0.45f));
  Pen top(Color(static_cast<BYTE>(110 * alpha), 220, 230, 240), 1.0f), bottom(Color(static_cast<BYTE>(200 * alpha), 0, 0, 0), 1.0f);
  g.DrawLine(&top, r.X, r.Y + 0.5f, r.GetRight(), r.Y + 0.5f);
  g.DrawLine(&bottom, r.X, r.GetBottom() - 0.5f, r.GetRight(), r.GetBottom() - 0.5f);
}

void Outlined(Graphics& g, const std::wstring& s, RectF r, FontFamily* family, int style, float size, ARGB fill, Align a,
              float lean, float outline) {
  if (s.empty()) return;
  GraphicsPath path(FillModeWinding);  // winding: the fonts' overlapping contours stay filled
  path.AddString(s.c_str(), -1, family, style, size, r, Format(a, false));
  if (lean != 0.0f) {  // lean around the middle of the row, so the text stays where it was laid out
    const float cy = r.Y + r.Height / 2;
    Matrix m(1, 0, -lean, 1, lean * cy, 0);
    path.Transform(&m);
  }
  const float w = outline >= 0 ? outline : size * 0.13f;
  if (w > 0) {
    Pen pen(Color(static_cast<BYTE>((fill >> 24) & 0xFF), 0, 0, 0), w);
    pen.SetLineJoin(LineJoinRound);
    g.DrawPath(&pen, &path);
  }
  SolidBrush b{Color(fill)};
  g.FillPath(&b, &path);
}

void Title(Graphics& g, const std::wstring& s, float x, float top, float size) {
  RectF r(x, top, 1200, size * 1.25f);
  GraphicsPath path(FillModeWinding);  // winding: the fonts' overlapping contours stay filled
  path.AddString(s.c_str(), -1, TitleFamily(), FontStyleRegular, size, r, Format(Align::Left, false));
  const float cy = top + size * 0.62f;
  Matrix lean(1, 0, -0.3f, 1, 0.3f * cy, 0);
  path.Transform(&lean);
  Pen pen(Color(255, 0, 0, 0), size * 0.12f);
  pen.SetLineJoin(LineJoinRound);
  g.DrawPath(&pen, &path);
  LinearGradientBrush fill(PointF(0, top + size * 0.15f), PointF(0, top + size * 1.1f), Color(col::silver), Color(col::silverLo));
  g.FillPath(&fill, &path);
}

void Rules(Graphics& g, float x0, float x1, float y, float whiteFrom) {
  SolidBrush red{Color(col::red)};
  g.FillRectangle(&red, x0, y, x1 - x0, 2.5f);
  SolidBrush white{Color(col::white)};
  g.FillRectangle(&white, whiteFrom, y + 5.5f, x1 - whiteFrom, 1.2f);
}

void Panel(Graphics& g, RectF r, float alpha) {
  LinearGradientBrush b(PointF(r.X, r.Y), PointF(r.GetRight(), r.Y), Color(static_cast<BYTE>(175 * alpha), 34, 34, 34),
                        Color(static_cast<BYTE>(120 * alpha), 30, 32, 34));
  g.FillRectangle(&b, r);
  Pen edge(Color(static_cast<BYTE>(70 * alpha), 200, 200, 200), 1.0f);
  g.DrawLine(&edge, r.X, r.Y, r.GetRight(), r.Y);
}

void Row(Graphics& g, RectF r, bool selected) {
  LinearGradientBrush b(PointF(r.X, r.Y), PointF(r.GetRight(), r.Y), selected ? Color(215, 88, 88, 88) : Color(175, 30, 30, 30),
                        selected ? Color(120, 70, 70, 70) : Color(40, 30, 30, 30));
  g.FillRectangle(&b, r);
  if (selected) {
    SolidBrush o{Color(col::orange)};
    g.FillRectangle(&o, r.X, r.GetBottom() - 2.0f, r.Width, 2.0f);
  }
}

void Cell(Graphics& g, RectF r, bool selected) {
  LinearGradientBrush b(PointF(r.X, r.Y), PointF(r.GetRight(), r.Y), selected ? Color(220, 70, 70, 70) : Color(200, 38, 38, 38),
                        Color(30, 38, 38, 38));
  g.FillRectangle(&b, r);
  Pen sep(Color(150, 0, 0, 0), 1.0f);
  g.DrawLine(&sep, r.X, r.GetBottom() - 0.5f, r.GetRight(), r.GetBottom() - 0.5f);
  if (selected) {
    SolidBrush o{Color(col::orange)};
    g.FillRectangle(&o, r.X, r.Y, 3.0f, r.Height);
  }
}

void Item(Graphics& g, const std::wstring& s, RectF r, float size, bool selected, Align a) {
  Outlined(g, s, r, ItemFamily(), FontStyleBold, size, selected ? col::orange : col::text, a, 0.0f);
}

void Plain(Graphics& g, const std::wstring& s, RectF r, float size, ARGB c, Align a, bool wrap, bool bold) {
  Font font(BodyFamily(), size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
  const Font* f = &font;
  Font fallback(BodyFamily(), size, FontStyleRegular, UnitPixel);
  if (font.GetLastStatus() != Ok) f = &fallback;
  SolidBrush b{Color(c)};
  g.DrawString(s.c_str(), -1, f, r, Format(a, wrap), &b);
}

float PlainWidth(Graphics& g, const std::wstring& s, float size, bool bold) {
  Font font(BodyFamily(), size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
  RectF out;
  g.MeasureString(s.c_str(), -1, &font, PointF(0, 0), Format(Align::Left, false), &out);
  return out.Width;
}

void Glyph(Graphics& g, RectF r, const std::wstring& label, bool round, ARGB ring) {
  SolidBrush fill(Color(235, 18, 22, 26));
  Pen edge(Color(ring), 1.6f);
  if (round) {
    g.FillEllipse(&fill, r);
    g.DrawEllipse(&edge, r);
  } else {
    GraphicsPath p;
    const float k = 4;
    p.AddArc(r.X, r.Y, k * 2, k * 2, 180, 90);
    p.AddArc(r.GetRight() - k * 2, r.Y, k * 2, k * 2, 270, 90);
    p.AddArc(r.GetRight() - k * 2, r.GetBottom() - k * 2, k * 2, k * 2, 0, 90);
    p.AddArc(r.X, r.GetBottom() - k * 2, k * 2, k * 2, 90, 90);
    p.CloseFigure();
    g.FillPath(&fill, &p);
    g.DrawPath(&edge, &p);
  }
  Outlined(g, label, RectF(r.X - 4, r.Y, r.Width + 8, r.Height), ItemFamily(), FontStyleBold, r.Height * 0.56f, col::silver,
           Align::Center, 0.0f, 0.0f);
}

void Mark(Graphics& g, float x, float y, float size, bool ok) {
  SolidBrush b(Color(ok ? 0x337BC86C : 0x33E0453A));
  g.FillEllipse(&b, x, y, size, size);
  Pen p(Color(ok ? col::ok : col::bad), size * 0.12f);
  p.SetStartCap(LineCapRound);
  p.SetEndCap(LineCapRound);
  if (ok) {
    const PointF t[3] = {{x + size * 0.25f, y + size * 0.53f}, {x + size * 0.44f, y + size * 0.72f}, {x + size * 0.77f, y + size * 0.3f}};
    g.DrawLines(&p, t, 3);
  } else {
    g.DrawLine(&p, x + size * 0.3f, y + size * 0.3f, x + size * 0.7f, y + size * 0.7f);
    g.DrawLine(&p, x + size * 0.7f, y + size * 0.3f, x + size * 0.3f, y + size * 0.7f);
  }
}

void Progress(Graphics& g, RectF r, double fraction) {
  SolidBrush back(Color(220, 12, 14, 16));
  g.FillRectangle(&back, r);
  const float w = static_cast<float>(std::clamp(fraction, 0.0, 1.0)) * (r.Width - 4);
  if (w > 0.5f) {
    LinearGradientBrush b(PointF(0, r.Y), PointF(0, r.GetBottom()), Color(col::orangeHi), Color(col::orangeLo));
    g.FillRectangle(&b, r.X + 2, r.Y + 2, w, r.Height - 4);
  }
  Pen edge(Color(160, 120, 130, 140), 1.0f);
  g.DrawRectangle(&edge, r);
}

}  // namespace hp2style
