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

#include "cone_glow.h"
#include "vis_semantics.h"   // VISIBILITY-SEMANTICS-1 supersession

#include "msl.h"
#include "txmmgr.h"
#include "camera.h"
#include "timing.h"

#include <cmath>
#include <cstdio>    // CONE-AIM-DIAG-1 / GHOST-LIGHT-DUMP-1 throttled dumps
#include <cstdlib>
#include <vector>

namespace mc2_cone_glow {

namespace {

// ---- env knobs (parse-once) -------------------------------------------------
// NOTE: each std::getenv("MC2_...") literal is kept on its own line at the
// call site (no name-forwarding helper) so scripts/flag-registry-auto-gen.py's
// single-line getenv sweep and check-env-registry.sh both see the readers.

float clampf(float f, float lo, float hi)
{
    if (f < lo) return lo;
    if (f > hi) return hi;
    return f;
}

// ---- per-frame glow queue ---------------------------------------------------
// Reservation (addTriangle) happens at queue time (update phase); vertex fill
// is deferred to FlushFrame() so the MC2_ISSPOTLGT vertex-block allocation in
// addVertices sees the FULL frame reservation (txmmgr.h:1014-1020 allocates on
// first addVertices with capacity = reservations so far).

struct QueuedTri { gos_VERTEX v[3]; };

std::vector<QueuedTri> s_tris;
long s_queueTurn = -1;

// Hard sanity caps.
const int   kMaxConeVerts = 512;
const size_t kMaxFrameTris = 8192;

// Resolve nodeId -> (rec, typeShape) or nullptrs.
const TG_ShapeRec* resolveSpotNode(TG_MultiShape* shape, long nodeId,
                                   const TG_TypeShape** outType)
{
    *outType = nullptr;
    if (!shape) return nullptr;
    const TG_ShapeRec* rec = shape->GetShapeRec((int)nodeId);
    if (!rec || !rec->node || !rec->processMe) return nullptr;
    TG_TypeNodePtr tn = rec->node->GetMyType();
    if (!tn || tn->GetNodeType() != SHAPE_NODE) return nullptr;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(tn);
    if (!ts->GetTypeVertices()) return nullptr;
    *outType = ts;
    return rec;
}

// Node-local cone axis (unit) + length from the type geometry. The cone apex
// is authored at/near the node pivot, so the vertex centroid direction IS the
// beam direction; length = max vertex projection along it. Returns false on
// degenerate geometry (too few verts / centroid at origin).
bool localConeAxis(const TG_TypeShape* ts, Stuff::Point3D& axisOut, float& lenOut)
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
    const float inv = 1.0f / (float)n;
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
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_NODES");
        return !(v && v[0] == '0');   // default ON
    }();
    return s;
}

// CONE-SPILL-GRADE-1: see cone_glow.h. Default ON preserves the behaviour
// c1f98b45 shipped, so adding this gate is itself a no-op; OFF restores the
// pre-c1f98b45 pool placement for grading.
bool spill_coupling_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_CONE_SPILL_COUPLING");
        return !(v && v[0] == '0');   // default ON
    }();
    return s;
}

bool glow_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_GLOW");
        return !(v && v[0] == '0');   // default ON
    }();
    return s;
}

float glow_alpha()
{
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_GLOW_ALPHA");
        float f = 40.0f;
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 0.0f, 255.0f);
    }();
    return s;
}

float spill_scale()
{
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_SPILL");
        float f = 1.0f;
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 0.0f, 3.0f);
    }();
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
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_VIS_GATE");
        return (v && v[0] == '1');   // PARKED default OFF
    }();
    return s;
}

// GHOST-LIGHT-VISIBILITY-1 (mover fairness rule) — see cone_glow.h for the
// sensorLevel semantics and why the appearance `visible` flag is not a
// fog-of-war predicate.
// GHOST-CONE-VERIFY-4 (2026-07-25): PARKED default-OFF alongside
// MC2_LIGHT_CONE_VIS_GATE — see the note there. =1 re-arms.
bool mover_vis_gate_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_MOVER_VIS_GATE");
        return (v && v[0] == '1');   // PARKED default OFF
    }();
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
// Gate OFF (default) -> every path below is skipped -> byte-identical.

bool realism_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_REALISM");
        return (v && v[0] == '1');   // default OFF
    }();
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
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_REALISM_CONE_OPACITY");
        float f = 0.40f;   // variant B (user-approved)
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 0.0f, 4.0f);
    }();
    return s;
}

// Silhouette feather exponent. Higher = the bright core narrows and the rim
// fades sooner (softer edge). 0 disables the radial feather.
static float realism_cone_feather()
{
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_REALISM_CONE_FEATHER");
        float f = 2.5f;   // AUDIT-TRUTH-1: variant B (user-approved; A was 1.5)
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 0.0f, 8.0f);
    }();
    return s;
}

