// HotPursuit2Launcher.exe - the player's front end of the native game (ADR 0016): first-run setup from the user's own
// disc (find, copy, check, prepare), then Play, settings, memory card backups, screenshots, disc & setup, about.
// Drawn in Hot Pursuit 2's own front-end style (hp2_style, shared with the in-game menu): the blueprint grid, steel strips,
// the outlined italic title, menu rows with the orange selection. Mouse, keyboard and an Xbox controller (XInput).
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

#include "hp2_style.h"
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
constexpr float kTitleH = 36;                    // the window's own title bar

enum class Page { Home, Settings, Saves, Screenshots, Setup, About, Count };
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
  Thumbs thumbs;
  std::unique_ptr<Bitmap> bg;  // cached background (grid, strips) of the current size
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
// The look comes from hp2_style (shared with the in-game menu): the game's own front-end style.
using hp2style::Align;
namespace st = hp2style;
namespace hc = hp2style::col;

// The project's mark (the window icon): orange chevrons.
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

// A button in the game's menu style: a band with the label, orange and underlined when it has the focus.
void Button(Graphics& gr, int id, RectF r, const std::wstring& label, int row, int c, float size = 22, bool enabled = true,
            Align a = Align::Center) {
  const bool f = enabled && Add(id, r, row, c);  // a disabled button takes no focus
  st::Row(gr, r, f);
  const RectF t(r.X + 16, r.Y, r.Width - 32, r.Height);
  if (enabled) st::Item(gr, label, t, size, f, a);
  else st::Outlined(gr, label, t, st::ItemFamily(), FontStyleBold, size, hc::faint, a, 0.0f, 0.0f);
}

// A small orange heading (like the game's section labels).
void Section(Graphics& gr, const std::wstring& s, float x, float y, float w) {
  st::Outlined(gr, s, RectF(x, y, w, 24), st::ItemFamily(), FontStyleBold, 17, hc::orange, Align::Left, 0.0f, 2.0f);
}
// Running text.
void Body(Graphics& gr, const std::wstring& s, RectF r, ARGB c = hc::text, float size = 16, bool wrap = true) {
  st::Plain(gr, s, r, size, c, Align::Left, wrap);
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
// Layout (logical 1280x720), after the game's main menu: title strip with the page title, red/white rules, the page
// menu on the left, a translucent panel for the page, the ticker strip and the button hints at the bottom.
constexpr float kStripY = 62, kStripH = 46, kRulesY = 114, kPanelX = 280, kPanelY = 140, kPanelR = 1252, kPanelB = 632;
constexpr float kTickerY = 642, kTickerH = 30;
constexpr float kIn0 = 304, kIn1 = 1228;  // inside the panel

void PaintBackground(Graphics& gr) {
  st::Grid(gr, RectF(0, 0, kW, kH), 40);
  st::SteelStrip(gr, RectF(0, kStripY, kW, kStripH));
  st::Rules(gr, 0, kW, kRulesY, 250);
  st::SteelStrip(gr, RectF(250, kTickerY, kW - 250, kTickerH), 0.95f);
}

// --------------------------------------------------------------------------------------------------- chrome ----
const wchar_t* kPageTitles[] = {L"Hot Pursuit 2", L"Settings", L"Memory Card", L"Screenshots", L"Disc & Setup", L"About"};

void PaintTitleBar(Graphics& gr) {
  SolidBrush dark(Color(0xE6030507));
  gr.FillRectangle(&dark, 0.0f, 0.0f, kW, kTitleH);
  Chevrons(gr, 14, 9, 15, 0xFFE47C08, 2, 0.35f);
  st::Outlined(gr, L"HotPursuit2PC", RectF(42, 0, 200, kTitleH), st::ItemFamily(), FontStyleBold, 17, hc::silver, Align::Left, 0.0f, 0.0f);
  st::Plain(gr, std::wstring(L"v") + kVersion, RectF(152, 0, 200, kTitleH), 13, hc::faint);
  const float bw = 46;
  for (int i = 0; i < 2; ++i) {
    const float x = kW - bw * (2 - i);
    if (g.hoverTitle == i + 1) {
      SolidBrush hb(Color(i == 1 ? 0xFFB3141C : 0x40FFFFFF));
      gr.FillRectangle(&hb, x, 0.0f, bw, kTitleH);
    }
    Pen p(Color(hc::silver), 1.3f);
    const float cx = x + bw / 2, cy = kTitleH / 2;
    if (i == 0) gr.DrawLine(&p, cx - 5, cy, cx + 5, cy);
    else {
      gr.DrawLine(&p, cx - 5, cy - 5, cx + 5, cy + 5);
      gr.DrawLine(&p, cx + 5, cy - 5, cx - 5, cy + 5);
    }
  }
}

void PaintPageTitle(Graphics& gr, const std::wstring& title) { st::Title(gr, title, 300, kStripY - 22, 62); }

void PaintRail(Graphics& gr) {
  const wchar_t* names[] = {L"Play", L"Settings", L"Memory Card", L"Screenshots", L"Disc & Setup", L"About"};
  for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
    const RectF r(18, kPanelY + 8 + i * 54.0f, 240, 46);
    const bool sel = static_cast<int>(g.page) == i;
    const bool f = Add(kRail + i, r, i, -1);
    st::Row(gr, r, sel || f);  // the page shown keeps its band; only the focus is orange (as in the game, one at a time)
    const RectF t(r.X + 16, r.Y, r.Width - 24, r.Height);
    if (f) st::Item(gr, names[i], t, 25, true);
    else st::Outlined(gr, names[i], t, st::ItemFamily(), FontStyleBold, 25, sel ? hc::silver : hc::text, Align::Left);
  }
}

