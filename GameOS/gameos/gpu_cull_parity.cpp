#include "gpu_cull_parity.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace gpu_cull {

static bool parity_isEnabled_impl() {
    return getenv("MC2_GPU_CULL_AABB_PARITY") != nullptr &&
           getenv("MC2_GPU_CULL_AABB_PARITY")[0] != '0';
}

bool parity_isEnabled() {
    static bool s_enabled = parity_isEnabled_impl();
    return s_enabled;
}

// Counters
static uint64_t s_totalChecked  = 0;
static uint64_t s_mismatches    = 0;
static uint64_t s_flushCount    = 0;

static constexpr float kEpsilon = 0.1f;  // world units; center should match nearly exactly

void parity_checkRecord(uint32_t actorId, const char* catName,
                        const float recCenter[3],
                        float posX, float posY, float posZ) {
    if (!parity_isEnabled()) return;
    ++s_totalChecked;

    // worldCenter is in raw MC2 world coords (x=east, y=north, z=elev).
    // The terrainMVP bakes the cameraPos axis swap internally — do NOT pre-swap here.
    float expCx = posX;
    float expCy = posY;
    float expCz = posZ;

    float dx = recCenter[0] - expCx;
    float dy = recCenter[1] - expCy;
    float dz = recCenter[2] - expCz;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);

    if (dist > kEpsilon) {
        ++s_mismatches;
        MC2_DIAGF("[GPU_CULL v1] event=parity_mismatch actor=%u cat=%s "
               "recCenter=(%.2f,%.2f,%.2f) expected=(%.2f,%.2f,%.2f) delta=%.4f\n",
               actorId, catName,
               recCenter[0], recCenter[1], recCenter[2],
               expCx, expCy, expCz, dist);
        fflush(stdout);
    }
}

void parity_flushSummary() {
    if (!parity_isEnabled()) return;
    ++s_flushCount;
    if ((s_flushCount % 600) == 1 || s_flushCount == 1) {
        MC2_DIAGF("[GPU_CULL v1] event=parity_summary mismatches=%llu total=%llu flush=%llu\n",
               (unsigned long long)s_mismatches,
               (unsigned long long)s_totalChecked,
               (unsigned long long)s_flushCount);
        fflush(stdout);
    }
}

} // namespace gpu_cull
