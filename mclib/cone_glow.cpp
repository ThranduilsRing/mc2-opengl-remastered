//-------------------------------------------------------------------------------
// LIGHT-CONE-MODELS-1 — implementation. See cone_glow.h for the design brief.
//
// Coordinate frames (canonical MAX<->MC2 swizzle, msl.cpp:1298-1302):
//   worldMC2.x = -worldMAX.x;  worldMC2.y = worldMAX.z;  worldMC2.z = worldMAX.y
// TG_ShapeRec::shapeToWorld outputs MAX-frame world positions; the camera
// statics (TG_Shape::s_worldToClip, viewMul*/viewAdd*) consume MAX-frame world
// directly (legacy: shapeToClip = shapeToWorld * s_worldToClip, msl.cpp:1798).
//
// Screen-transform math mirrors MultiTransformShape (tgl.cpp:1918-1950) and the
// z-range triangle rejection mirrors TG_Shape::Render (tgl.cpp:3303-3308).

#include <algorithm>
#include "cone_glow.h"
#include "vis_semantics.h"   // VISIBILITY-SEMANTICS-1 supersession

#include "msl.h"
#include "render_submissions.h"
#include "camera.h"
#include "timing.h"

#include <cmath>
#include <cstdio>    // CONE-AIM-DIAG-1 / GHOST-LIGHT-DUMP-1 throttled dumps
#include <cstdlib>
#include <optional>
#include <vector>
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

namespace mc2_cone_glow {

namespace {

// ---- tuning knobs (parse-once, read through the typed gate registry) --------

float clampf(float f, float lo, float hi)
{
    if (f < lo) return lo;
    if (f > hi) return hi;
    return f;
}

// ---- per-frame glow queue ---------------------------------------------------
// Reservation (addTriangle) happens at queue time (update phase); vertex fill
// is deferred to FlushFrame() so the MC2_ISSPOTLGT vertex-block allocation in
// addVertices sees the FULL frame reservation (render_submissions.h:1014-1020 allocates on
// first addVertices with capacity = reservations so far).

struct QueuedTri { gos_VERTEX v[3]; };

std::vector<QueuedTri> s_tris;
long s_queueTurn = -1;

// Hard sanity caps.
const int   kMaxConeVerts = 512;
const size_t kMaxFrameTris = 8192;

// Resolve nodeId -> (rec, typeShape), or nothing.
struct SpotNode { const TG_ShapeRec* rec; const TG_TypeShape* ts; };
std::optional<SpotNode> resolveSpotNode(TG_MultiShape* shape, long nodeId)
{
    if (!shape) return std::nullopt;
    const TG_ShapeRec* rec = shape->GetShapeRec(static_cast<int>(nodeId));
    if (!rec || !rec->node || !rec->processMe) return std::nullopt;
    TG_TypeNodePtr tn = rec->node->GetMyType();
    if (!tn || tn->GetNodeType() != SHAPE_NODE) return std::nullopt;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(tn);
    if (!ts->GetTypeVertices()) return std::nullopt;
    return SpotNode{rec, ts};
}

// Node-local cone axis (unit) + length from the type geometry. The cone apex
// is authored at/near the node pivot, so the vertex centroid direction IS the
// beam direction; length = max vertex projection along it. Returns false on
// degenerate geometry (too few verts / centroid at origin).
bool localConeAxis(const TG_TypeShape* ts, glm::vec3& axisOut, float& lenOut)
{
    const TG_TypeVertex* verts = ts->GetTypeVertices();
    const int n = const_cast<TG_TypeShape*>(ts)->GetNumTypeVertices();
    if (!verts || n < 3 || n > kMaxConeVerts) return false;

    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        cx += verts[i].position.x;
        cy += verts[i].position.y;
        cz += verts[i].position.z;
    }
    const float inv = 1.0f / static_cast<float>(n);
    cx *= inv; cy *= inv; cz *= inv;

    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (clen < 0.01f) return false;   // symmetric blob — no usable aim

    axisOut.x = cx / clen; axisOut.y = cy / clen; axisOut.z = cz / clen;

    float maxProj = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float p = verts[i].position.x * axisOut.x
                      + verts[i].position.y * axisOut.y
                      + verts[i].position.z * axisOut.z;
        if (p > maxProj) maxProj = p;
    }
    if (maxProj < 0.5f) return false;
    lenOut = maxProj;
    return true;
}

} // namespace

// ---- gates ------------------------------------------------------------------

bool nodes_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_CONE_NODES">();
    return s;
}

// CONE-SPILL-GRADE-1: see cone_glow.h.
//
// DEFAULT FLIPPED TO OFF on measured evidence. MC2_LIGHT_CONE_DIAG=1 on mc2_10
// (attributable exe 6823bc001562) over all 9 distinct building spotlight
// fixtures in the mission:
//
//   hit=1 (pool kept, repositioned) : 2   -- horizErr 7.0 world units, i.e. the
//                                           "true intersection" moves the pool
//                                           an imperceptible distance
//   hit=0 (pool SUPPRESSED entirely): 7   -- every one of these HAD a pool
//                                           before c1f98b45
//   clamped=1 (200u clamp would fire): 0  -- the clamp removal is a no-op here
//
// Why the misses: the stock fixtures are authored near-horizontal
// (aim.z = -0.089, coneLen = 167.8 -> only 14.9 units of drop across the whole
// beam) from an apex ~17 units above the terrain, and 3 more point straight UP
// (aim = (0,0,1) beacons). The authored cone length is a VISUAL extent, not a
// physical reach, so requiring a real ray/terrain intersection within that
// length rejects almost every fixture. The pre-c1f98b45 code snapped z to the
// terrain and therefore always produced a pool near where the beam pointed.
//
// Net effect of c1f98b45 at default: 7 of 9 night ground pools DELETED for an
// imperceptible reposition of the other 2 -- a regression against the very
// complaint it was written for ("the actual lights are not on at ALL").
// Hence OFF by default. Set MC2_CONE_SPILL_COUPLING=1 to re-enable the
// intersection path (e.g. to re-grade it after the fixture authoring or the
// miss policy is revisited -- see .claude/CONE-SPILL-GRADE-1-STATUS.md).
bool spill_coupling_enabled()
{
    static const bool s = mc2::gate::get<"MC2_CONE_SPILL_COUPLING">();
    return s;
}

// CONE-SPILL-POLICY-1 -- pool placement policy for the DEFAULT path (the arm
// taken when MC2_CONE_SPILL_COUPLING is OFF). The legacy placement is
// UNCONDITIONAL: apex + aim*min(coneLen*spill,200), z snapped to terrain --
// which drops a ground pool at the base of the mc2_23 straight-up beacons
// (aim=(0,0,1)) whose beams never touch the ground at all. The failed
// CONE-SPILL-COUPLING-1 fix over-rejected in the other direction because it
// required a hit within the AUTHORED cone length, which is a visual extent,
// not a physical reach (7/9 mc2_10 pools deleted -- see CONE-SPILL-GRADE-1).
// Policy 1 (default) does both correctly:
//   * aim.z > small up-threshold -> beam points at the sky -> NO pool;
//   * otherwise march the ray over an EXTENDED reach k*coneLen (k =
//     MC2_CONE_POOL_REACH, default 4); hit -> pool AT the intersection,
//     miss within the extended reach -> no pool.
// Validated offline by scripts/predict_cone_pools.py against the mc2_10 /
// mc2_23 fixture inventories before shipping. MC2_CONE_POOL_POLICY=0 is the
// killswitch back to the legacy unconditional placement.
int pool_policy()
{
    static const int s = mc2::gate::get<"MC2_CONE_POOL_POLICY">();
    return s;
}

