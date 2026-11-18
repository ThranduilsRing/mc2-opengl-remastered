#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <vector>

namespace mc2 {

enum class TerrainRequeueReason : unsigned {
    TerrainRequeueDynamic = 1u << 0,
    TerrainRequeueFalling = 1u << 1,
    TerrainRequeueFrameLights = 1u << 2,
    TerrainRequeueLightBake = 1u << 3,
    TerrainRequeueEffects = 1u << 4
};
inline constexpr unsigned TerrainRequeueReasonCount = 5;

inline unsigned terrainRequeueReasons(bool inView, bool staticNow, bool falling,
    bool hasFrameLights, bool lightBlocker, bool hasEffects) {
    // Reasons overlap; only visible owners emit frame-local lights.
    return (inView && !staticNow ? static_cast<unsigned>(TerrainRequeueReason::TerrainRequeueDynamic) : 0u) |
        (falling ? static_cast<unsigned>(TerrainRequeueReason::TerrainRequeueFalling) : 0u) |
        (inView && hasFrameLights ? static_cast<unsigned>(TerrainRequeueReason::TerrainRequeueFrameLights) : 0u) |
        (lightBlocker ? static_cast<unsigned>(TerrainRequeueReason::TerrainRequeueLightBake) : 0u) |
        (hasEffects ? static_cast<unsigned>(TerrainRequeueReason::TerrainRequeueEffects) : 0u);
}

// Sim mutations enqueue IDs; presentation drains after the simulation joins.
class TerrainDirtyQueue {
    std::mutex mutex_;
    std::vector<unsigned char> marked_;
    std::vector<long> pending_;
    // Settled statics whose presentation does not depend on the camera; any mark drops the flag.
    std::vector<unsigned char> resident_;
    // The last render visit submitted this handle to the static registry and nothing has marked it since;
    // the render extract may skip it. Implies resident_; any mark drops it.
    std::vector<unsigned char> renderResident_;
    bool lockedQuiescentReads_ = false;
    std::atomic<unsigned> quiescentViolations_{0};
public:
    void reset(std::size_t handleCount) {
        const std::lock_guard lock(mutex_);
        marked_.assign(handleCount, 0);
        resident_.assign(handleCount, 0);
        renderResident_.assign(handleCount, 0);
        pending_.clear();
    }
    void mark(long handle) {
        if (handle < 0) return;
        const std::lock_guard lock(mutex_);
        if (handle < 0 || static_cast<std::size_t>(handle) >= marked_.size()) return;
        resident_[handle] = 0;
        renderResident_[handle] = 0;
        if (marked_[handle]) return;
        marked_[handle] = 1;
        pending_.push_back(handle);
    }
    void markBlock(long firstHandle, long count) {
        for (long i = 0; i < count; ++i) mark(firstHandle + i);
    }
    // Camera change: only objects whose presentation still follows the view are revisited.
    template <class Bind>
    void markBlockNonResident(long firstHandle, long count, Bind&& bind) {
        markBlockNonResident(firstHandle, count, bind, [](long) { return false; });
    }
    // Camera-dependent LOD can retain its recipe until a camera move crosses a LOD boundary.
    template <class Bind, class Wake>
    void markBlockNonResident(long firstHandle, long count, Bind&& bind, Wake&& wake) {
        markBlockNonResident(firstHandle, count, bind, wake, [](long) { return false; });
    }
    template <class Bind, class Wake, class Skip>
    void markBlockNonResident(long firstHandle, long count, Bind&& bind, Wake&& wake, Skip&& skip) {
        const std::lock_guard lock(mutex_);
        for (long handle = std::max(firstHandle, 0L); handle < firstHandle + count; ++handle) {
            if (static_cast<std::size_t>(handle) >= marked_.size()) break;
            if (marked_[handle] || (resident_[handle] && !wake(handle))) continue;
            if (!resident_[handle] && skip(handle)) continue;
            resident_[handle] = renderResident_[handle] = 0;
            bind(handle);
            marked_[handle] = 1;
            pending_.push_back(handle);
        }
    }
    // Called after a presentation visit; a handle re-marked during the visit stays non-resident.
    void setResident(long handle) {
        const std::lock_guard lock(mutex_);
        if (handle < 0 || static_cast<std::size_t>(handle) >= marked_.size() || marked_[handle]) return;
        resident_[handle] = 1;
    }
    // Called from a render visit; true when the handle is presentation-settled and not re-marked.
    bool setRenderResident(long handle) {
        const std::lock_guard lock(mutex_);
        if (handle < 0 || static_cast<std::size_t>(handle) >= marked_.size() || marked_[handle] || !resident_[handle]) return false;
        renderResident_[handle] = 1;
        return true;
    }
    bool isRenderResident(long handle) {
        const std::lock_guard lock(mutex_);
        return handle >= 0 && static_cast<std::size_t>(handle) < renderResident_.size() && renderResident_[handle] && !marked_[handle];
    }
    // Render extract entry: declares the quiescent phase. If the sim job is in flight the lock-free read below is a data
    // race; log once, count it, and make isRenderResidentQuiescent take the lock for this extract (still correct).
    // One check per extract; the per-handle read only tests a plain bool (render thread owns it).
    void beginQuiescentRead(bool simActive) {
        lockedQuiescentReads_ = simActive;
        if (simActive) {
            if (quiescentViolations_.fetch_add(1, std::memory_order_relaxed) == 0)
                std::fputs("TerrainDirtyQueue: quiescent read while the sim job is in flight; falling back to locked reads\n", stderr);
        }
    }
    unsigned quiescentViolations() const { return quiescentViolations_.load(std::memory_order_relaxed); }
    // Render extract only: the sim is quiescent there (beginQuiescentRead), so the flag needs no lock.
    bool isRenderResidentQuiescent(long handle) {
        if (lockedQuiescentReads_) return isRenderResident(handle);
        return handle >= 0 && static_cast<std::size_t>(handle) < renderResident_.size() && renderResident_[handle] && !marked_[handle];
    }
    bool isResident(long handle) {
        const std::lock_guard lock(mutex_);
        return handle >= 0 && static_cast<std::size_t>(handle) < resident_.size() && resident_[handle];
    }
    std::vector<long> drain() {
        std::vector<long> result;
        {
            const std::lock_guard lock(mutex_);
            result.swap(pending_);
            for (long handle : result) marked_[handle] = 0;
        }
        // Keep the terrain handle order used by the old presentation walk.
        std::sort(result.begin(), result.end());
        return result;
    }
    void markAll() {
        const std::lock_guard lock(mutex_);
        for (std::size_t i = 1; i < marked_.size(); ++i) {
            resident_[i] = 0;
            renderResident_[i] = 0;
            if (marked_[i]) continue;
            marked_[i] = 1;
            pending_.push_back(static_cast<long>(i));
        }
    }
};

// Night drift: residents hold no frame lights/effects, so only the first frame, crossing zero
// (a lamp may now spawn) or a flip of isNight revisits them; a plain nightFactor drift does not.
inline bool terrainNightFullInvalidate(float prevNightFactor, float nightFactor, bool prevIsNight, bool isNight) {
    return prevNightFactor < 0.0f || (nightFactor > 0.0f) != (prevNightFactor > 0.0f) || isNight != prevIsNight;
}

inline TerrainDirtyQueue& terrainDirtyQueue() {
    static TerrainDirtyQueue queue;
    return queue;
}

} // namespace mc2
