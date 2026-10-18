// RenderCore/FramePackets.h -- the frame's published read-only inputs (shared-foundations P1).
//
// PREP builds the packets of a frame once, from one owner each (the camera owner for views, the lighting capture for the scene lighting), and publish()
// freezes them. Recording reads packets only: a consumer holds a `const FramePackets&` that changes only when the NEXT publish() runs, never while the
// frame is recorded. Packets live in the arena of the frame's slot (frame % 2); a slot is reset when a frame first begins in it, so a published set
// stays valid through the following frame and until a frame two later begins.
//
// Producers today: GameCamera::renderViewUniforms (views, lighting). Consumers today: the mech and static-prop island feeds (camera position) and the
// scene-lighting trace. The HUD / ImGui draw list (UiPacket in the design) has no frame-start producer: it is built during the present tail, so it moves
// with the HUD slice, not here.
#pragma once

#include <cstdint>
#include <new>

#include "EngineView.h"
#include "FrameArena.h"
#include "SceneLighting.h"

namespace RenderCore {

// One view of the frame: matrices (the ViewUniforms layout), viewport, and the frame / sim tick it was built for.
struct ViewPacket {
    ViewId       id       = kInvalidViewId;
    ViewKind     kind     = ViewKind::MainScene;
    ViewMode     mode     = ViewMode::Visual;
    int32_t      viewport[4] = {};   // x, y, w, h in pixels
    ViewUniforms uniforms = {};
    uint64_t     frame    = 0;       // the render frame counter when it was built
    uint64_t     simTick  = 0;       // the sim tick the frame renders
};

constexpr uint32_t kMaxViewsPerFrame = 8;   // main, shadow cascades, reflection, preview

struct LightingPacket {
    SceneLighting scene;
    bool          valid = false;
};

struct FramePackets {
    uint64_t       frame     = 0;
    uint32_t       viewCount = 0;
    ViewPacket     views[kMaxViewsPerFrame];
    LightingPacket lighting;

    // Appends a view; false when the set is full.
    bool addView(const ViewPacket& v) {
        if (viewCount >= kMaxViewsPerFrame) return false;
        views[viewCount++] = v;
        return true;
    }
    const ViewPacket* findView(ViewId id) const {
        for (uint32_t i = 0; i < viewCount; ++i)
            if (views[i].id == id) return &views[i];
        return nullptr;
    }
};
static_assert(std::is_trivially_destructible<FramePackets>::value, "packets live in a FrameArena");

// Two slot arenas over caller-owned memory. begin(frame) resets the slot's arena the first time a frame uses it and keeps bumping on later calls in the
// same frame (a second set in one frame does not disturb the first).
class FrameSlotArenas {
public:
    void init(void* mem0, void* mem1, size_t bytesPerSlot) {
        arenas_[0].init(mem0, bytesPerSlot);
        arenas_[1].init(mem1, bytesPerSlot);
        slotFrame_[0] = slotFrame_[1] = kNoFrame;
    }
    FrameArena& begin(uint64_t frame) {
        const uint32_t slot = static_cast<uint32_t>(frame & 1u);
        if (slotFrame_[slot] != frame) {
            arenas_[slot].reset(static_cast<uint32_t>(frame));
            slotFrame_[slot] = frame;
        }
        return arenas_[slot];
    }
    static uint32_t slotOf(uint64_t frame) { return static_cast<uint32_t>(frame & 1u); }
    uint64_t frameOfSlot(uint32_t slot) const { return slotFrame_[slot & 1u]; }

private:
    static constexpr uint64_t kNoFrame = ~0ull;
    FrameArena arenas_[2];
    uint64_t   slotFrame_[2] = {kNoFrame, kNoFrame};
};

class FramePacketPublisher {
public:
    FramePacketPublisher() {
        slots_.init(storage_[0], storage_[1], kSlotBytes);
        published_ = &empty_;
    }

    // Opens a writable set for `frame` in the frame's slot arena. Null when the arena is full (the caller keeps the previous published set).
    FramePackets* begin(uint64_t frame) {
        if (published_ != &empty_ && FrameSlotArenas::slotOf(published_->frame) == FrameSlotArenas::slotOf(frame) && published_->frame != frame)
            published_ = &empty_;   // this slot is about to be recycled under the published set (a frame skipped its publish)
        FrameArena& arena = slots_.begin(frame);
        RenderCore::Span<FramePackets> s = arena.allocArray<FramePackets>(1, "FramePackets");
        if (s.empty()) { ++overflows_; open_ = nullptr; return nullptr; }
        open_ = new (s.data) FramePackets();
        open_->frame = frame;
        return open_;
    }
    // Freezes the open set: from here consumers read it through published(). A publish with no open set changes nothing.
    const FramePackets& publish() {
        if (open_) { published_ = open_; open_ = nullptr; ++publishes_; }
        return *published_;
    }
    // The last published set (a zeroed set before the first publish).
    const FramePackets& published() const { return *published_; }
    // The last published main view, or a zeroed one before the first publish.
    const ViewPacket& mainView() const {
        const ViewPacket* v = published_->findView(kMainSceneViewId);
        return v ? *v : emptyView_;
    }
    uint64_t publishes() const { return publishes_; }
    uint32_t overflows() const { return overflows_; }

private:
    static constexpr size_t kSlotBytes = 64u * 1024u;
    alignas(16) uint8_t storage_[2][kSlotBytes];
    FrameSlotArenas slots_;
    FramePackets    empty_{};
    ViewPacket      emptyView_{};
    FramePackets*   open_      = nullptr;
    const FramePackets* published_ = nullptr;
    uint64_t        publishes_ = 0;
    uint32_t        overflows_ = 0;
};

// The process's publisher. Main thread only.
inline FramePacketPublisher& framePackets() {
    static FramePacketPublisher p;
    return p;
}

}  // namespace RenderCore
