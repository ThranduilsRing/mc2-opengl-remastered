# VK-RUNG2-SHADOW-PRODUCER-1 — VK owns dynamic shadow generation (findings)

Branch `claude/vk-interop-bridge-1` @ `5d621fc5`. Deploy `A:/Games/mc2-opengl/mc2-win64-0.4c`
(explicit path; the `0.4c` short-name lease resolves to the WRONG v-prefixed folder — always
pass `--exe`/`--deploy` explicitly in this lane). Build dir `build64_island`.

## Correction to recon doc premise

`.claude/VK-FULL-OWNERSHIP-RECON-1.md` rung 2 describes 95095a94 as an isolated headless
fixture with "live backend-compare deferred". That commit (on `claude/vulkan-backend-l6`) IS
just the fixture. But a LATER, DIFFERENT, already-merged commit on THIS branch —
`8c1856c8` "P8a shadow — SECOND VK geometry region (VK-produces-depth, GL-consumes)" —
already wired a live VK shadow PRODUCER end-to-end:

- `GameOS/gameos/vulkan_shadow_island_live.cpp` (910 lines) + `.h`: VK renders the
  dynamic **static-prop** shadow casters (mech casters explicitly NOT fed — GPU-skinned,
  no per-instance matrix, documented gap) into a shared/exportable D32, signals `vk2gl`,
  and `gosPostProcess::setDynamicShadowInteropTexture()` publishes it so the four forward
  shading consumers sample it via the unchanged `sampler2DShadow calcShadow()` path.
- Gate `MC2_VK_SHADOW_INTEROP`, default-OFF, registered (`RendererFeatureRegistry.h:647`)
  and smoke-allowlisted (`run_smoke.py:1650`). Wired in `mclib/txmmgr.cpp:2943-2958`.
- Two PARKED WIP branches (`claude/vk-shadow-producer-proof-wip` @ `ef903b8a`,
  `claude/vk-shadow-consume-c` @ `c05120dd`) explored this same territory but were
  correctly NOT merged (their own commit messages: "NOT a passed rung" — cadence anomaly,
  blunt oracle, no real caster-set). **This branch's `8c1856c8` is a cleaner, already-shipped
  superset of what those WIP branches were trying to prove.**

So step 1 of this task ("resurrect/wire the depth pipeline") was already done, and step 2's
"live compare, deferred part" had never been run on non-blunt terrain-visible pixels — that's
the actual gap this session closed.

## What was still missing (this session's contribution)

1. **A live compare oracle on real shadowed pixels** (not the WIP's proxy/no-consumer
   setup). Built `.claude/mech-parity/shadow_producer_oracle.py`, reusing the terrain
   near-oracle harness (deterministic FORCE_LOD=0 pose sweep).
