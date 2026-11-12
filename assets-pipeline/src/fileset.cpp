#include "hp2/fileset.h"

#include <algorithm>
#include <map>
#include <ostream>
#include <sstream>

#include "hp2/lzc.h"

namespace hp2 {

namespace {

constexpr std::size_t kChunk = 4u << 20;

bool starts_with(ByteView head, std::string_view magic) {
  if (head.size() < magic.size()) return false;
  for (std::size_t i = 0; i < magic.size(); ++i)
    if (head[i] != static_cast<std::uint8_t>(magic[i])) return false;
  return true;
}

std::uint32_t u32_or(ByteView head, std::size_t off, std::uint32_t fallback) {
  return head.size() >= off + 4 ? read_u32le(head, off) : fallback;
}

bool looks_like_text(ByteView head) {
  if (head.empty()) return false;
  for (std::uint8_t b : head)
    if (!(b == '\r' || b == '\n' || b == '\t' || (b >= 0x20 && b < 0x7F))) return false;
  return true;
}

// Replay files: 8 zero bytes, then the chunk id 0x00039100.
bool looks_like_replay(ByteView head) {
  return head.size() >= 12 && u32_or(head, 0, 1) == 0 && u32_or(head, 4, 1) == 0 &&
         u32_or(head, 8, 0) == 0x00039100u;
}

std::string_view extension_of(std::string_view name) {
  const std::size_t dot = name.rfind('.');
  const std::size_t sep = name.rfind('\\');
  if (dot == std::string_view::npos || (sep != std::string_view::npos && dot < sep)) return {};
  return name.substr(dot + 1);
}

std::string_view top_dir_of(std::string_view name) {
  const std::size_t sep = name.find('\\');
  return sep == std::string_view::npos ? std::string_view{} : name.substr(0, sep);
}

std::string_view base_of(std::string_view name) {
  const std::size_t sep = name.rfind('\\');
  return sep == std::string_view::npos ? name : name.substr(sep + 1);
}

fs::path relative_path_for(std::string_view name) {
  if (!is_safe_relative_name(name)) fail("internal error: unsafe output name '{}'", name);
  fs::path p;
  std::size_t start = 0;
  while (true) {
    const std::size_t sep = name.find('\\', start);
    p /= from_utf8(name.substr(start, sep == std::string_view::npos ? sep : sep - start));
    if (sep == std::string_view::npos) break;
    start = sep + 1;
  }
  return p;
}

std::string sha256_of_extent(const ReadOnlyFile& f, std::uint64_t offset, std::uint64_t size) {
  Sha256 h;
  Bytes buf(static_cast<std::size_t>(std::min<std::uint64_t>(size, kChunk)));
  std::uint64_t done = 0;
  while (done < size) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, kChunk));
    f.read_at(offset + done, std::span<std::uint8_t>(buf.data(), n));
    h.update(ByteView(buf.data(), n));
    done += n;
  }
  return Sha256::hex(h.finish());
}

std::string generic_utf8(const fs::path& p) {
  const std::u8string u8 = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string flags_of(const Entry& e) {
  std::string flags;
  auto add = [&](const std::string& s) {
    if (!flags.empty()) flags += ',';
    flags += s;
  };
  if (e.record.size == 0) add("empty");
  if (e.duplicate_of) add(std::format("dup-of={}", *e.duplicate_of));
  if (!e.named) add("unnamed");
  return flags.empty() ? "-" : flags;
}

}  // namespace

// ---------------------------------------------------------------- classify

FileKind classify_file(std::string_view name, ByteView head, std::uint64_t size) {
  if (size == 0) return {"placeholder", "empty placeholder entry"};
  const std::string_view ext = extension_of(name);
  const std::string_view dir = top_dir_of(name);
  const std::string_view base = base_of(name);

  if (starts_with(head, "COMP")) return {"bundle-lzc", "LZC-compressed bundle"};
  if (starts_with(head, "RIFF")) return {"movie", "RIFF/AVI movie"};
  if (u32_or(head, 0, 0) == 0xB0300001u) return {"texture-pack", "texture pack"};
  if (ext == "MSH") return {"sound-bank-header", "sound bank header"};
  if (ext == "MSB") return {"sound-bank-body", "sound bank sample data"};
  if (ext == "PCK") return {"speech-pack", "speech pack"};
  if (ext == "EVT") return {"speech-events", "speech event table"};
  if (ext == "JBC") return {"music", "jukebox music container"};
  if (ext == "CTB" || ext == "LTB") return {"engine-sound-table", "engine sound table"};
  if (ext == "ICO") return {"save-icon", "memory card save icon"};
  if (ext == "CAM") return {"nis-camera", "cut-scene camera track"};
  if (ext == "TXT") return {"text", "developer text note"};
  if (dir == "LANGUAGES") return {"strings", "localised string table"};
  if (dir == "MARKERS") return {"track-markers", "track marker table"};
  if (dir == "NIS") return {"nis-scene", "cut-scene data"};
  if (dir == "REPLAY" || looks_like_replay(head)) return {"replay", "recorded replay"};
  if (dir == "CARS" && (base == "GEOMETRY.BIN" || base == "DASHGEOM.BIN"))
    return {"car-geometry", "car geometry bundle"};
  if (ext == "BUN") return {"bundle", "chunked bundle"};
  if (name.empty() || dir == "_UNNAMED") {
    if (looks_like_text(head)) return {"text", "text (unnamed)"};
    return {"unknown", "unidentified data"};
  }
  if (ext == "BIN") return {"bundle", "chunked bundle"};
  return {"unknown", "unidentified data"};
}

