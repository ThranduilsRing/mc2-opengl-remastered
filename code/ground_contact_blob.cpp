//***************************************************************************
//
//	ground_contact_blob.cpp -- GROUND-CONTACT-BLOB-1
//
//	See ground_contact_blob.h for design notes.
//
//	Gate: MC2_GROUND_CONTACT_BLOB (env var; default OFF).
//	Off behavior: renderGroundContactBlobs() is a no-op.
//
//	Lives in code/ (not mclib/) because it walks ObjectManager's live mover
//	lists, and mclib must not depend on code/ (one-directional layering:
//	code/ depends on mclib/, never the reverse).
//
//	Copyright (C) Microsoft Corporation. All rights reserved.
//===========================================================================//

#include "ground_contact_blob.h"

#ifndef GAMECAM_H
#include "gamecam.h"
#endif

#ifndef OBJMGR_H
#include "objmgr.h"
#endif

#ifndef MOVER_H
#include "mover.h"
#endif

#ifndef MECH_H
#include "mech.h"
#endif

#ifndef GVEHICL_H
#include "gvehicl.h"
#endif

#include "terrain_runtime.h"  // TerrainRuntime::decalElevation
#include "gameos.hpp"         // WorldOverlayVert, gos_PushDecal

#include <cstdlib>
#include <cstdio>

namespace {

bool isEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("MC2_GROUND_CONTACT_BLOB");
        cached = (v && v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

// Half-size of the contact disc, world units (terrain cell == 42.67 units;
// this is a tight patch under the feet, not a full-cell splat -- recon
// landmine 2: keep the blob radius tight to foot-center to minimize overlap
// with footprint/crater decals sharing the same un-depth-sorted batch).
constexpr float kBlobRadius = 6.0f;

// Alpha clamp range -- recon landmine 3: never let the blob crush to pure
// black on an already-dark map, and never let it wash out on a bright one.
constexpr float kAlphaMin = 0.12f;
constexpr float kAlphaMax = 0.38f;

// Pack BGRA uint from floats [0,1] (matches dynamic_decal_ring.cpp convention:
// bits [31:24]=A [23:16]=R [15:8]=G [7:0]=B, little-endian BGRA).
unsigned int packBGRA(float r, float g, float b, float a)
{
    unsigned int ib = (unsigned int)(b * 255.0f + 0.5f);
    unsigned int ig = (unsigned int)(g * 255.0f + 0.5f);
    unsigned int ir = (unsigned int)(r * 255.0f + 0.5f);
    unsigned int ia = (unsigned int)(a * 255.0f + 0.5f);
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

// Ambient-coupled blob alpha: darker ambient -> lighter blob (so it still
// reads against a crushed-black map); brighter ambient -> the clamp caps it
// so it never oversells contact darkening on a well-lit scene.
float ambientCoupledAlpha()
{
    if (!eye) return kAlphaMin;
    // eye->ambient{Red,Green,Blue} are unsigned char [0,255].
    float ambientLum = (0.299f * eye->ambientRed +
                         0.587f * eye->ambientGreen +
                         0.114f * eye->ambientBlue) / 255.0f;
    // Inverse-couple: low ambient (dark map) -> alpha near kAlphaMax (blob
    // must be visible without pure-black crush); high ambient (bright map)
    // -> alpha near kAlphaMin (contact hint only, avoid stage-spotlight look).
    float t = 1.0f - ambientLum;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float a = kAlphaMin + (kAlphaMax - kAlphaMin) * t;
    if (a < kAlphaMin) a = kAlphaMin;
    if (a > kAlphaMax) a = kAlphaMax;
    return a;
}

// Push one contact-darkening quad (2 triangles) centered at worldPos.
void pushBlob(const Stuff::Vector3D& worldPos, float radius, unsigned int argb)
{
    float wz[4];
    float wx[4] = { worldPos.x - radius, worldPos.x + radius,
                    worldPos.x + radius, worldPos.x - radius };
    float wy[4] = { worldPos.y - radius, worldPos.y - radius,
                    worldPos.y + radius, worldPos.y + radius };
    for (int i = 0; i < 4; ++i) {
        Stuff::Vector3D p;
        p.x = wx[i]; p.y = wy[i]; p.z = 0.0f;
        wz[i] = TerrainRuntime::decalElevation(p);
    }

    // Sentinel texcoords: x<-0.5 = untextured solid-tint path (decal.frag);
    // y<-0.5 = self-darkening quad, skip decal.frag's inline sun-shadow
    // multiply (GROUND-CONTACT-BLOB-1 addition to decal.frag).
    const float su = -1.0f;
    const float sv = -1.0f;
    const float fog = 1.0f;  // no fog attenuation on the contact disc

    WorldOverlayVert t0[3];
    t0[0] = { wx[0], wy[0], wz[0], su, sv, fog, argb };
    t0[1] = { wx[1], wy[1], wz[1], su, sv, fog, argb };
    t0[2] = { wx[2], wy[2], wz[2], su, sv, fog, argb };
    gos_PushDecal(t0, 0);

    WorldOverlayVert t1[3];
    t1[0] = { wx[0], wy[0], wz[0], su, sv, fog, argb };
    t1[1] = { wx[2], wy[2], wz[2], su, sv, fog, argb };
    t1[2] = { wx[3], wy[3], wz[3], su, sv, fog, argb };
    gos_PushDecal(t1, 0);
}

int s_blobLogCount = 0;

} // anonymous namespace

void renderGroundContactBlobs(void)
{
    if (!isEnabled()) return;
    if (!ObjectManager) return;

    const unsigned int argb = packBGRA(0.0f, 0.0f, 0.0f, ambientCoupledAlpha());
    int pushed = 0;

    // Matches GameObjectManager::render()'s own mover iteration convention
    // (objmgr.cpp): walk the fixed-capacity mechs[]/vehicles[] arrays up to
    // their max* bound, skip null/non-existent/disabled slots.
    if (ObjectManager->mechs && (ObjectManager->numMechs < ObjectManager->maxMechs)) {
        for (long i = 0; i < ObjectManager->numMechs; ++i) {
            BattleMechPtr m = ObjectManager->mechs[i];
            if (!m || !m->getExists() || m->isDisabled()) continue;
            pushBlob(m->position, kBlobRadius, argb);
            ++pushed;
        }
    }

    if (ObjectManager->vehicles) {
        for (long i = 0; i < ObjectManager->maxVehicles; ++i) {
            GroundVehiclePtr v = ObjectManager->vehicles[i];
            if (!v || !v->getExists() || v->isDisabled()) continue;
            pushBlob(v->position, kBlobRadius, argb);
            ++pushed;
        }
    }

    if (pushed > 0 && s_blobLogCount < 8) {
        std::fprintf(stderr,
            "[GROUND_CONTACT_BLOB] event=submit count=%d radius=%.1f alpha=%.2f\n",
            pushed, kBlobRadius, ambientCoupledAlpha());
        std::fflush(stderr);
        ++s_blobLogCount;
    }
}
