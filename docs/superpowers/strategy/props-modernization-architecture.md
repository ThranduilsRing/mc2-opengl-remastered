# MC2 Props Modernization Architecture

**Status:** Strategy / design doc (long-term), 2026-06-11.
**Scope:** Static objects — trees, buildings, debris, gates, walls — across cull, lighting/material, LOD, update-cardinality (service lane), and editor parity.
**Siblings:** `asset-cook-pipeline-architecture.md` (owns LOD/material **cooking** — this doc only defines what the runtime consumes), `vfx-modernization-roadmap.md` (GL state policy — adopted verbatim here), `telemetry-oracle-cockpit-architecture.md` (budget/counter surfacing), `runtime-bridge-architecture.md`.
**Grounding:** `GameOS/gameos/gos_static_prop_batcher.cpp` (8.6k-line batcher; depth-prepass `flushDepthPrepassV6` ~:4989, gate `MC2_STATIC_PROP_DEPTH_PREPASS` ~:4999), `mclib/bdactor.cpp`/`bdactor.h` (appearances, tree LOD seam :3834/:3856/bdactor.h:478,518; black-tree bake invariant sites :4908,:4281,:2893…), `docs/gpu-static-prop-cull-lessons.md` (cull-chain anatomy + traps), `docs/hzb-visibility-mvp.md` + `docs/hzb-depth-convention.md` + `docs/hzb-staticprop-cull-readiness.md` (HZB substrate, probe counters, camera-discontinuity guard), `docs/static-prop-lighting-audit.md` (V-LIGHTING-STATIC-0), `docs/staticprop-material-orm-normal-recon.md`, `docs/ibl-plan.md` (V-IBL-STATIC-0), `docs/tree-override-lod-spec.md` + `.claude/tree_lod_bake.py` (repo root; walnut 508K→10K proven), `docs/static-building-skip-broadening-recon.md` + `docs/modernization-roadmap-2026-06-09.md` (service lanes), memory handoffs 2026-06-04 (FROZEN-STATIC-CULL-RECORDS spec on `claude/perf-gpucull-ownership-1` HEAD `eea91262`; foliage prepass Lane A).

---

## 1. North star

> **A static prop is uploaded once, culled on the GPU against frozen records, drawn through the batcher in O(visible) with distance-correct LOD and PBR-lit pixels — and the CPU touches it only when an event (destruction, override swap, editor edit) actually changes it. The legacy per-frame update/render walk survives only as the gameplay-service lane and the flag-off fallback, never as the render driver.**

Corollaries:

1. **One projection.** Cull, render, pick, and HZB all derive from the same GL-NDC matrix (`worldToClipGL`, reverse-Z `ZERO_TO_ONE`). Every props bug class in this repo's history (shadow vanish `09707cd8`, terrain cull mirror `a280dde2`, RTT pick split-brain) is a D3D↔GL split-brain. No new consumer may hand-fold axis swaps.
2. **Cull is conservative or it is wrong.** A false-positive draw costs microseconds; a false-negative cull is a vanished building (the wolfman-zoom ~87% FN angular cull, `docs/gpu-static-prop-cull-lessons.md`). Every new cull stage ships with an FN oracle (budget = 0) before it may suppress a draw.
3. **The cull gates are load-bearing for more than visibility.** `objBlockInfo.active` / `objVertexActive` gate object **AI update**, not just render (objmgr.cpp:1486-1488 walk). Render-visibility decisions (HZB, GPU cull, LOD) must never feed back into the gameplay gates — that is the service-lane split's job (§6), and conflating them is the #1 historical trap.
4. **GL state ownership is law** (vfx roadmap §rule 1): every props pass — prepass, opaque, alpha-test, impostor — sets depth test/func/mask, blend, cull explicitly on entry and restores on exit. The chunk-terrain depth-mask transparency saga (`f375e0ba`) is the precedent.
5. **Cook owns derivation, runtime owns consumption** (asset-cook doc §1): LOD meshes, impostor atlases, and material records are cooked sidecars with manifest entries; the runtime probes, validates, and falls back to legacy. No flag-day.

---

## 2. Current substrate map

