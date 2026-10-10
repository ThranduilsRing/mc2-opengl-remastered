//==========================================================================//
// File:    fx_cost_split.cpp                                                //
// Contents: FX cycle cost-split implementation. Q2-S0 observability slice.   //
//           Env-gated (MC2_FX_COST_SPLIT=1), default-off. See fx_cost_split.h//
//===========================================================================//

#include "fx_cost_split.h"

#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"

#if defined(_MSC_VER)
#  include <intrin.h>            // __rdtsc
#elif defined(__i386__) || defined(__x86_64__)
#  include <x86intrin.h>         // __rdtsc
#endif

namespace mc2 {
namespace fx_cost_split {

namespace {
    bool               g_init   = false;
    bool               g_on     = false;
    unsigned long long g_cyc [B_COUNT] = {0};
    unsigned long long g_call[B_COUNT] = {0};
    unsigned long long g_frames = 0;
}

bool is_enabled() {
    if (!g_init) {
        g_on = mc2::gate::get<"MC2_FX_COST_SPLIT">();
        g_init = true;
    }
    return g_on;
}

unsigned long long now_cycles() {
#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
    return __rdtsc();
#else
    return 0ULL;
#endif
}

void add(int bucket, unsigned long long cycles) {
    if (bucket < 0 || bucket >= B_COUNT) return;
    g_cyc[bucket]  += cycles;
    g_call[bucket] += 1ULL;
}

void roll_frame_and_maybe_emit() {
    if (!is_enabled()) return;
    ++g_frames;
    if (g_frames % 600ULL != 0ULL) return;

    const double f = 600.0;
    std::fprintf(stderr,
        "[FX_COST v1] event=summary frames=600 "
        "fx_execute_cyc=%.0f fx_execute_calls=%.1f "
        "particlecloud_execute_cyc=%.0f particlecloud_execute_calls=%.1f "
        "spinningcloud_animate_particle_cyc=%.0f spinningcloud_animate_particle_calls=%.1f "
        "tube_animate_profile_cyc=%.0f tube_animate_profile_calls=%.1f "
        "tube_bridge_flush_cyc=%.0f tube_bridge_flush_calls=%.1f "
        "particle_bridge_flush_cyc=%.0f particle_bridge_flush_calls=%.1f "
        "fx_forces_cyc=%.0f fx_forces_calls=%.1f "
        "fx_integrate_cyc=%.0f fx_integrate_calls=%.1f "
        "fx_birth_death_cyc=%.0f fx_birth_death_calls=%.1f\n",
        (double)g_cyc[B_FX_EXECUTE]            / f, (double)g_call[B_FX_EXECUTE]            / f,
        (double)g_cyc[B_PARTICLECLOUD_EXECUTE] / f, (double)g_call[B_PARTICLECLOUD_EXECUTE] / f,
        (double)g_cyc[B_SPINNINGCLOUD_ANIMATE] / f, (double)g_call[B_SPINNINGCLOUD_ANIMATE] / f,
        (double)g_cyc[B_TUBE_ANIMATE_PROFILE]  / f, (double)g_call[B_TUBE_ANIMATE_PROFILE]  / f,
        (double)g_cyc[B_TUBE_BRIDGE_FLUSH]     / f, (double)g_call[B_TUBE_BRIDGE_FLUSH]     / f,
        (double)g_cyc[B_PARTICLE_BRIDGE_FLUSH] / f, (double)g_call[B_PARTICLE_BRIDGE_FLUSH] / f,
        (double)g_cyc[B_FX_FORCES]             / f, (double)g_call[B_FX_FORCES]             / f,
        (double)g_cyc[B_FX_INTEGRATE]          / f, (double)g_call[B_FX_INTEGRATE]          / f,
        (double)g_cyc[B_FX_BIRTH_DEATH]        / f, (double)g_call[B_FX_BIRTH_DEATH]        / f);
    std::fflush(stderr);

    for (int i = 0; i < B_COUNT; ++i) { g_cyc[i] = 0; g_call[i] = 0; }
}

} // namespace fx_cost_split
} // namespace mc2
