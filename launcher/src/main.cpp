// HotPursuit2Launcher.exe - the player's front end of the native game (ADR 0016): first-run setup from the user's own
// disc (find, copy, check, prepare), then Play, settings, memory card backups, screenshots, disc & setup, about.
// Drawn with GDI+ in Hot Pursuit 2's visual language (ADR 0013): steel-blue angled translucent panels, light bold italic
// lettering, the selection in orange/amber. Mouse, keyboard and an Xbox controller (XInput, loaded at run time).
//
// Developer switches:
//   --selftest            draw every screen off-screen, check each has its controls, exit 0/1 (ctest)
//   --screenshot <dir>    draw every screen into <dir>\launcher-<screen>.png and exit (docs, visual checks)
//   --setup-from <dir>    run the setup from <dir> as if it were a DVD drive: copy, check, prepare (tests)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "overlay.h"
#include "tasks.h"

using namespace Gdiplus;
using namespace launcher;

namespace {

constexpr const wchar_t* kVersion = L"0.2.0-dev.1";
constexpr const wchar_t* kRepoUrl = L"https://github.com/awtocl/HotPursuit2PC";
constexpr const wchar_t* kGuideUrl = L"https://github.com/awtocl/HotPursuit2PC/blob/main/docs/GETTING-STARTED.md";
constexpr const wchar_t* kIssuesUrl = L"https://github.com/awtocl/HotPursuit2PC/issues";
constexpr UINT kJobDone = WM_APP + 1, kGameEnded = WM_APP + 2, kThumbsReady = WM_APP + 3;
constexpr UINT_PTR kTimer = 1;
constexpr float kW = 1280, kH = 720;            // logical size; everything is laid out in these units
constexpr float kTitleH = 36, kHintsY = 684;    // title bar, hints bar
constexpr float kRailW = 250, kX0 = 300, kX1 = 1240;

// ---- colours (0xAARRGGBB) ----
namespace col {
constexpr ARGB bg0 = 0xFF07101D, bg1 = 0xFF11233B, bg2 = 0xFF1D3A5F, steel = 0xFF3B6AA0, steelHi = 0xFF8FB3DC;
constexpr ARGB text = 0xFFE8EEF7, dim = 0xFFA9BCD3, faint = 0xFF6F89A8, orange = 0xFFFF921C, amber = 0xFFFFB347;
constexpr ARGB deep = 0xFFFF7A00, ok = 0xFF7BD88F, bad = 0xFFFF6B5E, ink = 0xFF0A1422;
constexpr ARGB panel = 0xB80B1828, panelHi = 0xD8173152, line = 0x553B6AA0;
}  // namespace col

enum class Page { Home, Settings, Saves, Screenshots, Setup, About, Count };
const wchar_t* kPageNames[] = {L"PLAY", L"SETTINGS", L"MEMORY CARD", L"SCREENSHOTS", L"DISC & SETUP", L"ABOUT"};
enum class Step { Welcome, Disc, Copy, Check, Prepare, Ready };
enum class Pkg { Unknown, Checking, Ok, Missing, Stale };

// Control ids (unique per screen).
enum : int {
  kRail = 100,  // + page
  kPlay = 1, kHomeSettings, kHomeFolder, kHomeLog, kHomeReport,
  kSetRow = 10,  // + row
  kSetReset = 30,
  kSaveBackup = 40, kSaveFolder, kSaveRow = 500,  // + backup (up to 50)
  kShotFolder = 60, kShot = 1000,                 // + screenshot
  kSetupCheck = 70, kSetupPrepare, kSetupDisc, kSetupFolder, kSetupShortcut, kJobCancel,
  kAboutProject = 80, kAboutGuide, kAboutReport, kAboutLog,
  kWizStart = 90, kWizFolder, kWizPlay, kWizLauncher, kWizRetry, kWizDrive = 300,  // + drive
};

struct Widget {
  int id;
  RectF r;
  int row, col;  // keyboard/controller movement; col -1: the page rail
  bool option;   // Left/Right change its value
};

struct Fonts {
  std::unique_ptr<FontFamily> heavy, regular;
  std::unique_ptr<Font> hero, h1, h2, body, note, nav, button, play, tiny, bodyBold;
};

struct Thumbs {  // screenshots decoded on a thread of their own (a 4K PNG takes a moment)
  std::mutex m;
  std::map<std::wstring, std::unique_ptr<Bitmap>> ready;  // path -> 448x252 thumbnail
  std::unique_ptr<Bitmap> hero;                           // the newest screenshot, 1920x1080 cover
  std::wstring heroPath;
  int heroVersion = 0;
  std::thread worker;
};

struct App {
  HWND wnd = nullptr;
  float scale = 1;
  int cw = 0, ch = 0;
  Paths paths;
  Config cfg;
  ui::Settings set;
  bool firstSettings = false;  // there was no hp2native.ini: the recommended picture quality is applied after setup
  bool wizard = false;
  Step step = Step::Welcome;
  Page page = Page::Home;
  int focus = -1;
  bool focusRail = true;
  std::vector<Widget> widgets;  // of the last drawn frame
  Progress job;
  Pkg pkg = Pkg::Unknown;
  bool playAfter = false;
  HANDLE game = nullptr;
  double gameStarted = 0;
  std::wstring problem;    // why the last game session ended badly
  std::wstring discError;  // setup: what was wrong with the chosen disc
  std::vector<DiscDrive> drives;
  double drivesAt = -10;
  std::wstring toast;
  double toastAt = -10;
  int armed = -1;  // a control waiting for its confirmation press
  std::vector<SaveEntry> saves, backups;
  std::vector<fs::path> shots;
  int viewer = -1;     // screenshot shown full size
  int shotScroll = 0;  // first visible row
  bool controller = false;
  int recommended = 3;  // internal resolution for this screen
  int screenW = 1920, screenH = 1080;
  bool dirty = true;
  POINT lastMouse{-1, -1};
  int hoverTitle = 0;  // 1 minimise, 2 close
  Fonts f;
  Thumbs thumbs;
  std::unique_ptr<Bitmap> bg;  // cached background of the current size
  std::unique_ptr<Bitmap> viewerBmp;  // the screenshot shown full size
  std::wstring viewerPath;
  DiscCheck disc;              // of cfg.disc (the quick check reads 3 MB: not at every frame)
  int bgKey = -1;
  HDC memDc = nullptr;
  HBITMAP memBmp = nullptr;
  int memW = 0, memH = 0;
};
App g;

// ---------------------------------------------------------------------------------------------- drawing helpers ----
enum class Align { Left, Center, Right };

std::unique_ptr<FontFamily> PickFamily(std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* n : names) {
    auto f = std::make_unique<FontFamily>(n);
    if (f->IsAvailable()) return f;
  }
  return std::unique_ptr<FontFamily>(FontFamily::GenericSansSerif()->Clone());
}
std::unique_ptr<Font> MakeFont(FontFamily* fam, float px, int style) {
  auto font = std::make_unique<Font>(fam, px, style, UnitPixel);
  if (font->GetLastStatus() != Ok) font = std::make_unique<Font>(fam, px, FontStyleRegular, UnitPixel);
  return font;
}
void MakeFonts() {
  g.f.heavy = PickFamily({L"Bahnschrift SemiBold", L"Bahnschrift", L"Segoe UI Semibold", L"Segoe UI"});
  g.f.regular = PickFamily({L"Bahnschrift", L"Segoe UI"});
  g.f.hero = MakeFont(g.f.heavy.get(), 76, FontStyleBold);
  g.f.h1 = MakeFont(g.f.heavy.get(), 36, FontStyleBold);
  g.f.h2 = MakeFont(g.f.heavy.get(), 19, FontStyleBold);
  g.f.nav = MakeFont(g.f.heavy.get(), 19, FontStyleBold);
  g.f.button = MakeFont(g.f.heavy.get(), 18, FontStyleBold);
  g.f.play = MakeFont(g.f.heavy.get(), 40, FontStyleBold);
  g.f.body = MakeFont(g.f.regular.get(), 17, FontStyleRegular);
  g.f.bodyBold = MakeFont(g.f.heavy.get(), 17, FontStyleBold);
  g.f.note = MakeFont(g.f.regular.get(), 14.5f, FontStyleRegular);
  g.f.tiny = MakeFont(g.f.heavy.get(), 12, FontStyleBold);
}

void Para(Graphics& gr, float x, float y, float w, float h, float skew, const Brush& b) {
  const PointF p[4] = {{x + skew, y}, {x + w + skew, y}, {x + w, y + h}, {x, y + h}};
  gr.FillPolygon(&b, p, 4);
}
void Para(Graphics& gr, float x, float y, float w, float h, float skew, ARGB c) { Para(gr, x, y, w, h, skew, SolidBrush(Color(c))); }
void ParaOutline(Graphics& gr, float x, float y, float w, float h, float skew, ARGB c, float width) {
  const PointF p[4] = {{x + skew, y}, {x + w + skew, y}, {x + w, y + h}, {x, y + h}};
  Pen pen(Color(c), width);
  gr.DrawPolygon(&pen, p, 4);
}

// One line (or a wrapped block); `slant` leans the letters like the game's italic menu type.
void Text(Graphics& gr, const std::wstring& s, RectF r, const Font* font, ARGB c, Align a = Align::Left, bool slant = false,
          bool wrap = false) {
  StringFormat sf;
  sf.SetAlignment(a == Align::Left ? StringAlignmentNear : a == Align::Center ? StringAlignmentCenter : StringAlignmentFar);
  sf.SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
  if (!wrap) {
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
  }
  SolidBrush b{Color(c)};
  if (!slant) {
    gr.DrawString(s.c_str(), -1, font, r, &sf, &b);
    return;
  }
  const GraphicsState st = gr.Save();
  const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
  gr.TranslateTransform(cx, cy);
  Matrix shear(1, 0, -0.2f, 1, 0, 0);
  gr.MultiplyTransform(&shear);
  gr.DrawString(s.c_str(), -1, font, RectF(-r.Width / 2, -r.Height / 2, r.Width, r.Height), &sf, &b);
  gr.Restore(st);
}
float TextWidth(Graphics& gr, const std::wstring& s, const Font* font) {
  RectF out;
  StringFormat sf(StringFormat::GenericTypographic());
  sf.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces);
  gr.MeasureString(s.c_str(), -1, font, PointF(0, 0), &sf, &out);
  return out.Width;
}

// The orange chevrons of the project's mark.
void Chevrons(Graphics& gr, float x, float y, float h, ARGB c, int count, float alphaStep) {
  const float w = h * 0.42f, t = h * 0.22f;
  for (int i = 0; i < count; ++i) {
    const float ox = x + i * (w + t * 0.35f);
    const BYTE a = static_cast<BYTE>(std::max(0.0f, ((c >> 24) & 0xFF) * (1.0f - alphaStep * static_cast<float>(i))));
    SolidBrush b(Color(a, static_cast<BYTE>(c >> 16), static_cast<BYTE>(c >> 8), static_cast<BYTE>(c)));
    const PointF p[6] = {{ox, y}, {ox + w, y + h / 2}, {ox, y + h}, {ox + t, y + h}, {ox + w + t, y + h / 2}, {ox + t, y}};
    gr.FillPolygon(&b, p, 6);
  }
}

