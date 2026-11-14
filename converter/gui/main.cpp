// HotPursuit2PC.exe - the converter's GUI (ADR 0003): one Win32 window over
// the same hp2core calls the CLI uses. No conversion logic lives here.
//
// Developer switches:
//   --selftest                         create the window, check its controls, exit 0/1
//   --selftest-convert <rom> <game>    run a real conversion through the window, exit 0/1/77
//   --screenshot <bmp>                 show the window, save a picture of it, exit
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hp2/convert.h"
#ifdef HP2_HAVE_TRANSLATOR
#include "code_builder.h"
#endif

using namespace hp2;

namespace {

enum : int {
  IDC_ROM_EDIT = 101,
  IDC_ROM_BROWSE,
  IDC_PORTABLE,
  IDC_NAME_EDIT,
  IDC_INSTALLED,
  IDC_DIR_EDIT,
  IDC_DIR_BROWSE,
  IDC_KEEP_ROM,
  IDC_FULL,
  IDC_REPAIR,
  IDC_STATUS,
  IDC_PROGRESS,
  IDC_PROGRESS_TEXT,
  IDC_LOG,
  IDC_START,
  IDC_CANCEL,
};

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;
constexpr wchar_t kClassName[] = L"HotPursuit2PC.Converter";
constexpr int kClientW = 700, kClientH = 680;  // at 96 DPI

struct ProgressMsg {
  std::wstring stage, item;
  std::uint64_t done = 0, total = 0;
};

struct App {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  HWND rom_edit = nullptr, rom_browse = nullptr;
  HWND portable = nullptr, name_edit = nullptr;
  HWND installed = nullptr, dir_edit = nullptr, dir_browse = nullptr;
  HWND keep_rom = nullptr, full = nullptr, repair = nullptr;
  HWND status = nullptr, progress = nullptr, progress_text = nullptr, log = nullptr;
  HWND start = nullptr, cancel = nullptr;
  HFONT font = nullptr, mono = nullptr;
  UINT dpi = 96;
  std::thread worker;
  std::atomic<bool> cancel_flag{false};
  bool running = false;
  bool close_when_done = false;
  // --selftest-convert: no message boxes, result recorded for the test.
  bool auto_test = false, auto_done = false, auto_ok = false, test_skip_assets = false;
};
App g;

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::wstring crlf(std::wstring s) {  // "\n" -> "\r\n" for edit/static controls
  std::wstring out;
  out.reserve(s.size() + 16);
  for (wchar_t c : s) {
    if (c == L'\n' && (out.empty() || out.back() != L'\r')) out += L'\r';
    out += c;
  }
  return out;
}

std::wstring text_of(HWND h) {
  const int n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(static_cast<std::size_t>(n));
  return s;
}

bool checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void set_check(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
int px(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

fs::path exe_dir() {
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  return fs::path(buf).parent_path();
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
          DWORD ex_style = 0) {
  HWND c = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
                           g.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
  if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
  return c;
}

// The game folder the current inputs describe (may be empty while typing).
fs::path chosen_game_dir() {
  if (checked(g.installed)) return fs::path(text_of(g.dir_edit));
  const std::wstring name = text_of(g.name_edit);
  return name.empty() ? fs::path() : exe_dir() / name;
}

void update_enabled() {
  const bool idle = !g.running;
  const bool portable = checked(g.portable);
  for (HWND h : {g.rom_edit, g.rom_browse, g.portable, g.installed, g.full, g.repair})
    EnableWindow(h, idle);
  EnableWindow(g.name_edit, idle && portable);
  EnableWindow(g.dir_edit, idle && !portable);
  EnableWindow(g.dir_browse, idle && !portable);
  EnableWindow(g.keep_rom, idle && !portable);  // portable always keeps its own copy
  if (portable) set_check(g.keep_rom, true);
  EnableWindow(g.start, idle);
  EnableWindow(g.cancel, g.running);
}

void refresh_status() {
  const fs::path dir = chosen_game_dir();
  std::wstring text, button = L"Convert";
  if (dir.empty()) {
    text = L"Choose where the game folder goes.";
  } else {
    const GameFolderStatus st = inspect_game_folder(dir);
    text = crlf(widen(st.describe()));
    if (st.is_game_folder && st.anything_done()) {
      button = L"Resume";
      text += L"\r\nA conversion already exists here. Resume keeps what is complete.";
    }
  }
  SetWindowTextW(g.status, text.c_str());
  if (!g.running) SetWindowTextW(g.start, button.c_str());
}

void append_log(const std::wstring& line) {
  const int len = GetWindowTextLengthW(g.log);
  SendMessageW(g.log, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
  const std::wstring text = line + L"\r\n";
  SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

bool pick_folder(const wchar_t* title, std::wstring& out) {
  bool ok = false;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(g.wnd))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          out = path;
          CoTaskMemFree(path);
          ok = true;
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  return ok;
}

void start_conversion() {
  ConvertRequest req;
  req.rom_dir = text_of(g.rom_edit);
  // An existing game folder with its own ROM copy can be resumed without the original.
  if (req.rom_dir.empty() && !inspect_game_folder(chosen_game_dir()).is_game_folder) {
    MessageBoxW(g.wnd, L"Select your ROM folder first: the folder that contains SYSTEM.CNF, "
                       L"SLUS_203.62 and the NFSHP2 folder.",
                L"HotPursuit2PC", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (checked(g.portable)) {
    const std::wstring name = text_of(g.name_edit);
    if (const auto why = why_bad_folder_name(to_utf8(fs::path(name)))) {
      MessageBoxW(g.wnd, widen(*why).c_str(), L"HotPursuit2PC", MB_OK | MB_ICONWARNING);
      return;
    }
    req.mode = Mode::Portable;
    req.rom_storage = RomStorage::Copy;
  } else {
    if (text_of(g.dir_edit).empty()) {
      MessageBoxW(g.wnd, L"Choose the folder to install into.", L"HotPursuit2PC", MB_OK | MB_ICONINFORMATION);
      return;
    }
    req.mode = Mode::Installed;
    req.rom_storage = checked(g.keep_rom) ? RomStorage::Copy : RomStorage::Reference;
  }
  req.game_dir = chosen_game_dir();
  req.full_check = checked(g.full);
  req.repair = checked(g.repair);
  req.unpack_assets = !g.test_skip_assets;

  g.cancel_flag = false;
  g.running = true;
  SetWindowTextW(g.log, L"");
  SendMessageW(g.progress, PBM_SETPOS, 0, 0);
  SetWindowTextW(g.progress_text, L"Starting ...");
  update_enabled();

  const HWND wnd = g.wnd;
  g.worker = std::thread([req, wnd] {
    ConvertEvents ev;
    ev.log = [wnd](std::string_view line) {
      PostMessageW(wnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(widen(line))));
    };
    DWORD last_tick = 0;
    int last_permille = -1;
    std::string last_stage;
    ev.progress = [&, wnd](const Progress& p) {
      const int permille = p.total ? static_cast<int>(p.done * 1000 / p.total) : 0;
      const DWORD now = GetTickCount();
      if (p.stage == last_stage && permille == last_permille && now - last_tick < 100) return;
      last_tick = now;
      last_permille = permille;
      last_stage = std::string(p.stage);
      PostMessageW(wnd, WM_APP_PROGRESS, 0,
                   reinterpret_cast<LPARAM>(new ProgressMsg{widen(p.stage), widen(p.item), p.done, p.total}));
    };
    ev.cancelled = [] { return g.cancel_flag.load(); };
    auto* result = new ConvertResult(convert(req, ev));
    PostMessageW(wnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(result));
  });
}

void on_done(ConvertResult* r) {
  if (g.worker.joinable()) g.worker.join();
  g.running = false;
  update_enabled();
  refresh_status();
  if (g.close_when_done) {
    delete r;
    DestroyWindow(g.wnd);
    return;
  }
  g.auto_done = true;
  g.auto_ok = r->ok;
  if (r->ok) {
    SendMessageW(g.progress, PBM_SETPOS, 1000, 0);
    SetWindowTextW(g.progress_text, L"Finished.");
    if (!g.auto_test) {
      const fs::path launcher = r->status.root / L"build" / L"HotPursuit2.exe";
      std::error_code ec;
      std::wstring msg = L"Conversion finished.\n\n";
      if (fs::is_regular_file(launcher, ec))
        msg += L"Start the game with:\n" + launcher.wstring() +
               L"\n\nThe game code runs translated to native code (hp2code.dll, when this converter includes "
               L"the translator); the PS2 hardware around it still runs on the Play! core built into that program, "
               L"so it is not yet a complete native port. Details are in README.txt next to it.";
      else
        msg += L"The ROM is stored and the game assets are unpacked, but this converter was built without the "
               L"game runtime, so no runnable game was installed.";
      MessageBoxW(g.wnd, msg.c_str(), L"HotPursuit2PC", MB_OK | MB_ICONINFORMATION);
    }
  } else {
    SetWindowTextW(g.progress_text, r->cancelled ? L"Cancelled." : L"Failed.");
    std::wstring msg = crlf(widen(r->error));
    if (!r->log_file.empty()) msg += L"\r\n\r\nLog file: " + r->log_file.wstring();
    if (g.auto_test)
      append_log(L"ERROR: " + msg);
    else
      MessageBoxW(g.wnd, msg.c_str(), L"HotPursuit2PC",
                  MB_OK | (r->cancelled ? MB_ICONINFORMATION : MB_ICONERROR));
  }
  delete r;
}

void create_controls() {
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
  g.font = CreateFontIndirectW(&ncm.lfMessageFont);

  const int m = 14, w = kClientW - 2 * m;
  int y = m;
  make(L"STATIC", L"ROM folder - the extracted disc of Need for Speed: Hot Pursuit 2 (PS2, NTSC-U, SLUS-20362):",
       0, m, y, w, 18, -1);
  y += 22;
  g.rom_edit = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, m, y, w - 96, 24, IDC_ROM_EDIT, WS_EX_CLIENTEDGE);
  g.rom_browse = make(L"BUTTON", L"Browse...", WS_TABSTOP, m + w - 88, y, 88, 24, IDC_ROM_BROWSE);
  y += 38;

  make(L"BUTTON", L"Game folder (everything the converter writes goes here)", BS_GROUPBOX, m, y, w, 118, -1);
  y += 24;
  g.portable = make(L"BUTTON", L"Portable - folder next to this program, named:",
                    BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, m + 12, y, 310, 22, IDC_PORTABLE);
  g.name_edit = make(L"EDIT", L"game", ES_AUTOHSCROLL | WS_TABSTOP, m + 330, y - 1, 150, 24, IDC_NAME_EDIT,
                     WS_EX_CLIENTEDGE);
  y += 30;
  g.installed = make(L"BUTTON", L"Installed - into this folder:", BS_AUTORADIOBUTTON, m + 12, y, 190, 22,
                     IDC_INSTALLED);
  g.dir_edit = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, m + 206, y - 1, w - 206 - 108, 24, IDC_DIR_EDIT,
                    WS_EX_CLIENTEDGE);
  g.dir_browse = make(L"BUTTON", L"Browse...", WS_TABSTOP, m + w - 96, y - 1, 84, 24, IDC_DIR_BROWSE);
  y += 30;
  g.keep_rom = make(L"BUTTON", L"Keep a copy of the ROM inside the game folder (needs about 3 GB more)",
                    BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, m + 12, y, w - 24, 22, IDC_KEEP_ROM);
  y += 44;

  g.full = make(L"BUTTON", L"Full ROM check before starting (hashes all 3 GB)", BS_AUTOCHECKBOX | WS_TABSTOP,
                m, y, 330, 22, IDC_FULL);
  g.repair = make(L"BUTTON", L"Repair: move files that do not match aside", BS_AUTOCHECKBOX | WS_TABSTOP,
                  m + 340, y, w - 340, 22, IDC_REPAIR);
  y += 30;

  g.status = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, m, y, w, 150, IDC_STATUS, WS_EX_CLIENTEDGE);
  // The status text is a small table: fixed-pitch font so its columns line up.
  g.mono = CreateFontW(-MulDiv(9, static_cast<int>(g.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
  if (g.mono) SendMessageW(g.status, WM_SETFONT, reinterpret_cast<WPARAM>(g.mono), TRUE);
  y += 158;
  g.progress_text = make(L"STATIC", L"", SS_ENDELLIPSIS, m, y, w, 18, IDC_PROGRESS_TEXT);
  y += 20;
  g.progress = make(PROGRESS_CLASSW, L"", 0, m, y, w, 18, IDC_PROGRESS);
  SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
  y += 26;
  g.log = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, m, y, w,
               kClientH - y - m - 40, IDC_LOG, WS_EX_CLIENTEDGE);
  SendMessageW(g.log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
  y = kClientH - m - 28;
  g.start = make(L"BUTTON", L"Convert", BS_DEFPUSHBUTTON | WS_TABSTOP, m + w - 220, y, 104, 28, IDC_START);
  g.cancel = make(L"BUTTON", L"Cancel", WS_TABSTOP, m + w - 104, y, 104, 28, IDC_CANCEL);

  // First run: portable, folder "game". Later runs: pick up the folder that exists.
  set_check(g.portable, true);
  set_check(g.keep_rom, true);
  SetWindowTextW(g.dir_edit, (exe_dir() / L"game").c_str());
  if (const auto existing = find_game_folder(exe_dir())) {
    const GameFolderStatus st = inspect_game_folder(*existing);
    SetWindowTextW(g.name_edit, existing->filename().c_str());
    SetWindowTextW(g.rom_edit, widen(st.rom_source).c_str());
  }
  update_enabled();
  refresh_status();
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      if (id == IDC_ROM_BROWSE) {
        std::wstring p;
        if (pick_folder(L"Select the ROM folder (contains SYSTEM.CNF and NFSHP2)", p))
          SetWindowTextW(g.rom_edit, p.c_str());
      } else if (id == IDC_DIR_BROWSE) {
        std::wstring p;
        if (pick_folder(L"Select the folder to install the game into", p)) SetWindowTextW(g.dir_edit, p.c_str());
      } else if (id == IDC_PORTABLE || id == IDC_INSTALLED) {
        update_enabled();
        refresh_status();
      } else if ((id == IDC_NAME_EDIT || id == IDC_DIR_EDIT) && code == EN_CHANGE) {
        if (g.status) refresh_status();
      } else if (id == IDC_START && !g.running) {
        start_conversion();
      } else if (id == IDC_CANCEL && g.running) {
        g.cancel_flag = true;
        SetWindowTextW(g.progress_text, L"Cancelling - finishing the current step ...");
      }
      return 0;
    }
    case WM_APP_LOG: {
      std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
      append_log(*line);
      return 0;
    }
    case WM_APP_PROGRESS: {
      std::unique_ptr<ProgressMsg> p(reinterpret_cast<ProgressMsg*>(lp));
      const int permille = p->total ? static_cast<int>(p->done * 1000 / p->total) : 0;
      SendMessageW(g.progress, PBM_SETPOS, static_cast<WPARAM>(permille), 0);
      if (!g.cancel_flag) {
        const std::wstring label = p->stage == L"validate" ? L"Checking the ROM"
                                   : p->stage == L"rom"    ? L"Storing the ROM"
                                   : p->stage == L"unpack" ? L"Unpacking assets" : p->stage;
        SetWindowTextW(g.progress_text,
                       (label + L"  " + std::to_wstring(permille / 10) + L"%   " + p->item).c_str());
      }
      return 0;
    }
    case WM_APP_DONE:
      on_done(reinterpret_cast<ConvertResult*>(lp));
      return 0;
    case WM_CTLCOLORSTATIC:
      // read-only edits and labels on the window background
      SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_CLOSE:
      if (g.running) {
        if (MessageBoxW(wnd, L"A conversion is running. Stop it and close?\n\nFinished work is kept; "
                             L"you can resume later.",
                        L"HotPursuit2PC", MB_YESNO | MB_ICONQUESTION) == IDYES) {
          g.cancel_flag = true;
          g.close_when_done = true;
        }
        return 0;
      }
      DestroyWindow(wnd);
      return 0;
    case WM_DESTROY:
      if (g.font) DeleteObject(g.font);
      if (g.mono) DeleteObject(g.mono);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

bool create_main_window(bool visible) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = g.inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc)) return false;

  // No WS_CLIPCHILDREN: themed group boxes rely on the parent painting behind them.
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  g.dpi = GetDpiForSystem();
  RECT rc{0, 0, MulDiv(kClientW, static_cast<int>(g.dpi), 96), MulDiv(kClientH, static_cast<int>(g.dpi), 96)};
  AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g.dpi);
  g.wnd = CreateWindowExW(0, kClassName, L"HotPursuit2PC - converter", style, CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, g.inst, nullptr);
  if (!g.wnd) return false;
  create_controls();
  if (visible) {
    ShowWindow(g.wnd, SW_SHOWNORMAL);
    UpdateWindow(g.wnd);
  }
  return true;
}

void pump_for(DWORD ms) {
  const DWORD end = GetTickCount() + ms;
  MSG msg;
  while (GetTickCount() < end) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(10);
  }
}

