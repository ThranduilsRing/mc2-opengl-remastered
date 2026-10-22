#pragma once
#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2shadow {
// Current-frame rigid poses, recorded in scene traversal order. Pointer identity
// only prevents duplicate captures; it never determines emission order.
class MoverPropFeed {
public:
    void pause() { ++pauseDepth_; }
    void resume() { --pauseDepth_; }
    bool paused() const { return pauseDepth_!=0; }
    void clear() { epoch_=~0ull; owners_.clear(); leaves_.clear(); }
    bool contains(const void* owner,uint64_t frame) const {
        return epoch_==frame && std::find(owners_.begin(),owners_.end(),owner)!=owners_.end();
    }
    void record(const void* owner,std::span<const GpuStaticPropInstance> leaves,uint64_t frame) {
        if (epoch_!=frame) { clear(); epoch_=frame; }
        if (paused() || !owner || leaves.empty() || contains(owner,frame)) return;
        owners_.push_back(owner); leaves_.insert(leaves_.end(),leaves.begin(),leaves.end());
    }
    void append(std::vector<GpuStaticPropInstance>& leaves,uint64_t frame) const {
        if (epoch_==frame) leaves.insert(leaves.end(),leaves_.begin(),leaves_.end());
    }
private:
    int pauseDepth_=0;
    uint64_t epoch_=~0ull;
    std::vector<const void*> owners_;
    std::vector<GpuStaticPropInstance> leaves_;
};
inline MoverPropFeed& moverPropFeed() { static MoverPropFeed feed; return feed; }
class ScopedMoverCapturePause {
public:
    ScopedMoverCapturePause() { moverPropFeed().pause(); }
    ~ScopedMoverCapturePause() { moverPropFeed().resume(); }
    ScopedMoverCapturePause(const ScopedMoverCapturePause&)=delete;
    ScopedMoverCapturePause& operator=(const ScopedMoverCapturePause&)=delete;
};
}
