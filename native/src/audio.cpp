// Audio output (XAudio2) and an SPU2-style mixer. PS-ADPCM ("VAG") is the SPU's sample format: 16-byte blocks of 28
// samples, byte 0 = predictor << 4 | shift, byte 1 = flags (bit 0 end of a loop, bit 1 repeat, bit 2 loop start), then 14
// bytes of 4-bit samples. A voice reaching a block with bit 0 jumps to its loop start when bit 1 is set, else it stops.
#include "audio.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace audio {
namespace {

using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

constexpr std::uint32_t kRate = 48000, kFrames = 256, kQueued = 4;  // ~21 ms queued ahead
constexpr std::uint32_t kSpuSize = 2u << 20, kVoices = 48, kStreams = 8;

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_out = nullptr;
HANDLE g_event = nullptr;
std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mutex;
std::vector<std::uint8_t> g_spu(kSpuSize);

int DecodeBlock(const std::uint8_t* blk, int hist[2], std::int16_t* out, std::size_t stride) {  // returns the flags byte
  static const int kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};
  const int pred = std::min(blk[0] >> 4, 4), shift = blk[0] & 15;
  for (int i = 0; i < 28; ++i) {
    const int nib = (blk[2 + i / 2] >> ((i & 1) * 4)) & 15;
    int s = shift > 12 ? 0 : static_cast<std::int16_t>(static_cast<std::uint16_t>(nib << 12)) >> shift;
    s += (hist[0] * kF0[pred] + hist[1] * kF1[pred] + 32) >> 6;
    s = std::clamp(s, -32768, 32767);
    hist[1] = hist[0];
    hist[0] = s;
    out[static_cast<std::size_t>(i) * stride] = static_cast<std::int16_t>(s);
  }
  return blk[1];
}

float Vol(std::uint16_t v) { return static_cast<float>(std::min<std::uint16_t>(v & 0x7FFF, 0x3FFF)) / 16383.0f; }

struct Voice {
  bool on = false, release = false, last = false;
  std::uint32_t start = 0, addr = 0, block = 0, loop = 0, pitch = 0x1000, frac = 0;
  int hist[2] = {}, pos = 28, s0 = 0, s1 = 0;
  std::int16_t pcm[28] = {};
  float left = 0, right = 0, env = 0;
  int Next() {  // next source sample; clears `on` after the last block
    if (pos == 28) {
      if (last) {
        on = false;
        return 0;
      }
      block = addr & (kSpuSize - 16);
      const int flags = DecodeBlock(g_spu.data() + block, hist, pcm, 1);
      if (flags & 4) loop = block;
      if (flags & 1) {
        if (flags & 2) addr = loop;
        else last = true;
      } else {
        addr = block + 16;
      }
      pos = 0;
    }
    return pcm[pos++];
  }
} g_voice[kVoices];

struct Stream {
  bool on = false, eof = false;
  Reader read = nullptr;
  int channels = 2;
  std::uint64_t start = 0, generation = 0, played = 0;
  std::uint32_t bytes = 0, pos = 0, interleave = 0x8000, pitch = 0x1000, frac = 0;
  std::deque<std::pair<std::uint64_t, std::uint32_t>> queue;
  int hist[2][2] = {}, s0[2] = {}, s1[2] = {};
  std::vector<std::int16_t> pcm[2];
  std::size_t rd = 0;
  float left = 1, right = 1;
  std::size_t Available() const { return pcm[0].size() - rd; }
} g_stream[kStreams];

