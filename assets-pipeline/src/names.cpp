#include "hp2/names.h"

#include <algorithm>
#include <iterator>

namespace hp2 {

namespace {

struct RawName {
  std::uint32_t hash;
  const char* name;
  char tier;
};

// Generated at configure time from data/hp2_slus20362_names.txt.
constexpr RawName kBuiltinNames[] = {
#include "hp2_names.inc"
};

}  // namespace

NameTable::NameTable(std::vector<NameEntry> entries) : entries_(std::move(entries)) {
  std::sort(entries_.begin(), entries_.end(),
            [](const NameEntry& a, const NameEntry& b) { return a.hash < b.hash; });
}

const NameTable& NameTable::builtin() {
  static const NameTable table = [] {
    std::vector<NameEntry> v;
    v.reserve(std::size(kBuiltinNames));
    for (const RawName& r : kBuiltinNames) v.push_back({r.hash, r.name, r.tier});
    return NameTable(std::move(v));
  }();
  return table;
}

const NameEntry* NameTable::find(std::uint32_t hash) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                             [](const NameEntry& e, std::uint32_t h) { return e.hash < h; });
  return (it != entries_.end() && it->hash == hash) ? &*it : nullptr;
}

bool is_safe_relative_name(std::string_view name) {
  if (name.empty() || name.size() > 200) return false;
  std::size_t start = 0;
  while (true) {
    const std::size_t sep = name.find('\\', start);
    const std::string_view part = name.substr(start, sep == std::string_view::npos ? sep : sep - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (part.back() == '.' || part.back() == ' ' || part.front() == ' ') return false;
    for (char ch : part) {
      const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                      ch == ' ' || ch == '.' || ch == '-';
      if (!ok) return false;
    }
    if (sep == std::string_view::npos) break;
    start = sep + 1;
  }
  return true;
}

}  // namespace hp2
