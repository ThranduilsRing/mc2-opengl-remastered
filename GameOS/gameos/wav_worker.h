#pragma once
#include "gos_profiler.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <climits>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using WavChunk = std::unique_ptr<Mix_Chunk, decltype(&Mix_FreeChunk)>;

// packaged_task futures never wait on destruction. Dropping a resource cancels
// its delivery; the task owns the path and frees any abandoned PCM itself.
struct WavDelivery {
    std::future<WavChunk> future;
    bool take(WavChunk& destination) {
        if (!future.valid() || future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        destination = future.get(); // Synchronizes with completion of the entire decode.
        return true;
    }
};

class WavWorker {
public:
    WavWorker() : thread_([this] {
        for (;;) {
            std::packaged_task<WavChunk()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }) {}
    ~WavWorker() {
        { std::lock_guard lock(mutex_); stopping_ = true; tasks_.clear(); }
        ready_.notify_one();
        thread_.join(); // Shutdown only, before closing SDL_mixer.
    }
    WavDelivery request(std::string path) {
        std::packaged_task<WavChunk()> task([path = std::move(path)] {
            ZoneScopedN("Audio.Wav.Worker");
            ZoneText(path.c_str(), path.size());
            size_t size = 0;
            std::unique_ptr<void, decltype(&SDL_free)> bytes(nullptr, SDL_free);
            { ZoneScopedN("Audio.Stream.LoadFile.Read"); bytes.reset(SDL_LoadFile(path.c_str(), &size)); }
            WavChunk chunk(nullptr, Mix_FreeChunk);
            if (!bytes || size > static_cast<size_t>(INT_MAX)) return chunk;
            auto* input = SDL_RWFromConstMem(bytes.get(), static_cast<int>(size));
            if (!input) return chunk;
            { ZoneScopedN("Audio.Stream.LoadFile.Decode"); chunk.reset(Mix_LoadWAV_RW(input, 1)); }
            return chunk; // Mix_LoadWAV produces the opened mixer's output format.
        });
        WavDelivery delivery{task.get_future()};
        { std::lock_guard lock(mutex_); tasks_.push_back(std::move(task)); }
        ready_.notify_one();
        return delivery;
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<WavChunk()>> tasks_;
    bool stopping_ = false;
    std::thread thread_;
};