float pool_reach_scale()
{
    static const float s = mc2::gate::get<"MC2_CONE_POOL_REACH">();
    return s;
}

// SPOTLIGHT-DIRECTION-1. Two coordinate-space defects at the building
// spotlight registration site (mclib/bdactor.cpp):
//
//  (a) TG_Light::direction is never set for spotlightLights_. Despite its
//      name, `direction` is a POSITION for POINT/SPOT lights -- see the
//      comment at mclib/tgl.h:213 ("This is the light spot center for POINT
//      and SPOT") and the consumer at mclib/msl.cpp, which does
//      `lightPos = light->direction; shapePosition -= lightPos;`. Left at the
//      init() default it is (0,0,0), so every lit vertex is shaded as though
//      the lamp sat at the WORLD ORIGIN rather than on its own building.
//      The sibling terrain-light site (bdactor.cpp) sets it correctly, with
//      the MC2->Stuff position swizzle (-x, z, y).
//
//  (b) SetLightToWorld() is built from the RAW MC2-space childPos while every
//      other consumer of that matrix expects the same swizzled Stuff-space
//      position. The terrain-light site again shows the correct form.
//
// Measured on the deployed build before gating (FULLBAKE_DIAG): mc2_10 (night)
// consumed 4,310,486 active POINT/SPOT lights in the CPU bake over a 15 s run,
// of which 3,995,686 (92.7%) still carried direction == (0,0,0). The day
// missions mc2_01 / mc2_03 / mc2_24 measured 0 -- these lights are night-gated
// (`eye->isNight`), so the defect is night-only but is NOT latent.
//
// Gated because the correction changes default night appearance on mc2_10.
bool spot_direction_fix_enabled()
{
    static const bool s = mc2::gate::is<"MC2_SPOTLIGHT_DIRECTION_FIX", "1">()
                       || mc2::gate::is<"MC2_SPOTLIGHT_DIRECTION_FIX", "2">();   // default OFF
    return s;
}

// SPOTLIGHT-DIRECTION-1 falsification arm. `=2` writes a WILDLY displaced
// light-spot position instead of the correct one. Purpose: the `=1` arm proved
// byte-identical to the OFF arm across all 19 building-face poses on
// mc2_01/03/10/24 while the FULLBAKE_DIAG counters simultaneously proved the
// consumer WAS reached with changed data. Two explanations fit that: (i) the
// correction is too small to move a pixel, or (ii) the value the consumer
// computes is discarded downstream and no direction can ever move a pixel.
// `=2` separates them: if an absurd position still renders byte-identically,
// the consumer's output is provably dead on the default GPU static-prop path.
// Zero is an exaggeration bound, not a shipping setting.
// Diagnostic only -- never a shipping value.
bool spot_direction_exaggerate()
{
    static const bool s = mc2::gate::is<"MC2_SPOTLIGHT_DIRECTION_FIX", "2">();
    return s;
}

bool glow_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_CONE_GLOW">();
    return s;
}

float glow_alpha()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_CONE_GLOW_ALPHA">();
    return s;
}

float spill_scale()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_CONE_SPILL">();
    return s;
}

// GHOST-LIGHT-VISIBILITY-1: emitters must follow the model's own draw
// predicate. Default ON (correctness fix, same convention as
// MC2_LIGHT_POOL_VIS_GATE / MC2_LIGHT_CULL_RADIUS). =0 restores the legacy
// "emit even when the owner leaf was never transformed this frame" behaviour.
// GHOST-CONE-VERIFY-4 (2026-07-25): PARKED default-OFF. See
// .claude/GHOST-CONE-VERIFY-4-STATUS.md. The `visible`/sensorLevel state is
// overloaded across three independent questions (is the mesh drawn / is the
// unit known to the player / does the unit emit + receive light), so gating
// emission on any one of them trades one failure for another: default-ON
// closed the hidden-enemy position leak and was reported to blind mechs'
// lights in a tier1 smoke. Blinding the player's own units is worse than a
// cosmetic leak, so both gates are parked OFF pending a proper semantic fix.
// =1 re-arms.
static bool vis_gate_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_CONE_VIS_GATE">();
    return s;
}

// GHOST-LIGHT-VISIBILITY-1 (mover fairness rule) — see cone_glow.h for the
// sensorLevel semantics and why the appearance `visible` flag is not a
// fog-of-war predicate.
// GHOST-CONE-VERIFY-4 (2026-07-25): PARKED default-OFF alongside
// MC2_LIGHT_CONE_VIS_GATE — see the note there. =1 re-arms.
bool mover_vis_gate_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_MOVER_VIS_GATE">();
    return s;
}

// VISIBILITY-SEMANTICS-1 SUPERSESSION: the mover light/cone callers in
// mech3d.cpp / gvactor.cpp no longer call this — they call
// mc2_vis::emitsLight(), which separates "drawn / known / emits / lit".
// Kept as the legacy fallback that mc2_vis consults when MC2_VIS_SEMANTICS=0,
// and for any caller that only has a sensorLevel.
bool mover_model_shown(long sensorLevel)
{
    if (mc2_vis::force_all_visible()) return true;
    if (!mover_vis_gate_enabled()) return true;   // legacy: emit regardless
    return (sensorLevel == 0) || (sensorLevel >= 5);
}

// VIS-SEMANTICS-FINISH-2 — arming predicate shared by BOTH job-(a) MESH DRAWN
// guards in this file: the alpha guard below and the stale-emitter turn guard
// in QueueConeGlow. Before this lane the turn guard consulted
// vis_gate_enabled() alone, i.e. the PARKED MC2_LIGHT_CONE_VIS_GATE, so a
// culled / LOD-dropped / off-map-staged mover still emitted its cone even with
// MC2_VIS_SEMANTICS on — the "lit beam hanging in mid-air with no emitter under
// it" report. Both guards now arm off MC2_VIS_SEMANTICS (default ON), with the
// parked legacy gate still arming them independently when =1.
//
// This is a per-frame DATA-FRESHNESS predicate, not a fog-of-war one: it fires
// only when the emitter's own leaf was not transformed this frame, in which
// case the mesh is not on screen at all. It therefore CANNOT blind a player or
// allied unit that is actually being drawn — the regression mode that got
// MC2_LIGHT_MOVER_VIS_GATE parked came from the sensorLevel fairness predicate,
// which lives in mc2_vis::emitsLight() and exempts friendlies outright.
static bool vis_job_a_armed()
{
    if (mc2_vis::force_all_visible()) return false;
    return mc2_vis::enabled() || vis_gate_enabled();
}

// GHOST-CONE-VERIFY-4 — cone GEOMETRY leak; see cone_glow.h for the full
// rationale and why the predicate is alpha-based rather than sensorLevel-based.
// VISIBILITY-SEMANTICS-1: this IS job (a) MESH DRAWN, expressed on the only
// state tgl.cpp has. mc2_vis::meshDrawn() uses the same `alpha != 0` rule for
// foreign movers, so the two agree by construction. Static building lamps sit
// at 0xff and are untouched — buildings are exempt from every mover rule.
bool cone_geom_hidden_by_alpha(unsigned alphaValue)
{
    // Armed by MC2_VIS_SEMANTICS (default ON) — this lane owns job (a) now.
    // The parked MC2_LIGHT_CONE_VIS_GATE=1 still arms it independently.
    if (!vis_job_a_armed()) return false;
    return (alphaValue == 0u);
}

