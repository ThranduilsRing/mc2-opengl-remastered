// RenderCore/FramePackets.h -- the frame's published read-only inputs (shared-foundations P1).
//
// PREP builds the packets of a frame once, from one owner each (the camera owner for views, the lighting capture for the scene lighting), and publish()
// freezes them. Recording reads packets only: a consumer holds a `const FramePackets&` that changes only when the NEXT publish() runs, never while the
// frame is recorded. Packet arenas use CPU admission sequence % 3 in the game loop; inline tools use frame % 3.
// A retained slot cannot recycle until Main receives CPU recording completion and retires its sequence.
//
// CONTRACT (one page)
//  - Who builds: one owner per packet kind, on the thread that owns the sim tick, between the last logic tick of the frame and recording. Today: the camera
//    owner (GameCamera::renderViewUniforms) fills the views and the scene lighting. Packets hold only per-submission data (matrices, viewport, frame and
//    tick, sun / ambient): no scene database, no pointers into sim objects, nothing a consumer could use to reach back into sim state.
//  - Parallel fill: jobs may fill disjoint ranges of one packet's arrays (the view list, later the instance rows); one owner calls publish() after the jobs
//    joined. A packet is never written by two owners.
//  - Immutable: from publish() the set is read-only; a consumer reads it through published() / mainView() and sees the same bytes for the rest of the
//    frame. A later begin() allocates a NEW set; it never edits the published one.
//  - Retains: a published set lives in its frame's slot arena (frame % 3). It stays valid through the next two frames; it is dropped (published() falls back to
//    the zeroed set) only if its slot is about to be recycled by a frame that skipped its publish.
//  - Recycles: a slot's arena is reset when a frame first begins in it, i.e. three frames after the slot's previous user. CPU-side lifetime only: anything a
//    recorded-but-unsubmitted command buffer or an in-flight submit reads from a packet must be copied into the frame ring (whose slot restarts only after
//    the ticket of the frame three back completed), never referenced into this arena.
//  - The game loop retains/retires CPU slots around extraction and recording. Other callers copy synchronously: a consumer takes a packet's bytes by value (copyMainView(), or a
//    struct copy of published()) inside the call that needs them and keeps no reference, pointer or span into the arena across a frame boundary. The
//    arena recycle above aborts on an outstanding CPU lease; inline tools must finish reads before the next recycle. Both packet types are
//    trivially copyable (static_assert below) so the copy is the whole story. Skipped presents: a frame whose present was skipped discards its recorded
//    composite (EnginePresentVk::presentGLFrame), so no recording outlives the frame that built it and a packet-derived value cannot be read late.
//  - Pass order: NOT fixed by this slice. The recording order is today the order the GL-era draw_screen call graph reaches each island; fixing it as data
//    needs a one-frame trace of that graph first (open item).
//
// Producers today: GameCamera::renderViewUniforms (views, lighting). Consumers today: the mech and static-prop island feeds (camera position) and the
// scene-lighting trace. The HUD / ImGui draw list (UiPacket in the design) has no frame-start producer: it is built during the present tail, so it moves
// with the HUD slice, not here.
#pragma once

#include <cstdint>
#include <cstdlib>

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
    uint64_t       sequence  = 0; // CPU admission identity; independent of display frame / GPU ticket
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
static_assert(std::is_trivially_copyable<ViewPacket>::value && std::is_trivially_copyable<FramePackets>::value, "consumers copy packets by value (see the contract: readers copy synchronously)");

