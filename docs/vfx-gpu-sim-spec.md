<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX GPU Simulation Spec — first class: CardCloud (VFX-GPU-SIM-SPEC-0)

Spec for moving the gosFX **CardCloud** per-particle *simulation* onto the GPU,
using the shipped CPU-oracle render bridge as the parity target. **Docs only —
no implementation in this slice.** Stage 2 of the originals-restoration arc
(design: `docs/vfx-originals-restoration-design.md`). Phase 1 (CPU-oracle
render) shipped for CardCloud (`4409fdf8`) and ShardCloud (`78b7a2bc`).

All file:line grep-confirmed against `claude/track-rv-VFX` @ the ShardCloud
commit. Re-grep before quoting.

---

## 0. Target choice

**First GPU-sim target = CardCloud (1318).** It is the lead class (oracle render
already shipped + validated), the populated workhorse (~30 live in tier1), and
its *simulation* is the standard `ParticleCloud`+`SpinningCloud` model shared
with ShardCloud. CardCloud's harder bits (aspect, animated UV frame, rotation)
are **render-side** and already deferred in Phase 1 — they do not complicate the
*sim* port. ShardCloud's extra (triangle shape via `m_angle`) is likewise
render-side. So the sim spec applies near-identically to both; CardCloud first.

---

## 1. CPU simulation authority (what the GPU must reproduce)

Class hierarchy: `CardCloud : SpinningCloud : ParticleCloud : Effect`.

| Concern | Site |
|---|---|
| Per-effect age advance | `ParticleCloud::Execute` `mclib/gosfx/particlecloud.cpp:370` (`m_age += dT*m_ageRate`) |
| Birth accumulation | Start seeds `m_birthAccumulator += startingPopulation` (`particlecloud.cpp:350-353`); Execute adds `dT*particlesPerSecond` (`:398-401`) |
| Birth realization (active++ ) | `ParticleCloud::Execute` `:456-470` (`while m_birthAccumulator>=1 && active<max: CreateNewParticle; AnimateParticle; birthAccumulator-=1`) |
| Per-particle creation | `ParticleCloud::CreateNewParticle` `:523-592` — child seed, lifetime, emitter-volume offset, initial velocity |
| Per-particle age | `ParticleCloud::Execute` `:423` (`particle->m_age += dT*particle->m_ageRate`) |
| Death / removal | `m_age>=1` → `DestroyParticle` (`:466-467`, `:600` sets `m_age=1`); cloud finished when `Effect::HasFinished() && activeCount==0` (`:493`) |
| **Per-particle motion** | **`SpinningCloud::AnimateParticle` `mclib/gosfx/spinningcloud.cpp:406-…`** — drag (`m_pDrag`), ether (`m_pEtherVelocityX/Y/Z`), acceleration (`m_pAcceleration*`), velocity integrate (`AddScaled`), position integrate; CardCloud particles DO move + spin (`m_localRotation`) |
| Per-particle color | `CardCloud::AnimateParticle` `cardcloud.cpp:423-426` → `m_P_color[i]` |
| Per-particle size/aspect | `CardCloud::AnimateParticle` `:379-382` → `m_halfX/m_halfY` (birth-time from `m_halfHeight`*`m_aspectRatio`); `m_scale` per-frame (SpinningCloud) |

**Authoritative per-particle state** (in `m_data`, via `GetParticle(i)`):
`m_age`, `m_ageRate`, `m_seed`, `m_localLinearVelocity`/`m_worldLinearVelocity`,
`m_localTranslation` (position), `m_localRotation`, `m_scale`, `m_radius`,
`m_halfX`/`m_halfY` (CardCloud). Parallel render array `m_P_color[i]`.

**Determinism:** color/size/velocity/lifetime are deterministic functions of
`FCurve::ComputeValue(age, seed)` (`fcurve.hpp:130/186/245`) **given a particle's
age + seed**. But **births are NOT deterministic from (spec, effect-seed)
alone** — `CreateNewParticle` draws `Stuff::Random::GetFraction()` for child
seed, emitter offset (YawPitchRange), and initial velocity direction. The RNG
stream state is the hidden input. **This is the single biggest parity hazard
(see §8 R1) and drives the architecture choice in §4.**

