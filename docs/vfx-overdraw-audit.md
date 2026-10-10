<!-- doc-status: stale | reviewed:2026-10-11 -->
# VFX Overdraw & Draw-Coverage Audit (VFX-OVERDRAW-AUDIT-0)

Measures current GPU-particle cost and resolves two correctness/coverage
questions before any visual work: **(1) is there dual-draw?** and **(2) which
gosFX classes bypass the GPU bridge?** No code changed — this is a read-only
audit built from existing instrumentation (`MC2_GPU_PARTICLES_LOG`,
`MC2_GOSFX_GROUP_LOG`, `MC2_FX_TRACE`) plus a code trace.

Worktree `track-rv-VFX` @ `c00b771a`. Deploy target for this lane is the
lane-private `A:/Games/mc2-opengl/mc2-win64-water/` (NOT the shared `v0.4`).
Runtime numbers from tier1 `mc2_10` / `mc2_24` / `mc2_01`, 30 s each, smoke
seed `0xC0FFEE`. Pairs with `docs/vfx-rv-arc-recon.md` (§4 blend matrix, §7
risks R4/R5/R8/R9).

---

## 1. Dual-draw: RESOLVED — **no dual-draw; legacy MLR is suppressed**

When `MC2_GPU_PARTICLES` is ON (default), a routed gosFX leaf renders **once**
(GPU billboard only). Each of the 5 producers, when `Batcher::is_enabled()`,
runs `Spawn(); <Parent>::Draw(info); return;` — the `return` skips its own
legacy MLR submission block, and the parent call resolves to `Effect::Draw`
which **only recurses children** (emits no geometry).

Verified first-hand:
- `mclib/particles/batcher.cpp:82` — default-ON gate (`!v → enabled`).
- `mclib/gosfx/card.cpp:505-508` — `if (Batcher::is_enabled()) { Spawn(...);
  Singleton::Draw(info); return; }` BEFORE the `DrawEffectInformation` MLR
  block at `:516+`.
- Same pattern: `cardcloud.cpp:503-506`, `pointcloud.cpp:478-481`,
  `shardcloud.cpp:322-325`, `tube.cpp:1175-1178`.
- `mclib/gosfx/effect.cpp:798-828` — the only `Draw` definition; body
  (`:813-827`) iterates children, no `DrawEffect`/`addTriangle`. Parents
  Singleton/ParticleCloud/SpinningCloud do not override `Draw`.

So for routed effects, `mcTextureManager->renderLists()` enqueues **zero**
legacy particle triangles. The recon's R8 ("possible double-render") is
**closed: it does not happen.** Runtime corroboration: `MC2_FX_TRACE`'s
`mlr_enqueue` counter is **empty** in all three missions (no particle MLR
enqueues), while GPU `emit_total` is large in combat.

---

## 2. Draw coverage: 5 routed / 5 unrouted concrete classes

Routing switch `mclib/particles/spawn.cpp:35-59` on `GetClassID()`; enum
`mclib/gosfx/gosfx.hpp:19-36`.

| gosFX class | routed to GPU? |
|---|---|
| Card, CardCloud, PointCloud, ShardCloud, Tube | **YES** (5) |
| PertCloud, ShapeCloud, Shape, DebrisCloud, PointLight | **NO — CPU-MLR only** (5) |
| Effect/ParticleCloud/SpinningCloud/Singleton | abstract (never instantiated) |
| EffectCloud | container; no own route — its leaves route individually |

Unrouted concrete classes (`spawn.cpp:56-57` default → `return false`) render
**only** via legacy MLR and are therefore **invisible to the GPU particle debug
views and to `MC2_VFX_DEBUG_MODE`**. They are NOT suppressed (no Batcher-gated
early-return on their `Draw`), so they continue drawing legacy geometry
normally — no regression, just no GPU-lane visibility.

### Runtime confirmation of the coverage gap

`mc2_01` (no-combat mission): **47 gosFX spawns but `emit_total = 0` GPU
particles** across 4403 flushes. The active effects there
(Jump_Jets, Mech_Smoking, Vehicle_Dust_Cloud, large_poof) produced **zero**
GPU billboards — the `MC2_FX_TRACE` class histogram shows only class `1312`
(Vehicle_Dust_Cloud) and `1318` (Dust), both of which emitted nothing to the
batcher. So at least the **ambient effect set (dust/jets/smoking/poof) is
CPU-MLR-only**; the GPU lane carries essentially the combat-hit/flare/explosion
effects. A debug-view or visual slice that only touches the GPU bridge will
have **no effect on quiet/ambient scenes** — important to state honestly.

---

## 3. Cost / overdraw numbers

Process-lifetime GPU emit aggregates (`flush_total` ≈ frames ran):

