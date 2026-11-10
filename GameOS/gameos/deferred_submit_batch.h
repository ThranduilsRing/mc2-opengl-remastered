#pragma once
// The ordering logic of VulkanContext's deferred submit batch, with no Vulkan in it (Cb is any handle type; Cb{} = null). The context holds one of
// these under submitMutex_; tests/unit/test_deferred_submit_batch.cpp drives it directly.
//
// An entry is one timeline value plus (usually) one command buffer. submitRecorded() reserves an entry whose command buffer does not exist yet; the
// recording fills it at resolve(). A prefix of the batch goes to the queue as one submit that signals the LAST entry's value, so queue order = entry
// order and an unresolved entry may never be overtaken by a later one. Rule: only the thread that can run the recordings (the render thread, "main")
// may queue past an unresolved entry, and only after resolve(). The present worker queues the prefix before the first reservation and flags a
// violation: the present enqueue resolves on main, so a reservation still pending on the worker means the ordering was broken upstream.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace mc2vk {

template <class Cb>
class DeferredSubmitBatch {
public:
    using Clock = std::chrono::steady_clock;
    using RecordFn = std::function<bool(Cb)>;
    using DoneFn = std::function<void(bool ok)>;

    struct Entry { Cb cb{}; uint64_t value = 0; };   // cb null = a reservation not resolved yet, or one that failed
    struct Record { size_t entry = 0; RecordFn rec; DoneFn done; Cb cb{}; bool ok = false; Clock::time_point reservedAt{}; };
    struct Flush { std::vector<Cb> cbs; uint64_t value = 0; size_t count = 0; };   // count 0 = nothing to queue
    struct Plan { bool resolveFirst = false; bool violation = false; size_t count = 0; };

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    bool hasRecords() const { return !records_.empty(); }
    size_t firstRecordEntry() const { return records_.front().entry; }
    Clock::time_point firstRecordReservedAt() const { return records_.front().reservedAt; }
    Clock::time_point since() const { return since_; }
    const std::vector<Entry>& entries() const { return entries_; }

    void addPlain(Cb cb, uint64_t value, Clock::time_point now) {
        if (entries_.empty()) since_ = now;
        entries_.push_back({cb, value});
    }
    void reserve(uint64_t value, RecordFn rec, DoneFn done, Clock::time_point now) {
        if (entries_.empty()) since_ = now;
        entries_.push_back({Cb{}, value});
        Record r;
        r.entry = entries_.size() - 1;
        r.rec = std::move(rec);
        r.done = std::move(done);
        r.reservedAt = now;
        records_.push_back(std::move(r));
    }

    // Runs every recording (acquire() hands a command buffer; run(n, body) executes body(i) for i in [0,n), any order, joined), then fills the
    // entries in reserved order and calls done(ok) in reserved order. A failed recording leaves its entry's cb null: its value is still signalled.
    // The batch is already consistent when done() runs (the callbacks may not call back into it).
    template <class Acquire, class Run>
    void resolve(Acquire acquire, Run run) {
        if (records_.empty()) return;
        const int n = static_cast<int>(records_.size());
        const auto recordOne = [&](int i) {
            Record& r = records_[static_cast<size_t>(i)];
            r.cb = acquire();
            r.ok = r.cb != Cb{} && r.rec(r.cb);
        };
        run(n, recordOne);
        // A thread without a lane (more threads than lanes) records here.
        for (Record& r : records_)
            if (r.cb == Cb{}) { r.cb = acquire(); r.ok = r.cb != Cb{} && r.rec(r.cb); }
        std::vector<Record> recs = std::move(records_);
        records_.clear();
        for (Record& r : recs) entries_[r.entry].cb = r.ok ? r.cb : Cb{};
        for (Record& r : recs) if (r.done) r.done(r.ok);
    }

    // What a flush point may queue. No reservation: everything. Reservations on a thread that can resolve: resolve, then everything. On the present
    // worker: only the prefix before the first reservation, and that is an ordering violation (the enqueue on main should have resolved them).
    Plan planFlush(bool onWorker) const {
        Plan p;
        if (records_.empty()) { p.count = entries_.size(); return p; }
        if (onWorker) { p.violation = true; p.count = records_.front().entry; return p; }
        p.resolveFirst = true;
        p.count = entries_.size();
        return p;
    }

    // Removes the first `count` entries; the caller queues cbs as one submit signalling `value` (the last removed entry's).
    Flush takePrefix(size_t count) {
        Flush f;
        count = count < entries_.size() ? count : entries_.size();
        if (count == 0) return f;
        f.count = count;
        f.value = entries_[count - 1].value;
        f.cbs.reserve(count);
        for (size_t i = 0; i < count; ++i)
            if (entries_[i].cb != Cb{}) f.cbs.push_back(entries_[i].cb);
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count));
        for (Record& r : records_) r.entry -= count;
        return f;
    }

private:
    std::vector<Entry> entries_;
    std::vector<Record> records_;
    Clock::time_point since_{};
};

}  // namespace mc2vk