---

## 2. Inputs the GPU needs

Per-effect-instance (uniform/SSBO metadata):
- spec id / curve set, `m_maxParticleCount`, `m_localToParent`, parent→world
  (`local_to_world = m_localToParent * parentToWorld`), `m_seed`, current
  `m_age`/`m_ageRate`, `m_birthAccumulator`, `m_activeParticleCount`,
  `m_state` texture handle + alpha/blend, `dT`.

Per-spec curves (baked, see §3): `m_particlesPerSecond`, `m_startingPopulation`,
`m_pLifeSpan`, `m_emitterSizeX/Y/Z`, `m_minimum/maximumDeviation`,
`m_startingSpeed`, `m_pEtherVelocityX/Y/Z`, `m_pAcceleration X/Y/Z`, `m_pDrag`,
`m_pRed/Green/Blue/Alpha`, CardCloud `m_halfHeight`, `m_aspectRatio`,
`m_USize/VSize/UOffset/VOffset`, `m_pIndex` (UV frame — render, deferred).

Per-particle persistent state (GPU buffer): position, velocity, age, ageRate,
seed (+ for full-GPU births: RNG sub-stream). Color/size are *derived* each
frame from curves(age,seed) — need not be stored, but the output record carries
the current sampled values for the renderer.

---

## 3. Curve evaluation on GPU

`FCurve::ComputeValue(t, seed)` = age-curve sample, optionally modulated by a
seed-curve. **Bake each curve to a small 1-D LUT** (proposed 64 taps over
t∈[0,1]; seed-curve as a second LUT or a 2-D table when `m_seeded`). Upload one
LUT texture/SSBO per active spec; the compute shader does a clamped linear
sample. This avoids porting the FCurve spline evaluator and bounds GPU cost.
Bake on the CPU at effect-spec load (one-time), keyed by spec id. Parity note:
LUT quantization introduces small error vs the CPU spline — budget it in the
compare tolerances (§5), not as an exact match.

---

## 4. GPU simulation design — RECOMMENDED: hybrid (CPU births, GPU integrate)

Because births depend on the CPU RNG stream (§1), the **lowest-parity-risk first
GPU sim keeps births on the CPU and moves only the per-frame integration to the
GPU**:

- **CPU (unchanged authority for births):** each frame, the existing CPU sim (or
  a thin birth-only driver) runs `CreateNewParticle` for new births — drawing the
  same `Random::GetFraction()` sequence as the oracle, so initial
  seed/position/velocity match **by construction**. New particles are appended to
  the GPU persistent buffer (CPU writes the new records).
- **GPU compute (the ported work):** per frame, for each live particle, advance
  `age += dT*ageRate`; integrate velocity (drag/ether/accel from curve LUTs) and
  position (`AddScaled`); evaluate color/size from curve LUTs at the new age;
  mark dead when `age>=1`. Writes the per-particle output record.
- **Render:** the existing `particle_billboard` path consumes the GPU records
  (same as the oracle bridge), once parity is accepted.

This is **GPU-sim for the per-frame physics + curve eval** (the bulk of the cost)
while sidestepping RNG parity. A later slice (VFX-GPU-SIM-BIRTHS) can port births
to GPU with a matched PRNG once the integration path is proven.

(Alternative — full-GPU births — is documented as deferred: needs a GPU PRNG
bit-matching `Stuff::Random` + GPU birth accumulator; high parity risk; not the
first slice.)

### Buffer layout / lifecycle
- **Persistent particle SSBO** (per-effect or a shared pool with per-effect
  ranges): array of GPU-sim particle structs (position, velocity, age, ageRate,
  seed). Capacity = `m_maxParticleCount` per effect.
- **Per-effect metadata SSBO/UBO:** the §2 per-instance values + curve-LUT
  handles + active range.
- **Spawn:** CPU appends new births into free slots (free list or
  append+compact). **Death:** `age>=1` flagged; compaction either GPU
  prefix-sum compaction or simply skip-dead at render (the oracle already skips
  `m_age>=1`). For the first slice, **skip-dead at emit** (no compaction) — keep
  it simple; compaction is an optimization slice.
