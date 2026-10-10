//==========================================================================//
// File:    spawn.cpp                                                        //
// Contents: mc2::particles::Spawn polymorphic dispatcher implementation.    //
//           Plan v6 §5.4 B1 Stage 2' C7-revised.                            //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/card.hpp"
#include "gosfx/cardcloud.hpp"
#include "gosfx/pointcloud.hpp"
#include "gosfx/shardcloud.hpp"
#include "gosfx/tube.hpp"

#include "spawn.h"
#include "spawn_card.h"
#include "spawn_cardcloud.h"
#include "spawn_point.h"
#include "spawn_shard.h"
#include "spawn_tube.h"

#include <cstdlib>
#include <cstdio>    // std::fprintf (gated min/max age summary)
#include "Gates/gate.h"

namespace mc2 {
namespace particles {

// VFX-AGE-SAMPLE-1: gate + age resolution. Lazy, process-lifetime cached.
namespace {
bool g_ageGate_initialized = false;
bool g_ageGate_value       = false;   // MC2_VFX_AGE_SAMPLE (default OFF)
bool g_ageLog_value        = false;   // MC2_GPU_PARTICLES_LOG (reuse for summary)
void ageGateInitIfNeeded() {
    if (g_ageGate_initialized) return;
    g_ageGate_value = mc2::gate::get<"MC2_VFX_AGE_SAMPLE">();
    g_ageLog_value  = mc2::gate::get<"MC2_GPU_PARTICLES_LOG">();
    g_ageGate_initialized = true;
}
}  // namespace

float resolveSampleAge(float callerAge) {
    ageGateInitIfNeeded();
    if (!g_ageGate_value) return 0.5f;                 // gate OFF → byte-identical
    // Reject NaN (all comparisons false), sentinel -1, and out-of-range.
    const bool valid = (callerAge >= 0.0f && callerAge <= 1.0f);
    const float age  = valid ? callerAge : 0.5f;       // fallback when unavailable

    if (g_ageLog_value) {
        // Rate-limited min/max/count summary so the gate-ON age spread is
        // observable in smoke logs (validation evidence). No per-spawn spew.
        static unsigned long long s_count = 0;
        static float s_min = 2.0f, s_max = -1.0f;
        static unsigned long long s_fallback = 0;
        if (!valid) ++s_fallback;
        if (age < s_min) s_min = age;
        if (age > s_max) s_max = age;
        if ((++s_count % 2000ull) == 0ull) {
            std::fprintf(stderr,
                "[VFX_AGE_SAMPLE v1] resolved=%llu min=%.3f max=%.3f fallback=%llu\n",
                s_count, (double)s_min, (double)s_max, s_fallback);
            std::fflush(stderr);
            s_min = 2.0f; s_max = -1.0f;   // reset window so spread tracks over time
        }
    }
    return age;
}

bool Spawn(gosFX::Effect::Specification* spec,
           const Stuff::LinearMatrix4D*  parentToWorld,
           float                          spawnSeed,
           float                          callerAge)
{
    if (!spec) {
        return false;
    }

    // GetClassID() Check_Object's the spec internally (see effect.hpp:112).
    const Stuff::RegisteredClass::ClassID id = spec->GetClassID();

    switch (id) {
        case gosFX::CardClassID:
            SpawnCard(static_cast<const gosFX::Card__Specification*>(spec),
                      parentToWorld, spawnSeed, callerAge);
            return true;
        case gosFX::CardCloudClassID:
            SpawnCardCloud(static_cast<const gosFX::CardCloud__Specification*>(spec),
                           parentToWorld, spawnSeed, callerAge);
            return true;
        case gosFX::PointCloudClassID:
            SpawnPoint(static_cast<const gosFX::PointCloud__Specification*>(spec),
                       parentToWorld, spawnSeed, callerAge);
            return true;
        case gosFX::ShardCloudClassID:
            SpawnShard(static_cast<const gosFX::ShardCloud__Specification*>(spec),
                       parentToWorld, spawnSeed, callerAge);
            return true;
        case gosFX::TubeClassID:
            SpawnTube(static_cast<const gosFX::Tube__Specification*>(spec),
                      parentToWorld, spawnSeed, callerAge);
            return true;
        default:
            // Pert / Shape / Debris / EffectCloud / unknown - B2 deferred.
            return false;
    }
}

}  // namespace particles
}  // namespace mc2
