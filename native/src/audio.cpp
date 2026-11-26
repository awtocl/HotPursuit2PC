// Audio output (XAudio2) and the music stream decoder. PS-ADPCM ("VAG") is the SPU's sample format: 16-byte blocks of
// 28 samples, byte 0 = predictor << 4 | shift, byte 1 = flags (bit 0 end of data), then 14 bytes of 4-bit samples.
#include "audio.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
constexpr std::uint32_t kBaseRate = 32000;

std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_quit{false};

struct Music {
  bool active = false, ended = false;
  std::uint64_t generation = 0;
  Reader read = nullptr;
  std::uint64_t start = 0;
  std::uint32_t bytes = 0, interleave = 0x8000, rate = kBaseRate, pos = 0;
  float volume = 1.0f;
  int hist[2][2] = {};
  std::uint64_t playedBase = 0;  // SamplesPlayed of the voice when this stream started
  std::uint64_t startTick = 0;   // GetTickCount64 at MusicStart
} g_music;
std::atomic<bool> g_finished{true};

// Decodes up to `blocks` PS-ADPCM blocks; returns the number decoded, stops after a block with the end flag.
std::size_t Decode(const std::uint8_t* in, std::size_t blocks, int hist[2], std::int16_t* out, std::size_t stride, bool& end) {
  static const int kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};
  std::size_t b = 0;
  for (; b < blocks; ++b) {
    const std::uint8_t* blk = in + b * 16;
    const int pred = std::min(blk[0] >> 4, 4), shift = blk[0] & 15;
    for (int i = 0; i < 28; ++i) {
      const int nib = (blk[2 + i / 2] >> ((i & 1) * 4)) & 15;
      int s = static_cast<std::int16_t>(static_cast<std::uint16_t>(nib << 12)) >> shift;
      s += (hist[0] * kF0[pred] + hist[1] * kF1[pred] + 32) >> 6;
      s = std::clamp(s, -32768, 32767);
      hist[1] = hist[0];
      hist[0] = s;
      out[(b * 28 + static_cast<std::size_t>(i)) * stride] = static_cast<std::int16_t>(s);
    }
    if (blk[1] & 1) {
      end = true;
      return b + 1;
    }
  }
  return b;
}

void Worker() {
  std::vector<std::int16_t> bufs[4];
  int next = 0;
  std::uint64_t seen = 0;
  std::vector<std::uint8_t> raw;
  while (!g_quit) {
    Sleep(5);
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_music.generation != seen) {  // new stream or stop: drop what is queued
      seen = g_music.generation;
      g_voice->Stop();
      g_voice->FlushSourceBuffers();
      for (XAUDIO2_VOICE_STATE st{}; g_voice->GetState(&st, 0), st.BuffersQueued > 0;) Sleep(1);
      XAUDIO2_VOICE_STATE st{};
      g_voice->GetState(&st, 0);
      g_music.playedBase = st.SamplesPlayed;
      if (g_music.active) {
        g_voice->SetFrequencyRatio(static_cast<float>(g_music.rate) / static_cast<float>(kBaseRate));
        g_voice->SetVolume(g_music.volume);
        g_voice->Start();
      }
    }
    if (!g_music.active) continue;
    // the game starts and stops every track of the playlist in a burst before settling on one: wait until it settles
    if (GetTickCount64() - g_music.startTick < 250) continue;
    XAUDIO2_VOICE_STATE st{};
    g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (g_music.ended) {
      if (st.BuffersQueued == 0) g_finished = true;
      continue;
    }
    if (st.BuffersQueued >= 3) continue;
    // one interleave block per channel: left then right
    const std::uint32_t il = g_music.interleave;
    if (g_music.pos >= g_music.bytes) {
      g_music.ended = true;
      continue;
    }
    raw.resize(static_cast<std::size_t>(il) * 2);
    const Reader read = g_music.read;
    const std::uint64_t at = g_music.start + g_music.pos;
    lock.unlock();
    const bool ok = read && read(at, raw.data(), raw.size());
    lock.lock();
    if (g_music.generation != seen) continue;  // replaced while reading
    if (!ok) {
      g_music.ended = true;
      continue;
    }
    std::vector<std::int16_t>& pcm = bufs[next];
    next = (next + 1) & 3;
    const std::size_t blocks = il / 16;
    pcm.assign(blocks * 28 * 2, 0);
    bool endL = false, endR = false;
    const std::size_t nl = Decode(raw.data(), blocks, g_music.hist[0], pcm.data(), 2, endL);
    const std::size_t nr = Decode(raw.data() + il, blocks, g_music.hist[1], pcm.data() + 1, 2, endR);
    const std::size_t n = std::min(nl, nr);
    g_music.pos += il * 2;
    if (endL || endR) g_music.ended = true;
    if (n == 0) continue;
    // development: the first 20 s of the latest track as raw 16-bit stereo (HP2_AUDIO_DUMP=1), restarted with every track
    static const bool dumpOn = std::getenv("HP2_AUDIO_DUMP") != nullptr;
    static FILE* dump = nullptr;
    static std::size_t dumped = 0;
    static std::uint64_t dumpGen = 0;
    if (dumpOn && dumpGen != seen) {
      if (dump && dumped < kBaseRate * 20) fclose(dump);
      dump = nullptr;
      if (fopen_s(&dump, ".cache/native/music.raw", "wb") != 0) dump = nullptr;
      dumped = 0;
      dumpGen = seen;
    }
    if (dump && dumped < kBaseRate * 20) {
      fwrite(pcm.data(), 4, n * 28, dump);
      dumped += n * 28;
      if (dumped >= kBaseRate * 20) fclose(dump);
    }
    XAUDIO2_BUFFER xb = {};
    xb.AudioBytes = static_cast<UINT32>(n * 28 * 2 * sizeof(std::int16_t));
    xb.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
    g_voice->SubmitSourceBuffer(&xb);
  }
}

}  // namespace

