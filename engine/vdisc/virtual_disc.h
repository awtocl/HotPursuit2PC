// Virtual disc: presents the user's ROM folder to the PS2 core as a DVD image
// without writing one. The sectors are the ISO 9660 layout computed by
// hp2/iso.h (metadata held in memory, file data read straight from the
// 27 files of the ROM folder, which are opened read-only).
//
// This header is deliberately plain C++17 with no hp2 includes: the launcher
// translation unit that uses it also includes the Play! headers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hp2
{
	class VirtualDisc
	{
	public:
		// rom_dir: the extracted disc, or the rom\ folder of a game folder (which
		// may contain only rom-reference.tsv pointing at the real location).
		// The ROM is validated first (same quick check as the converter).
		// Returns null and an explanation (UTF-8) if it cannot be used.
		static std::shared_ptr<VirtualDisc> Open(const std::wstring& rom_dir, std::string& error);

		~VirtualDisc();

		std::uint64_t Size() const;
		// Copies up to count bytes starting at offset; returns the number copied
		// (less than count only at the end of the disc). Throws std::runtime_error
		// if a ROM file cannot be read.
		std::uint64_t Read(std::uint64_t offset, void* out, std::uint64_t count) const;
		// The disc file that holds the byte at `offset` ("NFSHP2/ZZDATA.BIN"), or "(metadata)" / "(gap)".
		std::string FileAt(std::uint64_t offset) const;
		// One line for the log, e.g. "SLUS-20362 from D:\...\rom (27 files, 3120594944 bytes)".
		const std::string& Description() const;

	private:
		VirtualDisc();
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