// Three slot arenas over caller-owned memory. begin(frame) resets the slot's arena the first time a frame uses it and keeps bumping on later calls in the
// same frame (a second set in one frame does not disturb the first).
class FrameSlotArenas {
public:
    static constexpr uint32_t kSlots = 3;
    void init(void* mem0, void* mem1, void* mem2, size_t bytesPerSlot) {
        arenas_[0].init(mem0, bytesPerSlot);
        arenas_[1].init(mem1, bytesPerSlot);
        arenas_[2].init(mem2, bytesPerSlot);
        slotFrame_[0] = slotFrame_[1] = slotFrame_[2] = kNoFrame;
        for (auto& retained : retained_) retained = false;
    }
    FrameArena& begin(uint64_t frame) {
        const uint32_t slot = slotOf(frame);
        if (slotFrame_[slot] != frame) {
            if (retained_[slot]) std::abort();
            arenas_[slot].reset(static_cast<uint32_t>(frame));
            slotFrame_[slot] = frame;
        }
        return arenas_[slot];
    }
    // Main admits and retires CPU readers. Refuse stale retirement and occupied
    // slots without touching their bytes; GPU consumers must still copy values.
    bool retain(uint64_t sequence) {
        const auto slot = slotOf(sequence);
        if (retained_[slot]) return false;
        begin(sequence);
        retained_[slot] = true;
        return true;
    }
    bool retire(uint64_t sequence) {
        const auto slot = slotOf(sequence);
        if (!retained_[slot] || slotFrame_[slot] != sequence) return false;
        retained_[slot] = false;
        return true;
    }
    static uint32_t slotOf(uint64_t frame) { return static_cast<uint32_t>(frame % kSlots); }
    uint64_t frameOfSlot(uint32_t slot) const { return slotFrame_[slot % kSlots]; }

private:
    static constexpr uint64_t kNoFrame = ~0ull;
    FrameArena arenas_[kSlots];
    uint64_t   slotFrame_[kSlots] = {kNoFrame, kNoFrame, kNoFrame};
    bool retained_[kSlots]{};
};

class FramePacketPublisher {
public:
    FramePacketPublisher() {
        slots_.init(storage_[0], storage_[1], storage_[2], kSlotBytes);
        published_ = &empty_;
    }

    // Opens a writable set for `frame` in the frame's slot arena. Null when the arena is full (the caller keeps the previous published set).
    FramePackets* begin(uint64_t frame) {
        const auto sequence = buildingSequence_ ? buildingSequence_ : frame;
        if (published_ != &empty_ && FrameSlotArenas::slotOf(published_->sequence) == FrameSlotArenas::slotOf(sequence) && published_->sequence != sequence)
            published_ = &empty_;   // this slot is about to be recycled under the published set (a frame skipped its publish)
        FrameArena& arena = slots_.begin(sequence);
        RenderCore::Span<FramePackets> s = arena.allocArray<FramePackets>(1, "FramePackets");
        if (s.empty()) { ++overflows_; open_ = nullptr; return nullptr; }
        open_ = s.data;
        *open_ = empty_;   // trivially copyable: copying the default set initializes the arena memory
        open_->frame = frame;
        open_->sequence = sequence;
        return open_;
    }
    void retain(uint64_t sequence) {
        if (published_ != &empty_ && FrameSlotArenas::slotOf(published_->sequence) == FrameSlotArenas::slotOf(sequence))
            published_ = &empty_;
        if (!sequence || !slots_.retain(sequence)) std::abort();
        buildingSequence_ = sequence;
        open_ = nullptr;
    }
    void retire(uint64_t sequence) {
        if (!sequence || !slots_.retire(sequence)) std::abort();
        if (buildingSequence_ == sequence) {
            buildingSequence_ = 0;
            open_ = nullptr;
        }
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
    // The by-value form consumers use: the bytes are theirs, so nothing refers into the arena after the call.
    ViewPacket copyMainView() const { return mainView(); }
    uint64_t publishes() const { return publishes_; }
    uint32_t overflows() const { return overflows_; }

private:
    static constexpr size_t kSlotBytes = 64u * 1024u;
    alignas(16) uint8_t storage_[FrameSlotArenas::kSlots][kSlotBytes];
    FrameSlotArenas slots_;
    FramePackets    empty_{};
    ViewPacket      emptyView_{};
    FramePackets*   open_      = nullptr;
    const FramePackets* published_ = nullptr;
    uint64_t        publishes_ = 0;
    uint32_t        overflows_ = 0;
    uint64_t        buildingSequence_ = 0;
};

// The process's publisher. Main thread only.
inline FramePacketPublisher& framePackets() {
    static FramePacketPublisher p;
    return p;
}

}  // namespace RenderCore