void PaintTicker(Graphics& gr) {
  std::wstring s;
  if (g.job.running && g.job.job != Job::CheckPackage) s = g.job.Status();
  else if (g.wizard) s = L"Your disc is only read. Nothing is downloaded and nothing leaves this PC.";
  else if (g.pkg == Pkg::Checking) s = L"Checking the game files...";
  else if (g.pkg == Pkg::Stale || g.pkg == Pkg::Missing) s = L"The game needs preparing (about a minute) - press Play.";
  else s = L"Ready to race! In the game: F1 settings, F12 screenshot, Alt+Enter full screen.";
  st::Outlined(gr, s, RectF(270, kTickerY, kW - 300, kTickerH), st::ItemFamily(), FontStyleBold, 19, hc::silver, Align::Left, 0.2f, 2.4f);
}

void Hint(Graphics& gr, float& x, float y, const std::wstring& key, const std::wstring& what, ARGB ring) {
  const bool round = g.controller && key.size() <= 2;
  const float kw = round ? 26 : std::max(30.0f, st::PlainWidth(gr, key, 12, true) + 18);
  st::Glyph(gr, RectF(x, y, kw, 26), key, round, ring);
  x += kw + 8;
  const float tw = st::PlainWidth(gr, what, 15);
  st::Plain(gr, what, RectF(x, y, tw + 20, 26), 15, hc::text);
  x += tw + 24;
}

void PaintHints(Graphics& gr) {
  float x = 640;
  const float y = 684;
  if (g.controller) {
    Hint(gr, x, y, L"A", L"Select", 0xFF6BBF59);
    Hint(gr, x, y, L"B", L"Back", 0xFFD9453A);
    if (!g.wizard) Hint(gr, x, y, L"LB RB", L"Pages", 0xFF8F959B);
    if (!g.wizard) Hint(gr, x, y, L"START", L"Play", 0xFF8F959B);
  } else {
    Hint(gr, x, y, L"ENTER", L"Select", 0xFF8F959B);
    Hint(gr, x, y, L"ESC", L"Back", 0xFF8F959B);
    Hint(gr, x, y, L"← →", L"Change", 0xFF8F959B);
    if (!g.wizard) Hint(gr, x, y, L"PGUP PGDN", L"Pages", 0xFF8F959B);
  }
}

void PaintToast(Graphics& gr) {
  const double age = Clock() - g.toastAt;
  if (g.toast.empty() || age > 3.5) return;
  const float a = static_cast<float>(age > 3.0 ? (3.5 - age) / 0.5 : 1.0);
  const float w = std::min(600.0f, st::PlainWidth(gr, g.toast, 16) + 48), h = 40, x = kPanelR - w - 16, y = kPanelB - h - 14;
  SolidBrush b(Color(static_cast<BYTE>(235 * a), 30, 30, 30));
  gr.FillRectangle(&b, x, y, w, h);
  SolidBrush o(Color(static_cast<BYTE>(255 * a), 0xE4, 0x7C, 0x08));
  gr.FillRectangle(&o, x, y + h - 2, w, 2.0f);
  st::Plain(gr, g.toast, RectF(x + 24, y, w - 30, h), 16, Color::MakeARGB(static_cast<BYTE>(255 * a), 0xC9, 0xCB, 0xCD));
}

// ----------------------------------------------------------------------------------------------------- pages ----
void HeroInPanel(Graphics& gr) {  // the player's newest screenshot inside the panel, like the game's video panel
  std::lock_guard l(g.thumbs.m);
  if (!g.thumbs.hero) return;
  const RectF r(kPanelX, kPanelY, kPanelR - kPanelX, kPanelB - kPanelY);
  const GraphicsState s = gr.Save();
  gr.SetClip(r);
  DrawCover(gr, g.thumbs.hero.get(), r);
  LinearGradientBrush shade(PointF(r.X, 0), PointF(r.GetRight(), 0), Color(225, 5, 8, 11), Color(70, 5, 8, 11));
  gr.FillRectangle(&shade, r);
  gr.Restore(s);
}