std::string_view sniff_extension(ByteView head) {
  if (starts_with(head, "COMP")) return "LZC";
  if (starts_with(head, "RIFF")) return "AVI";
  if (looks_like_text(head)) return "TXT";
  return "BIN";
}

// ------------------------------------------------------------------ ZFileset

ZFileset ZFileset::open(const fs::path& rom_dir, const NameTable& names) {
  const fs::path zdir_path = rom_dir / "NFSHP2" / "ZDIR.BIN";
  const fs::path zz_path = rom_dir / "NFSHP2" / "ZZDATA.BIN";
  std::error_code ec;
  for (const fs::path& p : {zdir_path, zz_path}) {
    if (!fs::is_regular_file(p, ec))
      fail("'{}' not found - the ROM folder must be the extracted disc root "
           "(SYSTEM.CNF, SLUS_203.62 and the NFSHP2 folder)",
           to_utf8(p));
  }

  ZFileset set;
  const Bytes raw = read_whole_file(zdir_path, 64u << 20);
  const std::vector<ZdirRecord> records = parse_zdir(raw);
  set.data_ = std::make_shared<ReadOnlyFile>(zz_path);
  set.layout_ = check_zdir_layout(records, set.data_->size());
  if (!set.layout_.ok()) {
    std::string all;
    for (std::size_t i = 0; i < set.layout_.errors.size() && i < 5; ++i)
      all += "\n  - " + set.layout_.errors[i];
    fail("ZDIR.BIN does not match ZZDATA.BIN ({} problem(s)) - the dump is damaged or "
         "the two files come from different discs:{}",
         set.layout_.errors.size(), all);
  }

  std::map<std::uint32_t, std::uint32_t> first_index;
  set.entries_.reserve(records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    Entry e;
    e.index = static_cast<std::uint32_t>(i);
    e.record = records[i];
    e.head_size = static_cast<std::uint8_t>(std::min<std::uint32_t>(e.record.size, 16));
    if (e.head_size)
      set.data_->read_at(e.record.offset(), std::span<std::uint8_t>(e.head.data(), e.head_size));
    const ByteView head(e.head.data(), e.head_size);

    if (const NameEntry* n = names.find(e.record.hash)) {
      e.name = std::string(n->name);
      e.named = true;
      e.tier = n->tier;
    } else {
      e.name = std::format("_UNNAMED\\{:08X}.{}", e.record.hash, sniff_extension(head));
    }
    e.kind = classify_file(e.name, head, e.record.size);

    const auto [it, inserted] = first_index.emplace(e.record.hash, e.index);
    if (!inserted) e.duplicate_of = it->second;
    set.entries_.push_back(std::move(e));
  }
  return set;
}

std::size_t ZFileset::named_count() const {
  return static_cast<std::size_t>(
      std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.named; }));
}

Bytes ZFileset::read(const Entry& e) const {
  return data_->read_at(e.record.offset(), static_cast<std::size_t>(e.record.size));
}

// ----------------------------------------------------------------- inventory

void write_inventory_tsv(const ZFileset& set, std::ostream& out, const InventoryOptions& opt) {
  out << "index\thash\tsector\tsize\t";
  if (opt.with_sha256) out << "sha256\t";
  out << "tier\tkind\tflags\tname\n";
  for (const Entry& e : set.entries()) {
    out << e.index << '\t' << std::format("{:08X}", e.record.hash) << '\t' << e.record.sector
        << '\t' << e.record.size << '\t';
    if (opt.with_sha256)
      out << sha256_of_extent(set.data(), e.record.offset(), e.record.size) << '\t';
    out << (e.named ? e.tier : '-') << '\t' << e.kind.id << '\t' << flags_of(e) << '\t' << e.name
        << '\n';
  }
}

