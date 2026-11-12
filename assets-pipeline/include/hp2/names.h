// File names of the SLUS-20362 Z fileset, recovered from the owner's dump
// (docs/dump-analysis.md §4, ADR 0004). Compiled in from
// assets-pipeline/data/hp2_slus20362_names.txt.
#pragma once

#include <optional>

#include "hp2/common.h"

namespace hp2 {

struct NameEntry {
  std::uint32_t hash = 0;
  std::string_view name;  // upper-case, '\' separators, e.g. "GLOBAL\\GLOBALA.BUN"
  char tier = 'A';        // confidence tier A..D, see docs/dump-analysis.md §4
};

class NameTable {
 public:
  // The built-in table for SLUS-20362 v1.00, sorted by hash.
  static const NameTable& builtin();

  explicit NameTable(std::vector<NameEntry> entries);

  const NameEntry* find(std::uint32_t hash) const;
  const std::vector<NameEntry>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<NameEntry> entries_;
};

// True if the name is safe to use as a relative output path: components of
// [A-Z0-9_ .-] separated by single '\', no "..", no drive, no leading/trailing
// separator, no component ending in '.' or ' '.
bool is_safe_relative_name(std::string_view name);

}  // namespace hp2