| Subsystem | State | Anchor |
|---|---|---|
| **Batcher** | `gos_static_prop_batcher.cpp` — instanced GPU static-prop path, MaterialGpu records, baked light slots, static replay (`touch()`/`ResubmitCachedGpuLightData`), Tracy `Render.GpuStaticProps` | whole file; lessons doc |
| **CPU angular cull** | Legacy per-terrain-vertex cone cull → `objBlockInfo.active`/`objVertexActive` → gates update AND render. Known-broken at wolfman zoom (FN). Terrain side now chunk-derived (8a/8c, `produceActiveSetFromChunks`) | `mclib/terrain.cpp:1040-1127`, `mclib/bdactor.cpp:1090-1167` (recalcBounds/inView), objmgr.cpp |
| **GPU cull** | Slice-A oracle corrected; H4 bimodal (~0.5% steady, 30-65% on camera motion) = per-frame static cull-record rebuild. **FROZEN-STATIC-CULL-RECORDS specced, not built**: frozen pool-aligned records, substrate order static-prefix `[0,S)` + dynamic `[S,S+D)` (M1 = binding-8 order reversal) | branch `claude/perf-gpucull-ownership-1` HEAD `eea91262`; HANDOFF 2026-06-04 |
| **HZB** | Build + diagnostic probe SHIPPED, default-OFF, `neverAppliedToDraws=1`. MIN-reduce R32F per-level ladder (not mips), reverse-Z larger=closer, camera-discontinuity guard (>30°/frame or >0.25 mapHalfExtent ⇒ `unsafeForCull`). Counters: `wouldCullRaw = wouldCullGuarded + skippedCameraDiscont` | `MC2_HZB_BUILD`/`MC2_HZB_PROBE`; `docs/hzb-visibility-mvp.md`, `hzb-staticprop-cull-readiness.md`, `shaders/hzb_reduce.frag`, `tests/unit/test_depth_hzb.cpp` |
| **Foliage depth prepass (Lane A)** | Implemented, default-OFF `MC2_STATIC_PROP_DEPTH_PREPASS` (`flushDepthPrepassV6`, batcher ~:4989). Alpha-discard depth-only pass to collapse foliage overdraw. 2 review bugs fixed. Default-ON user-gated (§9) | batcher :747,:1337,:4967-4999 |
| **Tree LOD** | Pinned to LOD 0 forever: `currentLOD=0` (bdactor.cpp:3834), `lodDistance[]` loaded (:3634-3639) but unused; single `treeRenderShape` (bdactor.h:478), `getTreeRenderShape(long)` discards lod under override (bdactor.h:518). Bake tool proven: `.claude/tree_lod_bake.py` walnut 508K→10K tris. Spec exists (`tree-override-lod-spec.md`) with the LIGHTING SAFETY RULE | bdactor.cpp/h; Lane B prototype |
| **Impostors (Lane C)** | Pending, after Lane A | HANDOFF 2026-06-04 foliage |
| **Materials/lighting** | Per-vertex Gouraud × albedo; no PBR/normal/ORM/IBL/AO (`static-prop-lighting-audit.md`). ORM+normal recon done: MaterialGpu ABI, KTX loader, manifest validator, RGBA8 cook already in place; verdict = ORM first, normal-map separate arc (tangent prereq) | audit + recon docs; `shaders/static_prop.vert/.frag` |
| **IBL** | V-IBL-STATIC-0 planned; cooked `.specular.ktx2`/`.irradiance.ktx2`/`brdf_lut.ktx2` sidecars next to the EXR (asset-cook doc §2 table) | `docs/ibl-plan.md`, `gos_hdri.cpp` |
| **Service lane** | R2b: static-natural (trees) skip default-ON; static-**building** skip default-OFF (`98af2c80`); broadening recon done, widening deferred | `docs/static-building-skip-broadening-recon.md`, docs/archive/SERVICE-LANE-DECOMPOSITION.md |

---

## 3. Cull architecture — CPU / GPU / HZB ownership

**Decision: three stages, strictly layered, each conservative, each owning a different question.**

```
CPU coarse gate (chunk-derived active set)      "is it anywhere near the camera?"
  → feeds: gameplay-service lane + batcher admission   O(active blocks), NOT per-prop
GPU frustum cull (frozen static cull records)   "is its AABB in the frustum?"
  → feeds: indirect draw commands                compute, per-record, per-frame
GPU HZB occlusion cull (same compute pass)      "is it behind something?"
  → refines: the same indirect commands          guarded, default-OFF until oracle-clean
```

### 3.1 CPU vs GPU split

