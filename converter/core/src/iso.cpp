#include "hp2/iso.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <tuple>

namespace hp2 {

namespace {

constexpr std::uint32_t kSec = 2048;
constexpr std::uint32_t kPvdSector = 16;
constexpr std::uint32_t kFirstFreeSector = 18;  // 16 = PVD, 17 = terminator
constexpr std::size_t kChunk = 4u << 20;

// ---- little helpers for the both-endian fields of ECMA-119 ---------------------
void put16le(Bytes& b, std::size_t o, std::uint16_t v) {
  b[o] = static_cast<std::uint8_t>(v);
  b[o + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put16be(Bytes& b, std::size_t o, std::uint16_t v) {
  b[o] = static_cast<std::uint8_t>(v >> 8);
  b[o + 1] = static_cast<std::uint8_t>(v);
}
void put32le(Bytes& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put32be(Bytes& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>(v >> (8 * (3 - i)));
}
void put16both(Bytes& b, std::size_t o, std::uint16_t v) {
  put16le(b, o, v);
  put16be(b, o + 2, v);
}
void put32both(Bytes& b, std::size_t o, std::uint32_t v) {
  put32le(b, o, v);
  put32be(b, o + 4, v);
}
void put_text(Bytes& b, std::size_t o, std::size_t width, std::string_view text) {
  for (std::size_t i = 0; i < width; ++i)
    b[o + i] = static_cast<std::uint8_t>(i < text.size() ? text[i] : ' ');
}
void put_long_date(Bytes& b, std::size_t o, const IsoTime& t) {  // 17-byte "dec-datetime"
  const std::string s = std::format("{:04}{:02}{:02}{:02}{:02}{:02}00", t.year, t.month, t.day, t.hour,
                                    t.minute, t.second);
  for (std::size_t i = 0; i < 16; ++i) b[o + i] = static_cast<std::uint8_t>(s[i]);
  b[o + 16] = 0;
}
std::uint32_t sectors_for(std::uint64_t bytes) { return static_cast<std::uint32_t>((bytes + kSec - 1) / kSec); }

bool is_d_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

void check_dir_name(std::string_view n, std::string_view full) {
  if (n.empty() || n.size() > 8 || !std::all_of(n.begin(), n.end(), is_d_char))
    fail("'{}' cannot go into the disc image: folder name '{}' is not an ISO 9660 level 1 name "
         "(1-8 characters A-Z, 0-9, _)",
         full, n);
}

void check_file_name(std::string_view n, std::string_view full) {
  const std::size_t dot = n.find('.');
  const std::string_view stem = n.substr(0, dot);
  const std::string_view ext = dot == std::string_view::npos ? std::string_view{} : n.substr(dot + 1);
  const bool ok = !stem.empty() && stem.size() <= 8 && ext.size() <= 3 &&
                  std::all_of(stem.begin(), stem.end(), is_d_char) &&
                  std::all_of(ext.begin(), ext.end(), is_d_char);
  if (!ok)
    fail("'{}' cannot go into the disc image: file name '{}' is not an ISO 9660 level 1 name "
         "(8.3, characters A-Z, 0-9, _)",
         full, n);
}

struct Dir {
  std::string name;                   // "" for the root
  int parent = 0;                     // index into the directory list
  std::map<std::string, int> subdirs;  // name -> directory index (sorted)
  std::map<std::string, std::size_t> files;  // name -> index into the source list (sorted)
  std::uint32_t sector = 0;
  std::uint32_t bytes = 0;            // extent size, multiple of 2048
  std::uint16_t number = 0;           // 1-based position in the path table
  IsoTime time;
};

std::size_t record_len(std::size_t id_len) { return 33 + id_len + (id_len % 2 == 0 ? 1 : 0); }

// Appends one directory record; records never straddle a sector boundary.
void put_record(Bytes& b, std::size_t& pos, std::string_view id, std::uint32_t sector,
                std::uint32_t size, bool directory, const IsoTime& t) {
  const std::size_t len = record_len(id.size());
  if (pos % kSec + len > kSec) pos = (pos / kSec + 1) * kSec;
  b[pos] = static_cast<std::uint8_t>(len);
  put32both(b, pos + 2, sector);
  put32both(b, pos + 10, size);
  b[pos + 18] = static_cast<std::uint8_t>(t.year - 1900);
  b[pos + 19] = static_cast<std::uint8_t>(t.month);
  b[pos + 20] = static_cast<std::uint8_t>(t.day);
  b[pos + 21] = static_cast<std::uint8_t>(t.hour);
  b[pos + 22] = static_cast<std::uint8_t>(t.minute);
  b[pos + 23] = static_cast<std::uint8_t>(t.second);
  b[pos + 24] = 0;  // UTC
  b[pos + 25] = directory ? 0x02 : 0x00;
  put16both(b, pos + 28, 1);
  b[pos + 32] = static_cast<std::uint8_t>(id.size());
  for (std::size_t i = 0; i < id.size(); ++i) b[pos + 33 + i] = static_cast<std::uint8_t>(id[i]);
  pos += len;
}

// Size of a directory extent holding ".", ".." and the given identifiers.
std::uint32_t dir_extent_bytes(const std::vector<std::string>& ids) {
  std::size_t pos = 2 * record_len(1);
  for (const std::string& id : ids) {
    const std::size_t len = record_len(id.size());
    if (pos % kSec + len > kSec) pos = (pos / kSec + 1) * kSec;
    pos += len;
  }
  return sectors_for(pos) * kSec;
}

IsoTime later(const IsoTime& a, const IsoTime& b) {
  const auto key = [](const IsoTime& t) {
    return std::tuple(t.year, t.month, t.day, t.hour, t.minute, t.second);
  };
  return key(a) < key(b) ? b : a;
}

IsoTime time_of_file(const fs::path& p) {
  std::error_code ec;
  const auto ft = fs::last_write_time(p, ec);
  IsoTime t;
  if (ec) return t;
  std::time_t tt = 0;
  try {
    tt = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(ft));
  } catch (const std::exception&) {
    return t;  // no time zone data on this system: keep the default date
  }
  std::tm tm{};
  if (gmtime_s(&tm, &tt) != 0) return t;
  t.year = std::clamp(tm.tm_year + 1900, 1970, 2155);
  t.month = tm.tm_mon + 1;
  t.day = tm.tm_mday;
  t.hour = tm.tm_hour;
  t.minute = tm.tm_min;
  t.second = std::min(tm.tm_sec, 59);
  return t;
}

}  // namespace

// ----------------------------------------------------------------------- build

IsoLayout IsoLayout::build(std::vector<IsoSource> sources, std::string_view volume_id,
                           std::string_view system_id) {
  if (sources.empty()) fail("a disc image needs at least one file");
  if (volume_id.size() > 32 || !std::all_of(volume_id.begin(), volume_id.end(), is_d_char))
    fail("volume id '{}' is not valid (up to 32 characters A-Z, 0-9, _)", volume_id);

  // ---- directory tree --------------------------------------------------------
  std::vector<Dir> dirs(1);
  IsoTime newest;
  for (std::size_t fi = 0; fi < sources.size(); ++fi) {
    const IsoSource& s = sources[fi];
    if (s.size >= (1ull << 32))
      fail("'{}' is {} bytes; a single ISO 9660 file extent is limited to 4 GiB", s.iso_path, s.size);
    newest = later(newest, s.time);
    int cur = 0;
    std::size_t start = 0, depth = 1;
    while (true) {
      const std::size_t sep = s.iso_path.find('/', start);
      const std::string part = s.iso_path.substr(start, sep == std::string::npos ? sep : sep - start);
      if (sep == std::string::npos) {
        check_file_name(part, s.iso_path);
        if (dirs[cur].subdirs.count(part) || !dirs[cur].files.emplace(part, fi).second)
          fail("'{}' appears twice in the disc image", s.iso_path);
        break;
      }
      check_dir_name(part, s.iso_path);
      if (dirs[cur].files.count(part)) fail("'{}' is both a file and a folder in the disc image", part);
      if (++depth > 8) fail("'{}' is nested deeper than the 8 levels ISO 9660 allows", s.iso_path);
      const auto it = dirs[cur].subdirs.find(part);
      if (it != dirs[cur].subdirs.end()) {
        cur = it->second;
      } else {
        Dir d;
        d.name = part;
        d.parent = cur;
        const int idx = static_cast<int>(dirs.size());
        dirs[cur].subdirs.emplace(part, idx);
        dirs.push_back(std::move(d));
        cur = idx;
      }
      start = sep + 1;
    }
  }
  for (Dir& d : dirs) d.time = newest;

  // ---- path table order: breadth first, children sorted by name ----------
  std::vector<int> order = {0};
  for (std::size_t i = 0; i < order.size(); ++i)
    for (const auto& [name, idx] : dirs[order[i]].subdirs) order.push_back(idx);
  std::size_t path_table_bytes = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    Dir& d = dirs[order[i]];
    d.number = static_cast<std::uint16_t>(i + 1);
    const std::size_t id_len = d.name.empty() ? 1 : d.name.size();
    path_table_bytes += 8 + id_len + (id_len % 2);
  }

  // ---- sector assignment -------------------------------------------------------
  const std::uint32_t pt_sectors = sectors_for(path_table_bytes);
  const std::uint32_t l_table = kFirstFreeSector;
  const std::uint32_t m_table = l_table + pt_sectors;
  std::uint32_t next = m_table + pt_sectors;

  const auto ids_of = [&](const Dir& d) {
    std::vector<std::string> ids;
    for (const auto& [name, idx] : d.subdirs) ids.push_back(name);
    for (const auto& [name, fi] : d.files) ids.push_back(name + ";1");
    std::sort(ids.begin(), ids.end());
    return ids;
  };
  for (int di : order) {
    Dir& d = dirs[di];
    d.bytes = dir_extent_bytes(ids_of(d));
    d.sector = next;
    next += d.bytes / kSec;
  }
  const std::uint32_t data_start = next;

  IsoLayout layout;
  std::vector<std::uint32_t> file_sector(sources.size(), 0);
  for (int di : order) {
    for (const auto& [name, fi] : dirs[di].files) {
      const IsoSource& s = sources[fi];
      const std::uint32_t n = sectors_for(s.size);
      file_sector[fi] = n ? next : 0;
      if (n) layout.files_.push_back({s.iso_path, s.host_path, next, s.size});
      else layout.files_.push_back({s.iso_path, s.host_path, 0, 0});
      if (static_cast<std::uint64_t>(next) + n > 0xFFFFFFFFull) fail("the disc image would exceed 2^32 sectors");
      next += n;
    }
  }
  std::stable_sort(layout.files_.begin(), layout.files_.end(),
                   [](const FileExtent& a, const FileExtent& b) { return a.sector < b.sector; });
  layout.total_sectors_ = next;

  // ---- metadata bytes -------------------------------------------------------------
  Bytes& b = layout.metadata_;
  b.assign(static_cast<std::size_t>(data_start) * kSec, 0);

  // primary volume descriptor
  std::size_t p = static_cast<std::size_t>(kPvdSector) * kSec;
  b[p] = 1;
  put_text(b, p + 1, 5, "CD001");
  b[p + 6] = 1;
  put_text(b, p + 8, 32, system_id);
  put_text(b, p + 40, 32, volume_id);
  put32both(b, p + 80, layout.total_sectors_);
  put16both(b, p + 120, 1);
  put16both(b, p + 124, 1);
  put16both(b, p + 128, static_cast<std::uint16_t>(kSec));
  put32both(b, p + 132, static_cast<std::uint32_t>(path_table_bytes));
  put32le(b, p + 140, l_table);
  put32be(b, p + 148, m_table);
  {
    std::size_t root_pos = p + 156;
    put_record(b, root_pos, std::string_view("\0", 1), dirs[0].sector, dirs[0].bytes, true, newest);
  }
  put_text(b, p + 190, 128, "");   // volume set
  put_text(b, p + 318, 128, "");   // publisher
  put_text(b, p + 446, 128, "");   // data preparer
  put_text(b, p + 574, 128, "HOTPURSUIT2PC CONVERTER");  // application
  put_text(b, p + 702, 37, "");
  put_text(b, p + 739, 37, "");
  put_text(b, p + 776, 37, "");
  put_long_date(b, p + 813, newest);
  put_long_date(b, p + 830, newest);
  put_text(b, p + 847, 16, "0000000000000000");
  put_text(b, p + 864, 16, "0000000000000000");
  b[p + 881] = 1;

  // volume descriptor set terminator
  p = static_cast<std::size_t>(kPvdSector + 1) * kSec;
  b[p] = 255;
  put_text(b, p + 1, 5, "CD001");
  b[p + 6] = 1;

  // path tables (L = little endian, M = big endian)
  std::size_t lp = static_cast<std::size_t>(l_table) * kSec, mp = static_cast<std::size_t>(m_table) * kSec;
  for (int di : order) {
    const Dir& d = dirs[di];
    const std::string_view id = d.name.empty() ? std::string_view("\0", 1) : std::string_view(d.name);
    const std::uint16_t parent = dirs[d.parent].number;
    b[lp] = b[mp] = static_cast<std::uint8_t>(id.size());
    put32le(b, lp + 2, d.sector);
    put32be(b, mp + 2, d.sector);
    put16le(b, lp + 6, parent);
    put16be(b, mp + 6, parent);
    for (std::size_t i = 0; i < id.size(); ++i) b[lp + 8 + i] = b[mp + 8 + i] = static_cast<std::uint8_t>(id[i]);
    const std::size_t len = 8 + id.size() + (id.size() % 2);
    lp += len;
    mp += len;
  }

  // directory extents
  for (int di : order) {
    const Dir& d = dirs[di];
    const Dir& parent = dirs[d.parent];
    std::size_t pos = static_cast<std::size_t>(d.sector) * kSec;
    put_record(b, pos, std::string_view("\0", 1), d.sector, d.bytes, true, d.time);
    put_record(b, pos, std::string_view("\1", 1), parent.sector, parent.bytes, true, parent.time);
    for (const std::string& id : ids_of(d)) {
      const auto sub = d.subdirs.find(id);
      if (sub != d.subdirs.end()) {
        const Dir& c = dirs[sub->second];
        put_record(b, pos, id, c.sector, c.bytes, true, c.time);
      } else {
        const std::size_t fi = d.files.at(id.substr(0, id.size() - 2));
        put_record(b, pos, id, file_sector[fi], static_cast<std::uint32_t>(sources[fi].size), false,
                   sources[fi].time);
      }
    }
  }
  return layout;
}

IsoLayout iso_layout_for_rom(const fs::path& rom_root, const RomProfile& profile) {
  std::vector<IsoSource> sources;
  std::error_code ec;
  for (const RomManifestEntry& e : profile.files) {
    const fs::path host = rom_root / from_utf8(e.path);
    if (!fs::is_regular_file(host, ec)) {
      if (e.optional) continue;
      fail("'{}' is missing from the ROM folder - validate the ROM first", e.path);
    }
    IsoSource s;
    s.iso_path = e.path;
    for (char& c : s.iso_path)
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    s.host_path = host;
    s.size = e.size;
    s.time = time_of_file(host);
    sources.push_back(std::move(s));
  }
  std::string volume = profile.serial;  // "SLUS-20362" -> "SLUS_20362"
  for (char& c : volume)
    if (!is_d_char(c)) c = '_';
  return IsoLayout::build(std::move(sources), volume);
}

// ------------------------------------------------------------------- write_iso

void write_iso(const IsoLayout& layout, const fs::path& out_path, const ProgressFn& progress) {
  OutputFile out(out_path);
  Progress p;
  p.stage = "image";
  p.total = layout.total_bytes();
  out.write(layout.metadata());
  std::uint64_t pos = layout.metadata().size();
  p.done = pos;
  Bytes buf(kChunk);
  const Bytes zeros(kSec, 0);
  for (const IsoLayout::FileExtent& f : layout.files()) {
    if (f.size == 0) continue;
    if (pos != static_cast<std::uint64_t>(f.sector) * kSec)
      fail("internal error: image position {} does not match the planned sector {} of '{}'", pos,
           f.sector, f.iso_path);
    const ReadOnlyFile in(f.host_path);
    if (in.size() != f.size)
      fail("'{}' is {} bytes but {} were planned - the ROM folder changed; validate it again",
           to_utf8(f.host_path), in.size(), f.size);
    std::uint64_t done = 0;
    while (done < f.size) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(f.size - done, kChunk));
      in.read_at(done, std::span<std::uint8_t>(buf.data(), n));
      out.write(ByteView(buf.data(), n));
      done += n;
      p.done = pos + done;
      if (progress) {
        p.item = f.iso_path;
        progress(p);
      }
    }
    pos += f.size;
    if (const std::uint64_t rem = pos % kSec) {
      out.write(ByteView(zeros.data(), static_cast<std::size_t>(kSec - rem)));
      pos += kSec - rem;
    }
  }
  if (pos != layout.total_bytes())
    fail("internal error: wrote {} bytes, planned {}", pos, layout.total_bytes());
  out.commit();
}

