#pragma once
// vk_render_pass_timer_ring.h -- the frame ring of the VK pass timer, without Vulkan: which query pair a bracket owns,
// which brackets a frame publishes, and when a frame is dropped.
//
// WHY separate: the rules are where the timer can go wrong silently (a stale result published as current, a whole frame
// dropped for one missing pass), so they live in plain code the unit tests drive with a fake query reader.
// Rules:
//  * a bracket is read only if it was closed (End) and not cancelled (its command buffer was not submitted, so its
//    queries still hold whatever an older frame left there);
//  * at FrameEnd the oldest slot, which the next frame reuses, is published if every readable bracket is ready and
//    dropped (counted) otherwise; younger slots wait, oldest first;
//  * a hard read error discards the slot and is reported apart from drops, so the caller can log it once.

#include <cstdint>
#include <vector>

namespace vk_render_pass_timer {

enum class ReadResult { Ready, NotReady, Error };

// Runs `cancel` when the scope ends unless submitted() was called: an error return between a bracket's End and the queue
// submit must cancel the bracket, and one guard serves every island instead of a Cancel before each return.
template <class CancelFn>
class SubmitGuard {
public:
    explicit SubmitGuard(CancelFn cancel) : cancel_(cancel) {}
    ~SubmitGuard() { if (!submitted_) cancel_(); }
    void submitted() { submitted_ = true; }
    SubmitGuard(const SubmitGuard&) = delete;
    SubmitGuard& operator=(const SubmitGuard&) = delete;
private:
    CancelFn cancel_;
    bool submitted_ = false;
};

template <int kPasses, int kRing>
class Ring {
public:
    struct Bracket {
        int      pass = -1;
        uint32_t query = 0;      // first of the bracket's two queries
        bool     closed = false; // End stamped and not cancelled
    };
    struct FrameEndCounts {
        unsigned published = 0, dropped = 0, errors = 0;
    };

    explicit Ring(uint32_t bracketsPerSlot) : perSlot_(bracketsPerSlot) {
        for (int p = 0; p < kPasses; ++p) open_[p] = last_[p] = -1;
    }

    // The query pair for a new bracket of `pass` in the current frame; -1 when the pass is already open or the frame is full.
    int64_t begin(int pass) {
        if (pass < 0 || pass >= kPasses || open_[pass] >= 0) return -1;
        Slot& s = ring_[cur_];
        if (s.brackets.size() >= perSlot_) { ++overflow_; return -1; }
        Bracket b;
        b.pass = pass;
        b.query = (static_cast<uint32_t>(cur_) * perSlot_ + static_cast<uint32_t>(s.brackets.size())) * 2u;
        open_[pass] = static_cast<int>(s.brackets.size());
        s.brackets.push_back(b);
        return b.query;
    }

    // The end query of the pass's open bracket, which is now closed; -1 when no bracket is open.
    int64_t end(int pass) {
        if (pass < 0 || pass >= kPasses || open_[pass] < 0) return -1;
        Bracket& b = ring_[cur_].brackets[static_cast<size_t>(open_[pass])];
        b.closed = true;
        last_[pass] = open_[pass];
        open_[pass] = -1;
        return b.query + 1u;
    }

    // The pass's latest bracket of this frame was recorded on a command buffer that will not be submitted: never read it.
    void cancel(int pass) {
        if (pass < 0 || pass >= kPasses) return;
        Slot& s = ring_[cur_];
        const int i = (open_[pass] >= 0) ? open_[pass] : last_[pass];
        if (i >= 0) s.brackets[static_cast<size_t>(i)].closed = false;
        open_[pass] = last_[pass] = -1;
    }

    // Close the frame, advance, and settle the slots: read(b, &ms) -> ReadResult; publish(ms[kPasses], has[kPasses]).
    template <class Read, class Publish>
    FrameEndCounts frameEnd(Read&& read, Publish&& publish) {
        FrameEndCounts c;
        for (int p = 0; p < kPasses; ++p) open_[p] = last_[p] = -1;   // an open bracket is never read
        Slot& closing = ring_[cur_];
        closing.pending = !closing.brackets.empty();
        cur_ = (cur_ + 1) % kRing;
        for (int k = 0; k < kRing; ++k) {
            Slot& s = ring_[(cur_ + k) % kRing];
            if (!s.pending) continue;
            double ms[kPasses] = {};
            bool   has[kPasses] = {};
            bool   any = false;
            ReadResult r = ReadResult::Ready;
            for (const Bracket& b : s.brackets) {
                if (!b.closed) continue;
                double one = 0.0;
                r = read(b, &one);
                if (r != ReadResult::Ready) break;
                ms[b.pass] += one;
                has[b.pass] = true;
                any = true;
            }
            if (r == ReadResult::Ready) {
                // A frame whose brackets were all cancelled has nothing to say: publishing it would report an empty frame as a sample.
                if (any) {
                    publish(ms, has);
                    ++c.published;
                }
            } else if (r == ReadResult::Error) {
                ++c.errors;
            } else if (k == 0) {
                ++c.dropped;
            } else {
                break;   // younger slots cannot be finished either
            }
            s.pending = false;
            s.brackets.clear();
        }
        return c;
    }

    int current() const { return cur_; }
    unsigned takeOverflow() { const unsigned n = overflow_; overflow_ = 0; return n; }

private:
    struct Slot {
        std::vector<Bracket> brackets;
        bool pending = false;
    };
    uint32_t perSlot_;
    Slot     ring_[kRing];
    int      cur_ = 0;
    int      open_[kPasses];
    int      last_[kPasses];
    unsigned overflow_ = 0;
};

}  // namespace vk_render_pass_timer
