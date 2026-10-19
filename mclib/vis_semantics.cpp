//---------------------------------------------------------------------------
// vis_semantics.cpp — VISIBILITY-SEMANTICS-1. See vis_semantics.h for the
// full rationale and the measured facts this encodes.
//---------------------------------------------------------------------------
#include "vis_semantics.h"
#include "objstatus.h"

#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace mc2_vis {

bool enabled()
{
    // ONE gate. MC2_LIGHT_MOVER_VIS_GATE / MC2_LIGHT_CONE_VIS_GATE are
    // SUPERSEDED, not ANDed in: GHOST-CONE-VERIFY-4 parked both default-OFF
    // (commit 89f0ed4f) as a safe holding position, so consulting them here
    // would leave this lane's fix dead on arrival. With MC2_VIS_SEMANTICS=0
    // the fallbacks below reproduce mover_model_shown()'s ARMED predicate
    // (legacyVisible && (sensorLevel==0 || sensorLevel>=5)) inline; note that
    // in its PARKED state mover_model_shown() itself returns true
    // unconditionally, so =0 is stricter than stock, not looser.
    // VIS-SEMANTICS-FINISH-2: mover_model_shown() now has no live callers —
    // both job-(a) guards in cone_glow.cpp arm off vis_job_a_armed().
    static const bool s = mc2::gate::get<"MC2_VIS_SEMANTICS">();
    return s;
}

bool force_all_visible()
{
    static const bool s = mc2::gate::get<"MC2_FORCE_ALL_VISIBLE">();
    return s;
}

bool powered(const MoverVis& v)
{
    return (v.status != OBJECT_STATUS_DESTROYED)
        && (v.status != OBJECT_STATUS_DISABLED)
        && (v.status != OBJECT_STATUS_SHUTDOWN);
}

bool friendly(const MoverVis& v)
{
    return (v.relation == REL_OWN) || (v.relation == REL_ALLIED);
}

bool meshDrawn(const MoverVis& v)
{
    if (force_all_visible()) return true;
    if (!enabled())
    {
        // Legacy: the pre-lane approximation, sensorLevel buckets.
        return v.legacyVisible
            && ((v.sensorLevel == 0) || (v.sensorLevel >= 5));
    }
    // Friendly units are never faded out — assert it rather than depend on it,
    // so a stray setAlphaValue elsewhere can never blank a player unit.
    if (friendly(v)) return true;
    // The real mechanism: the mesh is hidden by fading alpha to 0. Covers the
    // contact fade-in/fade-out transitions for free, and does not care that
    // sensorLevel means different things for mechs and vehicles.
    return (v.meshAlpha != 0u);
}

bool knownToPlayer(const MoverVis& v)
{
    if (force_all_visible()) return true;
    if (!enabled()) return meshDrawn(v);
    if (friendly(v)) return true;
    // A blip IS contact: the player knows something is there, just not what.
    if ((v.sensorLevel > 0) && (v.sensorLevel < 5)) return true;
    return meshDrawn(v);
}

bool emitsLight(const MoverVis& v)
{
    if (!powered(v)) return false;          // wrecks emit nothing, always
    if (force_all_visible()) return true;
    if (!enabled())
        return v.legacyVisible
            && ((v.sensorLevel == 0) || (v.sensorLevel >= 5));
    // RULE: player-owned and allied units ALWAYS emit. Their lights are not a
    // fairness problem and gating them on any contact predicate is what
    // produced the "no mech lights at all" regression.
    if (friendly(v)) return true;
    // RULE: an enemy the player cannot see must not advertise itself with a
    // pool of light or a searchlight cone. Emission follows the mesh exactly.
    return meshDrawn(v);
}

bool selfLit(const MoverVis& v)
{
    if (force_all_visible()) return true;
    if (!enabled()) return true;            // legacy never gated self-lighting
    // Something that is not drawn cannot be lit; everything drawn is lit,
    // including wrecks (a burnt-out hull still catches nearby lamps).
    return meshDrawn(v);
}

// --- measurement -------------------------------------------------------------

bool trace_enabled()
{
    static const bool s = mc2::gate::get<"MC2_VIS_SEMANTICS_TRACE">();
    return s;
}

static unsigned long trace_every()
{
    static const unsigned long s = []() -> unsigned long {
        const unsigned long n = static_cast<unsigned long>(mc2::gate::get<"MC2_VIS_SEMANTICS_TRACE_EVERY">());
        return n ? n : 120ul;
    }();
    return s;
}

void trace(const char* cls, const void* actor, const MoverVis& v)
{
    if (!trace_enabled()) return;
    static unsigned long frameCounter = 0ul;
    static const void* firstActorOfFrame = nullptr;
    // Cheap frame detection: the object walk visits actors in a stable order,
    // so seeing the same actor again means a new frame started.
    if (firstActorOfFrame == nullptr) firstActorOfFrame = actor;
    else if (firstActorOfFrame == actor) ++frameCounter;
    if ((frameCounter % trace_every()) != 0ul) return;

    mc2::fmt::print_err("[VIS-SEM v1] cls={} actor={:016X} frame={:d} sensorLevel={:d} meshAlpha={:d} status={:d} relation={:d} legacyVisible={:d} | meshDrawn={:d} knownToPlayer={:d} emitsLight={:d} selfLit={:d} powered={:d} friendly={:d}\n", mc2::fmt::safe(cls ? cls : "?"), mc2::fmt::as_ptr(actor), mc2::fmt::as_u32(frameCounter), mc2::fmt::as_i32(v.sensorLevel), mc2::fmt::as_u32(v.meshAlpha), mc2::fmt::as_i32(v.status), mc2::fmt::as_i32(v.relation), mc2::fmt::as_i32(static_cast<int>(v.legacyVisible)), mc2::fmt::as_i32(static_cast<int>(meshDrawn(v))), mc2::fmt::as_i32(static_cast<int>(knownToPlayer(v))), mc2::fmt::as_i32(static_cast<int>(emitsLight(v))), mc2::fmt::as_i32(static_cast<int>(selfLit(v))), mc2::fmt::as_i32(static_cast<int>(powered(v))), mc2::fmt::as_i32(static_cast<int>(friendly(v))));
    std::fflush(stderr);
}

} // namespace mc2_vis
