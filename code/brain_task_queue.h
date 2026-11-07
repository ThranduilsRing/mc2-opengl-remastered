#pragma once

// BrainTaskQueue is mission-runtime state. It is persisted by the save v2 BRN1 section (code/save_brain_section.cpp).

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <span>
#include <optional>
#include "mc2fmt.h"

// Task types - added in BRAIN-RUNTIME-1B.
// NOTE: avoid names that collide with legacy #define macros (e.g. dstd.h #define NONE -1).
// BrainTaskType prefix avoids pollution; enum class scope provides additional guard.
enum class BrainTaskType : uint8_t {
    BRAIN_TASK_NONE      = 0,  // 1A placeholder / unknown
    BRAIN_TASK_HOLD      = 1,  // issue TACTICAL_ORDER_STOP to the unit this slice
};

struct BrainTaskEntry {
    uint8_t       priority_tier;       // 0=Logistics..6=Background
    uint32_t      insertion_frame_ms;  // (uint32_t)(scenarioTime*1000+0.5) at push time — NOT render frame
    uint32_t      stable_seq_id;       // per-queue monotone counter incremented on push
    int32_t       warrior_id;          // vehicleWID — stable numeric, NOT pointer-derived
    BrainTaskType type;                // task type (BRAIN-RUNTIME-1B)
    uint8_t       _reserved[7];        // zeroed; reduced from 8 to fit type field
};

class BrainTaskQueue {
public:
    static constexpr int MAX_BRAIN_TASKS = 8;

    // push: returns false if full (soft-fail, no crash). Default task type = NONE.
    bool push(uint8_t tier, uint32_t frame_ms, int32_t warrior_id) {
        return pushTyped(tier, frame_ms, warrior_id, BrainTaskType::BRAIN_TASK_NONE, nullptr);
    }
    bool push(uint8_t tier, uint32_t frame_ms, int32_t warrior_id,
              BrainTaskType task_type, const uint8_t* payload7 = nullptr) {
        return pushTyped(tier, frame_ms, warrior_id, task_type, payload7);
    }

private:
    bool pushTyped(uint8_t tier, uint32_t frame_ms, int32_t warrior_id,
                   BrainTaskType task_type, const uint8_t* payload7) {
        if (count_ >= MAX_BRAIN_TASKS)
            return false;
        BrainTaskEntry& e = entries_[count_++];
        e.priority_tier      = tier;
        e.insertion_frame_ms = frame_ms;
        e.stable_seq_id      = seq_++;
        e.warrior_id         = warrior_id;
        e.type               = task_type;
        std::memset(e._reserved, 0, sizeof(e._reserved));
        if (payload7)
            std::memcpy(e._reserved, payload7, sizeof(e._reserved));
        return true;
    }

public:

    // sort: stable-sort entries so index 0 = highest priority. Called before pop.
    void sortEntries() {
        std::sort(entries_, entries_ + count_, [](const BrainTaskEntry& a, const BrainTaskEntry& b) {
            if (a.priority_tier      != b.priority_tier)      return a.priority_tier      < b.priority_tier;
            if (a.insertion_frame_ms != b.insertion_frame_ms) return a.insertion_frame_ms < b.insertion_frame_ms;
            if (a.stable_seq_id      != b.stable_seq_id)      return a.stable_seq_id      < b.stable_seq_id;
            return a.warrior_id < b.warrior_id;
        });
    }

    // drain: pops one task in deterministic order, returns true if task was popped.
    // In this slice: discards the task (no tacOrder write — stub).
    bool drain() {
        if (count_ == 0)
            return false;
        sortEntries();
        // Pop index 0 (highest priority): shift remaining entries down.
        for (int i = 1; i < count_; ++i)
            entries_[i - 1] = entries_[i];
        --count_;
        return true;
    }

    // drainWithTask: pops one task, or returns nullopt if empty.
    std::optional<BrainTaskEntry> drainWithTask() {
        if (count_ == 0)
            return std::nullopt;
        sortEntries();
        const BrainTaskEntry out = entries_[0];
        for (int i = 1; i < count_; ++i)
            entries_[i - 1] = entries_[i];
        --count_;
        return out;
    }

    // Save v2 BRN1 (B2): raw image access; restore() replaces the queue and keeps the monotone seq.
    const BrainTaskEntry* data() const { return entries_; }
    uint32_t seq() const { return seq_; }
    void restore(std::span<const BrainTaskEntry> image) {
        count_ = std::min(static_cast<int>(image.size()), MAX_BRAIN_TASKS);
        for (int i = 0; i < count_; ++i) entries_[i] = image[i];
    }
    void setSeq(uint32_t next) { seq_ = next; }

    bool isEmpty() const    { return count_ == 0; }
    int  numPending() const { return count_; }

    void reset() {
        count_ = 0;
        seq_   = 0;
    }

    // debug dump (MC2_BRAIN_TASKQ_TRACE gate, cheap)
    void dumpTrace(int32_t warrior_id_for_log) const {
        mc2::fmt::print_err("BRAIN_TASKQ: wid={:d} pending={:d}\n", mc2::fmt::as_i32(warrior_id_for_log), mc2::fmt::as_i32(count_));
        for (int i = 0; i < count_; ++i) {
            const BrainTaskEntry& e = entries_[i];
            mc2::fmt::print_err("  [{:d}] tier={:d} frame_ms={:d} seq={:d} wid={:d}\n",
                mc2::fmt::as_i32(i), mc2::fmt::as_u32((unsigned)e.priority_tier), mc2::fmt::as_u32(e.insertion_frame_ms), mc2::fmt::as_u32(e.stable_seq_id), mc2::fmt::as_i32(e.warrior_id));
        }
    }

private:
    BrainTaskEntry entries_[MAX_BRAIN_TASKS];
    int            count_ = 0;
    uint32_t       seq_   = 0;  // per-queue monotone insert counter
};
