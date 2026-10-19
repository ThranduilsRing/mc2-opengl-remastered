// Owned source bytes cross the worker boundary; File/FastFile extraction stays on its calling thread.
#pragma once
#include "TextureService.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace RenderCore {
class TextureDecodeWorker {
public:
    struct Result { TextureHandle handle; std::shared_ptr<const KtxImage> image; };
    TextureDecodeWorker() : worker_([this](std::stop_token stop) { run(stop); }) {}
    ~TextureDecodeWorker() {
        { std::lock_guard lock(mutex_); worker_.request_stop(); }
        ready_.notify_all();
    }
    void request(TextureHandle handle, std::vector<uint8_t> bytes) {
        request(handle,[bytes=std::move(bytes)] {
            auto parsed=ktxParse(bytes);
            return parsed.image ? std::make_shared<const KtxImage>(std::move(*parsed.image)) : nullptr;
        });
    }
    void request(TextureHandle handle, std::function<std::shared_ptr<const KtxImage>()> decode) {
        { std::lock_guard lock(mutex_); requests_.push_back({handle,std::move(decode)}); }
        ready_.notify_one();
    }
    std::deque<Result> take() {
        std::lock_guard lock(mutex_); std::deque<Result> out; out.swap(results_); return out;
    }
private:
    struct Request { TextureHandle handle; std::function<std::shared_ptr<const KtxImage>()> decode; };
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stop.stop_requested() || !requests_.empty(); });
                if (stop.stop_requested()) return;
                request=std::move(requests_.front()); requests_.pop_front();
            }
            Result result{request.handle,request.decode()};
            { std::lock_guard lock(mutex_); results_.push_back(std::move(result)); }
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> requests_;
    std::deque<Result> results_;
    std::jthread worker_; // joins before the queues and mutex die
};

struct TextureStagingBudget {
    size_t limit = 64u*1024u*1024u, used = 0;
    bool reserve(size_t bytes) {
        if (used && (used >= limit || bytes > limit-used)) return false;
        used+=bytes; return true;
    }
    void complete(size_t bytes) { assert(bytes<=used); used-=bytes; }
};
} // namespace RenderCore
