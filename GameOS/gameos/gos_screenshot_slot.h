// gos_screenshot_slot.h - the one pending screenshot request and its expiry.
//
// WHAT: holds a queued request (path + completion function) and counts the frames it has waited.
// WHY: a present that never reads back (declined, down, a fallback run) must not block every later request, so a request older than
// kMaxFramesPending frames is dropped. Header-only and engine-free so a doctest can exercise the expiry.
#pragma once

#include <string>
#include <utility>

namespace gos { namespace screenshot {

// Called once the file is written (ok) or the write failed (!ok); w/h are the presented frame size.
using DoneFn = void (*)(const char* path, bool ok, int w, int h);

class Slot {
public:
    static constexpr unsigned int kMaxFramesPending = 300;

    struct Request {
        std::string path;
        DoneFn done = nullptr;
    };

    bool pending() const { return pending_; }
    const std::string& path() const { return req_.path; }

    // False when a request is already waiting.
    bool begin(const char* path, DoneFn done) {
        if (pending_) return false;
        pending_ = true;
        framesWaited_ = 0;
        req_.path = path;
        req_.done = done;
        return true;
    }

    // Once per frame. True when the waiting request just expired; it is moved to *expired and the slot is free again.
    bool tick(Request* expired) {
        if (!pending_) return false;
        if (++framesWaited_ < kMaxFramesPending) return false;
        *expired = take();
        return true;
    }

    // Frees the slot and hands the request to the caller (empty when nothing was pending).
    Request take() {
        Request out = std::move(req_);
        req_ = Request{};
        pending_ = false;
        framesWaited_ = 0;
        return out;
    }

private:
    bool pending_ = false;
    unsigned int framesWaited_ = 0;
    Request req_;
};

}}  // namespace gos::screenshot