// --selftest: every control exists and the status panel has content.
int selftest() {
  if (!create_main_window(false)) return 1;
  pump_for(50);
  int failures = 0;
  for (HWND h : {g.rom_edit, g.rom_browse, g.portable, g.name_edit, g.installed, g.dir_edit, g.dir_browse,
                 g.keep_rom, g.full, g.repair, g.status, g.progress, g.progress_text, g.log, g.start, g.cancel})
    if (!h || !IsWindow(h)) ++failures;
  if (text_of(g.name_edit).empty()) ++failures;                 // default folder name is prefilled
  if (!checked(g.portable) || checked(g.installed)) ++failures;  // portable is the default mode
  if (IsWindowEnabled(g.keep_rom)) ++failures;                   // portable always keeps the ROM
  if (text_of(g.status).empty()) ++failures;                     // status panel describes the folder
  if (text_of(g.start).empty()) ++failures;
  if (IsWindowEnabled(g.cancel)) ++failures;                     // nothing running yet
  // switching to installed mode enables its inputs
  set_check(g.portable, false);
  set_check(g.installed, true);
  SendMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(IDC_INSTALLED, BN_CLICKED), 0);
  if (!IsWindowEnabled(g.dir_edit) || IsWindowEnabled(g.name_edit) || !IsWindowEnabled(g.keep_rom)) ++failures;
  DestroyWindow(g.wnd);
  return failures ? 1 : 0;
}

