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
                   unsigned long argb, float nightFactor)
{
    if (!glow_enabled() || !eye) return;
    if (nightFactor <= 0.001f) return;

    const TG_TypeShape* ts = nullptr;
    const TG_ShapeRec* rec = resolveSpotNode(shape, nodeId, &ts);
    if (!rec) return;

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

    const float alphaPeak = glow_alpha() * nightFactor;
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
        xv[j].fade = 1.0f - 0.85f * t;
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