// ---- LIGHT-REALISM-1 --------------------------------------------------------
// User rejection (2026-07-24, mc2_10 night): "the cones are ultra visible, they
// need to be feathered / more transparent ... the light sources need to mimic
// LIGHT SOURCES (real ones)."
//
// Two defects, both addressed under ONE default-OFF gate (MC2_LIGHT_REALISM):
//  (1) the cone reads as a solid painted wedge — every vertex at a given axial
//      position carries the SAME alpha, so the mesh silhouette is a hard edge;
//  (2) overall opacity is too high for a light shaft.
//
// Fix (1) is the classic screen-space volumetric fake: a view ray through a
// cone accumulates maximum thickness through the beam CENTRE and ~zero at the
// silhouette. The silhouette is exactly the set of vertices that project
// FARTHEST (perpendicular) from the beam's screen-space axis, so fading alpha
// by perpendicular screen distance feathers the silhouette while keeping a
// bright core running the full beam length. All inputs are screen coords the
// transform loop already computes — no extra transforms.
//
// Fix (2) is a straight opacity scalar on top of MC2_LIGHT_CONE_GLOW_ALPHA,
// plus a smoother (squared) axial falloff instead of the linear 1 -> 0.15 ramp.
//
// CONE-SPILL-POLICY-1 (2026-08-01): DEFAULT FLIPPED ON. Variant B (POOL_SPILL
// 2.2, POOL_CORE 0.0, CONE_OPACITY 0.40) is user-approved, and the cost was
// measured FREE with the default light grid (PERF-HARNESS-TRUTH-1: p50
// 6.405ms ON vs 6.454ms OFF at the busy mc2_10 night pose, 4K). =0 reverts.

bool realism_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_REALISM">();
    return s;
}

// Multiplies the cone's peak vertex alpha. <1 = more transparent.
// AUDIT-TRUTH-1: 0.40 is USER-APPROVED variant B. (Variant A shipped 0.55; the
// user graded A vs B side by side and chose B.) A later lane proposed 0.15 on
// the theory that the residual harshness is additive saturation (gos_Alpha_OneOne)
// rather than alpha -- that is an ALTERNATIVE, not the default; test it with
// MC2_LIGHT_REALISM_CONE_OPACITY=0.15.
static float realism_cone_opacity()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_REALISM_CONE_OPACITY">();
    return s;
}

// Silhouette feather exponent. Higher = the bright core narrows and the rim
// fades sooner (softer edge). 0 disables the radial feather.
static float realism_cone_feather()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_REALISM_CONE_FEATHER">();
    return s;
}

// CONE-AIM-DIAG-1 (batch-7 item 4): per-node aim/apex dump. Default-off.
static bool aim_diag_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_CONE_DIAG">();
    return s;
}

// CONE-AIM-FLIP-1 (batch-7 item 4, probe): negate the computed world aim so
// the pool lands on the OPPOSITE side — decisive A/B against the user report
// "the building light appears to be on the other side". Default OFF.
static bool aim_flip_enabled()
{
    static const bool s = mc2::gate::get<"MC2_LIGHT_CONE_AIM_FLIP">();
    return s;
}

// ---- (A) node aim -----------------------------------------------------------

bool GetNodeAim(TG_MultiShape* shape, long nodeId,
                glm::vec3& aimWorldOut, float& coneLenOut)
{
    const auto node = resolveSpotNode(shape, nodeId);
    if (!node) return false;
    const TG_ShapeRec* rec = node->rec;
    const TG_TypeShape* ts = node->ts;

    glm::vec3 axisLocal;
    float lenLocal = 0.0f;
    if (!localConeAxis(ts, axisLocal, lenLocal)) return false;

    // Transform origin and axis tip through the node's live matrix, convert
    // both to the MC2 frame, subtract -> world aim (handles any node basis).
    glm::vec3 zeroLocal;
    zeroLocal.x = zeroLocal.y = zeroLocal.z = 0.0f;
    glm::vec3 oMax, aMax;
    oMax = mc2::math::transformPoint(zeroLocal, rec->shapeToWorld);
    aMax = mc2::math::transformPoint(axisLocal, rec->shapeToWorld);

    glm::vec3 aim;
    aim.x = (-aMax.x) - (-oMax.x);
    aim.y = ( aMax.z) - ( oMax.z);
    aim.z = ( aMax.y) - ( oMax.y);

    const float alen = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
    if (alen < 0.001f) return false;
    aimWorldOut.x = aim.x / alen;
    aimWorldOut.y = aim.y / alen;
    aimWorldOut.z = aim.z / alen;

    // CONE-AIM-FLIP-1 probe: mirror the beam (see helper above).
    if (aim_flip_enabled())
    {
        aimWorldOut.x = -aimWorldOut.x;
        aimWorldOut.y = -aimWorldOut.y;
        aimWorldOut.z = -aimWorldOut.z;
    }

    const float scalar = rec->node->GetShapeScalarValue();
    coneLenOut = lenLocal * ((scalar > 0.0f) ? scalar : 1.0f);

    // CONE-AIM-DIAG-1: throttled apex/aim dump (first 48 calls + 1/600 after).
    if (aim_diag_enabled())
    {
        static unsigned long s_calls = 0;
        ++s_calls;
        if (s_calls <= 48 || (s_calls % 600) == 0)
        {
            // Apex (node origin) in MC2 frame — same swizzle as the aim.
            glm::vec3 apexMc2;
            mc2ToRender(apexMc2, oMax);
            mc2::fmt::print_err("[CONE_AIM_DIAG v1] shape={:016X} node={:d} apexMC2=({:.1f},{:.1f},{:.1f}) aimMC2=({:.3f},{:.3f},{:.3f}) lenLocal={:.1f} coneLen={:.1f} axisLocal=({:.3f},{:.3f},{:.3f}) flip={:d}\n", mc2::fmt::as_ptr(static_cast<void*>(shape)), mc2::fmt::as_i32(nodeId), apexMc2.x, apexMc2.y, apexMc2.z, aimWorldOut.x, aimWorldOut.y, aimWorldOut.z, lenLocal, coneLenOut, axisLocal.x, axisLocal.y, axisLocal.z, mc2::fmt::as_i32(aim_flip_enabled() ? 1 : 0));
            std::fflush(stderr);
        }
    }
    return true;
}

// LOCAL-LIGHTS-1: the stock building cone leaves (e.g. S_TurretControl's
// Spotlight_TurretCon) hold SEVERAL pyramids, each a down-light: one bright apex
// vertex (the lamp; vertex light tag 0xfdfad4) high on the building, joined by
// triangles to a dark base on the ground. The leaf centroid seen from the node
// origin (GetNodeAim) is unrelated to any of those beams -- it pointed the
// S_TurretControl light AWAY from the lit wedge. Each bright apex is one beam.
static bool isApexTag(DWORD tag)
{
    const DWORD r = (tag >> 16) & 0xffu, g = (tag >> 8) & 0xffu, b = tag & 0xffu;
    return std::max({r, g, b}) >= 0x40u;
}