- **Per-frame order:** CPU births → upload new records + metadata (dT, age,
  local_to_world) → GPU compute integrate → render reads records. One barrier
  between compute and the draw (SSBO → vertex read).
- **Bounds/count readback:** for compare mode only. Avoid per-frame blocking
  readback — use an atomic counter + a small results SSBO read **one frame
  late** (or every N frames) to dodge stalls (§8 R6).

### Output record
**Target the existing 64B `GpuParticle`** (`spec.h:42`): position, color, size,
age, lifetime, velocity, atlasIndex, kind_flags. All required fields fit. **No
struct bump for the first CardCloud GPU sim.** Deferred (same as Phase 1):
per-particle rotation, aspect, per-particle UV frame, shard triangle — these are
render-ABI extensions, orthogonal to the sim port.

---

## 5. Parity mode (compare-only first)

CPU sim stays authoritative and running. The GPU sim runs **in parallel, NOT
rendered**, and is compared against the CPU oracle. GPU output may be rendered
only after a separate accepted parity slice.

Compare metrics (logged, per effect or aggregate, rate-limited):
- `cpuCount` vs `gpuCount` (active/live particle count) — should match exactly
  (births are CPU-shared in the hybrid design).
- age min/max/avg (CPU vs GPU).
- alpha/color min/max range (CPU vs GPU) — within LUT tolerance.
- world-bounds min/max (CPU vs GPU) — within float/LUT tolerance.
- optional: sampled per-particle record diffs for the first K particles.

Tolerances: counts exact (hybrid); age/color/bounds within a small epsilon
covering LUT quantization + float order (propose ≤1% relative, tune in the
compare slice).

---

## 6. Gates (proposed; all default OFF)

- `MC2_VFX_GPU_SIM_CARDCLOUD=1` — run the GPU sim for CardCloud (compare-only
  unless render is explicitly enabled in a later slice). Default OFF.
- `MC2_VFX_GPU_SIM_COMPARE=1` — emit the §5 CPU-vs-GPU compare logs
  (`[VFX_GPU_SIM v1]`). Default OFF.
- **Kill-switch:** with the above OFF, behavior is exactly today's
  CPU-oracle render (under `MC2_VFX_ORACLE_RENDER`) / placeholder. The GPU sim
  never affects the rendered frame in this stage.
- Register in `RendererFeatureRegistry.h` (kFeatureTable) + `docs/tier1_env_vars.md`
  + capture `TRACKED_FLAGS` when implemented.

---

## 7. Validation plan (for the implementation slices that follow)

- Build RelWithDebInfo; env-registry PASS; object-ID firewall PASS;
  shader_reflect if a compute shader/uniform lands.
- tier1 gate-OFF 5/5 (byte-identical; GPU sim inert).
- `MC2_VFX_GPU_SIM_CARDCLOUD=1 MC2_VFX_GPU_SIM_COMPARE=1` on vfx_combat_10 /
  vfx_combat_24 / mc2_24: `[VFX_GPU_SIM v1]` shows cpuCount==gpuCount, age and
  alpha and bounds within tolerance, no GL errors, Δdestroys +0, no gameplay
  delta. Deploy mc2-win64-v0.3, all shader stages incl `.comp`; never
  `--kill-existing` (use direct-launch if the run_smoke guard blocks).
- Perf counters: coarse GPU-compute time vs CPU-sim time (the payoff signal for
  the eventual CPU-sim bypass).

---

## 8. Risks

- **R1 — birth RNG drift (HIGH).** `CreateNewParticle` uses
  `Stuff::Random::GetFraction()`; a GPU-side birth path cannot match the CPU RNG
  stream without a bit-exact GPU PRNG. **Mitigation: hybrid design (§4) keeps
  births on CPU** — counts/initial state match by construction. Full-GPU births
  deferred.
- **R2 — birth accumulator mismatch (MED).** Fractional `m_birthAccumulator`
  carry-over (`:469-477`) must be reproduced if births ever move to GPU; N/A for
  hybrid.
- **R3 — CPU/GPU float + LUT drift (MED).** Curve LUT quantization + float order
  differ from the CPU spline. Handle via compare tolerances, not exact match.