// ---------------------------------------------------------- read_iso_directory

namespace {

std::string text_field(ByteView b, std::size_t off, std::size_t width) {
  std::string s(reinterpret_cast<const char*>(b.data() + off), width);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

std::uint32_t both32(ByteView b, std::size_t off, std::string_view what) {
  const std::uint32_t le = read_u32le(b, off);
  const std::uint32_t be = (static_cast<std::uint32_t>(b[off + 4]) << 24) | (static_cast<std::uint32_t>(b[off + 5]) << 16) |
                           (static_cast<std::uint32_t>(b[off + 6]) << 8) | b[off + 7];
  if (le != be) fail("disc image: the two byte orders of {} disagree ({} vs {})", what, le, be);
  return le;
}

void walk(const ReadOnlyFile& iso, std::uint32_t sector, std::uint32_t bytes, const std::string& prefix,
          int depth, IsoInfo& info) {
  if (depth > 8) fail("disc image: directories nested deeper than 8 levels");
  if (bytes == 0 || bytes % kSec != 0 || bytes > (16u << 20))
    fail("disc image: directory '{}' has an implausible size of {} bytes", prefix, bytes);
  const Bytes dir = iso.read_at(static_cast<std::uint64_t>(sector) * kSec, bytes);
  std::size_t pos = 0;
  while (pos < dir.size()) {
    const std::size_t len = dir[pos];
    if (len == 0) {  // rest of this sector is padding
      pos = (pos / kSec + 1) * kSec;
      continue;
    }
    if (len < 34 || pos + len > dir.size() || pos % kSec + len > kSec)
      fail("disc image: malformed directory record in '{}' at offset {}", prefix, pos);
    const ByteView rec(dir.data() + pos, len);
    const std::size_t id_len = rec[32];
    if (33 + id_len > len) fail("disc image: directory record name overruns its record in '{}'", prefix);
    const std::uint32_t ext = both32(rec, 2, "an extent position");
    const std::uint32_t size = both32(rec, 10, "an extent size");
    const bool is_dir = (rec[25] & 0x02) != 0;
    std::string id(reinterpret_cast<const char*>(rec.data() + 33), id_len);
    pos += len;
    if (id == std::string(1, '\0') || id == std::string(1, '\1')) continue;  // "." and ".."
    if (const std::size_t semi = id.find(';'); semi != std::string::npos) id.resize(semi);
    const std::uint64_t end = static_cast<std::uint64_t>(ext) * kSec + size;
    if (size && end > iso.size())
      fail("disc image: '{}{}' lies outside the image (sector {}, {} bytes)", prefix, id, ext, size);
    info.entries.push_back({prefix + id, ext, size, is_dir});
    if (is_dir) walk(iso, ext, size, prefix + id + "/", depth + 1, info);
  }
}

}  // namespace

IsoInfo read_iso_directory(const ReadOnlyFile& iso) {
  if (iso.size() < static_cast<std::uint64_t>(kPvdSector + 2) * kSec)
    fail("'{}' is too small to be a disc image", to_utf8(iso.path()));
  const Bytes pvd = iso.read_at(static_cast<std::uint64_t>(kPvdSector) * kSec, kSec);
  if (pvd[0] != 1 || std::string_view(reinterpret_cast<const char*>(pvd.data() + 1), 5) != "CD001")
    fail("'{}' has no ISO 9660 primary volume descriptor at sector 16", to_utf8(iso.path()));
  IsoInfo info;
  info.system_id = text_field(pvd, 8, 32);
  info.volume_id = text_field(pvd, 40, 32);
  info.volume_sectors = both32(pvd, 80, "the volume size");
  if (static_cast<std::uint64_t>(info.volume_sectors) * kSec > iso.size())
    fail("disc image: the volume claims {} sectors but the file holds only {}", info.volume_sectors,
         iso.size() / kSec);
  const ByteView root(pvd.data() + 156, 34);
  walk(iso, both32(root, 2, "the root directory position"), both32(root, 10, "the root directory size"), "", 1, info);
  return info;
}

}  // namespace hp2
