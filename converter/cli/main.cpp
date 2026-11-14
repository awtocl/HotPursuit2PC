// hp2pc-cli - the converter's command line (same operations as the GUI, ADR 0003).
//
//   hp2pc-cli validate <rom_dir> [--full]
//   hp2pc-cli convert  --rom <rom_dir> [--name <folder> | --install-dir <dir>]
//                      [--rom-storage copy|reference] [--full-check] [--repair] [--no-assets]
//   hp2pc-cli status   [--name <folder> | --game <dir>]
//   hp2pc-cli version | help
//
// Exit codes: 0 ok, 1 failed, 2 bad usage, 3 the ROM folder is not usable, 4 cancelled.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "hp2/convert.h"
#ifdef HP2_HAVE_TRANSLATOR
#include "code_builder.h"
#endif
#include "hp2/iso.h"

using namespace hp2;

namespace {

std::atomic<bool> g_cancel{false};

BOOL WINAPI on_console_event(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    g_cancel = true;  // finish the current chunk, clean up the partial file, keep the rest
    return TRUE;
  }
  return FALSE;
}

std::string narrow(const std::wstring& w) { return to_utf8(fs::path(w)); }

fs::path exe_dir() {
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  buf.resize(n);
  return fs::path(buf).parent_path();
}

int usage(bool error) {
  std::fputs(
      "HotPursuit2PC converter - command line\n"
      "\n"
      "  hp2pc-cli validate <rom_dir> [--full]\n"
      "      Check that <rom_dir> is the extracted Need for Speed: Hot Pursuit 2 disc\n"
      "      (PS2, NTSC-U, SLUS-20362). --full also hashes the 3 GB data file.\n"
      "\n"
      "  hp2pc-cli convert --rom <rom_dir> [options]\n"
      "      Create or resume a game folder and convert into it.\n"
      "        --name <folder>          portable mode: folder next to this .exe (default: game)\n"
      "        --install-dir <dir>      installed mode: use this directory instead\n"
      "        --rom-storage copy|reference\n"
      "                                 copy (default) keeps the ROM inside the game folder;\n"
      "                                 reference (installed mode only) leaves it where it is\n"
      "        --full-check             hash every ROM file before starting\n"
      "        --repair                 move files that do not match aside and redo them\n"
      "        --no-assets              do not unpack the game assets\n"
      "\n"
      "  hp2pc-cli status [--name <folder> | --game <dir>]\n"
      "      Show what is already done in a game folder and what a new run would do.\n"
      "\n"
      "  hp2pc-cli image --rom <rom_dir> --out <file.iso>\n"
      "      Rebuild an ISO 9660 disc image from the validated ROM folder (the form a\n"
      "      PS2 runtime boots). The image is about 3 GB; an existing file is never replaced.\n"
      "\n"
      "  hp2pc-cli version\n"
      "\n"
      "The ROM folder is only read, never modified. Everything the converter writes\n"
      "goes into the one game folder.\n",
      error ? stderr : stdout);
  return error ? 2 : 0;
}

int cmd_validate(const fs::path& rom, bool full) {
  int last = -1;
  const RomReport r = check_rom(rom, full ? RomCheckLevel::Full : RomCheckLevel::Quick,
                                RomProfile::slus20362(), [&](const Progress& p) {
                                  const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
                                  if (pct != last) {
                                    last = pct;
                                    std::fprintf(stderr, "\rchecking %3d%%", pct);
                                  }
                                });
  if (last >= 0) std::fputs("\r              \r", stderr);
  std::fputs(r.summary().c_str(), stdout);
  return r.ok() ? 0 : 3;
}

int cmd_image(const fs::path& rom, const fs::path& out) {
  const RomReport r = check_rom(rom);
  if (!r.ok()) {
    std::fputs(r.summary().c_str(), stdout);
    return 3;
  }
  // C2: nothing is ever written into the ROM folder.
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(r.root, ec);
  const fs::path target = fs::weakly_canonical(fs::absolute(out, ec), ec);
  const std::wstring a = root.generic_wstring() + L"/", b = target.generic_wstring();
  if (b.size() >= a.size() && _wcsnicmp(a.c_str(), b.c_str(), a.size()) == 0) {
    std::fputs("error: the image would be written inside the ROM folder, which is never modified. "
               "Choose another location.\n",
               stderr);
    return 1;
  }
  const IsoLayout layout = iso_layout_for_rom(r.root);
  std::printf("Writing %s (%llu bytes, %zu files) ...\n", to_utf8(out).c_str(),
              static_cast<unsigned long long>(layout.total_bytes()), layout.files().size());
  int last = -1;
  write_iso(layout, out, [&](const Progress& p) {
    if (g_cancel) fail("cancelled - the partial image was removed");
    const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
    if (pct != last) {
      last = pct;
      std::fprintf(stderr, "\r  image %3d%%  %-40.40s", pct, std::string(p.item).c_str());
    }
  });
  std::fputs("\n", stderr);
  std::puts("Done.");
  return 0;
}

int cmd_status(const fs::path& game_dir) {
  const GameFolderStatus st = inspect_game_folder(game_dir);
  std::fputs(st.describe().c_str(), stdout);
  if (st.is_game_folder)
    std::puts("A new 'convert' run on this folder resumes: finished work is detected and kept.");
  return st.foreign_content ? 1 : 0;
}

