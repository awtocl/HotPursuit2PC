// Memory card of the native runtime: the I/O side's card server (RPC 80000400, mcserv) served from a folder of ordinary
// files. Port 0 (slot 1) holds the card, backed by <saves>\mc0\; port 1 has no card, so the game never asks which one to use.
// The game's deletions are moved to <saves>\_deleted\ (recoverable) instead of being erased.
#pragma once
#include <cstdint>
#include <filesystem>

namespace mc {

void SetRoot(const std::filesystem::path& savesFolder);
// One call of the card server: `send`/`recv` are guest buffers already bounds-checked by the caller (host pointers).
// Returns false when `fn` is not a card function (the caller keeps its default reply).
bool Service(std::uint32_t fn, const std::uint32_t* send, std::uint32_t sendSize, std::uint32_t* recv, std::uint32_t recvSize,
             std::uint8_t* ram, std::uint32_t ramSize);

}  // namespace mc