// A control: registered for input, drawn by the caller with its state.
bool Add(int id, RectF r, int row, int c, bool option = false) {
  g.widgets.push_back({id, r, row, c, option});
  return g.focus == id;
}

// Buttons: primary (orange) or secondary (steel), angled.
void Button(Graphics& gr, int id, float x, float y, float w, float h, const std::wstring& label, int row, int c, bool primary = false,
            bool enabled = true) {
  const bool f = enabled && Add(id, RectF(x, y, w + 10, h), row, c);  // a disabled button takes no focus
  const float skew = 10;
  if (primary) {
    LinearGradientBrush lg(PointF(x, y), PointF(x, y + h), Color(enabled ? (f ? 0xFFFFC266 : col::amber) : 0xFF6B5A44),
                           Color(enabled ? (f ? col::orange : col::deep) : 0xFF4A3B2A));
    Para(gr, x, y, w, h, skew, lg);
    if (f) ParaOutline(gr, x - 3, y - 3, w + 6, h + 6, skew, 0xFFFFD9A0, 2);
    Text(gr, label, RectF(x + skew, y, w, h), g.f.button.get(), enabled ? col::ink : 0xFF8C7B66, Align::Center, true);
  } else {
    Para(gr, x, y, w, h, skew, f ? col::orange : (enabled ? col::panelHi : 0x800B1828));
    if (!f) ParaOutline(gr, x, y, w, h, skew, enabled ? 0x993B6AA0 : 0x403B6AA0, 1.2f);
    Text(gr, label, RectF(x + skew, y, w, h), g.f.button.get(), f ? col::ink : (enabled ? col::text : col::faint), Align::Center, true);
  }
}

void Panel(Graphics& gr, float x, float y, float w, float h, ARGB c = col::panel) {
  Para(gr, x, y, w, h, 14, c);
  Para(gr, x + 2, y + h - 3, w * 0.4f, 3, 1, 0xCCFF921C);  // the orange accent stripe of the game's panels
}

void Heading(Graphics& gr, const std::wstring& title, const std::wstring& sub, float x = kX0, float w = kX1 - kX0) {
  Text(gr, title, RectF(x, 58, w, 48), g.f.h1.get(), col::text, Align::Left, true);
  Para(gr, x + 2, 106, 120, 4, 2, col::orange);
  if (!sub.empty()) Text(gr, sub, RectF(x, 116, w, 26), g.f.body.get(), col::dim);
}

void Check(Graphics& gr, float x, float y, bool ok) {  // a tick or a cross in a small circle
  SolidBrush b(Color(ok ? 0x337BD88F : 0x33FF6B5E));
  gr.FillEllipse(&b, x, y, 18.0f, 18.0f);
  Pen p(Color(ok ? col::ok : col::bad), 2.2f);
  if (ok) {
    const PointF t[3] = {{x + 4.5f, y + 9.5f}, {x + 8, y + 13}, {x + 14, y + 5.5f}};
    gr.DrawLines(&p, t, 3);
  } else {
    gr.DrawLine(&p, x + 5.5f, y + 5.5f, x + 12.5f, y + 12.5f);
    gr.DrawLine(&p, x + 12.5f, y + 5.5f, x + 5.5f, y + 12.5f);
  }
}

void Bar(Graphics& gr, float x, float y, float w, float h, double fraction) {
  Para(gr, x, y, w, h, 6, 0xC0081320);
  ParaOutline(gr, x, y, w, h, 6, 0x803B6AA0, 1);
  const float fw = static_cast<float>(std::clamp(fraction, 0.0, 1.0)) * (w - 4);
  if (fw > 1) {
    LinearGradientBrush lg(PointF(x, y), PointF(x + w, y), Color(col::deep), Color(col::amber));
    Para(gr, x + 2, y + 2, fw, h - 4, 5, lg);
  }
}

// ----------------------------------------------------------------------------------------------------- state ----
bool JobIs(Job j) { return g.job.running && g.job.job == j; }
void Toast(std::wstring s) {
  g.toast = std::move(s);
  g.toastAt = Clock();
  g.dirty = true;
}
std::wstring Recommendation() {
  return std::wstring(ui::Setting(1).names[g.recommended]) + L" for your " + std::to_wstring(g.screenW) + L" × " +
         std::to_wstring(g.screenH) + L" screen";
}
const wchar_t* kQuality[] = {L"Original (1x)", L"Performance (2x)", L"Balanced (3x)", L"Quality (4x)", L"Ultra (6x)", L"Maximum (8x)"};

void ReadScreen() {
  HMONITOR mon = MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof mi};
  if (GetMonitorInfoW(mon, &mi)) {
    g.screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    g.screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
  }
  // The guide's table: 1080p 4x, 1440p 6x, 4K 8x (the picture is rendered above the screen's size and averaged).
  g.recommended = g.screenH <= 1200 ? 3 : g.screenH <= 1600 ? 4 : 5;
}

void StartThumbs();
void RefreshLists() {
  g.saves = ListSaves(g.paths);
  g.backups = ListBackups(g.paths);
  g.shots = ListScreenshots(g.paths);
  StartThumbs();
}

void RefreshDisc() { g.disc = QuickCheck(g.cfg.disc); }

void CheckPackage() {
  g.pkg = Pkg::Checking;
  StartCheckPackage(g.job, g.paths, g.cfg.disc, g.wnd, kJobDone);
}

void Launch() {
  std::wstring error;
  HANDLE h = static_cast<HANDLE>(StartGame(g.paths, g.cfg.disc, error));
  g.playAfter = false;
  if (!h) {
    g.problem = error;
    g.dirty = true;
    return;
  }
  g.game = h;
  g.gameStarted = Clock();
  g.problem.clear();
  ShowWindow(g.wnd, SW_HIDE);
  HWND wnd = g.wnd;
  std::thread([h, wnd] {
    WaitForSingleObject(h, INFINITE);
    PostMessageW(wnd, kGameEnded, 0, 0);
  }).detach();
}

void Play() {
  if (g.game || (g.job.running && g.job.job != Job::CheckPackage)) return;
  if (g.pkg == Pkg::Ok) return Launch();
  g.playAfter = true;
  if (g.pkg == Pkg::Checking) return;  // continues when the check ends
  StartPrepare(g.job, g.paths, g.cfg.disc, g.wnd, kJobDone);
  g.dirty = true;
}

void GoPage(Page p, bool toRail) {
  if (p != g.page) {
    g.page = p;
    g.armed = -1;
    g.viewer = -1;
    g.shotScroll = 0;
    if (p == Page::Saves || p == Page::Screenshots || p == Page::Home) RefreshLists();
    if (p == Page::Setup) RefreshDisc();
  }
  g.focusRail = toRail;
  g.focus = toRail ? kRail + static_cast<int>(p) : -1;
  g.dirty = true;
}

void StartWizard(Step s) {
  g.wizard = true;
  g.step = s;
  g.focus = -1;
  g.focusRail = false;
  g.dirty = true;
}

void UseDiscFolder(const fs::path& folder) {
  g.discError.clear();
  if (IsOpticalDrive(folder)) {  // a DVD or a mounted .iso: copy it, so the disc need not stay in
    StartCopy(g.job, folder, g.paths.copiedDisc, g.wnd, kJobDone);
    g.step = Step::Copy;
    return;
  }
  const DiscCheck c = QuickCheck(folder);
  if (c.state != DiscState::Ok) {
    g.discError = c.detail;
    g.step = Step::Disc;
    return;
  }
  g.cfg.disc = folder.wstring();
  g.cfg.verified = 0;
  SaveConfig(g.paths, g.cfg);
  StartVerify(g.job, folder, g.wnd, kJobDone);
  g.step = Step::Check;
}

