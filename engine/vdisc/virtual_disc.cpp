#include "virtual_disc.h"

#include <algorithm>
#include <cstring>

#include "hp2/io.h"
#include "hp2/iso.h"
#include "hp2/rom.h"

namespace hp2 {

struct VirtualDisc::Impl {
  struct Extent {
    std::uint64_t start = 0;  // byte offset in the image
    std::uint64_t size = 0;
    std::size_t file = 0;     // index into files
  };
  Bytes metadata;
  std::vector<Extent> extents;  // ascending start
  std::vector<std::unique_ptr<ReadOnlyFile>> files;
  std::uint64_t total = 0;
  std::string description;
};

VirtualDisc::VirtualDisc() : m_impl(std::make_unique<Impl>()) {}
VirtualDisc::~VirtualDisc() = default;

namespace {

// <game>/rom may hold only "rom-reference.tsv" with a line "source<TAB><path>".
fs::path follow_reference(const fs::path& dir) {
  std::error_code ec;
  const fs::path ref = dir / "rom-reference.tsv";
  if (fs::is_regular_file(dir / "SYSTEM.CNF", ec) || !fs::is_regular_file(ref, ec)) return dir;
  const Bytes raw = read_whole_file(ref, 1u << 20);
  const std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.rfind("source\t", 0) == 0) return from_utf8(line.substr(7));
    pos = eol + 1;
  }
  fail("'{}' does not name the ROM folder (no 'source' line)", to_utf8(ref));
}

}  // namespace

std::shared_ptr<VirtualDisc> VirtualDisc::Open(const std::wstring& rom_dir, std::string& error) {
  try {
    const fs::path dir = follow_reference(fs::path(rom_dir));
    const RomProfile& profile = RomProfile::slus20362();
    const RomReport report = check_rom(dir, RomCheckLevel::Quick, profile);
    if (!report.ok()) {
      error = report.summary();
      return nullptr;
    }
    const IsoLayout layout = iso_layout_for_rom(report.root, profile);

    std::shared_ptr<VirtualDisc> disc(new VirtualDisc());
    Impl& d = *disc->m_impl;
    d.metadata = layout.metadata();
    d.total = layout.total_bytes();
    for (const IsoLayout::FileExtent& f : layout.files()) {
      if (f.size == 0) continue;
      auto file = std::make_unique<ReadOnlyFile>(f.host_path);
      if (file->size() != f.size)
        fail("'{}' changed size ({} bytes, {} expected)", to_utf8(f.host_path), file->size(), f.size);
      d.extents.push_back({static_cast<std::uint64_t>(f.sector) * kSectorSize, f.size, d.files.size()});
      d.files.push_back(std::move(file));
    }
    d.description = std::format("{} from {} ({} files, {} bytes)", profile.serial, to_utf8(report.root),
                                d.files.size(), d.total);
    return disc;
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

std::uint64_t VirtualDisc::Size() const { return m_impl->total; }
const std::string& VirtualDisc::Description() const { return m_impl->description; }

std::uint64_t VirtualDisc::Read(std::uint64_t offset, void* out, std::uint64_t count) const {
  const Impl& d = *m_impl;
  if (offset >= d.total) return 0;
  count = std::min<std::uint64_t>(count, d.total - offset);
  auto* dst = static_cast<std::uint8_t*>(out);
  std::uint64_t done = 0;
  while (done < count) {
    const std::uint64_t pos = offset + done;
    const std::uint64_t left = count - done;
    if (pos < d.metadata.size()) {
      const std::uint64_t n = std::min<std::uint64_t>(left, d.metadata.size() - pos);
      std::memcpy(dst + done, d.metadata.data() + pos, static_cast<std::size_t>(n));
      done += n;
      continue;
    }
    // First extent that starts after pos; the one before it may contain pos.
    const auto next = std::upper_bound(d.extents.begin(), d.extents.end(), pos,
                                       [](std::uint64_t p, const Impl::Extent& e) { return p < e.start; });
    if (next != d.extents.begin()) {
      const Impl::Extent& e = *(next - 1);
      if (pos < e.start + e.size) {
        const std::uint64_t n = std::min<std::uint64_t>(left, e.start + e.size - pos);
        d.files[e.file]->read_at(pos - e.start, std::span<std::uint8_t>(dst + done, static_cast<std::size_t>(n)));
        done += n;
        continue;
      }
    }
    // Sector padding after a file (or space before the next one): zeros.
    const std::uint64_t until = next != d.extents.end() ? next->start : d.total;
    const std::uint64_t n = std::min<std::uint64_t>(left, until - pos);
    std::memset(dst + done, 0, static_cast<std::size_t>(n));
    done += n;
  }
  return done;
}

}  // namespace hp2