| Mission | scene | GPU draw groups (sampled frame) | tex binds | emit_total | flush_total | ~sprites/frame avg | trail kind |
|---|---|---|---|---|---|---|---|
| `mc2_10` | mid combat | 2 | 2 | 484,527 | 4,180 | ~116 | 1 (MissileSmoke) |
| `mc2_24` | dense urban combat | **8** | **8** | **813,171** | 4,209 | **~193** | 2 (PpcBolt) |
| `mc2_01` | no combat | 0 | 0 | **0** | 4,403 | 0 | — |

**Worst case = `mc2_24`** (urban combat): most draw groups (8 → 8 texture
binds/frame), highest sprite throughput (~193/frame avg), PpcBolt head-sprite
trails. `mc2_10` is the moderate combat reference. `mc2_01` is the zero-GPU
floor.

### Overdraw characteristics (the actual cost driver)

From `docs/vfx-rv-arc-recon.md` §4 + `gos_particle_bridge.cpp`, confirmed by the
group log (**every** observed group was `blend=additive`):

- **Additive-dominant** — all sampled draw groups across mc2_10/mc2_24 used
  `GL_SRC_ALPHA, GL_ONE` (`gos_particle_bridge.cpp:388-392`). Alpha groups
  exist in code but combat is additive-heavy (flares/explosions).
- **Depth-write OFF + double-sided** (`:305`, `:269`) — no early-Z rejection
  between overlapping particles → raw fill is unbounded by depth.
- **Composited PRE-bloom** (recon §4) — additive overdraw is *amplified* by the
  bloom pass. This is the strongest argument that an **additive-clamp / brightness
  control (VFX-TUNING-UI-1 / a future VFX-ADDITIVE-CLAMP)** is the highest-value
  cost lever, not geometry/count reduction.
- Per-group `count` is small (1 in the sampled frames) but **group count
  scales with distinct on-screen effects/textures** (8 in mc2_24); each group
  is its own draw call + texture bind + SSBO sub-upload
  (`gos_particle_bridge.cpp:334+`). Draw-call count, not vertex count, is the
  CPU-side cost.

No GPU timer query exists for the particle pass (Tracy GPU zones cover
shadow/terrain/3D/post, not the bridge). A true per-pass GPU-ms number would
need a new `glBeginQuery(GL_TIME_ELAPSED)` around the flush — **out of scope**
for this read-only audit; noted as a follow-up if perf becomes a gate.

### Effect inventory (FX_TRACE spawn names, combat missions)

mc2_10 (209 spawns / 15 unique) and mc2_24 (330 / 19) top effects: hits
(Generic_hit, Ground_Hit_Water, AC_10_Hit, ppc_hit, missile_hit), flares
(Lrg_las_flare, Missile_flare, PPC_flare, mg_flare), misses
(MG_Miss, Missile_Miss, PPC_Miss), Large_Explosion, ac_10_Trail, plus the
ambient set (Jump_Jets, Mech_Smoking, Vehicle_Dust_Cloud, large_poof). Class
histogram ids seen: 1312/1314/1316/1318/1320/1322/1324 (Dust/Flare/Shards/
MG_hit/lozenge/core families).

---

## 4. Findings → implications for Batch 2

- **F1 — No dual-draw.** Routed effects are GPU-only; the legacy path is dead
  for them. Visual work on the GPU bridge fully owns those effects' look. (R8
  closed.)
- **F2 — Coverage is combat-only.** Ambient/dust effects are CPU-MLR-only
  (unrouted) → GPU debug views + any GPU visual slice are **invisible in quiet
  scenes**. Any "VFX improvement" claim must be scoped to combat effects, or a
  separate routing slice (NOT authorized) would be needed first.
- **F3 — Additive overdraw, pre-bloom, is the cost & look driver.** Worst at
  mc2_24 (~193 sprites/frame, 8 additive groups, bloom-amplified). The
  cheapest, safest first lever is an **additive-brightness / intensity scale**
  (VFX-TUNING-UI-1) — no count/lifetime/emitter change, default no-op.
- **F4 — No per-pass GPU timer.** Cost is inferred from sprite/group counts +
  blend mode, not measured ms. Adequate for "bound the risk"; a GL timer is a
  later option.
- **F5 — age=0.5 still the look blocker.** Orthogonal to cost: particles don't
  animate (recon §2). Belongs to VFX-VISUAL-PLAN-0, not here.

**Verdict:** VFX is **safe to tune** — no dual-draw, no hidden CPU duplication,
overdraw bounded and combat-scoped. The first visual lever should target
additive intensity (cheap, default-no-op) and/or GPU-side age/curve eval (look,
not cost). No blocker found that prevents Batch 2 slices 2–3.