- **CPU keeps only the coarse block-level gate** — the chunk-derived `objBlockInfo.active` producer (terrain 8a/8c pattern: angular cone + 1-ring dilation, O(active blocks)). Its consumers are (a) the gameplay-service update walk and (b) batcher *admission* (which props are resident/touched). The legacy per-actor `recalcBounds`/`inView` screen-space math (bdactor.cpp:1090-1167) is **not fixed, it is retired**: under the GPU path `inView` becomes "admitted by block gate", and precise visibility is the GPU's job. Nobody re-attempts the intricate angular math — that's the documented tar pit.
- **GPU owns per-prop frustum cull** via **frozen static cull records** (the 2026-06-04 spec): one immutable, pool-aligned record per static prop instance (AABB, LOD metadata, material/light slot), written at mission load / registration, mutated only by events (destruction, override swap, editor move). This dissolves H4 (per-frame static rebuild — the 30-65% camera-motion spike) by construction. Substrate order: frozen static prefix `[0,S)`, dynamic `[S,S+D)`; M1 = binding-8 order reversal, gated by the corrected Slice-A oracle, with `adversarial-plan-review` before coding (standing instruction from the handoff).

### 3.2 HZB consumer — the open tradeoff, resolved

**Who consumes:** the GPU frustum-cull compute pass itself. HZB rejection is a second test inside the same dispatch that reads frozen records and emits indirect commands — not a separate CPU readback consumer, and **never** a writer to `objBlockInfo.active` (corollary 3: occlusion must not gate AI).

