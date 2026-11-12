// "COMP" / .LZC streams - format verified in docs/dump-analysis.md §3.3.
#pragma once

#include "hp2/common.h"

namespace hp2 {

inline constexpr std::size_t kLzcHeaderSize = 16;
inline constexpr std::uint32_t kLzcVersion = 0x00001001u;
// Largest unpacked size accepted. The biggest stream on the retail disc
// unpacks to 14.4 MB; anything far above that is treated as corruption.
inline constexpr std::uint32_t kLzcMaxUnpacked = 256u * 1024u * 1024u;

struct LzcHeader {
  std::uint32_t version = 0;
  std::uint32_t unpacked_size = 0;
  std::uint32_t packed_size = 0;  // whole stream, header included
};

// True if the data starts with the "COMP" magic.
bool is_lzc(ByteView data);

// Parses and sanity-checks the 16-byte header. Throws hp2::Error.
LzcHeader lzc_header(ByteView data);

// Decompresses a complete COMP stream. Throws hp2::Error on any
// inconsistency (truncated input, distance before start of output, ...);
// never reads or writes out of bounds.
Bytes lzc_decompress(ByteView data);

}  // namespace hp2