// --selftest-convert <rom_dir> <game_dir>: drive the real window through a real
// conversion (installed mode, ROM referenced in place, no asset unpacking, so
// only a few KB are written) and check what the user would see afterwards.
// Exit 77 (= skipped) when the ROM folder does not exist.
int selftest_convert(const std::wstring& rom, const std::wstring& game) {
  std::error_code ec;
  if (!fs::is_directory(rom, ec)) return 77;
  if (!create_main_window(false)) return 1;
  g.auto_test = true;
  g.test_skip_assets = true;
  SetWindowTextW(g.rom_edit, rom.c_str());
  set_check(g.portable, false);
  set_check(g.installed, true);
  SendMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(IDC_INSTALLED, BN_CLICKED), 0);
  SetWindowTextW(g.dir_edit, game.c_str());
  set_check(g.keep_rom, false);
  SendMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(IDC_START, BN_CLICKED), 0);

  int failures = 0;
  if (!g.running) ++failures;                       // the click started a run
  if (IsWindowEnabled(g.start)) ++failures;         // inputs are locked while running
  if (!IsWindowEnabled(g.cancel)) ++failures;
  const DWORD deadline = GetTickCount() + 120000;
  while (!g.auto_done && GetTickCount() < deadline) pump_for(20);
  pump_for(50);

  const std::wstring log = text_of(g.log), status = text_of(g.status);
  if (!g.auto_done || !g.auto_ok) ++failures;
  if (g.running) ++failures;
  if (log.find(L"Checking the ROM folder") == std::wstring::npos) ++failures;  // log lines arrived
  if (log.find(L"ROM referenced in place") == std::wstring::npos) ++failures;
  if (log.find(L"Done.") == std::wstring::npos) ++failures;
  if (SendMessageW(g.progress, PBM_GETPOS, 0, 0) != 1000) ++failures;         // progress completed
  if (text_of(g.progress_text) != L"Finished.") ++failures;
  if (status.find(L"[done   ] Store the ROM") == std::wstring::npos) ++failures;  // status refreshed
  if (text_of(g.start) != L"Resume") ++failures;      // a second click would resume
  if (!IsWindowEnabled(g.start) || IsWindowEnabled(g.cancel)) ++failures;
  if (failures) {  // leave evidence for whoever reads the test log
    HANDLE f = CreateFileW((game + L".selftest-failure.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      const std::wstring dump = log + L"\r\n---- status ----\r\n" + status;
      DWORD put = 0;
      WriteFile(f, dump.data(), static_cast<DWORD>(dump.size() * sizeof(wchar_t)), &put, nullptr);
      CloseHandle(f);
    }
  }
  DestroyWindow(g.wnd);
  return failures ? 1 : 0;
}