// Reads and decodes the next part of a stream (called with the lock held; releases it while reading the disc).
void Refill(std::unique_lock<std::mutex>& lock, int id, std::vector<std::uint8_t>& raw) {
  Stream& s = g_stream[id];
  if (s.pos >= s.bytes) {
    if (s.queue.empty()) {
      s.eof = true;
      return;
    }
    s.start = s.queue.front().first;
    s.bytes = s.queue.front().second;
    s.queue.pop_front();
    s.pos = 0;
    std::memset(s.hist, 0, sizeof s.hist);
  }
  const std::uint32_t per = s.channels == 2 ? s.interleave : 0x4000;
  const std::uint32_t n = std::min(per * static_cast<std::uint32_t>(s.channels), s.bytes - s.pos) & ~15u;
  if (n == 0) {
    s.pos = s.bytes;
    return;
  }
  raw.resize(n);
  const Reader read = s.read;
  const std::uint64_t at = s.start + s.pos, generation = s.generation;
  lock.unlock();
  const bool ok = read && read(at, raw.data(), n);
  lock.lock();
  if (s.generation != generation || !s.on) return;  // replaced or stopped while reading
  s.pos += n;
  if (!ok) {
    s.eof = true;
    return;
  }
  const std::uint32_t half = s.channels == 2 ? n / 2 : n;
  const std::size_t blocks = half / 16;
  if (s.rd > 0) {
    for (int c = 0; c < s.channels; ++c) s.pcm[c].erase(s.pcm[c].begin(), s.pcm[c].begin() + static_cast<std::ptrdiff_t>(s.rd));
    s.rd = 0;
  }
  for (int c = 0; c < s.channels; ++c) {
    const std::size_t base = s.pcm[c].size();
    s.pcm[c].resize(base + blocks * 28);
    std::size_t b = 0;
    for (; b < blocks; ++b) {
      const int flags = DecodeBlock(raw.data() + c * half + b * 16, s.hist[c], s.pcm[c].data() + base + b * 28, 1);
      if ((flags & 3) == 1) {  // end of data
        ++b;
        s.pos = s.bytes;
        s.queue.clear();
        break;
      }
    }
    s.pcm[c].resize(base + b * 28);
  }
  if (s.channels == 2) {  // keep both channels the same length
    const std::size_t m = std::min(s.pcm[0].size(), s.pcm[1].size());
    s.pcm[0].resize(m);
    s.pcm[1].resize(m);
  }
}

void Mix(float* out) {  // kFrames stereo frames, lock held
  std::memset(out, 0, sizeof(float) * kFrames * 2);
  for (Voice& v : g_voice) {
    if (!v.on) continue;
    for (std::uint32_t f = 0; f < kFrames && v.on; ++f) {
      v.frac += v.pitch;
      while (v.frac >= 0x1000 && v.on) {
        v.frac -= 0x1000;
        v.s0 = v.s1;
        v.s1 = v.Next();
      }
      const float s = (static_cast<float>(v.s0) + static_cast<float>(v.s1 - v.s0) * static_cast<float>(v.frac) / 4096.0f) / 32768.0f;
      if (v.release) {
        v.env -= 1.0f / (kRate * 0.03f);  // 30 ms release
        if (v.env <= 0) {
          v.on = false;
          break;
        }
      } else if (v.env < 1.0f) {
        v.env = std::min(1.0f, v.env + 1.0f / 64.0f);  // no click at key on
      }
      out[f * 2] += s * v.left * v.env;
      out[f * 2 + 1] += s * v.right * v.env;
    }
  }
  for (Stream& s : g_stream) {
    if (!s.on) continue;
    for (std::uint32_t f = 0; f < kFrames; ++f) {
      s.frac += s.pitch;
      while (s.frac >= 0x1000) {
        if (s.Available() == 0) break;
        s.frac -= 0x1000;
        for (int c = 0; c < s.channels; ++c) {
          s.s0[c] = s.s1[c];
          s.s1[c] = s.pcm[c][s.rd];
        }
        ++s.rd;
        ++s.played;
      }
      if (s.Available() == 0 && s.eof) {
        s.on = false;
        break;
      }
      float o[2];
      for (int c = 0; c < s.channels; ++c)
        o[c] = (static_cast<float>(s.s0[c]) + static_cast<float>(s.s1[c] - s.s0[c]) * static_cast<float>(std::min<std::uint32_t>(s.frac, 0x1000)) / 4096.0f) / 32768.0f;
      if (s.channels == 1) o[1] = o[0];
      out[f * 2] += o[0] * s.left;
      out[f * 2 + 1] += o[1] * s.right;
    }
  }
}