int GetNodeBeams(TG_MultiShape* shape, long nodeId, NodeBeam* out, int maxBeams)
{
    const auto node = resolveSpotNode(shape, nodeId);
    if (!node || maxBeams <= 0) return 0;
    const TG_ShapeRec* rec = node->rec;
    const TG_TypeShape* ts = node->ts;
    const TG_TypeVertex* verts = ts->GetTypeVertices();
    const TG_TypeTriangle* tris = ts->GetTypeTriangles();
    const int nVerts = const_cast<TG_TypeShape*>(ts)->GetNumTypeVertices();
    const long nTris = ts->GetNumTypeTriangles();
    if (!verts || !tris || nVerts < 3 || nVerts > kMaxConeVerts || nTris <= 0) return 0;
    if (!out) {   // count only (mesh data; no pose needed)
        int n = 0;
        for (int a = 0; a < nVerts; ++a) n += isApexTag(verts[a].aRGBLight) ? 1 : 0;
        return std::min(n, maxBeams);
    }
    {   // Not transformed yet (same sentinel as QueueConeGlow): the beams have no world pose.
        const float* m = glm::value_ptr(rec->shapeToWorld);
        if (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) < 0.01f) return -1;
    }
    const float scalar = rec->node->GetShapeScalarValue();
    const float sc = scalar > 0.0f ? scalar : 1.0f;
    const auto toWorld = [&](const glm::vec3& local) {
        const glm::vec3 w = mc2::math::transformPoint(local * sc, rec->shapeToWorld);
        return mc2ToRender(w);   // Stuff -> MC2 frame
    };
    int count = 0;
    for (int a = 0; a < nVerts && count < maxBeams; ++a)
    {
        if (!isApexTag(verts[a].aRGBLight)) continue;
        bool linked[kMaxConeVerts] = {};
        for (long f = 0; f < nTris; ++f)
        {
            const DWORD* v = tris[f].Vertices;
            if (v[0] != static_cast<DWORD>(a) && v[1] != static_cast<DWORD>(a) && v[2] != static_cast<DWORD>(a)) continue;
            for (int k = 0; k < 3; ++k)
                if (v[k] < static_cast<DWORD>(nVerts) && v[k] != static_cast<DWORD>(a) && !isApexTag(verts[v[k]].aRGBLight))
                    linked[v[k]] = true;
        }
        const glm::vec3 apexW = toWorld(verts[a].position);
        glm::vec3 baseW(0.0f);
        int nb = 0;
        glm::vec3 baseVerts[kMaxConeVerts];
        for (int j = 0; j < nVerts; ++j)
            if (linked[j]) { baseVerts[nb] = toWorld(verts[j].position); baseW += baseVerts[nb]; ++nb; }
        if (nb == 0) continue;
        baseW /= static_cast<float>(nb);
        const glm::vec3 axis = baseW - apexW;
        const float len = glm::length(axis);
        if (len < 0.5f) continue;
        NodeBeam& beam = out[count++];
        beam.apex = apexW;
        beam.aim = axis / len;
        beam.length = len;
        float minCos = 1.0f;
        for (int j = 0; j < nb; ++j)
        {
            const glm::vec3 d = baseVerts[j] - apexW;
            const float dl = glm::length(d);
            if (dl > 1e-3f) minCos = std::min(minCos, glm::dot(d, beam.aim) / dl);
        }
        beam.cosHalf = minCos;
    }
    return count;
}

int GetMoverBeams(TG_MultiShape* shape, long nodeId, const glm::vec3& nodeWorld, NodeBeam* out, int maxBeams)
{
    const int n = GetNodeBeams(shape, nodeId, out, maxBeams);
    if (n != 0 || !out) return n;
    // Untagged mover cone (apex authored at the node pivot): one beam from the node.
    glm::vec3 aim;
    float len = 0.0f;
    if (!GetNodeAim(shape, nodeId, aim, len)) return 0;
    out[0] = NodeBeam{nodeWorld, aim, len, 0.94f};
    return 1;
}

int SelfLitLeaf(TG_MultiShape* shape)
{
    if (!shape) return -1;
    for (int i = 0; i < shape->GetNumShapes(); ++i)
    {
        const TG_ShapeRec* rec = shape->GetShapeRec(i);
        if (!rec || !rec->node || !rec->node->IsShapeNode()) continue;
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(rec->node->GetMyType());
        if (!ts) continue;
        const TG_TypeVertex* v = ts->GetTypeVertices();
        const int n = const_cast<TG_TypeShape*>(ts)->GetNumTypeVertices();
        if (!v || n < 3) continue;
        bool all = true;
        for (int k = 0; k < n && all; ++k) all = v[k].aRGBLight == 0xffffffffu;
        if (all) return i;
    }
    return -1;
}

bool SelfLitLeafPose(TG_MultiShape* shape, int leaf, glm::vec3& centerOut, glm::vec3& axisOut)
{
    const TG_ShapeRec* rec = (shape && leaf >= 0) ? shape->GetShapeRec(leaf) : nullptr;
    if (!rec || !rec->node) return false;
    const float* m = glm::value_ptr(rec->shapeToWorld);
    if (std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) < 0.01f) return false;
    const glm::vec3 o = mc2::math::transformPoint(glm::vec3(0.0f), rec->shapeToWorld);
    centerOut = mc2ToRender(o);   // Stuff -> MC2
    // The most horizontal local axis: it sweeps around as the leaf spins.
    float bestZ = 2.0f;
    axisOut = glm::vec3(1.0f, 0.0f, 0.0f);
    for (int k = 0; k < 3; ++k)
    {
        glm::vec3 e(0.0f); e[k] = 1.0f;
        const glm::vec3 p = mc2::math::transformPoint(e, rec->shapeToWorld);
        glm::vec3 a(-(p.x - o.x), p.z - o.z, p.y - o.y);
        const float l = glm::length(a);
        if (l < 1e-4f) continue;
        a /= l;
        if (std::fabs(a.z) < bestZ) { bestZ = std::fabs(a.z); axisOut = a; }
    }
    return true;
}

void ConfigureBeamLight(TG_Light* light, const NodeBeam& b)
{
    // A lamp spreads wider than its drawn shaft; the soft angular roll-off
    // (lpa_spotCone) keeps most of the light near the axis anyway.
    const float halfDeg = glm::degrees(std::acos(std::clamp(b.cosHalf, 0.0f, 1.0f)));
    const float outerDeg = std::clamp(halfDeg * 2.0f + 12.0f, 25.0f, 60.0f);
    light->spotCosOuter = std::cos(glm::radians(outerDeg));
    light->spotDir = b.aim;
    light->SetFalloffDistances(0.0f, std::clamp(b.length * 3.5f, 150.0f, 400.0f));
    glm::vec3 apex = b.apex;
    light->SetPosition(&apex);
}

// ---- (B) cone glow ----------------------------------------------------------