int screenshot(const std::wstring& path) {
  if (!create_main_window(true)) return 1;
  pump_for(400);
  RECT rc;
  GetWindowRect(g.wnd, &rc);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  PrintWindow(g.wnd, mem, 2 /* PW_RENDERFULLCONTENT */);
  BITMAPFILEHEADER fh{};
  const DWORD bytes = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
  fh.bfSize = fh.bfOffBits + bytes;
  int rcode = 1;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD put = 0;
    WriteFile(f, &fh, sizeof(fh), &put, nullptr);
    WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &put, nullptr);
    WriteFile(f, bits, bytes, &put, nullptr);
    CloseHandle(f);
    rcode = 0;
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  DestroyWindow(g.wnd);
  return rcode;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  g.inst = inst;
#ifdef HP2_HAVE_TRANSLATOR
  hp2::register_translator();
#endif
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
  if (argv) LocalFree(argv);

  if (!args.empty() && args[0] == L"--selftest") return selftest();
  if (args.size() == 3 && args[0] == L"--selftest-convert") {
    const int rc = selftest_convert(args[1], args[2]);
    if (g.worker.joinable()) g.worker.join();
    return rc;
  }
  if (args.size() == 2 && args[0] == L"--screenshot") return screenshot(args[1]);

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (!create_main_window(true)) {
    MessageBoxW(nullptr, L"The converter window could not be created.", L"HotPursuit2PC", MB_OK | MB_ICONERROR);
    return 1;
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(g.wnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (g.worker.joinable()) g.worker.join();
  if (SUCCEEDED(com)) CoUninitialize();
  return static_cast<int>(msg.wParam);
}