class Callback : public IXAudio2VoiceCallback {
 public:
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
  void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
  void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override { SetEvent(g_event); }
  void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
} g_callback;

void Worker() {
  std::vector<std::int16_t> bufs[kQueued + 2];
  for (auto& b : bufs) b.resize(kFrames * 2);
  std::vector<float> mix(kFrames * 2);
  std::vector<std::uint8_t> raw;
  std::size_t next = 0;
  // development: HP2_AUDIO_DUMP=1 writes the mixed output (48 kHz 16-bit stereo, up to 10 minutes) to .cache/native/audio.raw
  FILE* dump = nullptr;
  std::uint64_t dumped = 0;
  if (std::getenv("HP2_AUDIO_DUMP") && fopen_s(&dump, ".cache/native/audio.raw", "wb") != 0) dump = nullptr;
  while (!g_quit) {
    WaitForSingleObject(g_event, 20);
    for (;;) {
      XAUDIO2_VOICE_STATE st{};
      g_out->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
      if (st.BuffersQueued >= kQueued || g_quit) break;
      {
        std::unique_lock<std::mutex> lock(g_mutex);
        for (int id = 0; id < static_cast<int>(kStreams); ++id) {
          Stream& s = g_stream[id];
          // keep about a quarter of a second decoded
          for (int guard = 0; s.on && !s.eof && s.Available() < 12000 && guard < 4; ++guard) Refill(lock, id, raw);
        }
        Mix(mix.data());
      }
      std::vector<std::int16_t>& pcm = bufs[next];
      next = (next + 1) % (kQueued + 2);
      for (std::size_t i = 0; i < mix.size(); ++i) pcm[i] = static_cast<std::int16_t>(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f);
      if (dump && dumped < static_cast<std::uint64_t>(kRate) * 600) {
        fwrite(pcm.data(), 4, kFrames, dump);
        dumped += kFrames;
        if (dumped % (kRate * 5) < kFrames) fflush(dump);
      }
      XAUDIO2_BUFFER xb = {};
      xb.AudioBytes = kFrames * 4;
      xb.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
      g_out->SubmitSourceBuffer(&xb);
    }
  }
  if (dump) fclose(dump);
}

}  // namespace

char g_error[128] = "";
const char* LastError() { return g_error; }

bool Init() {
  if (g_out) return true;
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
  if (const char* q = std::getenv("HP2_AUDIO_SILENT"); q && *q && *q != '0') g_master->SetVolume(0.0f);  // tests: everything runs, nothing is heard
  WAVEFORMATEX wf = {};
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 2;
  wf.nSamplesPerSec = kRate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = 4;
  wf.nAvgBytesPerSec = kRate * 4;
  g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  hr = g_xa->CreateSourceVoice(&g_out, &wf, 0, 2.0f, &g_callback);
  if (FAILED(hr)) {
    snprintf(g_error, sizeof g_error, "CreateSourceVoice failed (%08lX)", static_cast<unsigned long>(hr));
    Shutdown();
    return false;
  }
  g_out->Start();
  g_quit = false;
  g_thread = std::thread(Worker);
  return true;
}

void Shutdown() {
  g_quit = true;
  if (g_event) SetEvent(g_event);
  if (g_thread.joinable()) g_thread.join();
  if (g_out) g_out->DestroyVoice();
  g_out = nullptr;
  if (g_master) g_master->DestroyVoice();
  g_master = nullptr;
  if (g_xa) g_xa->Release();
  g_xa = nullptr;
}

bool Available() { return g_out != nullptr; }