void QueueConeGlow(TG_MultiShape* shape, long nodeId,
                   unsigned long argb, float nightFactor,
                   bool ownerIsMover)
{
    if (!glow_enabled() || !eye) return;
    if (nightFactor <= 0.001f) return;

    const auto node = resolveSpotNode(shape, nodeId);
    if (!node) return;
    const TG_ShapeRec* rec = node->rec;
    const TG_TypeShape* ts = node->ts;

    // GHOST-LIGHT-VISIBILITY-1 (user report, mc2_10 @ 212019: "the mech model is
    // not visible but its light cone is ... if the mech is marked as not
    // visible, it should not produce light").
    //
    // The three callers (mech3d / gvactor / bdactor) gate the queue on the
    // owner's LIGHT predicate (isNight && visible && status != dead/disabled/
    // shutdown). That is NOT the predicate that decides whether the owner's
    // MODEL draws. TG_Shape::Render (tgl.cpp:3155) refuses to draw any leaf
    // whose `lastTurnTransformed != turn` — a leaf that was culled, LOD-dropped,
    // never spawned, or off-map-staged this frame simply never gets stamped, so
    // Render drops it. QueueConeGlow bypassed that guard entirely and re-rendered
    // the cone node's geometry straight from the (possibly stale) shapeToWorld,
    // producing a lit beam hanging in mid-air with no emitter under it.
    //
    // Fix: apply the renderer's OWN staleness predicate to the emitter. If the
    // model leaf would not be drawn this frame, its glow is not queued.
    // VIS-SEMANTICS-FINISH-2: this guard used to consult vis_gate_enabled()
    // alone. GHOST-CONE-VERIFY-4 (89f0ed4f) then PARKED MC2_LIGHT_CONE_VIS_GATE
    // default-OFF, which silently disarmed exactly the guard whose comment above
    // describes the user's symptom, leaving the successor MC2_VIS_SEMANTICS
    // covering only the alpha==0 path. It now arms off vis_job_a_armed()
    // (MC2_VIS_SEMANTICS default ON, legacy gate =1 still arms independently,
    // MC2_FORCE_ALL_VISIBLE=1 disarms for diagnostic captures).
    if (ownerIsMover && vis_job_a_armed() && rec->node
        && rec->node->GetLastTurnTransformed() != static_cast<DWORD>(turn))
    {
        if (aim_diag_enabled())
        {
            static unsigned long s_rejects = 0;
            if ((++s_rejects % 240) == 1)
            {
                mc2::fmt::print_err("[CONE_VIS_GATE v1] event=reject_stale shape={:016X} node={:d} lastTurnTransformed={:d} turn={:d} rejects={:d}\n", mc2::fmt::as_ptr(static_cast<void*>(shape)), mc2::fmt::as_i32(nodeId), mc2::fmt::as_u32(static_cast<unsigned long>(rec->node->GetLastTurnTransformed())), mc2::fmt::as_i32(turn), mc2::fmt::as_u32(s_rejects));
                std::fflush(stderr);
            }
        }
        return;
    }

    const TG_TypeVertex* verts = ts->GetTypeVertices();
    const TG_TypeTriangle* tris = ts->GetTypeTriangles();
    const int nVerts = const_cast<TG_TypeShape*>(ts)->GetNumTypeVertices();
    const long nTris = ts->GetNumTypeTriangles();
    if (!verts || !tris || nVerts < 3 || nVerts > kMaxConeVerts || nTris <= 0)
        return;

    // Beam fade parameterisation (apex bright -> far end dim).
    glm::vec3 axisLocal;
    float lenLocal = 0.0f;
    const bool haveAxis = localConeAxis(ts, axisLocal, lenLocal);

    // GHOST-LIGHT-DUMP-1 finding (batch-7): early frames can reach here BEFORE
    // the owner shape's first TransformMultiShape — shapeToWorld is still
    // zero/identity ([CONE_QUEUE_DIAG] apexMC2=(0,0,0)) and the cone would be
    // queued at world origin with a garbage screen transform. Real props never
    // sit at EXACT world origin (terrain elevation alone is nonzero), so a
    // near-zero translation is a reliable "not yet transformed" sentinel.
    {
        const float tx = glm::value_ptr(rec->shapeToWorld)[3];
        const float ty = glm::value_ptr(rec->shapeToWorld)[7];
        const float tz = glm::value_ptr(rec->shapeToWorld)[11];
        if (std::fabs(tx) + std::fabs(ty) + std::fabs(tz) < 0.01f)
            return;
    }

    // New-frame reset (renderLists may not have run last frame — pause etc).
    if (s_queueTurn != turn)
    {
        s_tris.clear();
        s_queueTurn = turn;
    }
    if (s_tris.size() >= kMaxFrameTris) return;

    // Compose shapeToClip exactly like msl.cpp:2068.
    glm::mat4 shapeToClip;
    shapeToClip = rec->shapeToWorld * TG_Shape::s_worldToClip;

    const float scalar = rec->node->GetShapeScalarValue();
    const float viewMulX = TG_Shape::viewMulX, viewAddX = TG_Shape::viewAddX;
    const float viewMulY = TG_Shape::viewMulY, viewAddY = TG_Shape::viewAddY;

    // LIGHT-REALISM-1: overall transparency scalar (gate OFF -> factor 1.0).
    const bool  realism = realism_enabled();
    const float alphaPeak = glow_alpha() * nightFactor
                          * (realism ? realism_cone_opacity() : 1.0f);
    if (alphaPeak < 1.0f) return;
    const DWORD rgb = static_cast<DWORD>((argb & 0x00fffffful));

    // GHOST-LIGHT-DUMP-1 (batch-7 item 5): throttled per-queue dump so a
    // floating cone wedge with no visible caster can be attributed to the
    // queuing shape (correlate apex position with the light-pool dump).
    if (aim_diag_enabled())
    {
        static unsigned long s_qcalls = 0;
        ++s_qcalls;
        if (s_qcalls <= 48 || (s_qcalls % 600) == 0)
        {
            mc2::fmt::print_err("[CONE_QUEUE_DIAG v1] shape={:016X} node={:d} apexMC2=({:.1f},{:.1f},{:.1f}) rgb={:06x} alphaPeak={:.1f} nightFactor={:.2f}\n", mc2::fmt::as_ptr(static_cast<void*>(shape)), mc2::fmt::as_i32(nodeId), -glm::value_ptr(rec->shapeToWorld)[3], glm::value_ptr(rec->shapeToWorld)[11], glm::value_ptr(rec->shapeToWorld)[7], mc2::fmt::as_u32(static_cast<unsigned long>(rgb)), alphaPeak, nightFactor);
            std::fflush(stderr);
        }
    }

    // Transform all verts once (cone meshes are tiny).
    struct XV { float x, y, z, rhw; bool ok; float fade; };
    XV xv[kMaxConeVerts];
    for (int j = 0; j < nVerts; ++j)
    {
        glm::vec3 pos = verts[j].position;
        if (scalar > 0.0f)
        {
            pos.x *= scalar; pos.y *= scalar; pos.z *= scalar;
        }

        Stuff::Vector4D xf;
        xf.MultiplyPoint(pos, shapeToClip);

        float sx, sy, sz, rhw;
        if (eye->usePerspective)
        {
            rhw = (xf.w != 0.0f) ? (1.0f / xf.w) : 1.0f;
            sx = (xf.x * rhw) * viewMulX + viewAddX;
            sy = (xf.y * rhw) * viewMulY + viewAddY;
            sz = (xf.z * rhw);
            rhw = static_cast<float>(fabs(rhw));
        }
        else
        {
            sx = (1.0f - xf.x) * viewMulX + viewAddX;
            sy = (1.0f - xf.y) * viewMulY + viewAddY;
            sz = xf.z;
            rhw = 0.000001f;
        }

        xv[j].x = sx; xv[j].y = sy; xv[j].z = sz; xv[j].rhw = rhw;
        xv[j].ok = (sz >= 0.0f) && (sz < 1.0f);

        // Beam-length fade 1 -> 0.15 (apex -> far end).
        float t = 0.0f;
        if (haveAxis && lenLocal > 0.001f)
        {
            const float p = verts[j].position.x * axisLocal.x
                          + verts[j].position.y * axisLocal.y
                          + verts[j].position.z * axisLocal.z;
            t = p / lenLocal;
            t = std::clamp(t, 0.0f, 1.0f);
        }
        // LIGHT-REALISM-1: smoother axial decay (squared) instead of the
        // linear ramp — bright near the emitter, gently dying out along the
        // shaft. Gate OFF keeps the exact legacy 1 - 0.85t.
        xv[j].fade = realism ? ((1.0f - t) * (1.0f - t) * 0.9f + 0.05f)
                             : (1.0f - 0.85f * t);
    }

    // LIGHT-REALISM-1: screen-space silhouette feather (see the block comment
    // by realism_cone_feather). Project the beam axis to screen, then fade each
    // vertex by its PERPENDICULAR screen distance from that axis: verts on the
    // extreme left/right silhouette go to ~0 alpha (soft edge) while verts
    // projecting near the centreline stay bright for the beam's full length
    // (bright core). Degenerate cases (no axis, axis pointing at the camera so
    // the screen axis collapses, zero spread) leave alpha untouched.
    // LOCAL-LIGHTS-1: a leaf whose lamps are tagged (bright apex, black base) carries
    // its own beam fade in the vertex-light tags -- use it; the leaf-centroid axis
    // and its screen feather do not describe multi-beam leaves.
    bool tagFade = false;
    for (int j = 0; j < nVerts && !tagFade; ++j) tagFade = isApexTag(verts[j].aRGBLight);
    if (tagFade)
        for (int j = 0; j < nVerts; ++j)
        {
            const DWORD t = verts[j].aRGBLight;
            const float lum = static_cast<float>(std::max({(t >> 16) & 0xffu, (t >> 8) & 0xffu, t & 0xffu})) / 255.0f;
            xv[j].fade = lum;
        }
    if (realism && haveAxis && nVerts > 0 && !tagFade)
    {
        const float featherExp = realism_cone_feather();
        if (featherExp > 0.001f)
        {
            // Apex (local origin) and axis tip, through the same transform.
            auto projectLocal = [&](const glm::vec3& p, float& ox, float& oy) -> bool {
                Stuff::Vector4D xf;
                xf.MultiplyPoint(p, shapeToClip);
                if (eye->usePerspective)
                {
                    if (xf.w == 0.0f) return false;
                    const float r = 1.0f / xf.w;
                    ox = (xf.x * r) * viewMulX + viewAddX;
                    oy = (xf.y * r) * viewMulY + viewAddY;
                }
                else
                {
                    ox = (1.0f - xf.x) * viewMulX + viewAddX;
                    oy = (1.0f - xf.y) * viewMulY + viewAddY;
                }
                return true;
            };

            glm::vec3 apexL; apexL.x = apexL.y = apexL.z = 0.0f;
            glm::vec3 tipL;
            const float sc = (scalar > 0.0f) ? scalar : 1.0f;
            tipL.x = axisLocal.x * lenLocal * sc;
            tipL.y = axisLocal.y * lenLocal * sc;
            tipL.z = axisLocal.z * lenLocal * sc;

            float ax = 0.0f, ay = 0.0f, tx = 0.0f, ty = 0.0f;
            if (projectLocal(apexL, ax, ay) && projectLocal(tipL, tx, ty))
            {
                float dx = tx - ax, dy = ty - ay;
                const float dlen = std::sqrt(dx * dx + dy * dy);
                if (dlen > 0.5f)   // axis not collapsed (beam not head-on)
                {
                    dx /= dlen; dy /= dlen;
                    // Perpendicular screen distance per vertex + max spread.
                    float perp[kMaxConeVerts];
                    float perpMax = 0.0f;
                    for (int j = 0; j < nVerts; ++j)
                    {
                        const float vx = xv[j].x - ax, vy = xv[j].y - ay;
                        float d = vx * dy - vy * dx;
                        if (d < 0.0f) d = -d;
                        perp[j] = d;
                        if (d > perpMax) perpMax = d;
                    }
                    if (perpMax > 0.5f)   // non-degenerate screen spread
                    {
                        for (int j = 0; j < nVerts; ++j)
                        {
                            float rN = perp[j] / perpMax;      // 0 core -> 1 rim
                            if (rN > 1.0f) rN = 1.0f;
                            xv[j].fade *= std::pow(1.0f - rN, featherExp);
                        }
                    }
                }
            }
        }
    }

    // Queue triangles (legacy spotlight path skipped backface culling —
    // tgl.cpp:2544 "(cosine > 0.0f) || isSpotlight" — so emit as authored).
    for (long f = 0; f < nTris; ++f)
    {
        const DWORD i0 = tris[f].Vertices[0];
        const DWORD i1 = tris[f].Vertices[1];
        const DWORD i2 = tris[f].Vertices[2];
        if (i0 >= static_cast<DWORD>(nVerts) || i1 >= static_cast<DWORD>(nVerts) || i2 >= static_cast<DWORD>(nVerts))
            continue;
        if (!xv[i0].ok || !xv[i1].ok || !xv[i2].ok)
            continue;   // z-range reject, mirrors Render (tgl.cpp:3303)
        if (s_tris.size() >= kMaxFrameTris)
            break;

        QueuedTri qt;
        const DWORD idx[3] = { i0, i1, i2 };
        for (int c = 0; c < 3; ++c)
        {
            const XV& s = xv[idx[c]];
            gos_VERTEX& gv = qt.v[c];
            gv.x = s.x; gv.y = s.y; gv.z = s.z; gv.rhw = s.rhw;
            gv.u = 0.0f; gv.v = 0.0f;
            // kVolumeGain: the peak alpha knobs were tuned while the pass ignored
            // alpha; premultiplied, 2.5x keeps the shaft a faint visible haze
            // (~16% of the lamp colour at the emitter) instead of vanishing.
            constexpr float kVolumeGain = 2.5f;
            DWORD a = static_cast<DWORD>((alphaPeak * s.fade * kVolumeGain));
            if (a > 255) a = 255;
            // The ISSPOTLGT pass blends ONE/ONE (VK legacy ADD pipeline, GL
            // gos_Alpha_OneOne): vertex alpha is ignored, so the fade must be
            // premultiplied into RGB, as QueueBeaconGlow already does. Writing
            // the fade into alpha alone added the cone at full lamp colour —
            // the opaque-looking yellow wedge.
            const DWORD pr = (((rgb >> 16) & 0xffu) * a) / 255u;
            const DWORD pg = (((rgb >>  8) & 0xffu) * a) / 255u;
            const DWORD pb = (( rgb        & 0xffu) * a) / 255u;
            gv.argb = (a << 24) | (pr << 16) | (pg << 8) | pb;
            gv.frgb = 0x00000000;   // fog disabled in the ISSPOTLGT pass
        }
        s_tris.push_back(qt);
        // Reserve now (update phase) — capacity lands in the vertex-block
        // allocation made by the FIRST MC2_ISSPOTLGT addVertices this frame.
        mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);
    }
}