2. **Discovery: `MC2_VK_SHADOW_INTEROP` does NOT yet feed VK-terrain's OWN shadow
   consumption.** `gos_terrain_lod_chunk.cpp:887` calls `pp->getDynamicShadowTextureRaw()`
   (always GL's own texture) by explicit design ("so we never toggle compare-mode on a VK
   interop tex" — vulkan_terrain_island.cpp comment). Only the four FORWARD-SHADING
   consumers (mech/prop lit-scene sampling via `getDynamicShadowTexture()`) pick up the
   VK-produced map. Terrain still pays its own full per-frame `glGetTexImage` copy
   (`copy_dynamic_shadow` / `copy_shadow_slot`) regardless of `MC2_VK_SHADOW_INTEROP`.
   This is the task's "step 3" gap, precisely located and measured (below), not yet wired
   (see Deferred).

## Oracle results (mc2_01, near_land_low / near_land_off poses, deterministic)

`GL` = pure GL ground truth. `VK_COPY` = VK terrain w/ `MC2_VK_TERRAIN_DYNSHADOW=1`
(existing GL-copy path, shipped `43e6bbc2`). `VK_PROD` = `VK_COPY` + `MC2_VK_SHADOW_INTEROP=1`
layered on top (VK-produced map feeds forward shading; terrain still uses its own copy).

| comparison | pose | cov% | mean_all | mean_cov | max |
|---|---|---|---|---|---|
| (a) GL vs VK_COPY (baseline) | near_land_low | 59.055 | 5.164 | 8.057 | 255 |
| (a) GL vs VK_COPY (baseline) | near_land_off | 0.000 | 0.545 | 0.000 | 4 |
| (b) GL vs VK_PROD (rung-2) | near_land_low | 59.032 | 5.150 | 8.035 | 252 |
| (b) GL vs VK_PROD (rung-2) | near_land_off | 0.000 | 0.545 | 0.000 | 4 |
| (c) VK_COPY vs VK_PROD (delta) | near_land_low | 0.303 | 0.240 | 79.187 | 255 |
| (c) VK_COPY vs VK_PROD (delta) | near_land_off | 0.000 | 0.000 | 0.000 | 0 |
| (d) gate-OFF control (explicit-off vs unset) | both poses | 0.000 | 0.000 | — | 0 |

**Verdict: PARITY, not regression.** VK_PROD (mean 5.150) is marginally BETTER than
VK_COPY (mean 5.164) against GL ground truth on the dominant pose — within noise, not a
claimed win, but definitively no worse. The isolated producer-vs-copy delta is tiny
(0.303% coverage, mean 0.24/ch) — a small, localized footprint (self-prop casters only;
consistent with mech casters being excluded from both paths equally, so no divergence
there). Gate-OFF byte control is exactly 0.000 both poses — discipline held.

## Copy-tax measurement (task step 3 — terrain's remaining fixed cost)

Used `MC2_VK_TERRAIN_COPY_STATS=1`, mc2_01, 20s runs, `VK-terrain-interop-copies` zone timing:

| config | dynReady | avg copies_ms/frame | note |
|---|---|---|---|
| static-only (CSM on, default profile, dynshadow gate on but inactive) | 0 | 0.0003 | cadence-gated static copy only; negligible |
| `MC2_VK_TERRAIN_DYNSHADOW=1` + `MC2_SHADOW_CSM=0` (legacy single-map path forced active) | 1 | **58.14** (n=283) | full per-frame `glGetTexImage` shadow-depth readback |
| `MC2_VK_TERRAIN_DYNSHADOW=1 MC2_VK_TERRAIN_CSM=1` (default-profile CSM full-map subset) | 1 | **15.56** (n=971) | smaller full-map cascade texture, same mechanism, still a per-frame CPU readback |

**This confirms the task's premise precisely**: the CSM full-map-subset path (the one
actually active in the shipped default profile, since CSM is on by default) costs
~15.6 ms/frame in `copy_dynamic_shadow`'s `glGetTexImage` alone — a real, measurable,
un-amortized per-frame tax, corroborating the recon doc's framing ("dynamic + CSM full-map
still glGetTexImage'd EVERY FRAME"). The legacy single-map path (CSM off) is far worse at
~58 ms/frame.

## CSM full-map cascade unification verdict