// CONE-AIM-DIAG-1 (batch-7 item 4): per-node aim/apex dump. Default-off.
static bool aim_diag_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_DIAG");
        return (v && v[0] == '1');
    }();
    return s;
}

// CONE-AIM-FLIP-1 (batch-7 item 4, probe): negate the computed world aim so
// the pool lands on the OPPOSITE side — decisive A/B against the user report
// "the building light appears to be on the other side". Default OFF.
static bool aim_flip_enabled()
{
    static const bool s = []() {
        const char* v = std::getenv("MC2_LIGHT_CONE_AIM_FLIP");
        return (v && v[0] == '1');
    }();
    return s;
}

// ---- (A) node aim -----------------------------------------------------------

bool GetNodeAim(TG_MultiShape* shape, long nodeId,
                Stuff::Vector3D& aimWorldOut, float& coneLenOut)
{
    const TG_TypeShape* ts = nullptr;
    const TG_ShapeRec* rec = resolveSpotNode(shape, nodeId, &ts);
    if (!rec) return false;

    Stuff::Point3D axisLocal;
    float lenLocal = 0.0f;
    if (!localConeAxis(ts, axisLocal, lenLocal)) return false;

    // Transform origin and axis tip through the node's live matrix, convert
    // both to the MC2 frame, subtract -> world aim (handles any node basis).
    Stuff::Point3D zeroLocal;
    zeroLocal.x = zeroLocal.y = zeroLocal.z = 0.0f;
    Stuff::Point3D oMax, aMax;
    oMax.Multiply(zeroLocal, rec->shapeToWorld);
    aMax.Multiply(axisLocal, rec->shapeToWorld);

    Stuff::Vector3D aim;
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
            std::fprintf(stderr,
                "[CONE_AIM_DIAG v1] shape=%p node=%ld apexMC2=(%.1f,%.1f,%.1f) "
                "aimMC2=(%.3f,%.3f,%.3f) lenLocal=%.1f coneLen=%.1f axisLocal=(%.3f,%.3f,%.3f) flip=%d\n",
                (void*)shape, nodeId, -oMax.x, oMax.z, oMax.y,
                aimWorldOut.x, aimWorldOut.y, aimWorldOut.z,
                lenLocal, coneLenOut,
                axisLocal.x, axisLocal.y, axisLocal.z,
                aim_flip_enabled() ? 1 : 0);
            std::fflush(stderr);
        }
    }
    return true;
}

// ---- (B) cone glow ----------------------------------------------------------