// ---- LIGHT-REALISM-1: beacon emission ---------------------------------------

// Beacon glow intensity: peak alpha (0..255) at the disc centre. Restrained by
// design — a beacon spills a gentle colored pool, never a headlight.
static float realism_beacon_alpha()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_REALISM_BEACON">();
    return s;
}

// Ground-disc radius in world units.
static float realism_beacon_radius()
{
    static const float s = mc2::gate::get<"MC2_LIGHT_REALISM_BEACON_RADIUS">();
    return s;
}

namespace {

// Case-insensitive substring test (mirrors tgl.cpp's tgNameContainsCI, kept
// local so this module does not reach into tgl internals).
bool nameHasCI(const char* hay, const char* needle)
{
    if (!hay || !needle) return false;
    for (const char* h = hay; *h; ++h)
    {
        const char* a = h; const char* b = needle;
        while (*a && *b)
        {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>((ca - 'A' + 'a'));
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>((cb - 'A' + 'a'));
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// Beacon colour from its authored node name. Stock content spells these
// "Redbeaconlight02/03", "Greenbeaconlight01", "Bluebeaconlight" (tgl.cpp:73).
// Unrecognised beacon cards (e.g. plain "runwaylight") get a warm amber, the
// stock runway-edge tint.
DWORD beaconRgbFromName(const char* nodeName)
{
    if (nameHasCI(nodeName, "red"))   return 0x00ff2a18ul;
    if (nameHasCI(nodeName, "green")) return 0x0028ff3cul;
    if (nameHasCI(nodeName, "blue"))  return 0x003c6cfful;
    return 0x00ffc040ul;   // amber
}

} // namespace

void QueueBeaconGlow(TG_MultiShape* shape, float nightFactor)
{
    // Default-OFF gate -> byte-identical.
    if (!realism_enabled() || !glow_enabled() || !shape || !eye) return;
    if (nightFactor <= 0.001f) return;

    const float peak = realism_beacon_alpha() * nightFactor;
    if (peak < 1.0f) return;
    const float radius = realism_beacon_radius();

    if (s_queueTurn != turn)
    {
        s_tris.clear();
        s_queueTurn = turn;
    }

    const float viewMulX = TG_Shape::viewMulX, viewAddX = TG_Shape::viewAddX;
    const float viewMulY = TG_Shape::viewMulY, viewAddY = TG_Shape::viewAddY;

    // Project the beacon node's OWN origin through the proven QueueConeGlow
    // transform (shapeToClip = shapeToWorld * s_worldToClip, msl.cpp:2068).
    //
    // BEACON-GEOMETRY-1: an earlier revision built the disc as a world-space
    // ring and projected it with s_worldToClip ALONE. That collapsed — the
    // rendered blob was identical at radius 30 and radius 200 — because
    // s_worldToClip is only half of the legacy transform; it is composed with
    // the node matrix, never applied to world points on its own. So the disc
    // is now built in SCREEN space around the projected node origin, which is
    // both correct and what a glow sprite would do anyway. The world radius is
    // converted to pixels with the standard perspective relation
    // (screen size of world length L ~= L * viewMulX * rhw), so distant
    // beacons still get proportionally smaller pools.
    struct SV { float x, y, z, rhw; bool ok; };

    const int kSeg = 12;   // disc tessellation (12 wedges reads as round)

    // BEACON-DIAG-1 (MC2_LIGHT_CONE_DIAG=1): attribute a missing beacon glow to
    // the stage that dropped it — no billboards on the shape at all vs found
    // but untransformed vs found but clipped.
    int dWalked = 0, dFound = 0, dUntransformed = 0, dClipped = 0, dEmitted = 0;

    for (int i = 0; i < shape->GetNumShapes(); ++i)
    {
        const TG_ShapeRec* recp = shape->GetShapeRec(i);
        if (!recp || !recp->processMe) continue;
        TG_Shape* child = recp->node.get();
        if (!child) continue;
        ++dWalked;
        // Reuse LIGHT-BILLBOARD-GPU-1's classification verbatim (tgl.h:1047).
        if (!child->GetIsLightBillboard()) continue;
        ++dFound;

        // "Not yet transformed" sentinel, same as QueueConeGlow: a real prop
        // is never at EXACT world origin.
        {
            const float tx = glm::value_ptr(recp->shapeToWorld)[3];
            const float ty = glm::value_ptr(recp->shapeToWorld)[7];
            const float tz = glm::value_ptr(recp->shapeToWorld)[11];
            if (std::fabs(tx) + std::fabs(ty) + std::fabs(tz) < 0.01f)
            { ++dUntransformed; continue; }
        }

        const DWORD rgb = beaconRgbFromName(child->getNodeName());

        // Project the node origin exactly the way the cone path does.
        glm::mat4 shapeToClip;
        shapeToClip = recp->shapeToWorld * TG_Shape::s_worldToClip;

        glm::vec3 originL; originL.x = originL.y = originL.z = 0.0f;
        Stuff::Vector4D xf;
        xf.MultiplyPoint(originL, shapeToClip);

        SV c;
        float pixPerWu;
        if (eye->usePerspective)
        {
            const float r = (xf.w != 0.0f) ? (1.0f / xf.w) : 1.0f;
            c.x = (xf.x * r) * viewMulX + viewAddX;
            c.y = (xf.y * r) * viewMulY + viewAddY;
            c.z = (xf.z * r);
            c.rhw = static_cast<float>(fabs(r));
            pixPerWu = viewMulX * static_cast<float>(fabs(r));
        }
        else
        {
            c.x = (1.0f - xf.x) * viewMulX + viewAddX;
            c.y = (1.0f - xf.y) * viewMulY + viewAddY;
            c.z = xf.z;
            c.rhw = 0.000001f;
            pixPerWu = viewMulX * 0.002f;   // ortho: fixed screen scale
        }
        c.ok = (c.z >= 0.0f) && (c.z < 1.0f);
        if (!c.ok) { ++dClipped; continue; }

        float radPx = radius * pixPerWu;
        if (radPx < 2.0f)   { ++dClipped; continue; }   // sub-pixel -> skip
        if (radPx > 400.0f) radPx = 400.0f;             // sanity clamp

        // BEACON-GEOMETRY-2: the disc is built as CONCENTRIC RINGS, not a
        // single centre-anchored fan, and its intensity is PRE-MULTIPLIED into
        // the RGB channels.
        //
        // Why rings: this batch renders FLAT-SHADED. A centre-anchored fan
        // gives every triangle the same provoking (centre) vertex, so the whole
        // disc took one uniform colour and rendered as a solid hard-edged blob
        // — the very artefact this slice exists to remove. The authored cone
        // meshes get away with per-vertex alpha only because they carry many
        // triangles along the beam, so flat shading still steps a gradient.
        // Banding the disc into kRings uniform-coloured bands reproduces that:
        // each band is flat, but band-to-band it steps smoothly, and under
        // additive blend at these alphas the steps are invisible.
        //
        // Why pre-multiplied RGB: the pass is gos_Alpha_OneOne (dst += src) and
        // untextured, so scaling RGB is the reliable way to scale contribution
        // regardless of how alpha is treated. Alpha is scaled in lockstep.
        //
        // Depth: rings inherit the fixture's depth nudged slightly toward the
        // camera. Without the nudge the lower half of every disc was clipped by
        // the ground in front of it (a half-moon, visible in the r40 probe).
        const int kRings = 5;
        const float zBias = c.z * 0.998f;

        const float cr = static_cast<float>(((rgb >> 16) & 0xFFu));
        const float cg = static_cast<float>(((rgb >>  8) & 0xFFu));
        const float cb = static_cast<float>(( rgb        & 0xFFu));
        const float aScale = clampf(peak / 255.0f, 0.0f, 1.0f);

        auto mkVert = [&](float px, float py, float inten, gos_VERTEX& gv) {
            gv.x = px; gv.y = py; gv.z = zBias; gv.rhw = c.rhw;
            gv.u = 0.0f; gv.v = 0.0f;
            DWORD R = static_cast<DWORD>((cr * inten * aScale));
            DWORD G = static_cast<DWORD>((cg * inten * aScale));
            DWORD B = static_cast<DWORD>((cb * inten * aScale));
            if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
            DWORD A = static_cast<DWORD>((255.0f * inten * aScale));
            if (A > 255) A = 255;
            gv.argb = (A << 24) | (R << 16) | (G << 8) | B;
            gv.frgb = 0x00000000;
        };
        // Smooth inverse-ish falloff: full at the fixture, 0 at the rim.
        auto ringInten = [&](float t) { const float u = 1.0f - t; return u * u; };

        bool emittedAny = false;
        for (int rg = 0; rg < kRings; ++rg)
        {
            const float t0 = static_cast<float>(rg) / static_cast<float>(kRings);
            const float t1 = static_cast<float>((rg + 1)) / static_cast<float>(kRings);
            const float r0 = t0 * radPx, r1 = t1 * radPx;
            // One flat colour per band (see the flat-shading note above).
            const float inten = ringInten((t0 + t1) * 0.5f);
            if (inten <= 0.002f) continue;

            for (int s = 0; s < kSeg; ++s)
            {
                if (s_tris.size() + 2 > kMaxFrameTris) { ++dEmitted; return; }
                const float a0 = static_cast<float>(s) * (6.28318530718f / static_cast<float>(kSeg));
                const float a1 = static_cast<float>((s + 1)) * (6.28318530718f / static_cast<float>(kSeg));
                const float c0 = std::cos(a0), s0 = std::sin(a0);
                const float c1 = std::cos(a1), s1 = std::sin(a1);

                const float ix0 = c.x + c0 * r0, iy0 = c.y + s0 * r0;
                const float ix1 = c.x + c1 * r0, iy1 = c.y + s1 * r0;
                const float ox0 = c.x + c0 * r1, oy0 = c.y + s0 * r1;
                const float ox1 = c.x + c1 * r1, oy1 = c.y + s1 * r1;

                if (rg == 0)
                {
                    // Innermost band degenerates to a fan triangle.
                    QueuedTri qt;
                    mkVert(c.x, c.y, inten, qt.v[0]);
                    mkVert(ox0, oy0, inten, qt.v[1]);
                    mkVert(ox1, oy1, inten, qt.v[2]);
                    s_tris.push_back(qt);
                    mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);
                }
                else
                {
                    QueuedTri q1;
                    mkVert(ix0, iy0, inten, q1.v[0]);
                    mkVert(ox0, oy0, inten, q1.v[1]);
                    mkVert(ox1, oy1, inten, q1.v[2]);
                    s_tris.push_back(q1);
                    mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);

                    QueuedTri q2;
                    mkVert(ix0, iy0, inten, q2.v[0]);
                    mkVert(ox1, oy1, inten, q2.v[1]);
                    mkVert(ix1, iy1, inten, q2.v[2]);
                    s_tris.push_back(q2);
                    mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);
                }
                emittedAny = true;
            }
        }
        if (emittedAny) ++dEmitted;
    }

