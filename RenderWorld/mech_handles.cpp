// RenderWorld/mech_handles.cpp -- the mech half of the render-object handle table: registration, retirement through the completion-gated slot free list,
// and the mission-boundary sweep. The record table itself is shared (RenderWorld_internal.h).
#include <cstdio>

#include "RenderWorld.h"
#include "RenderWorld_internal.h"
#include "mc2_diag_verbose.h"
#include "../RenderCore/HandleSlots.h"

namespace {
// Mech handle slots (P3): allocated by registerMech, retired by destroyMech and parked until the GPU frame that may still name the index completed
// (advanceFrame), reset by clearAllMechRecords. Mech handle indices are kMechHandleBase + slot. Guarded by s_objectRecordsMutex.
static RenderCore::HandleSlotTable s_mechSlots;
static std::atomic<uint64_t> s_currentFrame{0};     // the frame being recorded (the engine's frame counter)
static std::atomic<uint64_t> s_completedFrame{0};   // the newest frame whose GPU work completed
}  // namespace

namespace RenderWorld {

using detail::kMechHandleBase;
using detail::kTerrainHandleBase;
using detail::objectRecordsMutex;
using detail::objectRecords;
using detail::mechsAlive;

void advanceFrame(uint64_t frame, uint64_t completedFrame) {
    s_currentFrame.store(frame, std::memory_order_relaxed);
    s_completedFrame.store(completedFrame, std::memory_order_relaxed);
}

RenderCore::RenderObjectHandle registerMech(RenderMechDesc desc) {
    // A slot above kMechHandleBase (static-prop recipe indices stay below it): a fresh one, or one retired earlier whose GPU frame completed.
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::invalid();
    uint32_t idx = 0;
    uint16_t gen = 0;

    // Populate the unified record table with kind=Mech.
    {
        std::lock_guard<std::mutex> lk(objectRecordsMutex);
        if (!s_mechSlots.initialized()) s_mechSlots.init(kTerrainHandleBase - kMechHandleBase);   // first use
        const RenderCore::HandleSlotTable::Alloc a = s_mechSlots.alloc(s_completedFrame.load(std::memory_order_relaxed));
        if (!a.ok) {
            MC2_DIAGFE(stderr,
                "[RENDER_WORLD v1] WARN: event=mech_register_fail reason=range_exhausted live=%u\n",
                (unsigned)s_mechSlots.liveCount());
            return RenderCore::RenderObjectHandle::invalid();
        }
        idx = kMechHandleBase + a.slot;
        gen = static_cast<uint16_t>(a.generation);
        h   = RenderCore::RenderObjectHandle::make(idx, a.generation);
        if (idx >= objectRecords.size()) {
            const size_t want = static_cast<size_t>(idx) + 1;
            const size_t cap  = (want * 3) / 2 + 16;
            objectRecords.resize(cap);
        }
        auto& rec             = objectRecords[idx];
        rec.generation        = gen;
        rec.flags             = kRenderObjectFlagAlive;
        rec.meshHandleBits    = 0;
        rec.materialHandleBits = 0;
        rec.lodLevel          = 0xFFu;
        rec.pipelineId        = 0;
        rec.drawPacketIndex   = 0xFFFFFFFFu;
        rec.pathReasonCode    = 0;
        rec.gameObjectId      = desc.gameObjectId;
        rec.kind              = RenderObjectKind::Mech;
        rec.debugCookie       = desc.debugCookie;
    }

    mechsAlive.fetch_add(1, std::memory_order_relaxed);

    return h;
}

void destroyMech(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    const uint32_t idx = h.index();
    {
        std::lock_guard<std::mutex> lk(objectRecordsMutex);
        if (idx >= objectRecords.size()) return;
        auto& rec = objectRecords[idx];
        // Defensive: only retire if this is actually a mech record.
        if (rec.kind != RenderObjectKind::Mech) {
            MC2_DIAGFE(stderr,
                "[RENDER_WORLD v1] WARN: destroyMech called on non-Mech record "
                "handle.index=%u\n", (unsigned)idx);
            return;
        }
        // Idempotent retirement: if clearAllMechRecords (endMission sweep) or a
        // prior destroyMech already retired this slot, the alive flag is clear
        // and rec.generation has been bumped past h.generation(). Either signal
        // alone means "already retired" — return without decrementing
        // mechsAlive (would underflow the uint64_t counter).
        if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
            return;
        }
        if (h.generation() != static_cast<uint32_t>(rec.generation)) {
            return;
        }
        // The slot parks until the frame being recorded has completed on the GPU; the record mirrors the table's bumped generation.
        const uint32_t slot = idx - kMechHandleBase;
        if (!s_mechSlots.release(slot, h.generation(), s_currentFrame.load(std::memory_order_relaxed))) return;
        rec.flags &= static_cast<uint16_t>(~kRenderObjectFlagAlive);
        rec.generation = static_cast<uint16_t>(s_mechSlots.generationOf(slot));
    }
    mechsAlive.fetch_sub(1, std::memory_order_relaxed);

}

void clearAllMechRecords() {
    uint64_t cleared = 0;
    {
        std::lock_guard<std::mutex> lk(objectRecordsMutex);
        for (uint32_t i = kMechHandleBase;
             i < static_cast<uint32_t>(objectRecords.size()); ++i) {
            auto& rec = objectRecords[i];
            if (rec.kind == RenderObjectKind::Mech &&
                (rec.flags & kRenderObjectFlagAlive) != 0u) {
                rec.flags &= static_cast<uint16_t>(~kRenderObjectFlagAlive);
                rec.generation = static_cast<uint16_t>(rec.generation + 1u);
                ++cleared;
            }
        }
    }
    if (cleared > 0) {
        mechsAlive.fetch_sub(cleared, std::memory_order_relaxed);
    }
    // A mission boundary: every slot free again (no GPU work of the old mission may name these indices), generations kept moving (a handle held from the old mission stays stale).
    {
        std::lock_guard<std::mutex> lk(objectRecordsMutex);
        s_mechSlots.reset();
    }
}

} // namespace RenderWorld