void ChooseFolder() {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Folder with the contents of your Hot Pursuit 2 disc (SYSTEM.CNF, SLUS_203.62, NFSHP2)");
  if (SUCCEEDED(dlg->Show(g.wnd))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        if (!g.wizard) StartWizard(Step::Disc);
        UseDiscFolder(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  g.dirty = true;
}

bool MakeShortcut() {  // Desktop shortcut to the launcher (the user presses the button for it)
  PWSTR desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return false;
  const fs::path link = fs::path(desktop) / L"Hot Pursuit 2 (PC).lnk";
  CoTaskMemFree(desktop);
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  sl->SetPath(self);
  sl->SetWorkingDirectory(fs::path(self).parent_path().c_str());
  sl->SetDescription(L"Need for Speed: Hot Pursuit 2, native PC version (HotPursuit2PC)");
  IPersistFile* pf = nullptr;
  bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
  if (pf) pf->Release();
  sl->Release();
  return ok;
}

// ------------------------------------------------------------------------------------------------ thumbnails ----
void DrawCover(Graphics& gr, Bitmap* src, RectF dst) {
  const float sw = static_cast<float>(src->GetWidth()), sh = static_cast<float>(src->GetHeight());
  const float s = std::max(dst.Width / sw, dst.Height / sh);
  const float w = dst.Width / s, h = dst.Height / s;
  gr.DrawImage(src, dst, (sw - w) / 2, (sh - h) / 2, w, h, UnitPixel);
}

void StartThumbs() {
  if (g.thumbs.worker.joinable()) g.thumbs.worker.join();
  std::vector<fs::path> todo;
  {
    std::lock_guard l(g.thumbs.m);
    for (const fs::path& p : g.shots)
      if (!g.thumbs.ready.count(p.wstring())) todo.push_back(p);
    if (!g.shots.empty() && g.thumbs.heroPath != g.shots[0].wstring()) todo.insert(todo.begin(), g.shots[0]);
  }
  if (todo.empty()) return;
  const std::wstring heroPath = g.shots.empty() ? L"" : g.shots[0].wstring();
  HWND wnd = g.wnd;
  g.thumbs.worker = std::thread([todo, heroPath, wnd] {
    for (const fs::path& p : todo) {
      Bitmap src(p.c_str());
      if (src.GetLastStatus() != Ok || !src.GetWidth()) continue;
      auto make = [&](int w, int h) {
        auto b = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics tg(b.get());
        tg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        tg.SetPixelOffsetMode(PixelOffsetModeHalf);
        DrawCover(tg, &src, RectF(0, 0, static_cast<float>(w), static_cast<float>(h)));
        return b;
      };
      std::unique_ptr<Bitmap> hero = p.wstring() == heroPath ? make(1920, 1080) : nullptr;
      std::unique_ptr<Bitmap> thumb = make(448, 252);
      {
        std::lock_guard l(g.thumbs.m);
        if (hero) {
          g.thumbs.hero = std::move(hero);
          g.thumbs.heroPath = heroPath;
          ++g.thumbs.heroVersion;
        }
        g.thumbs.ready[p.wstring()] = std::move(thumb);
      }
      PostMessageW(wnd, kThumbsReady, 0, 0);
    }
  });
}

// ------------------------------------------------------------------------------------------------ background ----
void PaintBackground(Graphics& gr, bool home) {
  LinearGradientBrush bgb(PointF(0, 0), PointF(kW, kH), Color(col::bg0), Color(col::bg2));
  Color stops[3] = {Color(col::bg0), Color(col::bg1), Color(col::bg2)};
  REAL pos[3] = {0, 0.55f, 1};
  bgb.SetInterpolationColors(stops, pos, 3);
  gr.FillRectangle(&bgb, 0.0f, 0.0f, kW, kH);
  bool hero = false;
  {
    std::lock_guard l(g.thumbs.m);
    if (g.thumbs.hero) {
      hero = true;
      DrawCover(gr, g.thumbs.hero.get(), RectF(0, 0, kW, kH));
    }
  }
  if (hero) {  // the player's own newest screenshot, darkened towards the text
    LinearGradientBrush shade(PointF(0, 0), PointF(kW, 0), Color(home ? 0xF207101D : 0xF807101D), Color(home ? 0x4007101D : 0xD007101D));
    Color st[3] = {Color(home ? 0xF207101D : 0xF807101D), Color(home ? 0xB807101D : 0xEA07101D), Color(home ? 0x3007101D : 0xC807101D)};
    REAL ps[3] = {0, 0.45f, 1};
    shade.SetInterpolationColors(st, ps, 3);
    gr.FillRectangle(&shade, 0.0f, 0.0f, kW, kH);
    LinearGradientBrush bottom(PointF(0, kH - 260), PointF(0, kH), Color(0x0007101D), Color(0xE607101D));
    gr.FillRectangle(&bottom, 0.0f, kH - 260, kW, 260.0f);
    return;
  }
  // No screenshot yet: speed lines, angled panels and the chevrons of the banner.
  Pen line(Color(0x2E8FB3DC), 2);
  line.SetStartCap(LineCapRound);
  line.SetEndCap(LineCapRound);
  // x from, x to, y (the top two start right of the home screen's title)
  const float lines[][3] = {{880, 1240, 150}, {920, 1210, 178}, {700, 1180, 470}, {900, 1250, 500}, {620, 980, 600}, {1000, 1260, 620}};
  for (const auto& l : lines) gr.DrawLine(&line, l[0], l[2], l[1], l[2]);
  LinearGradientBrush pane(PointF(kRailW, 0), PointF(kW, 0), Color(0x403B6AA0), Color(0x053B6AA0));
  Para(gr, kRailW + 40, 230, 900, 170, 60, pane);
  Para(gr, kRailW + 120, 412, 760, 6, 3, 0x66FF921C);
  if (home) Chevrons(gr, 1010, 250, 120, 0xD0FF921C, 3, 0.3f);
}

// --------------------------------------------------------------------------------------------------- chrome ----
void PaintTitleBar(Graphics& gr) {
  SolidBrush dark(Color(0xB0050C16));
  gr.FillRectangle(&dark, 0.0f, 0.0f, kW, kTitleH);
  Chevrons(gr, 14, 10, 16, 0xFFFF921C, 2, 0.35f);
  Text(gr, L"HotPursuit2PC", RectF(46, 0, 300, kTitleH), g.f.h2.get(), col::text, Align::Left, true);
  Text(gr, std::wstring(L"v") + kVersion, RectF(196, 2, 200, kTitleH), g.f.note.get(), col::faint);
  // minimise / close
  const float bw = 46;
  for (int i = 0; i < 2; ++i) {
    const float x = kW - bw * (2 - i);
    if (g.hoverTitle == i + 1) {
      SolidBrush hb(Color(i == 1 ? 0xFFC42B1C : 0x40FFFFFF));
      gr.FillRectangle(&hb, x, 0.0f, bw, kTitleH);
    }
    Pen p(Color(col::text), 1.3f);
    const float cx = x + bw / 2, cy = kTitleH / 2;
    if (i == 0) gr.DrawLine(&p, cx - 5, cy, cx + 5, cy);
    else {
      gr.DrawLine(&p, cx - 5, cy - 5, cx + 5, cy + 5);
      gr.DrawLine(&p, cx + 5, cy - 5, cx - 5, cy + 5);
    }
  }
}

void PaintRail(Graphics& gr) {
  const PointF p[4] = {{0, kTitleH}, {kRailW, kTitleH}, {kRailW - 34, kHintsY}, {0, kHintsY}};
  SolidBrush b(Color(0xA6091422));
  gr.FillPolygon(&b, p, 4);
  Pen edge(Color(0x663B6AA0), 1.5f);
  gr.DrawLine(&edge, kRailW, kTitleH, kRailW - 34, kHintsY);
  Text(gr, L"NEED FOR SPEED", RectF(30, 58, 210, 20), g.f.tiny.get(), col::orange, Align::Left, true);
  Text(gr, L"HOT PURSUIT 2", RectF(28, 76, 220, 34), g.f.h2.get(), col::text, Align::Left, true);
  for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
    const float y = 140 + i * 56.0f, x = 18, w = kRailW - 60, h = 44;
    const bool sel = static_cast<int>(g.page) == i;
    const bool f = Add(kRail + i, RectF(x, y, w + 12, h), i, -1);
    if (sel) {
      LinearGradientBrush lg(PointF(x, y), PointF(x, y + h), Color(g.focusRail ? 0xFFFFC266 : col::amber), Color(col::deep));
      Para(gr, x, y, w, h, 12, lg);
    } else if (f) {
      Para(gr, x, y, w, h, 12, col::panelHi);
    }
    Text(gr, kPageNames[i], RectF(x + 22, y, w - 20, h), g.f.nav.get(), sel ? col::ink : (f ? col::text : col::dim), Align::Left, true);
  }
}

void Glyph(Graphics& gr, float& x, float y, const std::wstring& key, const std::wstring& what) {
  const float kw = std::max(26.0f, TextWidth(gr, key, g.f.tiny.get()) + 16);
  SolidBrush kb(Color(0xFF223B5C));
  if (g.controller && key.size() == 1) gr.FillEllipse(&kb, x, y, 24.0f, 24.0f);
  else Para(gr, x, y, kw, 24, 4, 0xFF223B5C);
  const float gw = g.controller && key.size() == 1 ? 24 : kw + 4;
  Text(gr, key, RectF(x, y, gw, 24), g.f.tiny.get(), col::text, Align::Center);
  x += gw + 8;
  const float tw = TextWidth(gr, what, g.f.note.get());
  Text(gr, what, RectF(x, y, tw + 24, 24), g.f.note.get(), col::dim);
  x += tw + 26;
}

void PaintHints(Graphics& gr) {
  SolidBrush dark(Color(0xC0050C16));
  gr.FillRectangle(&dark, 0.0f, kHintsY, kW, kH - kHintsY);
  Pen top(Color(0x553B6AA0), 1);
  gr.DrawLine(&top, 0.0f, kHintsY, kW, kHintsY);
  // left: what runs now
  std::wstring status;
  if (g.job.running && g.job.job != Job::CheckPackage) status = g.job.Status();
  else if (g.wizard) status = L"Setup";
  else if (g.pkg == Pkg::Ok) status = L"Ready to play";
  else if (g.pkg == Pkg::Checking) status = L"Checking the game files…";
  else if (g.pkg == Pkg::Stale || g.pkg == Pkg::Missing) status = L"The game needs preparing (about a minute)";
  Text(gr, status, RectF(24, kHintsY, 520, kH - kHintsY), g.f.note.get(), col::dim);
  // right: the controls
  float x = 640;
  const float y = kHintsY + 6;
  if (g.controller) {
    Glyph(gr, x, y, L"A", L"Select");
    Glyph(gr, x, y, L"B", L"Back");
    if (!g.wizard) Glyph(gr, x, y, L"LB RB", L"Pages");
    if (!g.wizard) Glyph(gr, x, y, L"START", L"Play");
  } else {
    Glyph(gr, x, y, L"ENTER", L"Select");
    Glyph(gr, x, y, L"ESC", L"Back");
    Glyph(gr, x, y, L"← →", L"Change");
    if (!g.wizard) Glyph(gr, x, y, L"PGUP PGDN", L"Pages");
  }
}

void PaintToast(Graphics& gr) {
  const double age = Clock() - g.toastAt;
  if (g.toast.empty() || age > 3.5) return;
  const float w = std::min(560.0f, TextWidth(gr, g.toast, g.f.body.get()) + 56), h = 44, x = kW - w - 34, y = kHintsY - h - 18;
  const BYTE a = static_cast<BYTE>(age > 3.0 ? 255 * (3.5 - age) / 0.5 : 255);
  Para(gr, x, y, w, h, 10, Color::MakeARGB(static_cast<BYTE>(a * 0.92), 0x17, 0x31, 0x52));
  Para(gr, x, y + h - 3, w, 3, 1, Color::MakeARGB(a, 0xFF, 0x92, 0x1C));
  Text(gr, g.toast, RectF(x + 22, y, w - 30, h), g.f.body.get(), Color::MakeARGB(a, 0xE8, 0xEE, 0xF7));
}

// ----------------------------------------------------------------------------------------------------- pages ----
void PaintHome(Graphics& gr) {
  Text(gr, L"NEED FOR SPEED", RectF(kX0 + 4, 120, 600, 28), g.f.h2.get(), col::orange, Align::Left, true);
  Text(gr, L"HOT PURSUIT 2", RectF(kX0 - 6, 140, 900, 96), g.f.hero.get(), col::text, Align::Left, true);
  Text(gr, L"Native PC version  ·  from your own PlayStation 2 disc", RectF(kX0 + 2, 236, 700, 28), g.f.body.get(), col::dim);

  // status chips
  float x = kX0;
  auto chip = [&](const std::wstring& s, bool ok, bool neutral = false) {
    const float w = TextWidth(gr, s, g.f.note.get()) + (neutral ? 30 : 50);
    Para(gr, x, 280, w, 30, 8, 0xB00B1828);
    ParaOutline(gr, x, 280, w, 30, 8, 0x663B6AA0, 1);
    if (!neutral) Check(gr, x + 12, 286, ok);
    Text(gr, s, RectF(x + (neutral ? 16 : 38), 280, w, 30), g.f.note.get(), col::text);
    x += w + 14;
  };
  chip(g.cfg.verified ? L"Disc checked" : L"Disc found", true);
  if (g.pkg == Pkg::Ok) chip(L"Game prepared", true);
  else if (g.pkg == Pkg::Checking || g.pkg == Pkg::Unknown) chip(L"Checking…", true, true);
  else chip(g.pkg == Pkg::Missing ? L"Not prepared yet" : L"Update: prepare again", false);
  chip(std::wstring(kQuality[g.set.resolution]) + L"  ·  " + (g.set.frameRate ? L"60" : L"30") + L" fps  ·  " +
           ui::Setting(6).names[g.set.aspect], true, true);

  // PLAY
  const bool preparing = JobIs(Job::Prepare);
  const bool needPrep = g.pkg == Pkg::Missing || g.pkg == Pkg::Stale;
  const float px = kX0, py = 352, pw = 380, ph = 92;
  const bool f = Add(kPlay, RectF(px, py, pw + 16, ph), 0, 0);
  LinearGradientBrush lg(PointF(px, py), PointF(px, py + ph), Color(f ? 0xFFFFC879 : col::amber), Color(f ? col::orange : col::deep));
  Para(gr, px, py, pw, ph, 16, lg);
  if (f) ParaOutline(gr, px - 4, py - 4, pw + 8, ph + 8, 16, 0xFFFFD9A0, 2.5f);
  if (preparing) {
    Text(gr, L"PREPARING", RectF(px + 34, py + 6, pw - 40, 46), g.f.h1.get(), col::ink, Align::Left, true);
    Bar(gr, px + 34, py + 58, pw - 80, 14, g.job.fraction);
  } else {
    Text(gr, needPrep ? L"PREPARE & PLAY" : L"PLAY", RectF(px + 34, py, pw - 40, ph), needPrep ? g.f.h1.get() : g.f.play.get(),
         col::ink, Align::Left, true);
    Chevrons(gr, px + pw - 92, py + 30, 32, 0xFF0A1422, 3, 0.3f);
  }
  Button(gr, kHomeSettings, kX0, 466, 180, 42, L"SETTINGS", 1, 0);
  Button(gr, kHomeFolder, kX0 + 200, 466, 200, 42, L"GAME FOLDER", 1, 1);

  std::wstring stats = g.cfg.sessions ? L"Last played " + FormatAgo(g.cfg.lastPlayed) + L"   ·   " +
                                            FormatDuration(g.cfg.playedSeconds) + L" in total"
                                      : L"Not played yet — have a good race.";
  Text(gr, stats, RectF(kX0, 524, 700, 26), g.f.body.get(), col::dim);

  // a problem with the last session, or the in-game keys
  if (!g.problem.empty()) {
    Panel(gr, kX0, 566, 900, 100, 0xD0301620);
    Check(gr, kX0 + 22, 582, false);
    Text(gr, g.problem, RectF(kX0 + 50, 576, 820, 30), g.f.bodyBold.get(), col::text);
    Button(gr, kHomeLog, kX0 + 50, 616, 160, 36, L"OPEN LOG", 2, 0);
    Button(gr, kHomeReport, kX0 + 226, 616, 230, 36, L"REPORT A PROBLEM", 2, 1);
  } else {
    Panel(gr, kX0, 580, 900, 82);
    Text(gr, L"IN THE GAME", RectF(kX0 + 24, 588, 300, 22), g.f.tiny.get(), col::orange, Align::Left, true);
    Text(gr, L"F1  Settings & quit      F12  Screenshot      Alt+Enter  Full screen      Select + Start  Settings (controller)",
         RectF(kX0 + 24, 612, 860, 34), g.f.note.get(), col::text);
  }
}

struct SettingRow {
  int setting;  // index into ui::Setting(), -1: the launcher's own option
  const wchar_t* help;
};
const SettingRow kRows[] = {
    {1, L"Renders the game at a multiple of the PS2's resolution and averages it down to your screen (supersampling): "
        L"higher is sharper and smoother, and needs a faster graphics card. If the game stutters, choose a lower one."},
    {2, L"60 doubles the PlayStation 2's 30 frames per second in races, with the game at its normal speed. 30 is the original."},
    {6, L"16:9 shows a wider view in races and keeps the menus' shape. Changing between 16:9 and 4:3 takes effect the next "
        L"time the game starts."},
    {3, L"Full screen covers the whole monitor. In the game, Alt+Enter or F11 switches at any time."},
    {4, L"On: steady motion without tearing. Off: slightly lower delay; the picture may tear."},
    {5, L"How the picture is scaled to your screen. Sharp bilinear keeps it crisp without jagged edges."},
    {7, L"Adds contrast to edges. Off or Low suits most screens; higher values can make edges shimmer."},
    {8, L"Darkens alternate lines like a CRT television."},
    {0, L"GPU draws the game with your graphics card (fast, with every picture option). Software is a slower reference "
        L"kept for comparison."},
    {-1, L"What happens when you quit the game: come back to this launcher, or close it too."},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

int RowValue(int row, int** field, int* count) {
  if (kRows[row].setting < 0) {
    if (field) *field = &g.cfg.afterGame;
    if (count) *count = 2;
    return g.cfg.afterGame;
  }
  const ui::SettingInfo& s = ui::Setting(kRows[row].setting);
  if (field) *field = &(g.set.*s.field);
  if (count) *count = s.count;
  return g.set.*s.field;
}

void PaintSettings(Graphics& gr) {
  Heading(gr, L"SETTINGS", L"The game reads these when it starts; in the game, F1 changes them while you play.");
  const float lx = kX0, vx = kX0 + 226, vw = 300, rh = 38;
  float y = 156;
  int focusedRow = -1;
  for (int r = 0; r < kRowCount; ++r) {
    if (r == 0 || r == kRowCount - 1) {
      Text(gr, r == 0 ? L"PICTURE" : L"LAUNCHER", RectF(lx, y, 300, 24), g.f.tiny.get(), col::orange, Align::Left, true);
      y += 26;
    }
    const int id = kSetRow + r;
    const bool f = Add(id, RectF(lx, y, vx + vw - lx + 12, rh - 4), r, 0, true);
    if (f) focusedRow = r;
    if (f) Para(gr, lx - 10, y, vx + vw - lx + 26, rh - 4, 8, 0x50173152);
    const std::wstring label = kRows[r].setting == 1 ? L"Picture quality" : kRows[r].setting < 0 ? L"After the game closes"
                                                                                                 : ui::Setting(kRows[r].setting).label;
    Text(gr, label, RectF(lx + 6, y, vx - lx - 16, rh - 4), g.f.bodyBold.get(), f ? col::text : col::dim, Align::Left, true);
    int count = 0;
    const int v = RowValue(r, nullptr, &count);
    const std::wstring value = kRows[r].setting == 1   ? kQuality[v]
                               : kRows[r].setting < 0 ? (v ? L"Close the launcher" : L"Show the launcher")
                                                      : ui::Setting(kRows[r].setting).names[v];
    Para(gr, vx, y + 2, vw, rh - 8, 8, f ? col::orange : 0xC00B1828);
    if (!f) ParaOutline(gr, vx, y + 2, vw, rh - 8, 8, 0x663B6AA0, 1);
    Text(gr, L"‹", RectF(vx + 6, y, 24, rh - 4), g.f.h2.get(), f ? col::ink : col::faint, Align::Center);
    Text(gr, L"›", RectF(vx + vw - 22, y, 24, rh - 4), g.f.h2.get(), f ? col::ink : col::faint, Align::Center);
    Text(gr, value, RectF(vx + 30, y, vw - 52, rh - 4), g.f.button.get(), f ? col::ink : col::text, Align::Center, true);
    if (kRows[r].setting == 1 && v == g.recommended)
      Text(gr, L"RECOMMENDED", RectF(vx + vw + 20, y, 110, rh - 4), g.f.tiny.get(), col::amber, Align::Left, true);
    y += rh;
  }
  Button(gr, kSetReset, lx, y + 14, 230, 38, L"RESET TO DEFAULTS", kRowCount, 0);
  // the focused row's explanation
  const float hx = kX0 + 660, hw = kX1 - hx;
  Panel(gr, hx, 156, hw, 330);
  if (focusedRow >= 0) {
    const std::wstring title = kRows[focusedRow].setting == 1 ? L"PICTURE QUALITY"
                               : kRows[focusedRow].setting < 0 ? L"AFTER THE GAME CLOSES"
                                                              : ui::Setting(kRows[focusedRow].setting).label;
    std::wstring upper = title;
    for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
    Text(gr, upper, RectF(hx + 22, 172, hw - 40, 26), g.f.h2.get(), col::text, Align::Left, true);
    std::wstring help = kRows[focusedRow].help;
    if (kRows[focusedRow].setting == 1) help += L"\n\nRecommended: " + Recommendation() + L".";
    Text(gr, help, RectF(hx + 22, 206, hw - 44, 270), g.f.note.get(), col::dim, Align::Left, false, true);
  } else {
    Text(gr, L"Choose a setting to see what it does.", RectF(hx + 22, 172, hw - 40, 60), g.f.note.get(), col::dim, Align::Left, false, true);
  }
  Text(gr, L"Saved in hp2native.ini in the game folder.", RectF(hx, 500, hw, 44), g.f.note.get(), col::faint, Align::Left, false, true);
}

void PaintSaves(Graphics& gr) {
  Heading(gr, L"MEMORY CARD", L"The game saves to a memory card in slot 1, kept as ordinary files in the game folder.");
  const float lw = 450;
  Panel(gr, kX0, 156, lw, 410);
  Text(gr, L"ON THE CARD", RectF(kX0 + 22, 168, 300, 24), g.f.tiny.get(), col::orange, Align::Left, true);
  if (g.saves.empty()) {
    Text(gr, L"Empty. Saves appear here once the game has saved (career progress, settings).", RectF(kX0 + 22, 198, lw - 50, 80),
         g.f.body.get(), col::dim, Align::Left, false, true);
  }
  float y = 198;
  for (std::size_t i = 0; i < g.saves.size() && i < 8; ++i, y += 44) {
    const SaveEntry& s = g.saves[i];
    Text(gr, s.name, RectF(kX0 + 22, y, lw - 60, 22), g.f.bodyBold.get(), col::text);
    Text(gr, FormatBytes(s.bytes) + L"  ·  changed " + FormatAgo(s.time), RectF(kX0 + 22, y + 20, lw - 60, 20), g.f.note.get(), col::dim);
  }
  Button(gr, kSaveBackup, kX0, 586, 200, 42, L"BACK UP NOW", 0, 0, true, !g.saves.empty());
  Button(gr, kSaveFolder, kX0 + 222, 586, 220, 42, L"OPEN SAVES FOLDER", 0, 1);

  const float bx = kX0 + lw + 40, bw = kX1 - bx;
  Panel(gr, bx, 156, bw, 472);
  Text(gr, L"BACKUPS", RectF(bx + 22, 168, 300, 24), g.f.tiny.get(), col::orange, Align::Left, true);
  if (g.backups.empty())
    Text(gr, L"No backups yet. A backup is a copy of the whole card; restoring one keeps the current card as a backup too.",
         RectF(bx + 22, 198, bw - 50, 80), g.f.body.get(), col::dim, Align::Left, false, true);
  y = 198;
  for (std::size_t i = 0; i < g.backups.size() && i < 9; ++i, y += 46) {
    const int id = kSaveRow + static_cast<int>(i);
    const bool f = Add(id, RectF(bx + 14, y, bw - 40, 40), static_cast<int>(i) + 1, 0);
    if (f) Para(gr, bx + 14, y, bw - 40, 40, 8, g.armed == id ? 0xFFB3261E : col::orange);
    Text(gr, g.backups[i].name, RectF(bx + 30, y, bw - 250, 40), g.f.bodyBold.get(), f ? col::ink : col::text, Align::Left, true);
    Text(gr, f ? (g.armed == id ? L"PRESS AGAIN TO RESTORE" : L"RESTORE") : FormatBytes(g.backups[i].bytes),
         RectF(bx + bw - 250, y, 210, 40), f ? g.f.tiny.get() : g.f.note.get(), f ? (g.armed == id ? col::text : col::ink) : col::dim,
         Align::Right, f);
  }
}

void PaintScreenshots(Graphics& gr) {
  Heading(gr, L"SCREENSHOTS", L"Press F12 in the game. Pictures are saved in the screenshots folder.");
  Button(gr, kShotFolder, kX1 - 200, 66, 190, 40, L"OPEN FOLDER", 0, 0);
  if (g.shots.empty()) {
    Panel(gr, kX0, 170, 640, 120);
    Text(gr, L"No screenshots yet.", RectF(kX0 + 24, 186, 560, 30), g.f.h2.get(), col::text, Align::Left, true);
    Text(gr, L"Press F12 while you play. The newest one also becomes the picture behind this launcher.",
         RectF(kX0 + 24, 220, 590, 60), g.f.body.get(), col::dim, Align::Left, false, true);
    return;
  }
  const int cols = 4, visibleRows = 3;
  const float tw = 216, th = 121.5f, gap = 16, top = 160;
  std::lock_guard l(g.thumbs.m);
  for (int i = 0; i < static_cast<int>(g.shots.size()); ++i) {
    const int row = i / cols, c = i % cols;
    if (row < g.shotScroll || row >= g.shotScroll + visibleRows) {
      g.widgets.push_back({kShot + i, RectF(-1000, -1000, 1, 1), row + 1, c, false});  // reachable by keys
      continue;
    }
    const float x = kX0 + c * (tw + gap), y = top + (row - g.shotScroll) * (th + gap + 26);
    const bool f = Add(kShot + i, RectF(x, y, tw, th), row + 1, c);
    auto it = g.thumbs.ready.find(g.shots[i].wstring());
    if (it != g.thumbs.ready.end()) gr.DrawImage(it->second.get(), RectF(x, y, tw, th));
    else Para(gr, x, y, tw, th, 0, 0xC00B1828);
    Pen border(Color(f ? col::orange : 0x663B6AA0), f ? 3.0f : 1.0f);
    gr.DrawRectangle(&border, x, y, tw, th);
    std::wstring name = g.shots[i].stem().wstring();
    if (name.rfind(L"Hot Pursuit 2 ", 0) == 0) name = name.substr(14);
    Text(gr, name, RectF(x, y + th + 2, tw, 22), g.f.note.get(), f ? col::text : col::dim);
  }
  const int rows = (static_cast<int>(g.shots.size()) + cols - 1) / cols;
  if (rows > visibleRows)
    Text(gr, L"Row " + std::to_wstring(g.shotScroll + 1) + L"–" + std::to_wstring(std::min(rows, g.shotScroll + visibleRows)) +
             L" of " + std::to_wstring(rows),
         RectF(kX0, 650, 400, 24), g.f.note.get(), col::faint);
}

void PaintViewer(Graphics& gr) {
  SolidBrush dark(Color(0xF0040912));
  gr.FillRectangle(&dark, 0.0f, kTitleH, kW, kHintsY - kTitleH);
  const fs::path& p = g.shots[static_cast<std::size_t>(g.viewer)];
  if (g.viewerPath != p.wstring()) {
    g.viewerBmp = std::make_unique<Bitmap>(p.c_str());
    g.viewerPath = p.wstring();
  }
  Bitmap* full = g.viewerBmp.get();
  if (full && full->GetLastStatus() == Ok && full->GetWidth()) {
    const float sw = static_cast<float>(full->GetWidth()), sh = static_cast<float>(full->GetHeight());
    const float s = std::min(1200 / sw, 590 / sh);
    gr.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    gr.DrawImage(full, RectF((kW - sw * s) / 2, 50, sw * s, sh * s));
  }
  Text(gr, p.filename().wstring() + L"   ·   " + std::to_wstring(g.viewer + 1) + L" of " + std::to_wstring(g.shots.size()) +
               L"   ·   ← → browse   ·   Esc close",
       RectF(40, 648, kW - 80, 30), g.f.note.get(), col::dim, Align::Center);
}

void PaintJob(Graphics& gr, float x, float y, float w) {  // a running job: bar, status, cancel
  Bar(gr, x, y, w, 18, g.job.fraction);
  std::wstring s = g.job.Status();
  if (g.job.job == Job::Copy && g.job.total) {
    const double el = std::max(0.5, Clock() - g.job.started);
    const double rate = static_cast<double>(g.job.bytes) / el;
    wchar_t b[160];
    swprintf(b, 160, L"%ls of %ls  ·  %.0f MB/s", FormatBytes(g.job.bytes).c_str(), FormatBytes(g.job.total).c_str(), rate / 1048576.0);
    s = b;
    if (rate > 0 && el > 3) s += L"  ·  about " + FormatDuration(static_cast<std::int64_t>((g.job.total - g.job.bytes) / rate) + 30) + L" left";
  }
  Text(gr, s, RectF(x, y + 26, w - 160, 26), g.f.note.get(), col::dim);
  wchar_t pct[16];
  swprintf(pct, 16, L"%.0f%%", g.job.fraction * 100);
  Text(gr, pct, RectF(x + w - 150, y + 26, 150, 26), g.f.bodyBold.get(), col::text, Align::Right);
}

void PaintSetup(Graphics& gr) {
  Heading(gr, L"DISC & SETUP", L"Your disc's files stay on this PC, read-only. Nothing is downloaded and nothing leaves it.");
  Panel(gr, kX0, 156, kX1 - kX0, 330);
  float y = 172;
  auto row = [&](const std::wstring& label, const std::wstring& value, int ok) {  // ok: 1 tick, 0 cross, -1 none
    Text(gr, label, RectF(kX0 + 24, y, 200, 26), g.f.bodyBold.get(), col::dim, Align::Left, true);
    if (ok >= 0) Check(gr, kX0 + 220, y + 4, ok == 1);
    Text(gr, value, RectF(kX0 + (ok >= 0 ? 248 : 220), y, kX1 - kX0 - 280, 26), g.f.body.get(), col::text);
    y += 40;
  };
  const DiscCheck& dc = g.disc;
  row(L"Disc folder", g.cfg.disc.empty() ? L"(none)" : g.cfg.disc, dc.state == DiscState::Ok ? 1 : 0);
  row(L"Edition", dc.state == DiscState::Ok ? L"North America, SLUS-20362 (supported)" : dc.detail, dc.state == DiscState::Ok ? 1 : 0);
  row(L"Disc files", g.cfg.verified ? L"All files match the original disc (checked " + FormatAgo(g.cfg.verified) + L")"
                                    : L"Not checked file by file yet", g.cfg.verified ? 1 : -1);
  std::error_code ec;
  const bool hasPkg = fs::exists(g.paths.package, ec);
  std::wstring pkg = g.pkg == Pkg::Ok ? L"Prepared for this version" : g.pkg == Pkg::Checking ? L"Checking…"
                     : hasPkg ? L"Made by another version: prepare again" : L"Not prepared yet";
  if (hasPkg && g.pkg == Pkg::Ok) pkg += L" (" + FormatBytes(fs::file_size(g.paths.package, ec)) + L")";
  row(L"Game program", pkg, g.pkg == Pkg::Ok ? 1 : g.pkg == Pkg::Checking ? -1 : 0);
  row(L"Game folder", g.paths.dir.wstring(), -1);
  row(L"Recompiler", g.paths.recomp.empty() ? L"hp2recomp.exe not found" : g.paths.recomp.filename().wstring(), g.paths.recomp.empty() ? 0 : 1);
  const bool busy = g.job.running && g.job.job != Job::CheckPackage;
  if (busy) {
    PaintJob(gr, kX0, 506, 700);
    Button(gr, kJobCancel, kX0 + 730, 500, 160, 40, L"CANCEL", 0, 0);
  } else {
    Button(gr, kSetupCheck, kX0, 506, 210, 42, L"CHECK ALL FILES", 0, 0, false, dc.state == DiscState::Ok);
    Button(gr, kSetupPrepare, kX0 + 228, 506, 210, 42, L"PREPARE AGAIN", 0, 1, false, dc.state == DiscState::Ok);
    Button(gr, kSetupDisc, kX0 + 456, 506, 210, 42, L"CHANGE DISC…", 0, 2);
    Button(gr, kSetupFolder, kX0, 562, 210, 42, L"OPEN GAME FOLDER", 1, 0);
    Button(gr, kSetupShortcut, kX0 + 228, 562, 250, 42, L"DESKTOP SHORTCUT", 1, 1);
  }
}

void PaintAbout(Graphics& gr) {
  Heading(gr, L"ABOUT", std::wstring(L"HotPursuit2PC ") + kVersion + L"  ·  MIT license  ·  development build");
  const float w = kX1 - kX0;
  Panel(gr, kX0, 156, w, 430);
  float y = 172;
  auto block = [&](const wchar_t* title, const wchar_t* body, float h) {
    Text(gr, title, RectF(kX0 + 24, y, w - 48, 22), g.f.tiny.get(), col::orange, Align::Left, true);
    Text(gr, body, RectF(kX0 + 24, y + 22, w - 48, h), g.f.note.get(), col::text, Align::Left, false, true);
    y += h + 34;
  };
  block(L"WHAT THIS IS",
        L"HotPursuit2PC runs Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) natively on Windows. The game's program on your "
        L"own disc is translated into PC code on your computer: no emulator runs, and no game files come with this project.",
        44);
  block(L"AN AI-ASSISTED PROJECT",
        L"I started HotPursuit2PC to get hands-on experience with using AI for software development, on a project big enough "
        L"to be a real test. Almost all of the code, tests and documentation were written by an AI coding agent (Anthropic's "
        L"Claude, in Claude Code) under my direction: I set the goals and the rules, made the decisions, tested the builds "
        L"and played the results.",
        64);
  block(L"CREDITS",
        L"Widescreen and 60 fps values from the PCSX2 community patches by nemesis2000, Aero_ and asasega. Code generation by "
        L"LLVM (Apache 2.0 with LLVM exceptions). The Play! emulator (BSD 2-Clause) served as a reference during development; "
        L"none of it is in the native game.",
        64);
  block(L"LEGAL",
        L"Not affiliated with, sponsored by or endorsed by Electronic Arts or Sony. Need for Speed and Hot Pursuit 2 are "
        L"trademarks of Electronic Arts Inc.; PlayStation is a trademark of Sony Interactive Entertainment.",
        44);
  Button(gr, kAboutProject, kX0, 606, 200, 42, L"PROJECT PAGE", 0, 0);
  Button(gr, kAboutGuide, kX0 + 218, 606, 220, 42, L"GETTING STARTED", 0, 1);
  Button(gr, kAboutReport, kX0 + 456, 606, 230, 42, L"REPORT A PROBLEM", 0, 2);
  Button(gr, kAboutLog, kX0 + 704, 606, 180, 42, L"OPEN LOG", 0, 3);
}

// --------------------------------------------------------------------------------------------------- the wizard ----
void PaintWizard(Graphics& gr) {
  const float x0 = 140, x1 = 1140;
  // steps
  const wchar_t* steps[] = {L"DISC", L"CHECK", L"PREPARE", L"READY"};
  const int cur = g.step == Step::Welcome ? -1 : g.step == Step::Disc || g.step == Step::Copy ? 0 : g.step == Step::Check ? 1
                  : g.step == Step::Prepare ? 2 : 3;
  for (int i = 0; i < 4; ++i) {
    const float sx = x0 + i * 250.0f;
    const ARGB c = i == cur ? col::orange : i < cur ? col::steel : 0xFF1B2D45;
    Para(gr, sx, 62, 230, 34, 10, c);
    Text(gr, std::to_wstring(i + 1) + L"  " + steps[i], RectF(sx + 18, 62, 200, 34), g.f.button.get(), i == cur ? col::ink : i < cur ? col::text : col::faint,
         Align::Left, true);
  }
  const float ty = 140;
  auto title = [&](const std::wstring& t, const std::wstring& sub) {
    Text(gr, t, RectF(x0, ty, x1 - x0, 60), g.f.h1.get(), col::text, Align::Left, true);
    Para(gr, x0 + 2, ty + 56, 120, 4, 2, col::orange);
    Text(gr, sub, RectF(x0, ty + 70, x1 - x0 - 100, 60), g.f.body.get(), col::dim, Align::Left, false, true);
  };
  switch (g.step) {
    case Step::Welcome:
      Text(gr, L"NEED FOR SPEED", RectF(x0 + 4, 150, 600, 28), g.f.h2.get(), col::orange, Align::Left, true);
      Text(gr, L"HOT PURSUIT 2", RectF(x0 - 6, 170, 900, 96), g.f.hero.get(), col::text, Align::Left, true);
      Text(gr, L"Welcome. Let's get the game ready to play on this PC — it takes a few minutes, once.\n\nYou need your own "
               L"disc of the North American release (SLUS-20362), in a DVD drive or as an .iso image of your disc. Its files "
               L"are copied and checked, then the game's program is translated for your processor. Nothing is downloaded.",
           RectF(x0, 280, 760, 160), g.f.body.get(), col::dim, Align::Left, false, true);
      Button(gr, kWizStart, x0, 470, 300, 60, L"GET STARTED", 0, 0, true);
      break;
    case Step::Disc: {
      title(L"YOUR DISC", L"Insert your disc, or double-click your .iso file in File Explorer so Windows shows it as a drive. "
                          L"It appears here by itself.");
      float y = 290;
      int row = 0;
      for (std::size_t i = 0; i < g.drives.size() && i < 3; ++i, y += 84, ++row) {
        const int id = kWizDrive + static_cast<int>(i);
        const bool f = Add(id, RectF(x0, y, 760, 70), row, 0);
        Para(gr, x0, y, 760, 70, 14, f ? col::orange : col::panelHi);
        const DiscDrive& d = g.drives[i];
        Text(gr, L"COPY FROM " + d.root.substr(0, 2) + (d.label.empty() ? L"" : L"  " + d.label), RectF(x0 + 30, y + 8, 700, 30),
             g.f.h2.get(), f ? col::ink : col::text, Align::Left, true);
        Text(gr, std::wstring(d.optical ? L"Your disc or a mounted .iso" : L"A removable drive") + L"  ·  copies " +
                     FormatBytes(DiscBytes()) + L" to the game folder (recommended)",
             RectF(x0 + 30, y + 36, 700, 26), g.f.note.get(), f ? col::ink : col::dim);
      }
      if (g.drives.empty()) {
        Panel(gr, x0, y, 760, 70, 0x800B1828);
        Text(gr, L"Looking for the disc in your drives…", RectF(x0 + 30, y, 700, 70), g.f.body.get(), col::dim, Align::Left, true);
        y += 84;
      }
      Button(gr, kWizFolder, x0, y + 10, 420, 46, L"CHOOSE A FOLDER WITH THE DISC'S FILES…", row, 0);
      if (!g.discError.empty()) {
        Check(gr, x0, y + 78, false);
        Text(gr, g.discError, RectF(x0 + 28, y + 72, 900, 60), g.f.body.get(), col::bad, Align::Left, false, true);
      }
      break;
    }
    case Step::Copy:
      title(L"COPYING YOUR DISC", L"The files go to the game folder, so the disc does not have to stay in the drive.");
      PaintJob(gr, x0, 300, 760);
      Button(gr, kJobCancel, x0, 380, 160, 42, L"CANCEL", 0, 0);
      break;
    case Step::Check:
      title(L"CHECKING THE FILES", L"Every file is compared with the original disc, so the game runs exactly as it should.");
      PaintJob(gr, x0, 300, 760);
      Button(gr, kJobCancel, x0, 380, 160, 42, L"CANCEL", 0, 0);
      break;
    case Step::Prepare:
      title(L"PREPARING THE GAME", L"The game's program is translated into code for your processor. This takes about a "
                                   L"minute and happens again only after an update.");
      if (g.job.running) {
        PaintJob(gr, x0, 300, 760);
        Button(gr, kJobCancel, x0, 380, 160, 42, L"CANCEL", 0, 0);
      } else {
        Check(gr, x0, 306, false);
        Text(gr, g.job.Error(), RectF(x0 + 28, 300, 900, 60), g.f.body.get(), col::bad, Align::Left, false, true);
        Button(gr, kWizRetry, x0, 380, 200, 42, L"TRY AGAIN", 0, 0, true);
      }
      break;
    case Step::Ready: {
      title(L"READY TO RACE", L"Everything is in place. You can change any of this later in the launcher.");
      Panel(gr, x0, 290, 760, 150);
      float y = 306;
      auto line = [&](const std::wstring& s) {
        Check(gr, x0 + 24, y + 4, true);
        Text(gr, s, RectF(x0 + 54, y, 680, 26), g.f.body.get(), col::text);
        y += 38;
      };
      line(L"Disc checked: North America, SLUS-20362");
      line(L"Game prepared for your processor");
      line(L"Picture quality: " + std::wstring(kQuality[g.set.resolution]) + (g.set.frameRate ? L", 60 fps, " : L", 30 fps, ") +
           ui::Setting(6).names[g.set.aspect] + (g.set.fullscreen ? L", full screen" : L", in a window"));
      Button(gr, kWizPlay, x0, 470, 260, 60, L"PLAY NOW", 0, 0, true);
      Button(gr, kWizLauncher, x0 + 290, 478, 260, 46, L"OPEN THE LAUNCHER", 0, 1);
      break;
    }
  }
}

// ------------------------------------------------------------------------------------------------------ frame ----
void PaintAll(Graphics& gr) {
  g.widgets.clear();
  gr.SetSmoothingMode(SmoothingModeAntiAlias);
  gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  gr.SetPixelOffsetMode(PixelOffsetModeHalf);
  gr.SetInterpolationMode(InterpolationModeHighQualityBilinear);
  // background: cached per size and screenshot
  const bool home = !g.wizard && g.page == Page::Home;  // the screenshot shows through most on the home screen
  int key = g.cw * 7919 + g.ch * 31 + (home ? 1 : 0);
  {
    std::lock_guard l(g.thumbs.m);
    key += g.thumbs.heroVersion * 104729;
  }
  if (!g.bg || key != g.bgKey || static_cast<int>(g.bg->GetWidth()) != g.cw || static_cast<int>(g.bg->GetHeight()) != g.ch) {
    g.bg = std::make_unique<Bitmap>(std::max(1, g.cw), std::max(1, g.ch), PixelFormat32bppPARGB);
    Graphics bgr(g.bg.get());
    bgr.SetSmoothingMode(SmoothingModeAntiAlias);
    bgr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    bgr.SetPixelOffsetMode(PixelOffsetModeHalf);
    bgr.ScaleTransform(g.scale, g.scale);
    PaintBackground(bgr, home);
    g.bgKey = key;
  }
  gr.DrawImage(g.bg.get(), Rect(0, 0, g.cw, g.ch));
  gr.ScaleTransform(g.scale, g.scale);
  if (g.wizard) {
    PaintWizard(gr);
  } else {
    PaintRail(gr);
    switch (g.page) {
      case Page::Home: PaintHome(gr); break;
      case Page::Settings: PaintSettings(gr); break;
      case Page::Saves: PaintSaves(gr); break;
      case Page::Screenshots: PaintScreenshots(gr); break;
      case Page::Setup: PaintSetup(gr); break;
      case Page::About: PaintAbout(gr); break;
      case Page::Count: break;
    }
    if (g.viewer >= 0 && g.viewer < static_cast<int>(g.shots.size())) PaintViewer(gr);
  }
  PaintTitleBar(gr);
  PaintHints(gr);
  PaintToast(gr);
  // keep the focus on something that exists
  auto exists = [](int id) { return std::any_of(g.widgets.begin(), g.widgets.end(), [id](const Widget& w) { return w.id == id; }); };
  if (!exists(g.focus)) {
    g.dirty = true;  // draw the focus chosen here
    g.focus = -1;
    for (const Widget& w : g.widgets)
      if ((w.col < 0) == g.focusRail) {
        g.focus = w.id;
        break;
      }
  }
}

void Render(HDC target) {
  if (g.cw <= 0 || g.ch <= 0) return;
  if (!g.memDc || g.memW != g.cw || g.memH != g.ch) {
    if (g.memDc) DeleteDC(g.memDc);
    if (g.memBmp) DeleteObject(g.memBmp);
    g.memDc = CreateCompatibleDC(target);
    g.memBmp = CreateCompatibleBitmap(target, g.cw, g.ch);
    SelectObject(g.memDc, g.memBmp);
    g.memW = g.cw;
    g.memH = g.ch;
  }
  {
    Graphics gr(g.memDc);
    PaintAll(gr);
  }
  BitBlt(target, 0, 0, g.cw, g.ch, g.memDc, 0, 0, SRCCOPY);
}

// ------------------------------------------------------------------------------------------------------ input ----
enum class In { Up, Down, Left, Right, Accept, Back, PrevPage, NextPage, Start };

const Widget* Find(int id) {
  for (const Widget& w : g.widgets)
    if (w.id == id) return &w;
  return nullptr;
}

void ChangeOption(int id, int delta) {
  const int row = id - kSetRow;
  if (row < 0 || row >= kRowCount) return;
  int* field = nullptr;
  int count = 0;
  RowValue(row, &field, &count);
  *field = (*field + delta + count) % count;
  if (kRows[row].setting < 0) SaveConfig(g.paths, g.cfg);
  else ui::Save(g.set);
}

void JobDone(Job job);

void Activate(int id) {
  if (id >= kRail && id < kRail + static_cast<int>(Page::Count)) return GoPage(static_cast<Page>(id - kRail), false);
  if (id >= kSetRow && id < kSetRow + kRowCount) return ChangeOption(id, 1);
  if (id >= kSaveRow && id < kSaveRow + 50) {
    if (g.armed != id) {
      g.armed = id;
      return;
    }
    g.armed = -1;
    std::wstring msg;
    RestoreSaves(g.paths, g.backups[static_cast<std::size_t>(id - kSaveRow)].name, msg);
    RefreshLists();
    return Toast(msg);
  }
  if (id >= kShot && id < kShot + 100000) {
    g.viewer = id - kShot;
    return;
  }
  if (id >= kWizDrive && id < kWizDrive + 26) {
    const std::size_t i = static_cast<std::size_t>(id - kWizDrive);
    if (i < g.drives.size()) {
      g.discError.clear();
      StartCopy(g.job, g.drives[i].root, g.paths.copiedDisc, g.wnd, kJobDone);
      g.step = Step::Copy;
    }
    return;
  }
  switch (id) {
    case kPlay: return Play();
    case kHomeSettings: return GoPage(Page::Settings, false);
    case kHomeFolder: case kSetupFolder: return OpenFolder(g.paths.dir);
    case kHomeLog: case kAboutLog: return OpenFolder(g.paths.game.parent_path());
    case kHomeReport: case kAboutReport: return OpenUrl(kIssuesUrl);
    case kSetReset: {
      g.set = ui::Settings{};
      g.set.resolution = g.recommended;
      g.cfg.afterGame = 0;
      ui::Save(g.set);
      SaveConfig(g.paths, g.cfg);
      return Toast(L"Settings reset (picture quality as recommended for your screen)");
    }
    case kSaveBackup: {
      std::wstring msg;
      BackupSaves(g.paths, msg);
      RefreshLists();
      return Toast(msg);
    }
    case kSaveFolder: return OpenFolder(g.paths.game.parent_path() / L"saves");
    case kShotFolder: return OpenFolder(g.paths.game.parent_path() / L"screenshots");
    case kSetupCheck: StartVerify(g.job, g.cfg.disc, g.wnd, kJobDone); return;
    case kSetupPrepare:
      g.pkg = Pkg::Stale;
      StartPrepare(g.job, g.paths, g.cfg.disc, g.wnd, kJobDone);
      return;
    case kSetupDisc: return ChooseFolder();
    case kSetupShortcut: return Toast(MakeShortcut() ? L"Shortcut \"Hot Pursuit 2 (PC)\" placed on the desktop" : L"The shortcut could not be made");
    case kJobCancel: g.job.cancel = true; return;
    case kAboutProject: return OpenUrl(kRepoUrl);
    case kAboutGuide: return OpenUrl(kGuideUrl);
    case kWizStart:
      g.step = Step::Disc;
      g.drivesAt = -10;
      return;
    case kWizFolder: return ChooseFolder();
    case kWizRetry: StartPrepare(g.job, g.paths, g.cfg.disc, g.wnd, kJobDone); return;
    case kWizPlay:
      g.wizard = false;
      GoPage(Page::Home, false);
      return Play();
    case kWizLauncher:
      g.wizard = false;
      return GoPage(Page::Home, false);
    default: return;
  }
}

void Move(int dRow, int dCol) {
  const Widget* cur = Find(g.focus);
  if (!cur) return;
  const bool rail = cur->col < 0;
  const Widget* best = nullptr;
  float bestScore = 1e9f;
  for (const Widget& w : g.widgets) {
    if ((w.col < 0) != rail || w.id == cur->id) continue;
    if (dRow && (w.row - cur->row) * dRow <= 0) continue;
    if (dCol && (w.row != cur->row || (w.col - cur->col) * dCol <= 0)) continue;
    const float score = dRow ? std::abs(static_cast<float>(w.row - cur->row)) * 1000 + std::abs(w.r.X - cur->r.X)
                             : std::abs(static_cast<float>(w.col - cur->col));
    if (score < bestScore) {
      bestScore = score;
      best = &w;
    }
  }
  if (!best) return;
  g.focus = best->id;
  if (rail) GoPage(static_cast<Page>(best->id - kRail), true);
  if (g.page == Page::Screenshots && best->id >= kShot) {  // keep the focused thumbnail on screen
    if (best->row - 1 < g.shotScroll) g.shotScroll = best->row - 1;
    if (best->row - 1 >= g.shotScroll + 3) g.shotScroll = best->row - 3;
  }
}

void Input(In in) {
  g.dirty = true;
  if (g.viewer >= 0) {
    const int n = static_cast<int>(g.shots.size());
    if (in == In::Left) g.viewer = (g.viewer + n - 1) % n;
    else if (in == In::Right) g.viewer = (g.viewer + 1) % n;
    else if (in == In::Back || in == In::Accept) {
      g.focus = kShot + g.viewer;
      g.viewer = -1;
    }
    return;
  }
  if (in != In::Accept) g.armed = -1;
  if (!g.wizard && (in == In::PrevPage || in == In::NextPage)) {
    const int n = static_cast<int>(Page::Count);
    return GoPage(static_cast<Page>((static_cast<int>(g.page) + (in == In::NextPage ? 1 : n - 1)) % n), g.focusRail);
  }
  if (!g.wizard && in == In::Start) {
    GoPage(Page::Home, false);
    return Play();
  }
  const Widget* cur = Find(g.focus);
  switch (in) {
    case In::Up: return Move(-1, 0);
    case In::Down: return Move(1, 0);
    case In::Left:
    case In::Right: {
      const int d = in == In::Left ? -1 : 1;
      if (cur && cur->option) return ChangeOption(cur->id, d);
      if (cur && cur->col < 0) {
        if (d > 0) {
          g.focusRail = false;
          g.focus = -1;
        }
        return;
      }
      const int before = g.focus;
      Move(0, d);
      if (g.focus == before && d < 0 && !g.wizard) GoPage(g.page, true);
      return;
    }
    case In::Accept:
      if (cur) {
        if (cur->col < 0) {
          g.focusRail = false;
          g.focus = -1;
          return;
        }
        return Activate(cur->id);
      }
      return;
    case In::Back:
      if (g.wizard) {
        if (g.step == Step::Disc) g.step = Step::Welcome;
        return;
      }
      if (!g.focusRail) GoPage(g.page, true);
      return;
    default: return;
  }
}

void ClickAt(float x, float y, bool click) {
  for (const Widget& w : g.widgets) {
    if (!w.r.Contains(x, y)) continue;
    if (!click) {
      if (g.focus != w.id) {
        g.focus = w.id;
        g.focusRail = w.col < 0;
        g.dirty = true;
      }
      return;
    }
    g.focus = w.id;
    g.focusRail = w.col < 0;
    if (w.option) ChangeOption(w.id, x < w.r.X + w.r.Width * 0.55f ? -1 : 1);
    else Activate(w.id);
    g.dirty = true;
    return;
  }
}

// XInput, polled with the timer (the first controller).
void PollController() {
  using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
  static GetStateFn get = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
  }();
  static WORD prev = 0;
  static double heldSince = 0, lastRepeat = 0;
  XINPUT_STATE st{};
  const bool connected = get && get(0, &st) == ERROR_SUCCESS;
  if (connected != g.controller) {
    g.controller = connected;
    g.dirty = true;
  }
  if (!connected || GetForegroundWindow() != g.wnd) {
    prev = 0;
    return;
  }
  WORD b = st.Gamepad.wButtons;
  const SHORT lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
  if (ly > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
  if (ly < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
  if (lx < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
  if (lx > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
  const WORD pressed = static_cast<WORD>(b & ~prev);
  const double now = Clock();
  const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
  WORD fire = pressed;
  if (b & dirs) {
    if (pressed & dirs) heldSince = lastRepeat = now;
    else if (now - heldSince > 0.38 && now - lastRepeat > 0.11) {
      fire |= b & dirs;
      lastRepeat = now;
    }
  }
  prev = b;
  if (fire & XINPUT_GAMEPAD_DPAD_UP) Input(In::Up);
  if (fire & XINPUT_GAMEPAD_DPAD_DOWN) Input(In::Down);
  if (fire & XINPUT_GAMEPAD_DPAD_LEFT) Input(In::Left);
  if (fire & XINPUT_GAMEPAD_DPAD_RIGHT) Input(In::Right);
  if (fire & XINPUT_GAMEPAD_A) Input(In::Accept);
  if (fire & XINPUT_GAMEPAD_B) Input(In::Back);
  if (fire & XINPUT_GAMEPAD_LEFT_SHOULDER) Input(In::PrevPage);
  if (fire & XINPUT_GAMEPAD_RIGHT_SHOULDER) Input(In::NextPage);
  if (fire & XINPUT_GAMEPAD_START) Input(In::Start);
}

// ------------------------------------------------------------------------------------------------------- jobs ----
void JobDone(Job job) {
  const std::wstring error = g.job.Error();
  g.dirty = true;
  RefreshDisc();
  switch (job) {
    case Job::CheckPackage:
      g.pkg = g.job.result == 0 ? Pkg::Ok : g.job.result == 5 ? Pkg::Missing : Pkg::Stale;
      if (g.playAfter) {
        g.playAfter = false;
        Play();
      }
      return;
    case Job::Copy:
      if (!error.empty()) {
        g.discError = error;
        g.step = Step::Disc;
        return;
      }
      g.cfg.disc = g.paths.copiedDisc.wstring();
      g.cfg.verified = 0;
      SaveConfig(g.paths, g.cfg);
      if (const DiscCheck c = QuickCheck(g.cfg.disc); c.state != DiscState::Ok) {
        g.discError = c.detail;
        g.step = Step::Disc;
        return;
      }
      StartVerify(g.job, g.cfg.disc, g.wnd, kJobDone);
      g.step = Step::Check;
      return;
    case Job::Verify:
      if (!error.empty()) {
        g.cfg.verified = 0;
        SaveConfig(g.paths, g.cfg);
        if (g.wizard) {
          g.discError = error;
          g.step = Step::Disc;
        } else {
          Toast(error);
        }
        return;
      }
      g.cfg.verified = static_cast<std::int64_t>(std::time(nullptr));
      SaveConfig(g.paths, g.cfg);
      if (g.wizard) {
        g.step = Step::Prepare;
        StartPrepare(g.job, g.paths, g.cfg.disc, g.wnd, kJobDone);
      } else {
        Toast(L"All files match the original disc");
      }
      return;
    case Job::Prepare:
      if (!error.empty()) {
        g.pkg = Pkg::Stale;
        g.playAfter = false;
        if (!g.wizard) {
          if (g.job.cancel) Toast(error);
          else g.problem = error;
        }
        return;
      }
      g.pkg = Pkg::Ok;
      g.problem.clear();
      if (g.wizard) {
        if (g.firstSettings) {  // a new installation: the picture quality this screen can show
          g.set.resolution = g.recommended;
          ui::Save(g.set);
          g.firstSettings = false;
        }
        g.step = Step::Ready;
      } else if (g.playAfter) {
        Launch();
      } else {
        Toast(L"The game is prepared");
      }
      return;
    default: return;
  }
}

void GameEnded() {
  DWORD code = 0;
  GetExitCodeProcess(g.game, &code);
  CloseHandle(g.game);
  g.game = nullptr;
  const double played = Clock() - g.gameStarted;
  g.cfg.playedSeconds += static_cast<std::int64_t>(played);
  g.cfg.lastPlayed = static_cast<std::int64_t>(std::time(nullptr));
  ++g.cfg.sessions;
  SaveConfig(g.paths, g.cfg);
  const std::wstring reason = GameEndProblem(g.paths);
  g.problem = reason.empty() ? L"" : L"The game stopped: " + reason;
  g.set = ui::Load();  // changed in the game with F1
  RefreshLists();
  if (g.problem.empty() && g.cfg.afterGame == 1) {
    DestroyWindow(g.wnd);
    return;
  }
  GoPage(Page::Home, false);
  ShowWindow(g.wnd, SW_SHOW);
  SetForegroundWindow(g.wnd);
  g.dirty = true;
}

// ----------------------------------------------------------------------------------------------------- window ----
void Resize(int cw, int ch) {
  g.cw = cw;
  g.ch = ch;
  g.scale = std::min(cw / kW, ch / kH);
  g.dirty = true;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_NCCALCSIZE:
      if (w) return 0;  // the whole window is client area: the launcher draws its own title bar
      break;
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ScreenToClient(h, &p);
      const float x = p.x / g.scale, y = p.y / g.scale;
      if (y < kTitleH && x < kW - 92) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_SIZE: Resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      g.dirty = false;  // before drawing: the drawing may ask for another frame (a focus it chose)
      Render(dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_TIMER: {
      PollController();
      if (g.wizard && g.step == Step::Disc && Clock() - g.drivesAt > 2.0) {  // the disc appears when it is inserted/mounted
        auto drives = FindDiscDrives();
        g.drivesAt = Clock();
        if (drives.size() != g.drives.size()) g.dirty = true;
        g.drives = std::move(drives);
      }
      if ((g.job.running && g.job.job != Job::CheckPackage) || Clock() - g.toastAt < 3.6) g.dirty = true;
      if (g.dirty) InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      const float x = p.x / g.scale, y = p.y / g.scale;
      const int ht = y < kTitleH ? (x >= kW - 46 ? 2 : x >= kW - 92 ? 1 : 0) : 0;
      if (ht != g.hoverTitle) {
        g.hoverTitle = ht;
        g.dirty = true;
      }
      if (p.x != g.lastMouse.x || p.y != g.lastMouse.y) {
        g.lastMouse = p;
        if (g.viewer < 0) ClickAt(x, y, false);
      }
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      g.hoverTitle = 0;
      g.dirty = true;
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = GET_X_LPARAM(l) / g.scale, y = GET_Y_LPARAM(l) / g.scale;
      if (y < kTitleH && x >= kW - 92) {
        if (x >= kW - 46) PostMessageW(h, WM_CLOSE, 0, 0);
        else ShowWindow(h, SW_MINIMIZE);
        return 0;
      }
      if (g.viewer >= 0) {
        Input(In::Back);
        return 0;
      }
      ClickAt(x, y, true);
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (!g.wizard && g.page == Page::Screenshots && g.viewer < 0) {
        const int rows = (static_cast<int>(g.shots.size()) + 3) / 4;
        g.shotScroll = std::clamp(g.shotScroll + (GET_WHEEL_DELTA_WPARAM(w) < 0 ? 1 : -1), 0, std::max(0, rows - 3));
        g.dirty = true;
      }
      return 0;
    case WM_KEYDOWN:
      switch (w) {
        case VK_UP: Input(In::Up); break;
        case VK_DOWN: Input(In::Down); break;
        case VK_LEFT: Input(In::Left); break;
        case VK_RIGHT: Input(In::Right); break;
        case VK_RETURN: case VK_SPACE: Input(In::Accept); break;
        case VK_ESCAPE: case VK_BACK: Input(In::Back); break;
        case VK_PRIOR: Input(In::PrevPage); break;
        case VK_NEXT: case VK_TAB: Input(In::NextPage); break;
        default: break;
      }
      return 0;
    case kJobDone: JobDone(static_cast<Job>(w)); return 0;
    case kGameEnded: GameEnded(); return 0;
    case kThumbsReady: g.dirty = true; return 0;
    case WM_CLOSE:
      if (g.job.running && g.job.job != Job::CheckPackage) g.job.cancel = true;
      DestroyWindow(h);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
  }
  return DefWindowProcW(h, m, w, l);
}

HICON MakeIcon(int size) {  // the orange chevrons on steel blue, drawn (no image files in the repository)
  Bitmap b(size, size, PixelFormat32bppARGB);
  Graphics gr(&b);
  gr.SetSmoothingMode(SmoothingModeAntiAlias);
  const float s = static_cast<float>(size) / 32.0f;
  gr.ScaleTransform(s, s);
  LinearGradientBrush bg(PointF(0, 0), PointF(32, 32), Color(col::bg2), Color(col::bg0));
  GraphicsPath p;
  p.AddArc(0.0f, 0.0f, 10.0f, 10.0f, 180, 90);
  p.AddArc(22.0f, 0.0f, 10.0f, 10.0f, 270, 90);
  p.AddArc(22.0f, 22.0f, 10.0f, 10.0f, 0, 90);
  p.AddArc(0.0f, 22.0f, 10.0f, 10.0f, 90, 90);
  gr.FillPath(&bg, &p);
  Chevrons(gr, 6.5f, 7, 18, 0xFFFF921C, 2, 0.35f);
  HICON icon = nullptr;
  b.GetHICON(&icon);
  return icon;
}

bool EncoderClsid(const wchar_t* mime, CLSID* out) {
  UINT n = 0, bytes = 0;
  GetImageEncodersSize(&n, &bytes);
  if (!bytes) return false;
  std::vector<std::uint8_t> buf(bytes);
  auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
  GetImageEncoders(n, bytes, codecs);
  for (UINT i = 0; i < n; ++i)
    if (wcscmp(codecs[i].MimeType, mime) == 0) {
      *out = codecs[i].Clsid;
      return true;
    }
  return false;
}

// --selftest / --screenshot: every screen drawn off-screen at the logical size (scale 1) or 1.5.
int DrawScreens(const fs::path* outDir) {
  struct Screen { const wchar_t* name; bool wizard; Step step; Page page; };
  const Screen screens[] = {
      {L"home", false, Step::Welcome, Page::Home},          {L"settings", false, Step::Welcome, Page::Settings},
      {L"saves", false, Step::Welcome, Page::Saves},        {L"screenshots", false, Step::Welcome, Page::Screenshots},
      {L"setup", false, Step::Welcome, Page::Setup},        {L"about", false, Step::Welcome, Page::About},
      {L"wizard-welcome", true, Step::Welcome, Page::Home}, {L"wizard-disc", true, Step::Disc, Page::Home},
      {L"wizard-copy", true, Step::Copy, Page::Home},       {L"wizard-prepare", true, Step::Prepare, Page::Home},
      {L"wizard-ready", true, Step::Ready, Page::Home},
  };
  CLSID png{};
  if (outDir && !EncoderClsid(L"image/png", &png)) return 1;
  int failures = 0;
  for (const Screen& s : screens) {
    g.wizard = s.wizard;
    g.step = s.step;
    g.page = s.page;
    g.focusRail = false;
    g.focus = -1;
    g.viewer = -1;
    g.job.running = s.step == Step::Copy || s.step == Step::Prepare;  // a job in progress, for the picture
    g.job.job = s.step == Step::Copy ? Job::Copy : Job::Prepare;
    g.job.fraction = 0.42;
    g.job.total = DiscBytes();
    g.job.bytes = static_cast<std::uint64_t>(static_cast<double>(DiscBytes()) * 0.42);
    g.job.started = Clock() - 20;
    g.job.SetStatus(s.step == Step::Copy ? L"" : L"Compiling the game's code for your processor");
    if (s.page == Page::Settings) g.focus = kSetRow;
    for (int pass = 0; pass < 2; ++pass) {  // the second pass draws the focus chosen by the first
      Bitmap b(g.cw, g.ch, PixelFormat32bppARGB);
      Graphics gr(&b);
      PaintAll(gr);
      if (pass == 1 && outDir) b.Save((*outDir / (std::wstring(L"launcher-") + s.name + L".png")).c_str(), &png);
    }
    if (g.widgets.empty()) {
      fwprintf(stderr, L"screen %ls has no controls\n", s.name);
      ++failures;
    }
  }
  g.job.running = false;
  return failures ? 1 : 0;
}

void ReleaseGdiplusObjects() {  // before GdiplusShutdown (the App object outlives it)
  if (g.thumbs.worker.joinable()) g.thumbs.worker.join();
  g.bg.reset();
  g.viewerBmp.reset();
  {
    std::lock_guard l(g.thumbs.m);
    g.thumbs.ready.clear();
    g.thumbs.hero.reset();
  }
  g.f = {};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const bool selftest = argc >= 2 && std::wstring(argv[1]) == L"--selftest";
  const bool screenshot = argc >= 3 && std::wstring(argv[1]) == L"--screenshot";
  HANDLE single = nullptr;
  if (!selftest && !screenshot) {  // one launcher at a time: a second start brings the first to the front
    single = CreateMutexW(nullptr, TRUE, L"HotPursuit2PC.Launcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      if (HWND other = FindWindowW(L"HotPursuit2PC.Launcher", nullptr)) {
        ShowWindow(other, SW_SHOW);
        SetForegroundWindow(other);
      }
      return 0;
    }
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  GdiplusStartup(&token, &gsi, nullptr);
  MakeFonts();

  g.paths = FindPaths();
  g.cfg = LoadConfig(g.paths);
  {
    std::error_code ec;
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g.firstSettings = !fs::exists(fs::path(self).parent_path() / L"hp2native.ini", ec);
  }
  g.set = ui::Load();

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = MakeIcon(32);
  wc.hIconSm = MakeIcon(16);
  wc.lpszClassName = L"HotPursuit2PC.Launcher";
  RegisterClassExW(&wc);
  // Logical 1280x720 at the monitor's scale, kept inside the work area.
  const UINT dpi = GetDpiForSystem();
  float scale = static_cast<float>(dpi) / 96.0f;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  scale = std::min({scale, (work.right - work.left) * 0.92f / kW, (work.bottom - work.top) * 0.92f / kH});
  const int ww = static_cast<int>(kW * scale), wh = static_cast<int>(kH * scale);
  g.wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Hot Pursuit 2 - HotPursuit2PC", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          work.left + ((work.right - work.left) - ww) / 2, work.top + ((work.bottom - work.top) - wh) / 2, ww, wh,
                          nullptr, nullptr, inst, nullptr);
  if (!g.wnd) return 1;
  const MARGINS margins{0, 0, 0, 1};  // the system's shadow around a window without a frame
  DwmExtendFrameIntoClientArea(g.wnd, &margins);
  const int round = 2;                // DWMWCP_ROUND (Windows 11)
  DwmSetWindowAttribute(g.wnd, 33, &round, sizeof round);
  const BOOL darkMode = TRUE;
  DwmSetWindowAttribute(g.wnd, 20, &darkMode, sizeof darkMode);  // DWMWA_USE_IMMERSIVE_DARK_MODE
  SetWindowPos(g.wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  RECT rc{};
  GetClientRect(g.wnd, &rc);
  Resize(rc.right, rc.bottom);
  ReadScreen();

  if (selftest || screenshot) {
    g.cw = static_cast<int>(kW * (screenshot ? 1.5f : 1.0f));
    g.ch = static_cast<int>(kH * (screenshot ? 1.5f : 1.0f));
    g.scale = screenshot ? 1.5f : 1.0f;
    RefreshLists();
    if (g.thumbs.worker.joinable()) g.thumbs.worker.join();
    g.drives = FindDiscDrives();
    g.pkg = Pkg::Ok;
    RefreshDisc();
    const fs::path out = screenshot ? fs::path(argv[2]) : fs::path();
    const int r = DrawScreens(screenshot ? &out : nullptr);
    DestroyWindow(g.wnd);
    ReleaseGdiplusObjects();
    GdiplusShutdown(token);
    return r;
  }

  // Where to start: setup when there is no usable disc yet, else the home screen.
  if (QuickCheck(g.cfg.disc).state != DiscState::Ok) {
    if (QuickCheck(g.paths.copiedDisc).state == DiscState::Ok) {
      g.cfg.disc = g.paths.copiedDisc.wstring();
      SaveConfig(g.paths, g.cfg);
    }
  }
  RefreshDisc();
  if (argc >= 3 && std::wstring(argv[1]) == L"--setup-from") {
    StartWizard(Step::Copy);
    StartCopy(g.job, argv[2], g.paths.copiedDisc, g.wnd, kJobDone);
  } else if (g.disc.state == DiscState::Ok) {
    GoPage(Page::Home, false);
    RefreshLists();
    CheckPackage();
  } else {
    StartWizard(Step::Welcome);
  }
  ShowWindow(g.wnd, show);
  UpdateWindow(g.wnd);
  SetTimer(g.wnd, kTimer, 33, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g.job.cancel = true;
  JoinWorker();
  ReleaseGdiplusObjects();
  GdiplusShutdown(token);
  if (single) CloseHandle(single);
  CoUninitialize();
  return 0;
}