void PaintHome(Graphics& gr) {
  HeroInPanel(gr);
  const bool preparing = JobIs(Job::Prepare);
  const bool needPrep = g.pkg == Pkg::Missing || g.pkg == Pkg::Stale;
  const RectF play(kIn0, 166, 600, 96);
  const bool f = Add(kPlay, play, 0, 0);
  st::Row(gr, play, f);
  if (preparing) {
    st::Item(gr, L"Preparing...", RectF(play.X + 24, play.Y + 4, 560, 56), 44, true);
    st::Progress(gr, RectF(play.X + 24, play.Y + 64, 540, 16), g.job.fraction);
  } else {
    st::Outlined(gr, needPrep ? L"Prepare & Play" : L"Play", RectF(play.X + 24, play.Y, 560, play.Height), st::ItemFamily(),
                 FontStyleBold, needPrep ? 52.0f : 66.0f, f ? hc::orange : hc::text, Align::Left, 0.12f);
    Chevrons(gr, play.GetRight() - 96, play.Y + 30, 36, f ? hc::orange : 0xFF8F959B, 3, 0.3f);
  }
  Button(gr, kHomeSettings, RectF(kIn0, 276, 290, 44), L"Settings", 1, 0, 23, true, Align::Left);
  Button(gr, kHomeFolder, RectF(kIn0 + 310, 276, 290, 44), L"Game Folder", 1, 1, 23, true, Align::Left);
  const std::wstring stats = g.cfg.sessions ? L"Last played " + FormatAgo(g.cfg.lastPlayed) + L"   ·   " +
                                                  FormatDuration(g.cfg.playedSeconds) + L" behind the wheel"
                                            : L"Not played yet - have a good race!";
  Body(gr, stats, RectF(kIn0 + 4, 330, 600, 26), hc::dim, 16, false);

  // status, in cells like the game's music list
  Section(gr, L"Status", kIn0, 388, 300);
  float y = 416;
  auto cell = [&](const std::wstring& s, int ok) {  // ok: 1 tick, 0 cross, -1 none
    st::Cell(gr, RectF(kIn0, y, 360, 34), false);
    if (ok >= 0) st::Mark(gr, kIn0 + 10, y + 8, 18, ok == 1);
    st::Plain(gr, s, RectF(kIn0 + (ok >= 0 ? 38 : 14), y, 316, 34), 16, hc::text);
    y += 36;
  };
  cell(g.cfg.verified ? L"Disc checked" : L"Disc found", 1);
  if (g.pkg == Pkg::Ok) cell(L"Game prepared", 1);
  else if (g.pkg == Pkg::Checking || g.pkg == Pkg::Unknown) cell(L"Checking the game...", -1);
  else cell(g.pkg == Pkg::Missing ? L"Not prepared yet" : L"New version: prepare again", 0);
  cell(std::wstring(kQuality[g.set.resolution]) + L"  ·  " + (g.set.frameRate ? L"60" : L"30") + L" fps  ·  " +
           ui::Setting(6).names[g.set.aspect], -1);

  const float rx = kIn0 + 420, rw = kIn1 - rx;
  if (!g.problem.empty()) {
    Section(gr, L"Something went wrong", rx, 388, rw);
    st::Cell(gr, RectF(rx, 416, rw, 70), false);
    st::Mark(gr, rx + 10, 428, 18, false);
    st::Plain(gr, g.problem, RectF(rx + 38, 420, rw - 50, 62), 15, hc::text, Align::Left, true);
    Button(gr, kHomeLog, RectF(rx, 498, 220, 40), L"Open Log", 2, 0, 20);
    Button(gr, kHomeReport, RectF(rx + 236, 498, 250, 40), L"Report a Problem", 2, 1, 20);
  } else {
    Section(gr, L"In the game", rx, 388, rw);
    const wchar_t* keys[][2] = {{L"F1", L"Settings and Quit game"}, {L"F12", L"Screenshot"}, {L"Alt+Enter", L"Full screen"},
                                {L"Select + Start", L"Settings (controller)"}};
    float ky = 416;
    for (const auto& k : keys) {
      st::Cell(gr, RectF(rx, ky, rw, 34), false);
      st::Item(gr, k[0], RectF(rx + 14, ky, 150, 34), 18, false);
      st::Plain(gr, k[1], RectF(rx + 170, ky, rw - 180, 34), 16, hc::text);
      ky += 36;
    }
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

std::wstring RowLabel(int r) {
  return kRows[r].setting == 1 ? L"Picture Quality" : kRows[r].setting < 0 ? L"After the Game Closes" : ui::Setting(kRows[r].setting).label;
}

void PaintSettings(Graphics& gr) {
  const float x = kIn0, w = 590, rh = 37;
  float y = 154;
  int focusedRow = -1;
  for (int r = 0; r < kRowCount; ++r) {
    if (r == 0 || r == kRowCount - 1) {
      Section(gr, r == 0 ? L"Picture" : L"Launcher", x, y, 300);
      y += 26;
    }
    const RectF row(x, y, w, rh - 3);
    const bool f = Add(kSetRow + r, row, r, 0, true);
    if (f) focusedRow = r;
    st::Row(gr, row, f);
    st::Item(gr, RowLabel(r), RectF(x + 14, y, 250, rh - 3), 21, f);
    int count = 0;
    const int v = RowValue(r, nullptr, &count);
    std::wstring value = kRows[r].setting == 1   ? kQuality[v]
                         : kRows[r].setting < 0 ? (v ? L"Close the launcher" : L"Show the launcher")
                                                : ui::Setting(kRows[r].setting).names[v];
    if (f) value = L"‹  " + value + L"  ›";
    st::Item(gr, value, RectF(x + 270, y, w - 286, rh - 3), 21, f, Align::Right);
    if (kRows[r].setting == 1 && v == g.recommended)
      st::Outlined(gr, L"Recommended", RectF(x + w + 12, y, 140, rh - 3), st::ItemFamily(), FontStyleBold, 15, hc::orangeHi,
                   Align::Left, 0.0f, 0.0f);
    y += rh;
  }
  Button(gr, kSetReset, RectF(x, y + 8, 260, 36), L"Reset to Defaults", kRowCount, 0, 20);
  // the focused row's explanation
  const float hx = x + w + 30, hw = kIn1 - hx;
  if (focusedRow >= 0) {
    Section(gr, RowLabel(focusedRow), hx, 182, hw);
    std::wstring help = kRows[focusedRow].help;
    if (kRows[focusedRow].setting == 1) help += L"\n\nRecommended: " + Recommendation() + L".";
    Body(gr, help, RectF(hx, 212, hw, 300), hc::text, 15.5f);
  } else {
    Body(gr, L"Pick a setting to see what it does.", RectF(hx, 182, hw, 60), hc::dim, 15.5f);
  }
  Body(gr, L"Saved in hp2native.ini in the game folder. In the game, F1 changes them while you play.", RectF(hx, 540, hw, 60),
       hc::faint, 14);
}

void PaintSaves(Graphics& gr) {
  const float lw = 430;
  Section(gr, L"On the Card", kIn0, 154, lw);
  float y = 182;
  if (g.saves.empty())
    Body(gr, L"Nothing yet. Saves show up here once the game has saved (career progress, settings).", RectF(kIn0, y, lw, 60), hc::dim);
  for (std::size_t i = 0; i < g.saves.size() && i < 9; ++i, y += 40) {
    const SaveEntry& s = g.saves[i];
    st::Cell(gr, RectF(kIn0, y, lw, 38), false);
    st::Plain(gr, s.name, RectF(kIn0 + 14, y, lw - 180, 38), 16, hc::text, Align::Left, false, true);
    st::Plain(gr, FormatBytes(s.bytes) + L" · " + FormatAgo(s.time), RectF(kIn0 + lw - 180, y, 166, 38), 14, hc::dim, Align::Right);
  }
  Button(gr, kSaveBackup, RectF(kIn0, 576, 200, 40), L"Back Up Now", 0, 0, 21, !g.saves.empty());
  Button(gr, kSaveFolder, RectF(kIn0 + 216, 576, 214, 40), L"Open Folder", 0, 1, 21);

  const float bx = kIn0 + lw + 40, bw = kIn1 - bx;
  Section(gr, L"Backups", bx, 154, bw);
  y = 182;
  if (g.backups.empty())
    Body(gr, L"No backups yet. A backup is a copy of the whole card, and restoring one keeps the current card as a backup "
             L"too - so nothing gets lost.", RectF(bx, y, bw, 80), hc::dim);
  for (std::size_t i = 0; i < g.backups.size() && i < 10; ++i, y += 40) {
    const int id = kSaveRow + static_cast<int>(i);
    const RectF r(bx, y, bw, 38);
    const bool f = Add(id, r, static_cast<int>(i) + 1, 0);
    st::Cell(gr, r, f);
    st::Item(gr, g.backups[i].name, RectF(bx + 14, y, bw - 250, 38), 18, f);
    st::Plain(gr, f ? (g.armed == id ? L"Press again to restore" : L"Restore") : FormatBytes(g.backups[i].bytes),
              RectF(bx + bw - 230, y, 216, 38), 15, f ? (g.armed == id ? hc::orangeHi : hc::text) : hc::dim, Align::Right, false, f);
  }
}

void PaintScreenshots(Graphics& gr) {
  Button(gr, kShotFolder, RectF(kIn1 - 200, 152, 200, 38), L"Open Folder", 0, 0, 20);
  if (g.shots.empty()) {
    Section(gr, L"No screenshots yet", kIn0, 154, 600);
    Body(gr, L"Press F12 while you play. Your newest one also shows up on the Play page.", RectF(kIn0, 184, 640, 60), hc::dim);
    return;
  }
  Body(gr, L"Taken with F12 in the game.", RectF(kIn0, 156, 600, 30), hc::dim, 16, false);
  const int cols = 4, visibleRows = 3;
  const float tw = 212, th = 119.25f, gapX = 16, top = 200, stepY = th + 22;
  std::lock_guard l(g.thumbs.m);
  for (int i = 0; i < static_cast<int>(g.shots.size()); ++i) {
    const int row = i / cols, c = i % cols;
    if (row < g.shotScroll || row >= g.shotScroll + visibleRows) {
      g.widgets.push_back({kShot + i, RectF(-1000, -1000, 1, 1), row + 1, c, false});  // reachable by keys
      continue;
    }
    const float x = kIn0 + c * (tw + gapX), y = top + (row - g.shotScroll) * stepY;
    const bool f = Add(kShot + i, RectF(x, y, tw, th), row + 1, c);
    auto it = g.thumbs.ready.find(g.shots[i].wstring());
    if (it != g.thumbs.ready.end()) gr.DrawImage(it->second.get(), RectF(x, y, tw, th));
    else st::Cell(gr, RectF(x, y, tw, th), false);
    Pen border(Color(f ? hc::orange : 0x66000000), f ? 3.0f : 1.0f);
    gr.DrawRectangle(&border, x, y, tw, th);
    if (f) {
      std::wstring name = g.shots[i].stem().wstring();
      if (name.rfind(L"Hot Pursuit 2 ", 0) == 0) name = name.substr(14);
      st::Cell(gr, RectF(x, y + th - 24, tw, 24), true);
      st::Plain(gr, name, RectF(x + 8, y + th - 24, tw - 12, 24), 13, hc::text);
    }
  }
  const int rows = (static_cast<int>(g.shots.size()) + cols - 1) / cols;
  if (rows > visibleRows)
    st::Plain(gr, L"Row " + std::to_wstring(g.shotScroll + 1) + L"-" + std::to_wstring(std::min(rows, g.shotScroll + visibleRows)) +
                  L" of " + std::to_wstring(rows),
              RectF(kIn0, 600, 300, 24), 14, hc::faint);
}

void PaintViewer(Graphics& gr) {
  SolidBrush dark(Color(0xF0030507));
  gr.FillRectangle(&dark, 0.0f, kTitleH, kW, kH - kTitleH);
  const fs::path& p = g.shots[static_cast<std::size_t>(g.viewer)];
  if (g.viewerPath != p.wstring()) {
    g.viewerBmp = std::make_unique<Bitmap>(p.c_str());
    g.viewerPath = p.wstring();
  }
  Bitmap* full = g.viewerBmp.get();
  if (full && full->GetLastStatus() == Ok && full->GetWidth()) {
    const float sw = static_cast<float>(full->GetWidth()), sh = static_cast<float>(full->GetHeight());
    const float s = std::min(1200 / sw, 610 / sh);
    gr.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    gr.DrawImage(full, RectF((kW - sw * s) / 2, 48, sw * s, sh * s));
  }
  st::Plain(gr, p.filename().wstring() + L"   ·   " + std::to_wstring(g.viewer + 1) + L" of " + std::to_wstring(g.shots.size()) +
                L"   ·   ← → browse   ·   Esc close",
            RectF(40, 672, kW - 80, 32), 15, hc::dim, Align::Center);
}

void PaintJob(Graphics& gr, float x, float y, float w) {  // a running job: bar and status
  st::Progress(gr, RectF(x, y, w, 20), g.job.fraction);
  std::wstring s = g.job.Status();
  if (g.job.job == Job::Copy && g.job.total) {
    const double el = std::max(0.5, Clock() - g.job.started);
    const double rate = static_cast<double>(g.job.bytes) / el;
    wchar_t b[160];
    swprintf(b, 160, L"%ls of %ls  ·  %.0f MB/s", FormatBytes(g.job.bytes).c_str(), FormatBytes(g.job.total).c_str(), rate / 1048576.0);
    s = b;
    if (rate > 0 && el > 3) s += L"  ·  about " + FormatDuration(static_cast<std::int64_t>((g.job.total - g.job.bytes) / rate) + 30) + L" left";
  }
  st::Plain(gr, s, RectF(x, y + 28, w - 120, 26), 15, hc::dim);
  wchar_t pct[16];
  swprintf(pct, 16, L"%.0f%%", g.job.fraction * 100);
  st::Item(gr, pct, RectF(x + w - 120, y + 26, 120, 28), 22, false, Align::Right);
}

void PaintSetup(Graphics& gr) {
  float y = 154;
  auto row = [&](const std::wstring& label, const std::wstring& value, int ok) {  // ok: 1 tick, 0 cross, -1 none
    st::Cell(gr, RectF(kIn0, y, kIn1 - kIn0, 40), false);
    st::Item(gr, label, RectF(kIn0 + 14, y, 200, 40), 19, false);
    if (ok >= 0) st::Mark(gr, kIn0 + 214, y + 10, 20, ok == 1);
    st::Plain(gr, value, RectF(kIn0 + (ok >= 0 ? 244 : 214), y, kIn1 - kIn0 - 260, 40), 16, hc::text);
    y += 43;
  };
  const DiscCheck& dc = g.disc;
  row(L"Disc Folder", g.cfg.disc.empty() ? L"(none)" : g.cfg.disc, dc.state == DiscState::Ok ? 1 : 0);
  row(L"Edition", dc.state == DiscState::Ok ? L"North America, SLUS-20362 (supported)" : dc.detail, dc.state == DiscState::Ok ? 1 : 0);
  row(L"Disc Files", g.cfg.verified ? L"All files match the original disc (checked " + FormatAgo(g.cfg.verified) + L")"
                                    : L"Not checked file by file yet", g.cfg.verified ? 1 : -1);
  std::error_code ec;
  const bool hasPkg = fs::exists(g.paths.package, ec);
  std::wstring pkg = g.pkg == Pkg::Ok ? L"Prepared for this version" : g.pkg == Pkg::Checking ? L"Checking..."
                     : hasPkg ? L"Made by another version: prepare again" : L"Not prepared yet";
  if (hasPkg && g.pkg == Pkg::Ok) pkg += L" (" + FormatBytes(fs::file_size(g.paths.package, ec)) + L")";
  row(L"Game", pkg, g.pkg == Pkg::Ok ? 1 : g.pkg == Pkg::Checking ? -1 : 0);
  row(L"Game Folder", g.paths.dir.wstring(), -1);
  row(L"Recompiler", g.paths.recomp.empty() ? L"hp2recomp.exe not found" : g.paths.recomp.filename().wstring(), g.paths.recomp.empty() ? 0 : 1);
  const bool busy = g.job.running && g.job.job != Job::CheckPackage;
  if (busy) {
    PaintJob(gr, kIn0, 440, 640);
    Button(gr, kJobCancel, RectF(kIn0 + 680, 432, 180, 40), L"Cancel", 0, 0, 21);
  } else {
    Button(gr, kSetupCheck, RectF(kIn0, 440, 290, 42), L"Check All Files", 0, 0, 21, dc.state == DiscState::Ok);
    Button(gr, kSetupPrepare, RectF(kIn0 + 306, 440, 290, 42), L"Prepare Again", 0, 1, 21, dc.state == DiscState::Ok);
    Button(gr, kSetupDisc, RectF(kIn0 + 612, 440, 290, 42), L"Change Disc...", 0, 2, 21);
    Button(gr, kSetupFolder, RectF(kIn0, 494, 290, 42), L"Open Game Folder", 1, 0, 21);
    Button(gr, kSetupShortcut, RectF(kIn0 + 306, 494, 290, 42), L"Desktop Shortcut", 1, 1, 21);
  }
  Body(gr, L"Your disc is only ever read. Nothing is downloaded and nothing leaves this PC.", RectF(kIn0, 580, 800, 30), hc::faint, 14, false);
}

void PaintAbout(Graphics& gr) {
  const float w = kIn1 - kIn0;
  float y = 150;
  auto block = [&](const wchar_t* title, const wchar_t* body, float h) {
    Section(gr, title, kIn0, y, w);
    Body(gr, body, RectF(kIn0, y + 26, w, h), hc::text, 15);
    y += h + 34;
  };
  block(L"What this is",
        L"HotPursuit2PC runs Need for Speed: Hot Pursuit 2 (PlayStation 2, 2002) natively on Windows. The game's program on "
        L"your own disc is translated into PC code right on your computer: no emulator, and no game files come with it.",
        44);
  block(L"Made with AI",
        L"I started this project to get hands-on with building software together with AI. Almost all of the code was written "
        L"by an AI coding agent (Claude, in Claude Code) under my direction. AI isn't going away - it's a tool, and a pretty "
        L"good one for bringing the games of my childhood to the PC. More in the project's README.",
        64);
  block(L"Credits",
        L"Widescreen and 60 fps values from the PCSX2 community patches by nemesis2000, Aero_ and asasega - thank you! Code "
        L"generation by LLVM (Apache 2.0 with LLVM exceptions). The Play! emulator (BSD 2-Clause) was a reference during "
        L"development; none of it is in the native game.",
        64);
  block(L"Legal",
        L"Not affiliated with, sponsored by or endorsed by Electronic Arts or Sony. Need for Speed and Hot Pursuit 2 are "
        L"trademarks of Electronic Arts Inc.; PlayStation is a trademark of Sony Interactive Entertainment.",
        44);
  Button(gr, kAboutProject, RectF(kIn0, 580, 214, 40), L"Project Page", 0, 0, 20);
  Button(gr, kAboutGuide, RectF(kIn0 + 230, 580, 214, 40), L"Getting Started", 0, 1, 20);
  Button(gr, kAboutReport, RectF(kIn0 + 460, 580, 230, 40), L"Report a Problem", 0, 2, 20);
  Button(gr, kAboutLog, RectF(kIn0 + 706, 580, 214, 40), L"Open Log", 0, 3, 20);
  st::Plain(gr, std::wstring(L"HotPursuit2PC ") + kVersion + L"  ·  MIT license", RectF(kIn0, 548, 400, 24), 14, hc::faint);
}

// --------------------------------------------------------------------------------------------------- the wizard ----
void PaintWizard(Graphics& gr) {
  const float x0 = 150, x1 = 1130;
  const wchar_t* steps[] = {L"Disc", L"Check", L"Prepare", L"Ready"};
  const int cur = g.step == Step::Welcome ? -1 : g.step == Step::Disc || g.step == Step::Copy ? 0 : g.step == Step::Check ? 1
                  : g.step == Step::Prepare ? 2 : 3;
  for (int i = 0; i < 4; ++i) {
    const RectF r(x0 + i * 248.0f, kPanelY, 236, 36);
    st::Cell(gr, r, i == cur);
    st::Outlined(gr, std::to_wstring(i + 1) + L"  " + steps[i], RectF(r.X + 16, r.Y, r.Width - 20, r.Height), st::ItemFamily(),
                 FontStyleBold, 20, i == cur ? hc::orange : i < cur ? hc::text : hc::faint, Align::Left, 0.0f, 2.0f);
  }
  st::Panel(gr, RectF(x0 - 20, 190, x1 - x0 + 40, kPanelB - 190));
  const float ty = 210;
  auto heading = [&](const std::wstring& t, const std::wstring& sub) {
    st::Outlined(gr, t, RectF(x0, ty, x1 - x0, 50), st::ItemFamily(), FontStyleBold, 40, hc::text, Align::Left, 0.12f);
    Body(gr, sub, RectF(x0 + 2, ty + 56, x1 - x0 - 120, 60), hc::dim, 16.5f);
  };
  switch (g.step) {
    case Step::Welcome:
      st::Title(gr, L"Welcome!", x0, ty - 6, 64);
      Body(gr, L"Let's get Hot Pursuit 2 ready to race on this PC. It takes a few minutes, just once.\n\nYou'll need your own "
               L"disc of the North American release (SLUS-20362) - in a DVD drive, or as an .iso image of your disc. Its files "
               L"are copied and checked, then the game's program is translated for your processor. Nothing is downloaded.",
           RectF(x0 + 2, ty + 90, 760, 160), hc::text, 17);
      Button(gr, kWizStart, RectF(x0, 500, 320, 60), L"Let's Go", 0, 0, 34, true, Align::Left);
      break;
    case Step::Disc: {
      heading(L"Your Disc", L"Insert your disc, or double-click your .iso file in File Explorer so Windows shows it as a drive. "
                            L"It shows up here by itself.");
      float y = 326;
      int row = 0;
      for (std::size_t i = 0; i < g.drives.size() && i < 3; ++i, y += 72, ++row) {
        const DiscDrive& d = g.drives[i];
        const RectF r(x0, y, 760, 64);
        const bool f = Add(kWizDrive + static_cast<int>(i), r, row, 0);
        st::Row(gr, r, f);
        st::Item(gr, L"Copy from " + d.root.substr(0, 2) + (d.label.empty() ? L"" : L"  " + d.label), RectF(x0 + 18, y + 2, 720, 34), 26, f);
        st::Plain(gr, std::wstring(d.optical ? L"Your disc or a mounted .iso" : L"A removable drive") + L"  ·  copies " +
                          FormatBytes(DiscBytes()) + L" to the game folder (recommended)",
                  RectF(x0 + 20, y + 34, 720, 26), 14.5f, hc::dim);
      }
      if (g.drives.empty()) {
        st::Cell(gr, RectF(x0, y, 760, 56), false);
        st::Plain(gr, L"Looking for the disc in your drives...", RectF(x0 + 18, y, 700, 56), 16, hc::dim);
        y += 70;
      }
      Button(gr, kWizFolder, RectF(x0, y + 6, 520, 46), L"Choose a Folder With the Disc's Files...", row, 0, 22, true, Align::Left);
      if (!g.discError.empty()) {
        st::Mark(gr, x0, y + 70, 20, false);
        Body(gr, g.discError, RectF(x0 + 30, y + 68, 900, 60), hc::bad, 16);
      }
      break;
    }
    case Step::Copy:
      heading(L"Copying Your Disc", L"The files go to the game folder, so the disc doesn't have to stay in the drive.");
      PaintJob(gr, x0, 320, 760);
      Button(gr, kJobCancel, RectF(x0, 400, 180, 42), L"Cancel", 0, 0, 22);
      break;
    case Step::Check:
      heading(L"Checking the Files", L"Every file is compared with the original disc, so the game runs exactly as it should.");
      PaintJob(gr, x0, 320, 760);
      Button(gr, kJobCancel, RectF(x0, 400, 180, 42), L"Cancel", 0, 0, 22);
      break;
    case Step::Prepare:
      heading(L"Preparing the Game", L"The game's program is translated into code for your processor. This takes about a "
                                     L"minute and only happens again after an update.");
      if (g.job.running) {
        PaintJob(gr, x0, 320, 760);
        Button(gr, kJobCancel, RectF(x0, 400, 180, 42), L"Cancel", 0, 0, 22);
      } else {
        st::Mark(gr, x0, 324, 20, false);
        Body(gr, g.job.Error(), RectF(x0 + 30, 320, 900, 60), hc::bad, 16);
        Button(gr, kWizRetry, RectF(x0, 400, 220, 44), L"Try Again", 0, 0, 24);
      }
      break;
    case Step::Ready: {
      heading(L"Ready to Race!", L"Everything is in place. You can change any of this later in the launcher.");
      float y = 310;
      auto line = [&](const std::wstring& s) {
        st::Cell(gr, RectF(x0, y, 760, 36), false);
        st::Mark(gr, x0 + 10, y + 8, 20, true);
        st::Plain(gr, s, RectF(x0 + 42, y, 700, 36), 16, hc::text);
        y += 38;
      };
      line(L"Disc checked: North America, SLUS-20362");
      line(L"Game prepared for your processor");
      line(L"Picture quality: " + std::wstring(kQuality[g.set.resolution]) + (g.set.frameRate ? L", 60 fps, " : L", 30 fps, ") +
           ui::Setting(6).names[g.set.aspect] + (g.set.fullscreen ? L", full screen" : L", in a window"));
      Button(gr, kWizPlay, RectF(x0, 452, 300, 60), L"Play Now", 0, 0, 34, true, Align::Left);
      Button(gr, kWizLauncher, RectF(x0 + 320, 460, 300, 46), L"Open the Launcher", 0, 1, 22, true, Align::Left);
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
  // background (grid, strips, rules): drawn once per window size
  if (!g.bg || static_cast<int>(g.bg->GetWidth()) != g.cw || static_cast<int>(g.bg->GetHeight()) != g.ch) {
    g.bg = std::make_unique<Bitmap>(std::max(1, g.cw), std::max(1, g.ch), PixelFormat32bppPARGB);
    Graphics bgr(g.bg.get());
    bgr.SetSmoothingMode(SmoothingModeAntiAlias);
    bgr.SetPixelOffsetMode(PixelOffsetModeHalf);
    bgr.ScaleTransform(g.scale, g.scale);
    PaintBackground(bgr);
  }
  gr.DrawImage(g.bg.get(), Rect(0, 0, g.cw, g.ch));
  gr.ScaleTransform(g.scale, g.scale);
  if (g.wizard) {
    PaintPageTitle(gr, L"Setup");
    PaintWizard(gr);
  } else {
    PaintPageTitle(gr, kPageTitles[static_cast<int>(g.page)]);
    st::Panel(gr, RectF(kPanelX, kPanelY, kPanelR - kPanelX, kPanelB - kPanelY));
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
  PaintTicker(gr);
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

HICON MakeIcon(int size) {  // orange chevrons on a steel tile, drawn (no image files in the repository)
  Bitmap b(size, size, PixelFormat32bppARGB);
  Graphics gr(&b);
  gr.SetSmoothingMode(SmoothingModeAntiAlias);
  const float s = static_cast<float>(size) / 32.0f;
  gr.ScaleTransform(s, s);
  LinearGradientBrush bg(PointF(0, 0), PointF(32, 32), Color(hc::steelHi), Color(hc::ink));
  GraphicsPath p;
  p.AddArc(0.0f, 0.0f, 10.0f, 10.0f, 180, 90);
  p.AddArc(22.0f, 0.0f, 10.0f, 10.0f, 270, 90);
  p.AddArc(22.0f, 22.0f, 10.0f, 10.0f, 0, 90);
  p.AddArc(0.0f, 22.0f, 10.0f, 10.0f, 90, 90);
  gr.FillPath(&bg, &p);
  Chevrons(gr, 6.5f, 7, 18, hc::orange, 2, 0.35f);
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
  if (outDir)
    if (FILE* f = _wfopen((*outDir / L"launcher-fonts.txt").c_str(), L"w, ccs=UTF-8")) {  // which fonts this PC provided
      fwprintf(f, L"%ls\n", hp2style::FamilyNames().c_str());
      fclose(f);
    }
  return failures ? 1 : 0;
}

void ReleaseGdiplusObjects() {  // the launcher's own pictures, before the process ends
  if (g.thumbs.worker.joinable()) g.thumbs.worker.join();
  g.bg.reset();
  g.viewerBmp.reset();
  {
    std::lock_guard l(g.thumbs.m);
    g.thumbs.ready.clear();
    g.thumbs.hero.reset();
  }
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
  hp2style::Startup();

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
  if (single) CloseHandle(single);
  CoUninitialize();
  return 0;
}