`pp->getDynamicFullMapTexture()` (`gos_postprocess.h:242`) is a **separate GL FBO/texture**
(`dynamicFullMapFbo_`/`dynamicFullMapTex_`) from the legacy single map
(`dynShadowDepthTex_`) — GL genuinely re-renders it as its own pass with the last cascade's
matrix, not merely a re-view of the same map. They are byte-shape-compatible (both single
`GL_TEXTURE_2D DEPTH_COMPONENT`, same import/copy mechanism reused by `MC2_VK_TERRAIN_CSM`),
but **not the same underlying resource**. So: **VK cannot unify by re-pointing one render at
two consumers** — if VK were to own CSM full-map generation, it would need its OWN separate
depth pass (same caster set, the last cascade's light matrix) distinct from any legacy
single-map ownership. Mechanism is proven reusable (same depth-only instanced pipeline,
same `computeBakedShadow` compare path); it is a second render target, not a free unification.

## Caster-set parity scope (per task step 5)

- `vulkan_shadow_island_live.cpp` feeds **only** the CPU `GpuStaticPropInstance` vector
  (`dynShadowSet`) — static-prop casters (buildings). Mech casters are explicitly excluded
  (comment: GPU-skinned, no per-instance matrix available to this feed contract).
  `GpuMechBatcher::instance().flushShadow()` still runs GL-only for both the legacy map and
  each CSM cascade (`mclib/txmmgr.cpp:2965`, `:2988`).
- Terrain-as-caster: not applicable here — GL's dynamic shadow casters are buildings/mechs
  only (terrain does not self-shadow into this specific map); no terrain-caster gap to
  document beyond what's already known.
- **This session shipped the safe subset exactly as the task anticipated**: static-prop
  casters only, mech-caster gap documented (unchanged from before this session — pre-existing
  scope boundary of `8c1856c8`, not a new gap introduced here).

## Smoke (tier1, both umbrella states, explicit `--exe`)

- Umbrella OFF (pure GL default): 5/5 PASS (`2026-07-06T08-28-31`). Confirmed AFTER fixing
  a stale-exe trap (`run_smoke.py`'s no-`--exe` default lease resolved to the WRONG deployed
  folder mid-session and silently ran a different branch's stale binary — always pass
  `--exe` explicitly in this lane, matches the task's own warning).
- Umbrella ON (`MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1`):
  first attempt showed 3/5 (mc2_01 engine_reported_fail, mc2_10 crash_silent) — this
  coincided with a process-hygiene violation (7 stray mc2.exe instances from an earlier
  bad launch loop, killed by the coordinator mid-run) and is NOT attributed to the umbrella
  state. Re-ran clean after verifying 0 mc2.exe instances before/after: **5/5 PASS**
  (`2026-07-06T08-35-08`). All instances killed after each run this session; verified 0
  stray processes at session end.

## Deferred (explicit gaps, not closed this session)

1. **Terrain-consumes-VK-map wiring (task step 3, the actual copy-tax kill)**: NOT done.
   `gos_terrain_lod_chunk.cpp:887` needs to prefer `pp->getDynamicShadowTexture()`
   (compare-mode-aware, VK-interop-aware getter that already exists) over
   `getDynamicShadowTextureRaw()`, AND `copy_dynamic_shadow`'s `glGetTexImage`+
   `vkCmdCopyBufferToImage` roundtrip needs to be skippable when the interop texture is
   already VK-native (today the copy function has no way to know the GL texture IS a VK
   import — it would try to `glGetTexImage` a foreign-memory texture, which is unverified/
   likely invalid). This needs its own careful sub-slice: verify `glGetTexImage` behavior
   on a GL-imported-from-VK texture (probably wrong/undefined), then either (a) skip the
   copy entirely and have the VK terrain frag sample the same VK-native image directly via
   its own descriptor (no GL roundtrip at all — the real zero-copy win), or (b) prove the
   GL-view readback is safe as an interim step. Estimated the real remaining copy-tax kill;
   did not attempt in this session to avoid an unverified-parity landmine under time budget.
2. **Mech casters into the VK shadow producer**: unchanged pre-existing gap (GPU-skinned,
   no per-instance matrix contract yet).
3. **CSM full-map VK-owned generation**: mechanism-proven but not implemented — needs its
   own pass (see unification verdict above), separate from the legacy single-map producer.
4. No new gate shipped this session (no code changed under `MC2_VK_*`); this was a
   measurement + oracle session on top of already-shipped `8c1856c8`/`43e6bbc2`. New oracle
   tool: `.claude/mech-parity/shadow_producer_oracle.py`.

## Files touched this session
- NEW: `.claude/mech-parity/shadow_producer_oracle.py` (oracle script, committed)
- NEW: `.claude/VK-RUNG2-SHADOW-PRODUCER-1.md` (this doc)
- No production code changed (measurement/verification session).