void SpuWrite(std::uint32_t address, const void* data, std::uint32_t bytes) {
  std::lock_guard<std::mutex> lock(g_mutex);
  address &= kSpuSize - 1;
  bytes = std::min(bytes, kSpuSize - address);
  std::memcpy(g_spu.data() + address, data, bytes);
}

void VoiceSetup(int voice, std::uint32_t address, std::uint16_t pitch, std::uint16_t left, std::uint16_t right) {
  if (voice < 0 || voice >= static_cast<int>(kVoices)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Voice& v = g_voice[voice];
  v.start = address & (kSpuSize - 16);
  v.pitch = std::min<std::uint32_t>(pitch, 0x3FFF);
  v.left = Vol(left);
  v.right = Vol(right);
}

void VoiceKeyOn(int voice) {
  if (voice < 0 || voice >= static_cast<int>(kVoices) || !g_out) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Voice& v = g_voice[voice];
  v.on = true;
  v.release = v.last = false;
  v.addr = v.loop = v.block = v.start;
  v.frac = 0;
  v.pos = 28;
  v.hist[0] = v.hist[1] = v.s0 = v.s1 = 0;
  v.env = 0;
}

void VoiceKeyOff(int voice) {
  if (voice < 0 || voice >= static_cast<int>(kVoices)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_voice[voice].release = true;
}

void VoicePitch(int voice, std::uint16_t pitch) {
  if (voice < 0 || voice >= static_cast<int>(kVoices)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_voice[voice].pitch = std::min<std::uint32_t>(pitch, 0x3FFF);
}

void VoiceVolume(int voice, std::uint16_t left, std::uint16_t right) {
  if (voice < 0 || voice >= static_cast<int>(kVoices)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_voice[voice].left = Vol(left);
  g_voice[voice].right = Vol(right);
}

bool VoicePlaying(int voice, std::uint32_t* address, std::uint32_t* start) {
  if (voice < 0 || voice >= static_cast<int>(kVoices)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Voice& v = g_voice[voice];
  if (address) *address = v.block;
  if (start) *start = v.start;
  return v.on;
}

void StreamStart(int stream, Reader read, std::uint64_t start, std::uint32_t bytes, int channels, std::uint32_t interleave,
                 std::uint16_t pitch, std::uint16_t left, std::uint16_t right) {
  if (stream < 0 || stream >= static_cast<int>(kStreams) || !g_out) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Stream& s = g_stream[stream];
  s.on = true;
  s.eof = false;
  s.read = read;
  s.start = start;
  s.bytes = bytes;
  s.pos = 0;
  s.channels = channels == 1 ? 1 : 2;
  s.interleave = std::clamp<std::uint32_t>(interleave & ~15u, 0x800, 0x40000);
  s.pitch = std::clamp<std::uint32_t>(pitch, 0x100, 0x3FFF);
  s.frac = 0;
  s.left = Vol(left);
  s.right = Vol(right);
  s.queue.clear();
  std::memset(s.hist, 0, sizeof s.hist);
  s.s0[0] = s.s0[1] = s.s1[0] = s.s1[1] = 0;
  s.pcm[0].clear();
  s.pcm[1].clear();
  s.rd = 0;
  s.played = 0;
  ++s.generation;
}

void StreamQueue(int stream, std::uint64_t start, std::uint32_t bytes) {
  if (stream < 0 || stream >= static_cast<int>(kStreams)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Stream& s = g_stream[stream];
  if (!s.on) return;
  s.queue.emplace_back(start, bytes);
  s.eof = false;
}

void StreamStop(int stream) {
  if (stream < 0 || stream >= static_cast<int>(kStreams)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stream[stream].on = false;
  g_stream[stream].queue.clear();
  ++g_stream[stream].generation;
}

bool StreamPlaying(int stream) {
  if (stream < 0 || stream >= static_cast<int>(kStreams)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_stream[stream].on;
}

std::uint64_t StreamFramesPlayed(int stream) {
  if (stream < 0 || stream >= static_cast<int>(kStreams)) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_stream[stream].played;
}

}  // namespace audio