**When built:** from **previous-frame** depth (`MainDepth` after frame N-1's opaque scene), consumed at frame N cull. Same-frame build would require a depth prepass of the whole scene before cull — the terrain chunk path already lives with frame N-1 matrices for dispatch MVP ("baked dispatch MVP frame N-1" rule), so the convention is established. The one-frame staleness is exactly what the camera-discontinuity guard exists for: on `unsafeForCull` frames (forward Δ>30° or position jump) the compute pass **skips the HZB test entirely** (frustum-only), mirroring the probe's `skippedCameraDiscont` semantics. Conservative test: prop nearest-depth vs HZB MIN (farthest occluder) at the readiness-doc LOD selection; near-plane-crossing AABBs always KEEP (`nearClippedKeep`).

**Interaction with frozen records:** frozen records are the *input*; HZB never mutates them. The cull pass output (visible-instance compaction / indirect count) is per-frame transient. Because records are frozen, the HZB consumer needs zero new CPU-side plumbing — it is purely a shader change in the cull dispatch plus the pyramid bind.

**Promotion ladder (each step default-OFF, oracle-gated):**
1. `MC2_HZB_PROBE` shadow mode alongside the GPU cull pass — assert probe and compute pass agree (mismatch budget 0).
2. `MC2_HZB_CULL=1` applies to **shadow-caster and prepass lanes only** (a wrong cull there is a soft artifact, not a vanished building).
3. Apply to main opaque lane after N interactive sessions with `falseNegativeVanish` reports = 0 and golden-frame parity.

### 3.3 Anti-split-brain rule

The cull compute consumes the same `worldToClipGL`-derived planes as render (Gribb-Hartmann on the rasterized matrix — the `a280dde2` lesson, made law for props). The HZB unprojection uses `inverseViewProj_` set in the same place as the pyramid source depth (readiness doc: same-frame-coherent). Any new admit function must call the single shared `clipSpaceFrustumAdmit` — the dual-implementation collapse is part of finish-F1 (2026-06-05 handoff) and props work must not add a third.

---

## 4. Material + lighting plan (PBR)

Sequence (each slice independently shippable, per the ORM recon verdict):

1. **ORM first** (`STATICPROP-MATERIAL-ORM-1`): R=AO, G=roughness, B=metallic texture slot on the MaterialGpu record. ABI, KTX2 loader, manifest validator, and cook plumbing already exist — this is the contained, visually-sound slice. Shader: `shaders/static_prop.frag` gains a Cook-Torrance direct term using the existing sun direction (`gos_GetTerrainLightDir`, same feed as terrain 1b).
2. **IBL** (V-IBL-STATIC-0, `docs/ibl-plan.md`): prefiltered specular + irradiance + BRDF LUT as **cooked `.ktx2` sidecars** next to the EXR (cook lane owns this — asset-cook doc §2; runtime never prefilters). StaticPropOpaque is the designated first consumer lane (closure audit "YELLOW_BUT_READY").
3. **Normal mapping — its own arc, after ORM** (recon's explicit split): requires tangents (cook-generated, mikktspace, into the override GLB / cooked mesh record) and resolves the split-granularity trap the recon flagged. Do not bundle.

**Where material records live — decision: cooked, with a runtime-derived fallback.**
- **Cooked path (canonical):** per-asset `manifest.json` (Track G shape, `tools/asset_cook/trackg_cook.py`) grows a `material` block — texture slot stems (baseColor/normal/ORM/emissive), scalar fallbacks (roughness/metallic constants), alpha mode. The batcher's MaterialGpu record is populated from the manifest at registration. Validation via `validate_asset_manifest.py` before export, per the cook doc's ownership map.
- **Runtime-derived fallback (legacy assets):** the 2,947 stock `.ase`/`.tgl` props get a heuristic record at load — albedo from the existing 128/256/512/1024 `.ktx2` tiers, roughness≈0.85 constant, metallic=0, no normal/ORM. This keeps the no-cook path lit-plausible and means PBR is never gated on cooking the whole stock set.
- **Hot-color magic tags** (lighting audit) are preserved as an emissive-channel translation, not deleted — they're load-bearing for destruction/damage states.

**The black-prop invariant applies to materials too:** the LIGHTING SAFETY RULE (tree-override-lod-spec §2) generalizes — any record (light **or** material) that a draw can select must be fully populated before first static replay. Material slot changes re-arm `needsFullBakeNextFrame` exactly like light-data changes (bdactor.cpp re-arm sites).

---

## 5. LOD + impostor plan

Three tiers per prop archetype, all selected **on the GPU** from the frozen record's LOD table (CPU LOD-swap churn is the proven black-prop source — buildings' distance-swap blocks bdactor.cpp:3025/:3092/:3556):

| Tier | Source | Distance (initial, tunable per-archetype in manifest) |
|---|---|---|
| LOD0 full | authored/override mesh | 0 → ~300 world units |
| LOD1/LOD2 baked | `.claude/tree_lod_bake.py` cook output (walnut 508K→10K proven); GLB sidecars + manifest `lods[]` | ~300 → ~1200 |
| Impostor (Lane C) | cooked octahedral/billboard atlas `.ktx2` (cook lane) | ~1200 → cull |

Implementation rules (from `tree-override-lod-spec.md`, adopted wholesale):
- **Pre-register + pre-bake every LOD at mission load.** A LOD switch is a per-instance index into already-baked recipes — never invalidate+re-register. This is what makes GPU-side selection safe: all selectable geometry satisfies the lighting safety rule from frame 0.
- Structural seam: `treeRenderShape` → `treeRenderShape[MAX_LODS]` (bdactor.h:478); `getTreeRenderShape(long lod)` stops discarding `lod` under override (bdactor.h:518). Trees first; buildings generalize later (their existing churning swap path gets *replaced* by this, not extended).
- Collision/gameplay stays stock LOD0 (dual-shape invariant) — LOD is render-only.
- **Distance thresholds are cook-time manifest data, not code constants**, with a global scale env (`MC2_PROP_LOD_BIAS`) for tuning sessions. Hysteresis band (~10%) on the GPU selector to prevent threshold flicker.
- **Impostors (Lane C) start after Lane A defaults ON** (sequencing from the foliage handoff). Impostor pass is alpha-tested, lives in the prepass-compatible lane, sets its own GL state, and uses the same frozen-record selection — just a different draw bucket.
- Bake tool moves from `.claude/tree_lod_bake.py` (prototype) into `tools/asset_cook/` as a proper cook stage with manifest emission — per the cook doc, the Viewer drives it, Python owns it.

---

## 6. Static-service lane — unblock criteria

Per project memory: static-**building** update-skip shipped default-OFF (`98af2c80`); trees (R2b static-natural) default-ON (`ad6cff3c`); lane widening deferred. What unblocks them:

**Static-building skip → default-ON when ALL of:**
1. **Event-driven invalidation exists for every mutation class** the broadening recon found load-bearing: damage/destruction state, gate open/close, capture/team change, sensor contact, editor edits. Each mutation site calls a single `StaticPropDirty(handle, reason)` that re-enqueues exactly that prop for one full update (re-arm `needsFullBakeNextFrame` included). Recon (`static-building-skip-broadening-recon.md`) is the authoritative mutation inventory — bridges/forests/walls affect the move map (docs/archive/SERVICE-LANE-DECOMPOSITION.md:22), so move-map contributors get event hooks too.
2. **Oracle:** skip-ON vs skip-OFF A/B over tier1 + one interactive destruction-heavy session: per-prop appearance-state hash mismatch budget = 0; Δdestroys = 0; gate/turret functional sanity by hand.
3. **Frozen cull records are live** — because the skip removes the per-frame walk that today incidentally refreshes batcher state; frozen records make the render side walk-independent first. (Ordering: §11 phases 2 → 4.)

**Service-lane formal split (roadmap item 13) → starts when:** building skip has soaked default-ON for one release cut with zero dirty-event escapes. The split then makes the lanes structural: a `render-static` lane (event-driven, owns batcher/cull-record maintenance) vs `gameplay-service` lane (every frame, owns AI-relevant updates), per `modernization-roadmap-2026-06-09.md` §service lanes. Render visibility (HZB/GPU cull) connects ONLY to render-static; `objBlockInfo.active` connects ONLY to gameplay-service. That separation is the permanent fix for corollary 3.

---

## 7. Editor parity

Props must render **and pick** in the editor with the same substrate:

- The editor runs the default-on modern chain (terrain cutover precedent). The props batcher path must remain editor-clean: no game-only globals in the registration path; `#ifdef MC2_IS_EDITOR` only for editor-extra features, never for divergent render math.
- **Picking:** editor object pick currently has a known D3D↔GL split-brain under RTT (`MC2_EDITOR_RTT` default-OFF for this reason). Props picking must use the unified `worldToClipGL` projection (same as cull/render). Long-term: an ID-buffer pick pass over the same frozen records + indirect draws (one R32UI attachment on the prop passes) replaces CPU ray-vs-shape picking entirely and is RTT-proof by construction. Until then, CPU pick keeps using precise `inverseProject` on click only (the 1K-map cursor lesson `2817bc59`).
- **Editor edits are dirty-events:** move/rotate/place/delete in the editor flows through the same `StaticPropDirty` API as gameplay mutations (§6) and rewrites the frozen cull record + re-arms bake. WYSIWYG placement snap (`4a57e251`) already gives the transform; this gives it a single mutation funnel.
- **Frozen-record mutation rate in editor:** editing sessions mutate records constantly — frozen records must support O(1) single-record rewrite (they do, by design: pool-aligned slots), and the editor disables any "records are immutable for N frames" assumptions behind `MC2_IS_EDITOR`-aware asserts, not divergent code.
- Editor watchdog (`MC2_EDITOR_WATCHDOG`) + `MC2_EDITOR_GPU_TIMERS` are the editor-side budget instruments (§8).

---

## 8. Performance budgets

Measurable, per-pass, surfaced via the telemetry cockpit (telemetry-oracle-cockpit doc) and Tracy zones. Reference scene: metropolitan dense-urban (the 2,606-object interactive fixture) + 1kbasicmap; baseline = Baseline A off `mc2-win64-0.4c`.

| Pass / metric | Tracy zone / counter | Budget (frame @ target 60fps interactive) |
|---|---|---|
| GPU prop raster (opaque+alpha) | `Render.GpuStaticProps` | ≤ 3.0 ms GPU dense-urban; ≤ 1.5 ms typical. (Override forest pre-LOD pathology: 2.29 **s** — LOD work is gated on driving this into budget) |
| Depth prepass | `GpuSP.DepthPrepass` (batcher :4996) | ≤ 0.4 ms GPU; must show net win (§9) or it stays OFF |
| GPU cull + HZB test compute | new `GpuSP.Cull` zone | ≤ 0.3 ms GPU steady; **camera-motion spike ≤ 1.2× steady** (this is the H4 metric — frozen records pass/fail here) |
| HZB pyramid build | `runHzbReduce` | ≤ 0.25 ms GPU at 1080p ladder |
| CPU cull-record maintenance | new counter `propRecordsRewritten/frame` | steady-state = 0; event frames = #mutated props only (never O(total props)) |
| CPU static update walk | `MISSION_SPLIT`/service-lane counters | with building-skip ON: ≤ 0.5 ms dense-urban (today the walk is a top hotspot per H1a attribution) |
| Cull correctness | probe counters `wouldCullGuarded`/FN oracle | false-negative vanish = **0** (hard gate, not a budget) |
| VRAM (LOD+impostor atlases) | cockpit asset gauge | LOD sidecars ≤ 1.3× LOD0-only mesh VRAM; impostor atlas ≤ 64 MB/mission |

Every budget gets a red-log guard (the `[TerrainLOD prod]` zero-active pattern) so headless tier1 catches regressions without eyes; visual confirmation is reserved for what counters can't see (popping, impostor blend), per the vfx roadmap's provable-without-eyes rule.

---

## 9. Foliage prepass default-ON criteria (Lane A)

Restating the user-gated criteria from the 2026-06-04 handoff as the formal gate — `MC2_STATIC_PROP_DEPTH_PREPASS` flips default-ON when:

1. **Navigated-camera parity:** interactive session through foliage-heavy scenes, OFF vs ON golden frames byte-stable (or diff-classified as z-fight-only noise) — no alpha-test silhouette divergence between prepass discard and main-pass discard (same alpha ref, same mip/LOD bias in both shaders — assert this in code, not convention).
2. **Tracy win:** `Render.GpuStaticProps` (+ total GPU frame) OFF-vs-ON shows a net win on the foliage-heavy fixture; `GpuSP.DepthPrepass` cost ≤ the overdraw it saves. If net-negative on sparse maps, the flip can be per-mission/manifest-driven rather than global — but measure first (the pick-rect-prefilter lesson: measure the FIX).
3. tier1 5/5 both flag states, FATAL=0.

Once ON, the prepass becomes the first consumer of HZB culling (§3.2 ladder step 2) and the lane impostors render into (§5).

---

## 10. Anti-goals

- **No fixing the legacy angular cull math** (terrain.cpp:1040, recalcBounds). It gets bypassed and retired, not repaired — multiple sessions sank into it; the GPU owns precise visibility.
- **No HZB or render-visibility feedback into `objBlockInfo.active` / AI gates.** Occlusion-culled ≠ doesn't exist.
- **No CPU runtime LOD swapping** (invalidate+re-register churn = black-prop class). GPU selection over pre-baked recipes only.
- **No same-frame HZB** (would force scene-wide depth prepass before cull); frame N-1 + discontinuity guard is the contract.
- **No new GL passes that inherit state** (vfx roadmap law).
- **No runtime IBL prefiltering or runtime LOD decimation** — cook lane owns derivation (asset-cook doc).
- **No third frustum-admit implementation** — collapse toward one, never fork.
- **No flag-day:** every stage ships default-OFF with a kill switch and a legacy fallback until its oracle + soak gate passes (terrain cutover `a7b090be` is the template: single-source gate function + opt-out env).

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| HZB false-negative vanish (the worst props bug class) | Probe shadow-mode agreement oracle before any draw suppression; promote via shadow/prepass lanes first; discontinuity guard; per-stage kill switch |
| Frozen records drift from gameplay truth (destroyed building still drawn / live building culled) | Single `StaticPropDirty` funnel + per-prop state-hash A/B oracle (budget 0); soak default-OFF |
| Black-prop regression via LOD/material record introduction | LIGHTING SAFETY RULE as a mission-load assert: every selectable LOD's `lightData_` + material slot populated before first `touch()` |
| Prepass/main alpha-test divergence → silhouette shimmer | Shared alpha-ref constant + shared sampler state; golden-frame gate (§9.1) |
| Editor pick split-brain repeats on the GPU path | ID-buffer pick from the same matrices/records; never a second projection |
| H4 fix regresses dynamic props (substrate reorder M1 touches binding 8) | Slice-A corrected oracle gates M1; adversarial-plan-review before coding (standing instruction) |
| LOD pop / impostor parallax visually unacceptable | Hysteresis + manifest-tunable thresholds + `MC2_PROP_LOD_BIAS`; impostors last in sequence, behind their own flag |
| Cook dependency stalls runtime work | Runtime-derived material fallback (§4) and code-constant default LOD distances keep every slice testable on stock assets |

---

## 12. Phased roadmap

1. **P1 — Frozen static cull records (M1 substrate reversal → full records).** Dissolves H4; prerequisite for HZB consumer, GPU LOD select, and building-skip ON. Branch exists (`claude/perf-gpucull-ownership-1`).
2. **P2 — Foliage prepass default-ON** (§9 gates) + Tracy/cockpit budget wiring (§8 counters land here).
3. **P3 — HZB cull consumer** (§3.2 ladder: shadow-agreement oracle → prepass/shadow lanes → opaque).
4. **P4 — Tree LOD MVP** per `tree-override-lod-spec.md` (pre-bake all LODs, GPU select from frozen records); promote `tree_lod_bake.py` into the cook lane.
5. **P5 — ORM PBR slice** → **IBL (V-IBL-STATIC-0)** → normal-map arc (separate, after tangent cook).
6. **P6 — Static-building skip default-ON** (§6 criteria) → service-lane formal split.
7. **P7 — Impostors (Lane C)** + building LOD generalization + legacy angular-cull/inView retirement (the props "8z").

Dependencies: P1 → {P3, P4, P6}; P2 → P3-step-2; P4 → P7. P5 is parallel to everything except it lands on the batcher's MaterialGpu record (coordinate with P1's record layout once — define the frozen record to carry material+LOD fields from day one even if zero-filled).

