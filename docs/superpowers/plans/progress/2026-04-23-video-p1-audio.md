# Video P1 Spike: Audio Push-PCM Path Decision

**Date:** 2026-04-23
**Status:** DECISION REACHED — Path B

---

## 1. gosAudio Resource-Type Enum (gameos.hpp:578)

```cpp
enum gosAudio_ResourceType {
    gosAudio_CachedFile,          // 0 — load WAV from disk into RAM
    gosAudio_UserMemory,          // 1 — caller-owned PCM buffer + gosAudio_Format descriptor
    gosAudio_UserMemoryDecode,    // 2 — caller-owned WAV blob, lib decodes header
    gosAudio_UserMemoryPlayList,  // 3 — array of WAV pointers played in sequence
    gosAudio_StreamedFile,        // 4 — disk-backed streaming (file remains on disk)
    gosAudio_StreamedMusic,       // 5 — non-PCM/ADPCM music file
    gosAudio_StreamedFP           // 6 — streamed from file pointer
};
```

`gosAudio_UserMemory` (value 1) accepts a pre-converted, pre-allocated PCM buffer.
Its `gosAudio_Format` descriptor is:

```cpp
typedef struct _gosAudio_Format {
    WORD  wFormatTag;         // 1=PCM, 2=ADPCM
    WORD  nChannels;          // 1=Mono, 2=Stereo
    DWORD nSamplesPerSec;     // 44100, 22050, 11025
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;     // 8 or 16
    WORD  cbSize;             // 0 for PCM
} gosAudio_Format;
```

**There is no streaming/push/callback resource type.** `gosAudio_UserMemory` is
the closest: it accepts a full, already-decoded PCM buffer up-front. The lib then
converts it to the mixer's native format during `gosAudio_CreateResource()` and
assigns ownership to an internal `gosAudio` object backed by a `Mix_Chunk`.

---

## 2. Push / Callback / Position API Survey

`grep` for `gosAudio_QueueBuffer`, `gosAudio_FillBuffer`, `gosAudio_SetCallback`,
`gosAudio_GetPosition`, `gosAudio_GetSamplesPlayed` across all GameOS headers and
source produced **zero matches**.

The public gosAudio surface (gameos.hpp:662–720) exposes:

| Function | Purpose |
|---|---|
| `gosAudio_CreateResource(HGOSAUDIO*, gosAudio_ResourceType, ...)` | one-shot resource creation |
| `gosAudio_DestroyResource(HGOSAUDIO*)` | teardown |
| `gosAudio_AssignResourceToChannel(int, HGOSAUDIO)` | assign to a mixer channel |
| `gosAudio_SetChannelPlayMode(int, gosAudio_PlayMode)` | play/loop/stop/pause |
| `gosAudio_GetChannelPlayMode(int)` | returns play state via `Mix_Playing()` |
| `gosAudio_SetChannelSlider(int, gosAudio_Properties, ...)` | volume/pan/freq |
| `gosAudio_GetChannelSlider(int, gosAudio_Properties, ...)` | query slider |
| `gosAudio_GetChannelInfo(int, gosAudio_ChannelInfo*)` | info struct query |

**No push-buffer, no fill-callback, no sample-position query.**

`fCompletionRatio` exists in the `gosAudio_ChannelInfo` struct declaration but is
**never written** in the gameos_sound.cpp implementation — it will always return 0.

**Conclusion: Path A is not available.** gosAudio cannot be called directly for
streaming decoded PCM or for A/V clock queries.

---

## 3. SDL2_mixer Backend Analysis (gameos_sound.cpp)

Key findings from reading the full implementation:

- **`Mix_OpenAudio`** opens at engine init (gameos_sound.cpp:134). Parameters are
  stored in `SoundEngine::{frequency_, format_, channels_}`.
- **`Mix_AllocateChannels(32)`** — 32 effect channels, all already allocated.
- **`Mix_PlayChannel(-1, &chunk, loops)`** — used for all sound effects via
  `gosAudio_SetChannelPlayMode`.
