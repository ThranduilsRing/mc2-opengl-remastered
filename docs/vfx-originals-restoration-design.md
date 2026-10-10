<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX Originals Restoration — Design (CPU-oracle → GPU-sim arc)

**Status:** approved strategy; Phase 1 pending spec sign-off.
**Branch:** `claude/track-rv-VFX` (rebased onto `claude/nifty-mendeleev` HEAD `a1771f12`).
**Authoritative recon:** this doc's §2 (from the gosFX-vs-placeholder recon, 2026-05-29).

---

## 1. Problem

The recent CPU→GPU particle move was incomplete. For the 5 routed gosFX
classes (Card / CardCloud / PointCloud / ShardCloud / Tube), the GPU bridge is
a **placeholder**: each frame it calls `mc2::particles::Spawn()`, which samples
the effect's spec curves at **one fixed age (0.5)**, re-randomized per frame,
and emits a frozen single-billboard approximation. Result: no fade-in/out, no
grow/shrink, no motion, wrong particle count, shimmer. Missile-smoke / PPC use
a *separate* hand-tuned trail path (`gpu_trail.cpp`); the flamer + unrouted
classes still render on CPU. The original gosFX animations are **not** played
back on the GPU path.

**Goal:** the GPU path reproduces the original gosFX animations
(missile/smoke/flamer/PPC/explosions) faithfully, end-state = GPU owns
simulation + rendering, CPU gosFX sim eventually deleted for migrated classes —
done in **staged, gated, parity-validated** steps, one class/family at a time.

## 2. Key recon findings (grounding)