---

## 13. First 5 implementation slices

1. **FROZEN-RECORDS-M1:** binding-8 substrate order reversal (static prefix `[0,S)`, dynamic `[S,S+D)`) on `claude/perf-gpucull-ownership-1`, gated by the corrected Slice-A oracle; run adversarial-plan-review first. Exit: oracle green, H4 camera-motion spike unchanged-or-better, tier1 5/5.
2. **PROP-DIRTY-FUNNEL-1:** introduce `StaticPropDirty(handle, reason)` + counter `propRecordsRewritten/frame`; route the mutation sites from `static-building-skip-broadening-recon.md` through it (no behavior change yet — instrumentation + funnel only). Exit: dense-urban steady-state rewrites = 0 logged.
3. **PREPASS-DEFAULT-ON-GATE:** run the §9 protocol (navigated parity captures + Tracy OFF/ON on foliage fixture); flip `MC2_STATIC_PROP_DEPTH_PREPASS` default with `=0` opt-out if green; document the numbers in the cockpit.
4. **HZB-SHADOW-AGREEMENT-1:** wire the HZB test into the GPU cull compute in shadow mode (no suppression), assert per-frame agreement with `MC2_HZB_PROBE` counters; mismatch budget 0 over tier1 + one interactive session.
5. **TREE-LOD-MVP-1:** `treeRenderShape[MAX_LODS]` seam (bdactor.h:478/:518), pre-register+pre-bake all LODs at load, GPU distance select from frozen records, `tree_lod_bake.py`-cooked LOD1 for the walnut forest fixture. Exit: `Render.GpuStaticProps` on the override-forest fixture ≤ budget, zero black trees over a destruction session.