int cmd_convert(const ConvertRequest& req) {
  ConvertEvents ev;
  std::string last_stage;
  int last_pct = -1;
  bool bar_open = false;
  const auto close_bar = [&] {
    if (bar_open) std::fputs("\n", stderr);
    bar_open = false;
  };
  ev.log = [&](std::string_view line) {
    close_bar();
    std::printf("%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(stdout);
  };
  ev.progress = [&](const Progress& p) {
    const int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 0;
    if (p.stage == last_stage && pct == last_pct) return;
    last_stage = std::string(p.stage);
    last_pct = pct;
    std::fprintf(stderr, "\r  %-8.*s %3d%%  %-56.56s", static_cast<int>(p.stage.size()), p.stage.data(),
                 pct, std::string(p.item).c_str());
    bar_open = true;
  };
  ev.cancelled = [] { return g_cancel.load(); };

  const ConvertResult r = convert(req, ev);
  close_bar();
  std::puts("");
  std::fputs(r.status.describe().c_str(), stdout);
  if (!r.log_file.empty()) std::printf("Log: %s\n", to_utf8(r.log_file).c_str());
  if (r.ok) return 0;
  std::fprintf(stderr, "\n%s\n", r.error.c_str());
  if (r.cancelled) return 4;
  return r.failed_stage == "validate" ? 3 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
#ifdef HP2_HAVE_TRANSLATOR
  hp2::register_translator();
#endif
  SetConsoleCtrlHandler(on_console_event, TRUE);
  try {
    if (argc < 2) return usage(true);
    const std::wstring cmd = argv[1];
    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h" || cmd == L"/?") return usage(false);
    if (cmd == L"version" || cmd == L"--version") {
      std::printf("HotPursuit2PC converter %.*s (supports %s)\n",
                  static_cast<int>(kConverterVersion.size()), kConverterVersion.data(),
                  RomProfile::slus20362().serial.c_str());
      return 0;
    }

    if (cmd == L"validate") {
      fs::path rom;
      bool full = false;
      for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--full") full = true;
        else if (rom.empty() && a.rfind(L"--", 0) != 0) rom = a;
        else return usage(true);
      }
      if (rom.empty()) return usage(true);
      return cmd_validate(rom, full);
    }

    if (cmd == L"image") {
      fs::path rom, out;
      for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--rom" && i + 1 < argc) rom = argv[++i];
        else if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else return usage(true);
      }
      if (rom.empty() || out.empty()) return usage(true);
      return cmd_image(rom, out);
    }

    if (cmd == L"status" || cmd == L"convert") {
      ConvertRequest req;
      std::wstring name;
      fs::path install_dir, game_override;
      bool storage_given = false;
      for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == L"--rom" && has_value) req.rom_dir = argv[++i];
        else if (a == L"--name" && has_value) name = argv[++i];
        else if (a == L"--install-dir" && has_value) install_dir = argv[++i];
        else if (a == L"--game" && has_value) game_override = argv[++i];
        else if (a == L"--rom-storage" && has_value) {
          const auto s = parse_rom_storage(narrow(argv[++i]));
          if (!s) {
            std::fputs("error: --rom-storage must be 'copy' or 'reference'\n", stderr);
            return 2;
          }
          req.rom_storage = *s;
          storage_given = true;
        } else if (a == L"--full-check") req.full_check = true;
        else if (a == L"--repair") req.repair = true;
        else if (a == L"--no-assets") req.unpack_assets = false;
        else {
          std::fprintf(stderr, "error: unknown or incomplete option '%s'\n\n", narrow(a).c_str());
          return usage(true);
        }
      }
      if (!name.empty() && !install_dir.empty()) {
        std::fputs("error: use either --name (portable) or --install-dir (installed), not both\n", stderr);
        return 2;
      }
      if (!name.empty())
        if (const auto why = why_bad_folder_name(narrow(name))) {
          std::fprintf(stderr, "error: %s\n", why->c_str());
          return 2;
        }

      if (!install_dir.empty()) {
        req.mode = Mode::Installed;
        req.game_dir = install_dir;
      } else if (!game_override.empty()) {
        req.game_dir = game_override;
      } else if (!name.empty()) {
        req.game_dir = exe_dir() / name;
      } else if (const auto existing = find_game_folder(exe_dir())) {
        req.game_dir = *existing;  // re-run: pick up the folder created earlier
      } else {
        req.game_dir = exe_dir() / std::wstring(kDefaultGameFolderName.begin(), kDefaultGameFolderName.end());
      }

      if (cmd == L"status") return cmd_status(req.game_dir);

      // Resuming: unless told otherwise, keep the settings the folder was created with.
      const GameFolderStatus existing = inspect_game_folder(req.game_dir);
      if (existing.is_game_folder) {
        if (install_dir.empty()) req.mode = existing.mode;
        if (!storage_given) req.rom_storage = existing.rom_storage;
        if (req.rom_dir.empty() && !existing.rom_source.empty()) req.rom_dir = from_utf8(existing.rom_source);
      }
      if (req.rom_dir.empty() && !existing.is_game_folder) {
        std::fputs("error: --rom <rom_dir> is required for a new game folder\n\n", stderr);
        return usage(true);
      }
      return cmd_convert(req);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", narrow(cmd).c_str());
    return usage(true);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
