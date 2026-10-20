// mclib/particles/cardcloud_sim.h
//
// VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2 of the originals-restoration arc;
// see docs/vfx-gpu-sim-spec.md). Shared record for the CardCloud GPU
// simulation buffer. CardCloud::Draw (mclib) fills these from the live CPU
// sim state and submits them to the GameOS GL bridge (gos_cardcloud_sim.cpp),
// which uploads them to a persistent SSBO.
//
// THIS SLICE IS COMPARE-ONLY SUBSTRATE: the buffer is allocated + uploaded,
// but NOT integrated (no compute), NOT read back, and NOT rendered. CPU sim
// stays authoritative. COMPUTE-1 adds the compute integration + readback +
// parity compare on this same record layout.
//
// Layout: std430-clean 64 B. Scalars are packed into the .w lanes of the
// vec3s to avoid std430 vec3-stride pitfalls, so the GLSL mirror COMPUTE-1
// adds can use vec4 loads. Field set is future-proof for COMPUTE-1
// integration (position, velocity, age, ageRate, lifetime, color, size, flags).

#pragma once

#include <cstdint>

namespace mc2 { namespace particles {

struct CardCloudSimParticle {
    float    position[3];   //  0  world-space center (m_localToParent*parentToWorld * m_localTranslation)
    float    ageRate;       // 12  d(age)/dt for this particle (COMPUTE-1 age advance)
    float    velocity[3];   // 16  world linear velocity (COMPUTE-1 integration)
    float    age;           // 28  normalized age in [0,1)
    float    color[4];      // 32  straight RGBA (current CPU-animated value)
    float    size;          // 48  billboard radius (scale*sqrt(halfX^2+halfY^2))
    float    lifetime;      // 52  total seconds (informational)
    uint32_t flags;         // 56  bit0 = alive
    uint32_t _pad;          // 60  pad to 64
};

static_assert(sizeof(CardCloudSimParticle) == 64,
              "CardCloudSimParticle must stay 64B (update the COMPUTE-1 GLSL mirror in lockstep)");

inline constexpr uint32_t kCardCloudSimFlagAlive = 1u;

} } // namespace mc2::particles

// GameOS GL bridge entries (defined in GameOS/gameos/gos_cardcloud_sim.cpp).
//
// COMPUTE-1: submit() ACCUMULATES each CardCloud instance's compacted live
// records into a per-frame CPU buffer (fixes BUFFER-1's last-writer-wins).
// flush() — called once per frame from Batcher::Flush — uploads the whole
// accumulated array to the persistent SSBO, dispatches cardcloud_sim.comp to
// integrate one validation step, and (when MC2_VFX_GPU_SIM_COMPARE=1) reads
// back and compares against a CPU reference applying the SAME step to the same
// input (maxAgeError/maxPosError ≈ epsilon → validates the compute path).
// cpuActiveCount is the raw m_activeParticleCount (incl. dead slots) for the
// integrity comparison vs the compacted live `count`. No render, no CPU bypass.
extern "C" void gos_cardcloud_sim_submit(
    const mc2::particles::CardCloudSimParticle* records,
    unsigned int                                count,
    unsigned int                                cpuActiveCount);

// Per-frame: upload accumulated records, dispatch compute, (gated) read back +
// compare, then clear the accumulator. No-op when nothing was accumulated.
extern "C" void gos_cardcloud_sim_flush(void);
