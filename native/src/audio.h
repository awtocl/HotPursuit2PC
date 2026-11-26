// Audio output of the native runtime: XAudio2 (xaudio2_9.dll / xaudio2_8.dll, Windows system DLLs, loaded at run time).
// Music: the EA streamer's stream 0 is PS-ADPCM stereo read straight from the disc (left and right interleaved in blocks
// of `interleave` bytes), decoded on an audio thread and played at the stream's pitch.
#pragma once
#include <cstdint>

namespace audio {

using Reader = bool (*)(std::uint64_t offset, void* out, std::uint64_t count);  // thread-safe disc read

bool Init();  // false: no audio device or XAudio2 missing (the runtime stays silent)
void Shutdown();
bool Available();
const char* LastError();  // why Init failed

// Starts a stereo PS-ADPCM stream at disc byte `start`, `bytes` long; replaces the current one.
void MusicStart(Reader read, std::uint64_t start, std::uint32_t bytes, std::uint32_t interleave, std::uint32_t rate, float volume);
void MusicStop();
bool MusicFinished();             // the data ended (end flag or length) and everything queued has been played
std::uint64_t MusicFramesPlayed();  // sample frames played since MusicStart

}  // namespace audio