    if (aim_diag_enabled())
    {
        static unsigned long s_bcalls = 0;
        ++s_bcalls;
        if (s_bcalls <= 64 || (s_bcalls % 600) == 0)
        {
            mc2::fmt::print_err("[BEACON_DIAG v1] shape={:016X} shapes={:d} walked={:d} billboards={:d} untransformed={:d} clipped={:d} emitted={:d} peak={:.1f} radius={:.1f}\n", mc2::fmt::as_ptr(static_cast<void*>(shape)), mc2::fmt::as_i32(shape->GetNumShapes()), mc2::fmt::as_i32(dWalked), mc2::fmt::as_i32(dFound), mc2::fmt::as_i32(dUntransformed), mc2::fmt::as_i32(dClipped), mc2::fmt::as_i32(dEmitted), peak, radius);
            std::fflush(stderr);
        }
    }
}

bool GetSearchlightBeam(TG_MultiShape* shape, long nodeId, NodeBeam& out)
{
    const TG_ShapeRec* rec = shape ? shape->GetShapeRec(static_cast<int>(nodeId)) : nullptr;
    if (!rec || !rec->node || !rec->processMe) return false;
    const auto origin = mc2::math::transformPoint(glm::vec3(0, 0, 8), rec->shapeToWorld);
    const auto tip = mc2::math::transformPoint(glm::vec3(0, 0, 408), rec->shapeToWorld);
    const auto axis = mc2ToRender(tip - origin);
    if (glm::length(axis) < 1.0f) return false;
    out = NodeBeam{mc2ToRender(origin), glm::normalize(axis), 400.0f, 0.990268f}; // 8 degrees
    return true;
}

