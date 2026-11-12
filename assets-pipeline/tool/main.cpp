// hp2assets - developer tool around the asset pipeline library.
//
//   hp2assets info      <rom_dir>
//   hp2assets inventory <rom_dir> [--sha256] [--out <file.tsv>]
//   hp2assets unpack    <rom_dir> <out_dir> [--verify] [--repair]
//   hp2assets lzc       <in.lzc> <out.bin>
//   hp2assets hash      <name> [<name> ...]
//
// The ROM folder is only ever opened for reading.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>

#include "hp2/fileset.h"
#include "hp2/lzc.h"

using namespace hp2;

namespace {

std::string narrow(const wchar_t* w) { return to_utf8(fs::path(w)); }

int usage() {
  std::puts(
      "hp2assets - Hot Pursuit 2 (PS2, SLUS-20362) asset pipeline tool\n"
      "\n"
      "  hp2assets info      <rom_dir>                          directory + layout summary\n"
      "  hp2assets inventory <rom_dir> [--sha256] [--out FILE]  one line per file (TSV)\n"
      "  hp2assets unpack    <rom_dir> <out_dir> [--verify] [--repair]\n"
      "  hp2assets lzc       <in.lzc> <out.bin>                 decompress one COMP stream\n"
      "  hp2assets hash      <name> [...]                       name hash as stored in ZDIR.BIN\n"
      "\n"
      "<rom_dir> is the extracted disc root (SYSTEM.CNF, SLUS_203.62, NFSHP2\\...).\n"
      "It is opened read-only.");
  return 2;
}

int cmd_info(const fs::path& rom) {
  const ZFileset set = ZFileset::open(rom);
  const ZdirLayout& l = set.layout();
  std::printf("ZDIR records          : %zu\n", l.records);
  std::printf("distinct name hashes  : %zu\n", l.distinct_hashes);
  std::printf("empty records         : %zu\n", l.empty_records);
  std::printf("distinct extents      : %zu\n", l.distinct_extents);
  std::printf("repeated-hash records : %zu\n", l.duplicate_hash_records);
  std::printf("ZZDATA.BIN            : %llu bytes, %llu sectors\n",
              static_cast<unsigned long long>(set.data().size()),
              static_cast<unsigned long long>(l.data_sectors));
  std::printf("sorted by sector      : %s\n", l.sorted_by_sector ? "yes" : "no");
  std::printf("unreferenced sectors  : %llu\n", static_cast<unsigned long long>(l.gap_sectors));
  std::printf("exact tiling          : %s\n", l.exact_tiling() ? "yes" : "no");
  std::printf("named entries         : %zu of %zu\n", set.named_count(), set.entries().size());
  for (const std::string& w : l.warnings) std::printf("warning: %s\n", w.c_str());

  std::map<std::string_view, std::pair<std::size_t, std::uint64_t>> kinds;
  for (const Entry& e : set.entries()) {
    auto& k = kinds[e.kind.id];
    ++k.first;
    if (!e.duplicate_of) k.second += e.record.size;
  }
  std::printf("\n%-20s %6s %15s\n", "kind", "files", "bytes");
  for (const auto& [id, v] : kinds)
    std::printf("%-20.*s %6zu %15llu\n", static_cast<int>(id.size()), id.data(), v.first,
                static_cast<unsigned long long>(v.second));
  return 0;
}

int cmd_inventory(const fs::path& rom, bool sha, const fs::path& out_path) {
  const ZFileset set = ZFileset::open(rom);
  InventoryOptions opt;
  opt.with_sha256 = sha;
  if (out_path.empty()) {
    write_inventory_tsv(set, std::cout, opt);
  } else {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) fail("cannot write '{}'", to_utf8(out_path));
    write_inventory_tsv(set, out, opt);
    std::printf("wrote %s (%zu entries)\n", to_utf8(out_path).c_str(), set.entries().size());
  }
  return 0;
}

int cmd_unpack(const fs::path& rom, const fs::path& out_dir, bool verify, bool repair) {
  const ZFileset set = ZFileset::open(rom);
  UnpackOptions opt;
  opt.verify_existing = verify;
  opt.repair = repair;
  const auto t0 = std::chrono::steady_clock::now();
  int last_pct = -1;
  const UnpackResult r = unpack(set, out_dir, opt, [&](const UnpackProgress& p) {
    const int pct = p.bytes_total ? static_cast<int>(p.bytes_done * 100 / p.bytes_total) : 100;
    if (pct != last_pct) {
      last_pct = pct;
      std::printf("\r%3d%%  %u/%u files  %-50.50s", pct, p.files_done, p.files_total,
                  std::string(p.current).c_str());
      std::fflush(stdout);
    }
  });
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\nwritten %u, already present %u, repaired %u, duplicate records %u, %.1f MB in %.1f s\n",
              r.written, r.kept, r.repaired, r.duplicates, r.bytes_written / 1048576.0, secs);
  for (const std::string& n : r.notes) std::printf("note: %s\n", n.c_str());
  std::printf("manifest: %s\n", to_utf8(r.manifest).c_str());
  return 0;
}

int cmd_lzc(const fs::path& in, const fs::path& out_path) {
  const Bytes packed = read_whole_file(in, 1ull << 30);
  const Bytes raw = lzc_decompress(packed);
  OutputFile out(out_path);
  out.write(raw);
  out.commit();
  std::printf("%s: %zu -> %zu bytes\n", to_utf8(out_path).c_str(), packed.size(), raw.size());
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  try {
    if (argc < 2) return usage();
    const std::wstring cmd = argv[1];
    if (cmd == L"info" && argc == 3) return cmd_info(argv[2]);
    if (cmd == L"inventory" && argc >= 3) {
      bool sha = false;
      fs::path out;
      for (int i = 3; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--sha256") sha = true;
        else if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else return usage();
      }
      return cmd_inventory(argv[2], sha, out);
    }
    if (cmd == L"unpack" && argc >= 4) {
      bool verify = false, repair = false;
      for (int i = 4; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--verify") verify = true;
        else if (a == L"--repair") repair = true;
        else return usage();
      }
      return cmd_unpack(argv[2], argv[3], verify, repair);
    }
    if (cmd == L"lzc" && argc == 4) return cmd_lzc(argv[2], argv[3]);
    if (cmd == L"hash" && argc >= 3) {
      for (int i = 2; i < argc; ++i) {
        const std::string name = narrow(argv[i]);
        std::printf("%08X  %s\n", name_hash(name), name.c_str());
      }
      return 0;
    }
    return usage();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