- **No `Mix_HookMusic` call anywhere in gameos_sound.cpp.**
- **No `Mix_SetPostMix` call anywhere.**
- The music channel (channel `-1` in SDL_mixer's music API) is **completely
  unused** by the current implementation. All audio goes through effect channels
  via `Mix_PlayChannel`.

SDL2_mixer music API availability:
- `Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg)`
  is part of SDL2_mixer and is already linked (CMakeLists.txt links SDL2_mixer).
  It takes over the music channel with a fill callback. This is a classic
  push-PCM sink.
- `Mix_GetMusicPosition()` — not applicable here (music handle is NULL with
  HookMusic); we must track position ourselves.

**No conflict risk:** The music channel (Mix_PlayMusic / Mix_HookMusic) is 100%
separate from the 32 effect channels. Taking it over during video playback does
not affect any existing `gosAudio_*` call paths. In-mission voice, SFX, and
ambient all use effect channels. There is no existing music playback to preempt.

---

## 4. Decision: Path B

**Rationale:**
- Path A requires gosAudio to have a streaming/callback API it does not have.
- Path C (decode-to-WAV) wastes disk I/O, adds latency, and loses the ability to
  do audio-master A/V sync.
- Path B via `Mix_HookMusic` is clean: SDL2_mixer is already linked, the music
  channel is 100% free, and `Mix_HookMusic` provides exactly the push-PCM sink
  needed.

**Recommendation: Path B**

---

## 5. Concrete Implementation Plan for Task 12

### 5.1 New function in `mclib/soundsys.cpp` (or a new `mc2video_audio.cpp`)

```cpp
// Called once when video starts; installs the push callback.
// sample_rate / channels / bit_depth must match Mix_OpenAudio params
// (query with Mix_QuerySpec — engine opens at 44100, S16LSB, stereo by default).
void SoundSystem::startVideoPCM(int sample_rate, int channels, int bits);

// Called from the audio fill callback or decoder thread to queue decoded PCM.
// bytes is raw PCM in the mixer's native format.
void SoundSystem::pushVideoPCM(const uint8_t* pcm, int bytes);

// Returns elapsed audio bytes consumed by the fill callback since startVideoPCM.
// Use this / (sample_rate * channels * (bits/8)) to get the audio master clock
// in seconds.
int64_t SoundSystem::getVideoPCMBytesConsumed() const;

// Called when video ends or is aborted.
void SoundSystem::stopVideoPCM();
```

### 5.2 Internal ring buffer

The fill callback runs on SDL's audio thread; the decoder runs on another thread.
Use a lock-free ring buffer (power-of-two byte array + atomic read/write cursors).
`pushVideoPCM` writes; the callback reads. `getVideoPCMBytesConsumed` reads the
atomic read cursor.

### 5.3 SDL2_mixer hook installation

```cpp
// In startVideoPCM():
Mix_HookMusic(videoPCMFillCallback, this);

// Callback signature required by SDL2_mixer:
static void videoPCMFillCallback(void* udata, Uint8* stream, int len);
// Reads up to `len` bytes from the ring buffer into `stream`.
// SDL_memset(stream, 0, len) for any underrun portion (silence).
// Atomically advances read_cursor_ by bytes actually consumed.
```

### 5.4 Audio master clock

```cpp
double SoundSystem::getVideoPCMPositionSeconds() const {
    int64_t bytes = getVideoPCMBytesConsumed();
    return bytes / (double)(sample_rate_ * channels_ * (bits_ / 8));
}
```

This is monotonically accurate — no drift from wall-clock, no SDL timer jitter.

### 5.5 Teardown

```cpp
// In stopVideoPCM():
Mix_HookMusic(NULL, NULL);  // restores SDL_mixer's silence fill
// drain/reset ring buffer
```

### 5.6 Concerns

1. **Chunk size:** `Mix_OpenAudio` opens with `chunksize_=1024` bytes. The fill
   callback fires ~43 times/second at 44100 Hz stereo S16. The ring buffer should
   hold at least 4x the expected FFmpeg decode batch size (FFmpeg typically outputs
   ~8192 samples / ~32 KB per `avcodec_receive_frame` call). Recommend 256 KB ring.

2. **Music channel takeover:** Confirmed no existing music playback — no conflict.
   But note that if a future feature wants `Mix_PlayMusic` during a cutscene, it
   will conflict. Document this in the implementation.

3. **Thread safety:** SDL2_mixer docs say `Mix_HookMusic` is not thread-safe with
   respect to the audio thread. Install before starting the decoder thread; uninstall
   after the decoder thread has joined.

4. **Format matching:** The fill callback must supply audio in the exact format
   Mix_OpenAudio was called with. Query `Mix_QuerySpec(&rate, &fmt, &ch)` at
   `startVideoPCM` time and pass those constraints back to the FFmpeg resampler
   (swresample). Do not assume 44100/S16/stereo — query at runtime.

---

## Summary

| Item | Finding |
|---|---|
| gosAudio push-PCM API | None. Not available. |
| gosAudio position query | `fCompletionRatio` never written; not usable. |
| `Mix_HookMusic` in gameos_sound.cpp | Not present. Music channel is free. |
| SDL2_mixer linked | Yes (CMakeLists). |
| Path chosen | **B — `Mix_HookMusic` adapter in SoundSystem** |
| Key call Task 12 will use | `Mix_HookMusic(videoPCMFillCallback, this)` |
| Position source | Atomic read cursor in ring buffer, converted to seconds |