---

## 14. Follow-up prompts (Opus/Codex)

1. *"Implement Slice 1 (FROZEN-RECORDS-M1) per §3.1/§13.1 of `docs/superpowers/strategy/props-modernization-architecture.md`: on branch `claude/perf-gpucull-ownership-1` (HEAD `eea91262`), reverse the binding-8 substrate order to frozen-static prefix `[0,S)` + dynamic `[S,S+D)`, define the frozen per-prop record (AABB, LOD table, material slot — zero-filled OK), and validate with the corrected Slice-A GPU_CULL oracle. Run adversarial-plan-review on the plan before any code. tier1 5/5 both flag states; H4 camera-motion attribution before/after."*
2. *"Implement Slice 4 (HZB-SHADOW-AGREEMENT-1) per §3.2: add the HZB occlusion test (frame N-1 pyramid, MIN/farthest-occluder convention per `docs/hzb-depth-convention.md`, camera-discontinuity skip per `docs/hzb-staticprop-cull-readiness.md`) to the GPU static-prop cull compute in shadow mode — no draw suppression — and assert agreement with the `MC2_HZB_PROBE` counters (`wouldCullGuarded` match, budget 0). Default-OFF, `MC2_HZB_CULL_SHADOWMODE` gate, registered in `RenderCore/RendererFeatureRegistry.h`."*
3. *"Implement Slice 5 (TREE-LOD-MVP-1) per §5/§13.5 and `docs/tree-override-lod-spec.md`: `treeRenderShape[MAX_LODS]` in `mclib/bdactor.h:478` with `getTreeRenderShape` honoring lod under override (:518), pre-register+pre-bake every LOD at mission load (LIGHTING SAFETY RULE — no shape selectable before `lightData_` populated), GPU distance selection from the frozen cull record's LOD table, LOD1 baked via `.claude/tree_lod_bake.py`. Oracle: zero black trees across a destruction session; `Render.GpuStaticProps` OFF/ON Tracy delta on the override-forest fixture."*