- **R4 — sorting/order (LOW-MED).** Particle order in the GPU buffer may differ
  from CPU iteration; billboards are order-independent except for alpha-blend
  overdraw. Note but accept for compare; revisit at render-enable.
- **R5 — missing rotation/aspect/UV/triangle (LOW).** Render-ABI, already
  deferred; not a sim-parity concern.
- **R6 — readback stalls (MED).** Synchronous count/bounds readback per frame
  stalls the pipe. Use deferred/async readback or every-N-frames; compare-only.
- **R7 — premature CPU bypass (HIGH).** Do NOT bypass/delete the CPU sim until a
  later accepted-parity slice (VFX-CPU-SIM-BYPASS). Compare-only first.

---

## 9. Recommendation

1. **Implement GPU sim in compare-only mode first** (`MC2_VFX_GPU_SIM_CARDCLOUD`
   + `MC2_VFX_GPU_SIM_COMPARE`, default OFF), CPU oracle as reference.
2. **Hybrid architecture** (CPU births, GPU per-frame integrate + curve-LUT eval)
   to sidestep RNG-drift parity risk.
3. **No struct bump** — existing 64B record suffices for the CardCloud sim
   output; rotation/aspect/UV remain render-ABI follow-ups.
4. **CardCloud only**; do not spec/implement all gosFX families at once.
   ShardCloud reuses this spec next (sim is the same model).
5. **Do not bypass the CPU sim** until a separate accepted-parity slice.

### Status
- **VFX-GPU-SIM-CARDCLOUD-BUFFER-1 IMPLEMENTED** (split from VFX-GPU-SIM-CARDCLOUD-1):
  persistent GPU sim SSBO (`CardCloudSimParticle` 64B, `gos_cardcloud_sim.cpp`) +
  CardCloud live-state handoff + gates `MC2_VFX_GPU_SIM_CARDCLOUD` /
  `MC2_VFX_GPU_SIM_COMPARE` (default OFF) + CPU-side integrity log. OBSERVE-ONLY:
  no compute, no readback, no render. COMPUTE-1 adds the integration.

- **VFX-GPU-SIM-CARDCLOUD-COMPUTE-1 IMPLEMENTED.** Per-frame accumulation (all
  CardCloud instances, fixes BUFFER-1 last-writer-wins) + `cardcloud_sim.comp`
  (age+=ageRate·dt, pos+=vel·dt, dead-flag) dispatched once/frame from
  `Batcher::Flush` + SSBO barrier + gated `glGetBufferSubData` readback. The
  compare diffs GPU output against a **CPU reference applying the identical step
  to the same accumulated input** (`[VFX_GPU_SIM v2]` maxAgeError/maxPosError
  ≈ float epsilon) — this **validates the GPU compute path** (shader/SSBO/
  dispatch/barrier/readback/accumulation). It does **NOT** prove full gosFX
  lifetime physics: drag/ether/accel and life-long GPU-vs-gosFX parity (which
  needs stable per-particle IDs for cross-frame index correspondence) are
  deferred to **VFX-GPU-SIM-CARDCLOUD-PARITY-ID-1**. dt is a fixed validation
  constant (1/60); real per-frame dt is a parity-slice concern. Compare-only —
  nothing rendered, CPU sim still authoritative.

### Next slices
- VFX-GPU-SIM-CARDCLOUD-PARITY-ID-1 — stable particle IDs + birth tracking +
  drag/ether/accel in the compute; true GPU-vs-gosFX divergence over a
  particle's life (cross-frame, by ID).
- VFX-GPU-SIM-PARITY-1 — side-by-side CPU-oracle vs GPU-sim evidence.
- VFX-GPU-SIM-RENDER-1 — render from GPU-sim records (after parity).
- VFX-CPU-SIM-BYPASS-CARDCLOUD-1 — bypass CPU sim for CardCloud (kill-switch, soak).
- (later) VFX-GPU-SIM-BIRTHS-1 — port births to a matched GPU PRNG; ABI-extension
  for rotation/aspect/UV; ShardCloud/Card/Tube + trail families.