char g_error[128] = "";
const char* LastError() { return g_error; }

bool Init() {
  HMODULE dll = LoadLibraryW(L"xaudio2_9.dll");
  if (!dll) dll = LoadLibraryW(L"xaudio2_8.dll");
  const auto create = dll ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(dll, "XAudio2Create"))) : nullptr;
  if (!create) {
    snprintf(g_error, sizeof g_error, "XAudio2 DLL or XAudio2Create missing (error %lu)", GetLastError());
    return false;
  }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT hr = create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    snprintf(g_error, sizeof g_error, "XAudio2Create failed (%08lX)", static_cast<unsigned long>(hr));
    return false;
  }
  hr = g_xa->CreateMasteringVoice(&g_master);
  if (FAILED(hr)) {
    snprintf(g_error, sizeof g_error, "CreateMasteringVoice failed (%08lX)", static_cast<unsigned long>(hr));
    Shutdown();
    return false;
  }
  WAVEFORMATEX wf = {};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 2;
  wf.nSamplesPerSec = kBaseRate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = 4;
  wf.nAvgBytesPerSec = kBaseRate * 4;
  hr = g_xa->CreateSourceVoice(&g_voice, &wf, 0, 4.0f);
  if (FAILED(hr)) {
    snprintf(g_error, sizeof g_error, "CreateSourceVoice failed (%08lX)", static_cast<unsigned long>(hr));
    Shutdown();
    return false;
  }
  g_quit = false;
  g_thread = std::thread(Worker);
  return true;
}

void Shutdown() {
  g_quit = true;
  if (g_thread.joinable()) g_thread.join();
  if (g_voice) g_voice->DestroyVoice();
  g_voice = nullptr;
  if (g_master) g_master->DestroyVoice();
  g_master = nullptr;
  if (g_xa) g_xa->Release();
  g_xa = nullptr;
}

bool Available() { return g_voice != nullptr; }

void MusicStart(Reader read, std::uint64_t start, std::uint32_t bytes, std::uint32_t interleave, std::uint32_t rate, float volume) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_music.active = true;
  g_music.ended = false;
  g_music.read = read;
  g_music.start = start;
  g_music.bytes = bytes;
  g_music.interleave = std::clamp<std::uint32_t>(interleave & ~15u, 0x800, 0x40000);
  g_music.rate = std::clamp<std::uint32_t>(rate, 4000, 96000);
  g_music.volume = std::clamp(volume, 0.0f, 1.0f);
  g_music.pos = 0;
  g_music.startTick = GetTickCount64();
  g_music.hist[0][0] = g_music.hist[0][1] = g_music.hist[1][0] = g_music.hist[1][1] = 0;
  ++g_music.generation;
  g_finished = false;
}

void MusicStop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_music.active = false;
  ++g_music.generation;
  g_finished = true;
}

bool MusicFinished() { return g_finished; }

std::uint64_t MusicFramesPlayed() {
  if (!g_voice) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  XAUDIO2_VOICE_STATE st{};
  g_voice->GetState(&st, 0);
  return st.SamplesPlayed - g_music.playedBase;
}

}  // namespace audio
