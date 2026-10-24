// Owned source bytes cross the worker boundary; File/FastFile extraction stays on its calling thread.
#pragma once
#include "TextureService.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace RenderCore {
class TextureDecodeWorker {
public:
    struct Result { TextureHandle handle; std::shared_ptr<const KtxImage> image; uint32_t contentVersion=0; };
    TextureDecodeWorker() : worker_([this](std::stop_token stop) { run(stop); }) {}
    ~TextureDecodeWorker() {
        { std::lock_guard lock(mutex_); worker_.request_stop(); }
        ready_.notify_all();
    }
    void request(TextureHandle handle, std::vector<uint8_t> bytes, uint32_t contentVersion=0) {
        request(handle,[bytes=std::move(bytes)] {
            auto parsed=ktxParse(bytes);
            return parsed.image ? std::make_shared<const KtxImage>(std::move(*parsed.image)) : nullptr;
        },contentVersion);
    }
    void request(TextureHandle handle, std::function<std::shared_ptr<const KtxImage>()> decode, uint32_t contentVersion=0) {
        { std::lock_guard lock(mutex_); requests_.push_back({handle,std::move(decode),contentVersion}); }
        ready_.notify_one();
    }
    std::deque<Result> take() {
        std::lock_guard lock(mutex_); std::deque<Result> out; out.swap(results_); return out;
    }
    // Load barriers need both the queued and currently decoding request published.
    void waitIdle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&] { return requests_.empty() && !active_; });
    }
private:
    struct Request { TextureHandle handle; std::function<std::shared_ptr<const KtxImage>()> decode; uint32_t contentVersion=0; };
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stop.stop_requested() || !requests_.empty(); });
                if (stop.stop_requested()) return;
                request=std::move(requests_.front()); requests_.pop_front();
                active_=true;
            }
            Result result{request.handle,request.decode(),request.contentVersion};
            { std::lock_guard lock(mutex_); results_.push_back(std::move(result)); active_=false; }
            idle_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    bool active_=false;
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

// Decoded requests remain owned until the whole command buffer is submitted.
// A refused batch publishes nothing and returns every request for a later pump.
template<class Uploads, class Pending, class Ticket>
void finishTextureUploadBatch(Uploads& uploads, size_t firstNew, Pending& pending,
                              Pending& batch, TextureStagingBudget& budget, Ticket ticket) {
    if (ticket.gfx) {
        for (size_t i=firstNew; i<uploads.size(); ++i) uploads[i].ticket=ticket;
        batch.clear();
    } else {
        while (uploads.size() > firstNew) {
            budget.complete(uploads.back().bytes);
            uploads.pop_back();
        }
        while (!batch.empty()) {
            pending.push_front(std::move(batch.back()));
            batch.pop_back();
        }
    }
}

// Completed images retain their slot reservation and staging charge until publication
// succeeds (or the source is cancelled). A full table must never strand a live handle.
template<class Uploads, class Residents, class Complete>
void publishTextureUploads(TextureService& service, TextureDescriptorTable& table,
                           Uploads& uploads, Residents& residents, TextureStagingBudget& budget,
                           Complete complete) {
    while (!uploads.empty() && complete(uploads.front().ticket)) {
        auto& upload=uploads.front();
        auto it=residents.find(upload.handle.raw());
        if (it!=residents.end()) {
            auto& [raw,resident]=*it;
            if (service.isLive(upload.handle) && resident.contentVersion==upload.contentVersion) {
                const auto slot=table.insert(upload.handle,upload.image);
                if (!slot) break;
                resident.image=std::move(upload.image);
                resident.descriptor=slot;
            }
        }
        budget.complete(upload.bytes);
        uploads.pop_front();
    }
}
} // namespace RenderCore
