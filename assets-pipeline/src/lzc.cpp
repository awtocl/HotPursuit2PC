#include "hp2/lzc.h"

namespace hp2 {

bool is_lzc(ByteView data) {
  return data.size() >= 4 && data[0] == 'C' && data[1] == 'O' && data[2] == 'M' && data[3] == 'P';
}

LzcHeader lzc_header(ByteView data) {
  if (data.size() < kLzcHeaderSize)
    fail("LZC stream is {} bytes, shorter than its {}-byte header", data.size(), kLzcHeaderSize);
  if (!is_lzc(data)) fail("not an LZC stream (missing 'COMP' magic)");
  LzcHeader h;
  h.version = read_u32le(data, 4);
  h.unpacked_size = read_u32le(data, 8);
  h.packed_size = read_u32le(data, 12);
  if (h.version != kLzcVersion)
    fail("unsupported LZC version 0x{:08X} (expected 0x{:08X})", h.version, kLzcVersion);
  if (h.packed_size < kLzcHeaderSize || h.packed_size > data.size())
    fail("LZC header says the packed stream is {} bytes but {} bytes are available",
         h.packed_size, data.size());
  if (h.unpacked_size > kLzcMaxUnpacked)
    fail("LZC header claims an unpacked size of {} bytes (limit {})", h.unpacked_size,
         kLzcMaxUnpacked);
  return h;
}

Bytes lzc_decompress(ByteView data) {
  const LzcHeader h = lzc_header(data);
  const std::uint8_t* in = data.data();
  const std::size_t in_end = h.packed_size;
  const std::size_t out_end = h.unpacked_size;
  Bytes out(out_end);
  std::size_t ip = kLzcHeaderSize;
  std::size_t op = 0;

  while (op < out_end) {
    if (in_end - ip < 2)
      fail("LZC stream ends inside a flag word (input offset {}, output {}/{})", ip, op, out_end);
    std::uint32_t flags = static_cast<std::uint32_t>(in[ip]) | (static_cast<std::uint32_t>(in[ip + 1]) << 8);
    ip += 2;
    for (int bit = 0; bit < 16 && op < out_end; ++bit, flags >>= 1) {
      if (flags & 1u) {
        if (in_end - ip < 2)
          fail("LZC stream ends inside a match token (input offset {}, output {}/{})", ip, op, out_end);
        const std::uint32_t b0 = in[ip];
        const std::uint32_t b1 = in[ip + 1];
        ip += 2;
        std::size_t length = (b0 & 0x0Fu) + 3u;
        const std::size_t distance = ((b0 & 0xF0u) << 4) | b1;
        if (distance == 0 || distance > op)
          fail("LZC match at input offset {} points {} bytes back but only {} bytes were produced",
               ip - 2, distance, op);
        if (length > out_end - op) length = out_end - op;
        // Byte-wise on purpose: source and destination overlap when distance < length.
        for (std::size_t i = 0; i < length; ++i, ++op) out[op] = out[op - distance];
      } else {
        if (ip >= in_end)
          fail("LZC stream ends inside literal data (input offset {}, output {}/{})", ip, op, out_end);
        out[op++] = in[ip++];
      }
    }
  }
  return out;
}

}  // namespace hp2