// -------------------------------------------------------------------- unpack

UnpackResult unpack(const ZFileset& set, const fs::path& out_dir, const UnpackOptions& opt,
                    const std::function<void(const UnpackProgress&)>& progress) {
  UnpackResult result;
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) fail("cannot create output folder '{}': {}", to_utf8(out_dir), ec.message());

  UnpackProgress prog;
  std::vector<const Entry*> todo;
  for (const Entry& e : set.entries()) {
    if (opt.filter && !opt.filter(e)) {
      ++result.filtered_out;
      continue;
    }
    todo.push_back(&e);
    prog.bytes_total += e.record.size;
  }
  prog.files_total = static_cast<std::uint32_t>(todo.size());

  std::map<std::uint32_t, std::string> sha_by_index;
  std::ostringstream manifest;
  manifest << "index\thash\tsector\tsize\tsha256\ttier\tkind\tflags\tpath\n";
  Bytes buf(kChunk);

  auto tick = [&](const Entry& e) {
    if (progress) {
      prog.current = e.name;
      progress(prog);
    }
  };

  for (const Entry* ep : todo) {
    const Entry& e = *ep;
    const fs::path rel = relative_path_for(e.name);
    const fs::path target = out_dir / rel;
    const std::uint64_t size = e.record.size;
    const std::uint64_t offset = e.record.offset();
    tick(e);

    std::string sha;
    const auto first = e.duplicate_of ? sha_by_index.find(*e.duplicate_of) : sha_by_index.end();
    if (first != sha_by_index.end()) {
      // Same name stored twice on the disc: must be the same bytes.
      sha = sha256_of_extent(set.data(), offset, size);
      if (first->second != sha)
        fail("record {} and record {} share the name '{}' but hold different data - "
             "refusing to drop one of them",
             *e.duplicate_of, e.index, e.name);
      ++result.duplicates;
      prog.bytes_done += size;
    } else {
      bool need_write = true;
      const fs::file_status st = fs::status(target, ec);
      if (fs::exists(st)) {
        if (!fs::is_regular_file(st))
          fail("'{}' exists but is not a file - cannot extract '{}' there", to_utf8(target), e.name);
        const std::uint64_t have = fs::file_size(target, ec);
        bool matches = !ec && have == size;
        sha = sha256_of_extent(set.data(), offset, size);
        if (matches && opt.verify_existing) {
          ReadOnlyFile existing(target);
          matches = sha256_of_extent(existing, 0, existing.size()) == sha;
        }
        if (matches) {
          need_write = false;
          ++result.kept;
          prog.bytes_done += size;
        } else if (opt.repair) {
          fs::path aside;
          for (int n = 1; n < 10000; ++n) {
            aside = target;
            aside += std::format(L".mismatch-{}", n);
            if (!fs::exists(aside, ec)) break;
          }
          fs::rename(target, aside, ec);
          if (ec) fail("cannot move mismatching file '{}' aside: {}", to_utf8(target), ec.message());
          result.notes.push_back(std::format(
              "'{}' did not match the disc; kept the old file as '{}' and extracted it again",
              e.name, to_utf8(aside.filename())));
          ++result.repaired;
        } else {
          fail("'{}' already exists but does not match the disc (found {} bytes, the disc has {}). "
               "Nothing was overwritten. Re-run with repair to move the old file aside and "
               "extract it again.",
               to_utf8(target), have, size);
        }
      }
      if (need_write) {
        OutputFile out(target);
        Sha256 h;
        std::uint64_t done = 0;
        while (done < size) {
          const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, kChunk));
          set.data().read_at(offset + done, std::span<std::uint8_t>(buf.data(), n));
          h.update(ByteView(buf.data(), n));
          out.write(ByteView(buf.data(), n));
          done += n;
          prog.bytes_done += n;
          tick(e);
        }
        out.commit();
        sha = Sha256::hex(h.finish());
        ++result.written;
        result.bytes_written += size;
      }
    }
    sha_by_index[e.index] = sha;
    ++prog.files_done;
    manifest << e.index << '\t' << std::format("{:08X}", e.record.hash) << '\t' << e.record.sector
             << '\t' << size << '\t' << sha << '\t' << (e.named ? e.tier : '-') << '\t'
             << e.kind.id << '\t' << flags_of(e) << '\t' << generic_utf8(rel) << '\n';
  }
  if (progress) {
    prog.current = {};
    progress(prog);
  }

  result.manifest = out_dir / "manifest.tsv";
  const std::string text = manifest.str();
  write_file_replacing(result.manifest,
                       ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  return result;
}

}  // namespace hp2