- **The CPU gosFX simulation still runs every frame under
  `MC2_GPU_PARTICLES=1`.** `ParticleCloud::Execute` / `AnimateParticle`
  (`mclib/gosfx/particlecloud.cpp:358-486`) advance each particle's own age,
  velocity (drag/ether/accel, `pointcloud.cpp:302-448`), and per-frame color,
  populating **live parallel arrays**: `m_P_localTranslation[i]` (integrated
  position) and `m_P_color[i]` (RGBA at that particle's real age), e.g.
  `pointcloud.cpp:115-124`. Only `Draw()` is diverted to the placeholder —
  the real per-particle answer is computed and sitting in memory, then
  discarded.
- **Per-class cardinality:** Card = Singleton (1 particle, 4 verts); CardCloud
  / PointCloud / ShardCloud = many (`m_activeParticleCount` up to
  `m_maxParticleCount`); Tube = swept profile mesh (not particles).
- **GpuParticle record** (`mclib/particles/spec.h:42-52`, 64 B std430):
  `position[3], color[4], velocity[3], kind_flags, lifetime, age, size,
  atlasIndex`. Per-group texture/UV-rect/blend live in `GroupInfo`
  (`batcher.h:40-50`). The shader reads only position/color/size/uv/kind —
  `age`/`velocity`/`lifetime` are presently dead at render time.
- **Parity blockers, ranked:** (1) fixed-age-0.5 sample + no GPU age advance;
  (2) no motion integration + per-frame RNG shimmer; (3) birth-over-time
  dropped (only starting population); (4) VS size floor `max(size,8.0)`
  (`particle_billboard.vert:83`); (5) no per-particle rotation/aspect
  (CardCloud/ShardCloud spin, Card aspect/align); (6) Tube unrepresentable as
  one billboard; (7) per-group (not per-particle) UV-frame & blend.
- **Trails** (`gpu_trail.cpp`): missile-smoke / PPC are weaponbolt-driven
  ring-buffer billboards with hand-tuned constants — NOT gosFX-spec-driven.
  Flamer (`GpuTrailKind::None`) + unrouted classes still CPU MLR.

## 3. Architecture decision

**Three stages (approved):**

1. **CPU-oracle render parity** — GPU *renders* the CPU sim's final
   per-particle output. CPU sim stays authoritative; placeholder stays as
   fallback. This proves and locks the GPU **record/render ABI** against the
   real data without any GPU sim work.
2. **GPU simulation** — port each class's sim to GPU behind a gate, producing
   the *same record stream*, validated against the still-running CPU **oracle**
   (counts, lifetime, bounds, color ranges, captures).
3. **CPU-sim bypass/deletion** — only after accepted parity, per class, with a
   kill-switch and a soak.

CPU-sim→GPU-render is **explicitly the parity bridge, not the end-state.**

### The harvest tap (Phase 1 mechanism)

In each migrated class's `Draw()`, replace the placeholder `Spawn()` call with
a gated **harvest loop**: iterate `0 .. m_activeParticleCount`, read
`m_P_localTranslation[i]` (→ position, transformed by `m_localToWorld`) and
`m_P_color[i]` (→ color), set size from the per-spec scalar, and
`Batcher::Emit` one record per *active* particle. Still early-returns before
the legacy MLR draw → **no dual-draw**. Because the CPU already advanced each
particle's age, color/position are correct-over-life with zero GPU age logic.

### Record ABI

- **Phase 1 (PointCloud):** the existing 64 B `GpuParticle` is adequate —
  position ✓, color ✓ (already age-animated), size ✓; per-group
  texture/blend/UV. No struct change, no shader change (except the size-floor
  fix below).
- **Known ABI extensions (deferred to the class that needs them, not Phase 1):**
  per-particle `rotation` (CardCloud/ShardCloud spin), `aspect`/`size_y` (Card
  rectangle), `uvRect[4]` (per-particle frame), per-particle blend. A bump to
  ~80 B with a lockstep `shaders/include/particles.hglsl` mirror. Tube needs a
  separate swept-mesh primitive regardless — out of the billboard ABL.
- **Shader fix (Phase 1):** remove/relax the `effSize = max(p.size, 8.0)`
  floor at `particle_billboard.vert:83` so harvested sizes are honored
  (gate-guarded so default is unchanged).

## 4. Phase plan

| Phase | Slice | Deliverable | Gate | Commit |
|---|---|---|---|---|
| 1 | VFX-ORIGINAL-RECORD-ABI-1 | Harvest PointCloud CPU-oracle records → GPU render; placeholder = fallback | new gate, default OFF | `feat(vfx): render CPU-oracle particle records on GPU` |
| 2 | VFX-GPU-SIM-SPEC-0 | `docs/vfx-gpu-sim-spec.md` — full sim inputs for PointCloud | doc | `docs(vfx): specify GPU simulation for first effect class` |
| 3 | VFX-GPU-SIM-POINTCLOUD-1 | GPU sim for PointCloud behind a gate, same record stream | gate, CPU oracle still runs | `feat(vfx): simulate point cloud particles on GPU` |
| 4 | VFX-GPU-SIM-PARITY-1 | Side-by-side CPU-oracle vs GPU-sim evidence | — | `test(vfx): add GPU sim parity evidence` |
| 5 | VFX-CPU-SIM-BYPASS-1 | Bypass CPU sim for PointCloud, kill-switch, soak | kill-switch | `feat(vfx): bypass CPU sim for migrated particle class` |

**First class = CardCloud** (PIVOTED from PointCloud 2026-05-29). Probe finding:
under `MC2_GPU_PARTICLES=1` the CPU sim DOES run (`Execute` ticks), but
**PointCloud (classid 1314) reaches active=0 in stock tier1** ("Missile_Flare"
births nothing) — so a PointCloud harvest had nothing to read. **CardCloud
(1318, the Dust/flare workhorse) reaches ~30 live particles**; ShardCloud (1316)
~25. So the harvest premise holds for populated classes; the original target was
just empty. CardCloud is the executable first target. PointCloud/ShardCloud/
Card/Tube + trail families follow as their own class-by-class arcs.

CardCloud specifics vs the (abandoned) PointCloud plan: CardCloud has no
`m_P_localTranslation` array; the live per-particle center is
`GetParticle(i)->m_localTranslation`, color is `m_P_color[i]` (written in
`CardCloud::AnimateParticle`, live under GPU mode), size is
`m_scale*sqrt(m_halfX²+m_halfY²)`, and dead slots (`m_age>=1`) are filtered.
Per-particle rotation/aspect/UV-frame are deferred (existing 64B record).

**ShardCloud added (VFX-ORIGINAL-SHARDCLOUD-ABI-1).** Same CPU-oracle render
bridge under the same `MC2_VFX_ORACLE_RENDER` gate. ShardCloud is also
SpinningCloud→ParticleCloud (~25 live in tier1): center =
`GetParticle(i)->m_localTranslation`, color = `m_P_color[i*3]` (per-VERTEX,
3 per shard triangle, written in `ShardCloud::AnimateParticle`), size =
`m_scale*m_radius`, dead slots filtered. The shard **triangle shape**
(`m_angle`), per-particle rotation, and UV frame are DEFERRED — this slice
renders an approximate billboard (center+color+size). Both CardCloud and
ShardCloud are now covered by the bridge; the 64B record remains sufficient
for approximate billboard parity. Triangle/rotation/aspect fidelity needs the
ABI-extension follow-up.

### Phase 1 detail (the executable first slice)

- New gate (proposed `MC2_VFX_ORACLE_RENDER`, default OFF): when OFF, the
  current placeholder `Spawn()` path is byte-identical; when ON, PointCloud
  `Draw()` harvests live records → GPU.
- Emit one `GpuParticle` per active particle: position = `m_localToWorld *
  m_P_localTranslation[i]`, color = `m_P_color[i]`, size = per-spec scalar,
  atlasIndex/blend via the existing `BeginGroup` (same texture/blend the
  placeholder already resolves). No object-ID. No CPU-sim change beyond reading
  its already-computed output. No dual-draw (still skip MLR).
- Size-floor fix gated so default unchanged.
- Diagnostics `[VFX_ORACLE v1]` (rate-limited): harvested count vs
  `m_activeParticleCount` (must match), per-frame min/max bounds.

## 5. Parity methodology (Phases 1 & 4)

- **Counts:** harvested/GPU-sim particle count == CPU `m_activeParticleCount`
  per effect per frame (logged).
- **Bounds:** min/max world position + color/alpha ranges within CPU envelope.
- **Lifetime distribution** (Phase 4): age histogram CPU vs GPU.
- **Visual:** `vfx_combat_10` / `vfx_combat_24` captures (intro ~15–40 s
  particle window), CPU-draw vs oracle-render vs GPU-sim; confirm activity via
  `.log` (`enabled=1 sprites=N`), not PNG alone (transient VFX).
- **Perf delta** (Phase 4): coarse per-pass timing CPU-sim vs GPU-sim.
- **No gameplay/timing change** at every stage (Δdestroys +0, no emitter/
  lifetime/spawn-rate edits).

## 6. Hard constraints (all phases)

No all-at-once rewrite; one class/family at a time; gate every stage;
side-by-side parity evidence before any CPU-sim deletion. No gameplay/effect
timing regression, no weapon behavior change, no object-ID writes, no
texture/cook rewrite, no postprocess/tonemap change. Deploy: lane-private
`mc2-win64-v0.3`, **all** shader stages incl `.comp` (black-terrain lesson),
concurrent smoke OK, never `--kill-existing`.

## 7. Risks

- **R1 — harvest reads stale/again-cleared arrays.** Must confirm `m_P_*` are
  valid at the `Draw()` call point for PointCloud (recon says yes; verify in
  Phase 1). (MED)
- **R2 — ABI insufficiency surfaces mid-class.** PointCloud is billboard-clean;
  CardCloud/Shard/Card will force the struct bump. Sequence PointCloud first to
  defer it. (LOW for Phase 1)
- **R3 — size-floor fix changes default.** Must be gate-guarded; default path
  keeps `max(size,8.0)`. (MED — byte-identical gate-OFF is mandatory)
- **R4 — eventual CPU-sim deletion breaks unrouted classes / shared base.**
  Pert/Shape/Debris/PointLight stay CPU; deletion must be per-migrated-class,
  not wholesale `ParticleCloud::Execute` removal. (HIGH — Phase 5 guard)
- **R5 — trails are a separate model.** Folding missile/PPC/flamer onto the
  spec-driven model is its own family arc after the cloud classes. (scope)

## 8. Out of scope

Trail-family migration (separate arc), Tube swept-mesh primitive, GPU-side
FCurve/RNG port beyond the chosen class, any default flip before soak,
postprocess, object-ID, cook/texture work.