void QueueConeGlow(TG_MultiShape* shape, long nodeId,
                   unsigned long argb, float nightFactor,
                   bool ownerIsMover)
{
    if (!glow_enabled() || !eye) return;
    if (nightFactor <= 0.001f) return;

    const TG_TypeShape* ts = nullptr;
    const TG_ShapeRec* rec = resolveSpotNode(shape, nodeId, &ts);
    if (!rec) return;

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
        && rec->node->GetLastTurnTransformed() != (DWORD)turn)
    {
        if (aim_diag_enabled())
        {
            static unsigned long s_rejects = 0;
            if ((++s_rejects % 240) == 1)
            {
                std::fprintf(stderr,
                    "[CONE_VIS_GATE v1] event=reject_stale shape=%p node=%ld "
                    "lastTurnTransformed=%lu turn=%ld rejects=%lu\n",
                    (void*)shape, nodeId,
                    (unsigned long)rec->node->GetLastTurnTransformed(),
                    turn, s_rejects);
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
    Stuff::Point3D axisLocal;
    float lenLocal = 0.0f;
    const bool haveAxis = localConeAxis(ts, axisLocal, lenLocal);

    // GHOST-LIGHT-DUMP-1 finding (batch-7): early frames can reach here BEFORE
    // the owner shape's first TransformMultiShape — shapeToWorld is still
    // zero/identity ([CONE_QUEUE_DIAG] apexMC2=(0,0,0)) and the cone would be
    // queued at world origin with a garbage screen transform. Real props never
    // sit at EXACT world origin (terrain elevation alone is nonzero), so a
    // near-zero translation is a reliable "not yet transformed" sentinel.
    {
        const float tx = rec->shapeToWorld.entries[3];
        const float ty = rec->shapeToWorld.entries[7];
        const float tz = rec->shapeToWorld.entries[11];
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
    Stuff::Matrix4D shapeToClip;
    shapeToClip.Multiply(rec->shapeToWorld, TG_Shape::s_worldToClip);

    const float scalar = rec->node->GetShapeScalarValue();
    const float viewMulX = TG_Shape::viewMulX, viewAddX = TG_Shape::viewAddX;
    const float viewMulY = TG_Shape::viewMulY, viewAddY = TG_Shape::viewAddY;

    // LIGHT-REALISM-1: overall transparency scalar (gate OFF -> factor 1.0).
    const bool  realism = realism_enabled();
    const float alphaPeak = glow_alpha() * nightFactor
                          * (realism ? realism_cone_opacity() : 1.0f);
    if (alphaPeak < 1.0f) return;
    const DWORD rgb = (DWORD)(argb & 0x00fffffful);

    // GHOST-LIGHT-DUMP-1 (batch-7 item 5): throttled per-queue dump so a
    // floating cone wedge with no visible caster can be attributed to the
    // queuing shape (correlate apex position with the light-pool dump).
    if (aim_diag_enabled())
    {
        static unsigned long s_qcalls = 0;
        ++s_qcalls;
        if (s_qcalls <= 48 || (s_qcalls % 600) == 0)
        {
            std::fprintf(stderr,
                "[CONE_QUEUE_DIAG v1] shape=%p node=%ld apexMC2=(%.1f,%.1f,%.1f) "
                "rgb=%06lx alphaPeak=%.1f nightFactor=%.2f\n",
                (void*)shape, nodeId,
                -rec->shapeToWorld.entries[3],
                rec->shapeToWorld.entries[11],
                rec->shapeToWorld.entries[7],
                (unsigned long)rgb, alphaPeak, nightFactor);
            std::fflush(stderr);
        }
    }

    // Transform all verts once (cone meshes are tiny).
    struct XV { float x, y, z, rhw; bool ok; float fade; };
    XV xv[kMaxConeVerts];
    for (int j = 0; j < nVerts; ++j)
    {
        Stuff::Point3D pos = verts[j].position;
        if (scalar > 0.0f)
        {
            pos.x *= scalar; pos.y *= scalar; pos.z *= scalar;
        }

        Stuff::Vector4D xf;
        xf.Multiply(pos, shapeToClip);

        float sx, sy, sz, rhw;
        if (eye->usePerspective)
        {
            rhw = (xf.w != 0.0f) ? (1.0f / xf.w) : 1.0f;
            sx = (xf.x * rhw) * viewMulX + viewAddX;
            sy = (xf.y * rhw) * viewMulY + viewAddY;
            sz = (xf.z * rhw);
            rhw = (float)fabs(rhw);
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
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
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
    if (realism && haveAxis && nVerts > 0)
    {
        const float featherExp = realism_cone_feather();
        if (featherExp > 0.001f)
        {
            // Apex (local origin) and axis tip, through the same transform.
            auto projectLocal = [&](const Stuff::Point3D& p, float& ox, float& oy) -> bool {
                Stuff::Vector4D xf;
                xf.Multiply(p, shapeToClip);
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

            Stuff::Point3D apexL; apexL.x = apexL.y = apexL.z = 0.0f;
            Stuff::Point3D tipL;
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
        if (i0 >= (DWORD)nVerts || i1 >= (DWORD)nVerts || i2 >= (DWORD)nVerts)
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
            DWORD a = (DWORD)(alphaPeak * s.fade);
            if (a > 255) a = 255;
            gv.argb = (a << 24) | rgb;
            gv.frgb = 0x00000000;   // fog disabled in the ISSPOTLGT pass
        }
        s_tris.push_back(qt);
        // Reserve now (update phase) — capacity lands in the vertex-block
        // allocation made by the FIRST MC2_ISSPOTLGT addVertices this frame.
        mcTextureManager->addTriangle(0xffffffff, MC2_ISSPOTLGT);
    }
}

// ---- LIGHT-REALISM-1: beacon emission ---------------------------------------

// Beacon glow intensity: peak alpha (0..255) at the disc centre. Restrained by
// design — a beacon spills a gentle colored pool, never a headlight.
static float realism_beacon_alpha()
{
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_REALISM_BEACON");
        float f = 70.0f;   // AUDIT-TRUTH-1: variant B (user-approved; A was 45)
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 0.0f, 255.0f);
    }();
    return s;
}

// Ground-disc radius in world units.
static float realism_beacon_radius()
{
    static const float s = []() {
        const char* v = std::getenv("MC2_LIGHT_REALISM_BEACON_RADIUS");
        float f = 40.0f;   // AUDIT-TRUTH-1: variant B (user-approved; A was 28)
        if (v && v[0]) f = (float)std::atof(v);
        return clampf(f, 1.0f, 200.0f);
    }();
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
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
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
        TG_Shape* child = recp->node;
        if (!child) continue;
        ++dWalked;
        // Reuse LIGHT-BILLBOARD-GPU-1's classification verbatim (tgl.h:1047).
        if (!child->GetIsLightBillboard()) continue;
        ++dFound;

        // "Not yet transformed" sentinel, same as QueueConeGlow: a real prop
        // is never at EXACT world origin.
        {
            const float tx = recp->shapeToWorld.entries[3];
            const float ty = recp->shapeToWorld.entries[7];
            const float tz = recp->shapeToWorld.entries[11];
            if (std::fabs(tx) + std::fabs(ty) + std::fabs(tz) < 0.01f)
            { ++dUntransformed; continue; }
        }

        const DWORD rgb = beaconRgbFromName(child->getNodeName());

        // Project the node origin exactly the way the cone path does.
        Stuff::Matrix4D shapeToClip;
        shapeToClip.Multiply(recp->shapeToWorld, TG_Shape::s_worldToClip);

        Stuff::Point3D originL; originL.x = originL.y = originL.z = 0.0f;
        Stuff::Vector4D xf;
        xf.Multiply(originL, shapeToClip);

        SV c;
        float pixPerWu;
        if (eye->usePerspective)
        {
            const float r = (xf.w != 0.0f) ? (1.0f / xf.w) : 1.0f;
            c.x = (xf.x * r) * viewMulX + viewAddX;
            c.y = (xf.y * r) * viewMulY + viewAddY;
            c.z = (xf.z * r);
            c.rhw = (float)fabs(r);
            pixPerWu = viewMulX * (float)fabs(r);
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

        const float cr = (float)((rgb >> 16) & 0xFFu);
        const float cg = (float)((rgb >>  8) & 0xFFu);
        const float cb = (float)( rgb        & 0xFFu);
        const float aScale = clampf(peak / 255.0f, 0.0f, 1.0f);

        auto mkVert = [&](float px, float py, float inten, gos_VERTEX& gv) {
            gv.x = px; gv.y = py; gv.z = zBias; gv.rhw = c.rhw;
            gv.u = 0.0f; gv.v = 0.0f;
            DWORD R = (DWORD)(cr * inten * aScale);
            DWORD G = (DWORD)(cg * inten * aScale);
            DWORD B = (DWORD)(cb * inten * aScale);
            if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
            DWORD A = (DWORD)(255.0f * inten * aScale);
            if (A > 255) A = 255;
            gv.argb = (A << 24) | (R << 16) | (G << 8) | B;
            gv.frgb = 0x00000000;
        };
        // Smooth inverse-ish falloff: full at the fixture, 0 at the rim.
        auto ringInten = [&](float t) { const float u = 1.0f - t; return u * u; };

        bool emittedAny = false;
        for (int rg = 0; rg < kRings; ++rg)
        {
            const float t0 = (float)rg / (float)kRings;
            const float t1 = (float)(rg + 1) / (float)kRings;
            const float r0 = t0 * radPx, r1 = t1 * radPx;
            // One flat colour per band (see the flat-shading note above).
            const float inten = ringInten((t0 + t1) * 0.5f);
            if (inten <= 0.002f) continue;

            for (int s = 0; s < kSeg; ++s)
            {
                if (s_tris.size() + 2 > kMaxFrameTris) { ++dEmitted; return; }
                const float a0 = (float)s * (6.28318530718f / (float)kSeg);
                const float a1 = (float)(s + 1) * (6.28318530718f / (float)kSeg);
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
                    mcTextureManager->addTriangle(0xffffffff, MC2_ISSPOTLGT);
                }
                else
                {
                    QueuedTri q1;
                    mkVert(ix0, iy0, inten, q1.v[0]);
                    mkVert(ox0, oy0, inten, q1.v[1]);
                    mkVert(ox1, oy1, inten, q1.v[2]);
                    s_tris.push_back(q1);
                    mcTextureManager->addTriangle(0xffffffff, MC2_ISSPOTLGT);

                    QueuedTri q2;
                    mkVert(ix0, iy0, inten, q2.v[0]);
                    mkVert(ox1, oy1, inten, q2.v[1]);
                    mkVert(ix1, iy1, inten, q2.v[2]);
                    s_tris.push_back(q2);
                    mcTextureManager->addTriangle(0xffffffff, MC2_ISSPOTLGT);
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
            std::fprintf(stderr,
                "[BEACON_DIAG v1] shape=%p shapes=%d walked=%d billboards=%d "
                "untransformed=%d clipped=%d emitted=%d peak=%.1f radius=%.1f\n",
                (void*)shape, shape->GetNumShapes(), dWalked, dFound,
                dUntransformed, dClipped, dEmitted, peak, radius);
            std::fflush(stderr);
        }
    }
}

void FlushFrame()
{
    if (s_tris.empty()) return;
    // Only flush the queue built THIS frame; a stale queue (renderLists
    // skipped a frame) is dropped by the turn guard in QueueConeGlow.
    if (s_queueTurn == turn && mcTextureManager)
    {
        for (size_t i = 0; i < s_tris.size(); ++i)
            mcTextureManager->addVertices(0xffffffff, s_tris[i].v, MC2_ISSPOTLGT);
    }
    s_tris.clear();
}

} // namespace mc2_cone_glow
