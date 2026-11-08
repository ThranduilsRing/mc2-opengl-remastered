#pragma once
#include "../../3rdparty/dr_wav.h"
#include <SDL2/SDL.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <semaphore>
#include <thread>

// Each active music slot owns a decoder and a fixed queue. The mixer only copies
// ready PCM; disk IO and conversion never run on the audio callback.
class MusicStream {
public:
    static constexpr int ChunkBytes = 4096;
    static constexpr int QueueChunks = 8;
    static constexpr int PrebufferChunks = 4;
    static std::unique_ptr<MusicStream> open(const char* path, int rate, int channels) {
        auto result = std::unique_ptr<MusicStream>(new MusicStream);
        if (!drwav_init_file(&result->wav_, path, nullptr)) return {};
        result->opened_ = true;
        if (result->wav_.channels < 1 || result->wav_.channels > 8 ||
            result->wav_.sampleRate < 1000 || result->wav_.sampleRate > 384000 ||
            !result->wav_.totalPCMFrameCount) return {};
        result->convert_ = SDL_NewAudioStream(AUDIO_S16SYS, static_cast<Uint8>(result->wav_.channels),
            static_cast<int>(result->wav_.sampleRate), AUDIO_S16SYS, static_cast<Uint8>(channels), rate);
        if (!result->convert_) return {};
        for (int i = 0; i < PrebufferChunks; ++i) {
            result->free_.acquire();
            if (!result->produce()) return {};
        }
        result->worker_ = std::thread([self = result.get()] {
            SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
            while (true) {
                self->free_.acquire();
                if (self->stop_.load()) break;
                if (!self->produce()) { self->failed_ = true; break; }
            }
        });
        return result;
    }
    ~MusicStream() {
        stop_ = true;
        free_.release();
        if (worker_.joinable()) worker_.join();
        if (convert_) SDL_FreeAudioStream(convert_);
        if (opened_) drwav_uninit(&wav_);
    }
    void setLoop(bool loop) { loop_ = loop; }
    bool ended() const { return ended_.load(); }
    // Returns bytes copied. A short read is silence on underrun, never a decode.
    int read(void* destination, int length) {
        auto* out = static_cast<Uint8*>(destination);
        std::memset(out, 0, length);
        int copied = 0;
        while (copied < length && !ended_) {
            const auto r = read_.load(std::memory_order_relaxed);
            if (r == write_.load(std::memory_order_acquire)) {
                if (failed_) ended_ = true;
                break;
            }
            const auto& chunk = queue_[r % QueueChunks];
            const int limit = !loop_ && chunk.endOffset >= 0 ? chunk.endOffset : chunk.size;
            const int count = std::min(length - copied, limit - offset_);
            std::memcpy(out + copied, chunk.pcm.data() + offset_, count);
            copied += count; offset_ += count;
            if (!loop_ && offset_ == chunk.endOffset) { ended_ = true; break; }
            if (offset_ == chunk.size) {
                offset_ = 0;
                read_.store(r + 1, std::memory_order_release);
                free_.release();
            }
        }
        return copied;
    }
private:
    struct Chunk { std::array<Uint8, ChunkBytes> pcm{}; int size = 0; int endOffset = -1; };
    bool produce() {
        const auto w = write_.load(std::memory_order_relaxed);
        auto& chunk = queue_[w % QueueChunks];
        chunk.size = 0;
        chunk.endOffset = -1;
        while (chunk.size < ChunkBytes) {
            const int remaining = ChunkBytes - chunk.size;
            while (SDL_AudioStreamAvailable(convert_) < remaining && !eof_) {
                std::array<drwav_int16, 2048 * 8> pcm;
                const auto frames = drwav_read_pcm_frames_s16(&wav_, 2048, pcm.data());
                if (frames) {
                    if (SDL_AudioStreamPut(convert_, pcm.data(), static_cast<int>(frames * wav_.channels * 2)) < 0) return false;
                } else {
                    eof_ = true;
                    if (SDL_AudioStreamFlush(convert_) < 0) return false;
                }
            }
            const int count = SDL_AudioStreamGet(convert_, chunk.pcm.data() + chunk.size, remaining);
            if (count < 0) return false;
            chunk.size += count;
            cycleBytes_ += count;
            if (eof_ && SDL_AudioStreamAvailable(convert_) == 0) {
                if (!cycleBytes_) return false;
                if (firstCycle_) { chunk.endOffset = chunk.size; firstCycle_ = false; }
                SDL_AudioStreamClear(convert_);
                if (!drwav_seek_to_pcm_frame(&wav_, 0)) return false;
                cycleBytes_ = 0;
                eof_ = false;
            }
        }
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    MusicStream() = default;
    drwav wav_{};
    SDL_AudioStream* convert_ = nullptr;
    bool opened_ = false, eof_ = false, firstCycle_ = true;
    size_t cycleBytes_ = 0;
    std::array<Chunk, QueueChunks> queue_;
    std::counting_semaphore<QueueChunks + 1> free_{QueueChunks};
    std::atomic<size_t> read_{0}, write_{0};
    std::atomic<bool> stop_{false}, loop_{false}, ended_{false}, failed_{false};
    int offset_ = 0;
    std::thread worker_;
};