void QueueSearchlightGlow(TG_MultiShape* shape, long nodeId, unsigned long argb, float nightFactor)
{
    if (!eye || !glow_enabled() || nightFactor <= 0.0f || !mc2::renderSubmissions().active()) return;
    const TG_ShapeRec* rec = shape ? shape->GetShapeRec(static_cast<int>(nodeId)) : nullptr;
    if (!rec || !rec->node || !rec->processMe) return;
    if (s_queueTurn != turn) { s_tris.clear(); s_queueTurn = turn; }
    const glm::mat4 clip = rec->shapeToWorld * TG_Shape::s_worldToClip;
    // A faint, feathered shaft. Share the lamp hierarchy with the physical spot light;
    // use the existing additive queue, never the opaque static-prop pass.
    for (int band = 0; band < 8; ++band) {
        for (int side = 0; side < 12; ++side) {
            gos_VERTEX v[4]{};
            bool valid = true;
            for (int corner = 0; corner < 4; ++corner) {
                const float t = (band + (corner >= 2 ? 1.0f : 0.0f)) / 8.0f;
                const float angle = (side + (corner == 1 || corner == 2 ? 1.0f : 0.0f)) * 6.2831853f / 12.0f;
                const float radius = 2.0f + t * 56.0f;
                const glm::vec3 local(radius * std::cos(angle), radius * std::sin(angle), 8.0f + t * 400.0f);
                Stuff::Vector4D xf; xf.MultiplyPoint(local, clip);
                const float rhw = eye->usePerspective && xf.w != 0.0f ? 1.0f / xf.w : 1.0f;
                auto& dst = v[corner];
                dst.x = (eye->usePerspective ? xf.x * rhw : 1.0f - xf.x) * TG_Shape::viewMulX + TG_Shape::viewAddX;
                dst.y = (eye->usePerspective ? xf.y * rhw : 1.0f - xf.y) * TG_Shape::viewMulY + TG_Shape::viewAddY;
                dst.z = eye->usePerspective ? xf.z * rhw : xf.z;
                dst.rhw = eye->usePerspective ? std::fabs(rhw) : 0.000001f;
                valid &= dst.z >= 0.0f && dst.z < 1.0f && (!eye->usePerspective || xf.w > 0.0f);
                const float fade = nightFactor * 0.018f * (1.0f - t) * (1.0f - t);
                const auto r = static_cast<DWORD>(((argb >> 16) & 255) * fade);
                const auto g = static_cast<DWORD>(((argb >> 8) & 255) * fade);
                const auto b = static_cast<DWORD>((argb & 255) * fade);
                dst.argb = 0xff000000u | (r << 16) | (g << 8) | b;
            }
            if (!valid || s_tris.size() + 2 > kMaxFrameTris) continue;
            s_tris.push_back(QueuedTri{{v[0], v[1], v[2]}});
            s_tris.push_back(QueuedTri{{v[0], v[2], v[3]}});
            mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);
            mc2::renderSubmissions().addTriangle(0xffffffff, MC2_ISSPOTLGT);
        }
    }
}

void FlushFrame()
{
    if (s_tris.empty()) return;
    // Only flush the queue built THIS frame; a stale queue (renderLists
    // skipped a frame) is dropped by the turn guard in QueueConeGlow.
    if (s_queueTurn == turn && mc2::renderSubmissions().active())
    {
        for (size_t i = 0; i < s_tris.size(); ++i)
            mc2::renderSubmissions().addVertices(0xffffffff, s_tris[i].v, MC2_ISSPOTLGT);
    }
    s_tris.clear();
}

} // namespace mc2_cone_glow
