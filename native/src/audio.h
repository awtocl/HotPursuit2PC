// Audio output of the native runtime: XAudio2 (xaudio2_9.dll / xaudio2_8.dll, Windows system DLLs, loaded at run time).
// A mixer modelled on the PS2's SPU2 at 48 kHz: 2 MiB of sound memory, 48 voices playing PS-ADPCM from it (pitch 0x1000 =
// 48 kHz, loops from the blocks' flags), and disc streams (the EA streamer's music and speech) decoded from the disc.
// All functions are thread-safe; without an audio device (Init failed or not called) voices and streams do not play.
#pragma once
#include <cstdint>

namespace audio {

using Reader = bool (*)(std::uint64_t offset, void* out, std::uint64_t count);  // thread-safe disc read

bool Init();  // false: no audio device or XAudio2 missing (the runtime stays silent)
void Shutdown();
bool Available();
const char* LastError();  // why Init failed

// sound memory (filled whether or not a device is open)
void SpuWrite(std::uint32_t address, const void* data, std::uint32_t bytes);

// voices 0-47; volumes 0..0x3FFF (full), pitch 0x1000 = 48 kHz
void VoiceSetup(int voice, std::uint32_t address, std::uint16_t pitch, std::uint16_t left, std::uint16_t right);
void VoiceKeyOn(int voice);
void VoiceKeyOff(int voice);  // short release, then the voice is free
void VoicePitch(int voice, std::uint16_t pitch);
void VoiceVolume(int voice, std::uint16_t left, std::uint16_t right);
bool VoicePlaying(int voice, std::uint32_t* address = nullptr, std::uint32_t* start = nullptr);

// disc streams 0-7: PS-ADPCM, stereo streams alternate left and right in blocks of `interleave` bytes
void StreamStart(int stream, Reader read, std::uint64_t start, std::uint32_t bytes, int channels, std::uint32_t interleave,
                 std::uint16_t pitch, std::uint16_t left, std::uint16_t right);
void StreamQueue(int stream, std::uint64_t start, std::uint32_t bytes);  // plays after what is playing or queued
void StreamVolume(int stream, int left, int right);  // 0..0x3FFF, -1 keeps that side; moves there over ~20 ms
void StreamPitch(int stream, std::uint16_t pitch);  // 0 pauses the stream (the game's pause menu), other values resume at that rate
void StreamStop(int stream);
bool StreamPlaying(int stream);
std::uint64_t StreamFramesPlayed(int stream);  // sample frames played since StreamStart

}  // namespace audio
