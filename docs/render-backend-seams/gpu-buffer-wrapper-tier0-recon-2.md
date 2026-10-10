<!-- doc-status: stale | reviewed:2026-10-11 -->
# GPU-BUFFER-WRAPPER-TIER0-RECON-2 — first-wrapper decision (HUD vs Light SSBO)

**Arc:** RENDER-BACKEND-SEAMS / VULKAN-CONTRACT-MANIFEST · recon + decision only, NO code · 2026-06-22
**Decision:** **HUD-FIRST.** Next slice = `GPU-BUFFER-WRAPPER-TIER0-HUD-1`. Light SSBO is the
first high-impact follow-up (`…-LIGHT-1`) and ships SECOND, after the ring pattern is proven.

## Inputs (not re-derived)
- `gpu-buffer-wrapper-tier0-recon-1.md` — the B decision + live `[GPUBUF v1]` counter data.
- `gpu-buffer-owner-recon-1.md` — master buffer inventory + Tier-2 ring template.
- `gpu-buffer-wrapper-design-1.md` — `GpuRingBuffer<N>` design (§3 flat-enum SUPERSEDED; §4 adoption slices are the live part; HUD = slice B, light SSBO = slice C).

## Live data (mc2_01 steady state, `MC2_GPUBUF_COUNTER=1`)
```
orphan_calls=72  orphan_bytes≈1.84 MB/frame  by_owner=
  light:            1 call  / 1,855,008 B   ← 96% of bytes
  hud:             66 calls /    31,500 B   ← 92% of calls
  sp_shadow:        4 calls /    41,216 B
  gos_UpdateBuffer: 1 call  /        64 B
```

---

## Finalist comparison (against code, file:line in worktree nifty-mendeleev)

| Axis | **HUD (gosMesh)** | **Light SSBO** |
|---|---|---|
| **1. Orphan callsite(s)** | `GameOS/gameos/utils/gl_utils.cpp:432-442` — the private 5-arg `updateBuffer(...,GLenum type)`, which calls `MC2_GL_BufferData_Owner(target, size, data, type, Hud)` at **:438**. Callers: `gosMesh::draw` (`GameOS/gameos/gameos_graphics_draw.cpp:31`), `gosMesh::drawIndexed` (**:771-772**, two orphans/call), `gosMesh::uploadBuffers` (**:673-678**). | `GameOS/gameos/gameos_graphics.cpp` — `gos_LightDataSsbo_Upload` create **:8616**, grow **:8635**, orphan+SubData **:8653-8654**; `gos_LightDataSsbo_UploadSplit` split-orphan **:8685** + prefix/suffix SubData **:8687-8691**. All route through `MC2_GL_BufferData_Owner(...,LightSsbo)`. |
| **2. Ownership + lifetime** | Owned by `gosRenderer`: 6 fixed `gosMesh*` (quads/tris/indexed_tris/lines/points/text) created at `gameos_graphics.cpp:4700-4710`, destroyed **:5126-5131** (`gosMesh::destroy`). Single-owner per mesh, ~11 GL buffers. No mission coupling — session-lifetime, created once at renderer init. | Single owner `static GLuint s_lightDataSsbo` (`:8605`); created lazily on first upload, destroyed by `gos_LightDataSsbo_Destroy()` (**:8703**). Single-owner. Bound globally — every lit material reads it. No explicit onMapLoad/Unload, but size tracks per-mission light population. |
| **3. Update cadence + size** | **Per-draw-batch** (66 calls/frame at mc2_01; scales with on-screen HUD/VFX/text geometry). **Size is BOUNDED and FIXED**: each mesh is allocated at a fixed capacity at `makeMesh` (e.g. `1024*10` verts, indexed_tris `1024*60`); `addVertices`/`addIndices` reject overflow (`:642`, `:651`) — **there is NO grow path**. Each orphan re-specs only the *used* prefix but the capacity ceiling never moves → a ring sized to capacity never reallocs. | **Once per frame** (1 call at mc2_01), from `mclib/txmmgr_render_phases.cpp:94` (`UploadSplit`) / `:2345` / `:493`. **Size VARIES**: grows with combined mech+static light count (the whole reason it replaced the 64-slot UBO). Grow-realloc at **:8628/:8669**; below-capacity path orphans full + SubData. Size changes only on grow (rare, monotonic per mission) but the per-frame respec is full-buffer (1.85 MB). |
| **4. Consumer / binding** | Bound by **target** (`GL_ARRAY_BUFFER`/`GL_ELEMENT_ARRAY_BUFFER`), not a binding-base slot. Consumed *immediately in the same call* (orphan → bind → `glDrawArrays`/`glDrawElements`, `:755`/`:800`), then unbound to 0. Self-contained per draw; no cross-pass reader. | Bound at `glBindBufferBase(..., LIGHT_DATA_SSBO_BINDING=20, ...)` (`:8618/:8656/:8693`); program block→slot wired by `gos_BindLightDataStorageBlock` (**:8719-8726**). **Read by every lit shader for the whole frame** (mech.frag, terrain, static-prop, …). Written once, read by N draws → a ring's slot must stay valid for the entire frame's reads. |
| **5. Regression gate** | tier1 5/5 byte-identical visual (mc2_01 = baseline; **mc2_17/mc2_24 stress HUD** — most on-screen units/text/VFX); `[GPUBUF v1]` `hud` orphan calls → ~0 (the ring memcpys, no per-batch `glBufferData`); KHR_debug label present; no fence assert fires. No same-frame RAW hazard to fence (orphan-then-draw is self-contained). | tier1 5/5 byte-identical (mc2_17/mc2_24 = most lights); `[GPUBUF v1]` `light` orphan **bytes → ~0** (the 1.85 MB/frame win); **must preserve the LIGHTSSBO-ORPHAN-1 NVIDIA no-stall property** (verify on NVIDIA, not just AMD — the orphan exists specifically to dodge an ~80ms 1050 Ti stall); grow path A/B; split-upload parity. |
| **6. Risk / blast radius** | **LOWEST.** Self-contained 2D/HUD path, no fence logic today, no grow, no cross-pass reader, fixed capacity, lives in `gl_utils.cpp` + the gosMesh block of `gameos_graphics.cpp` (NOT foreign-WIP). A ring bug = visibly wrong HUD/text, easy to catch, no gameplay impact. | **MEDIUM-HIGH.** Central: feeds *all* lit shading; a bug = whole-scene lighting corruption/stall. Grow-realloc + split-upload + an already-tuned driver-specific orphan must all survive the ring. Lives in `gameos_graphics.cpp` (not foreign-WIP itself, but the same TU other lanes touch). |

---

## Decision: HUD-FIRST

The code **confirms** the recon-1 / design-1 prior — HUD is the correct pilot, and on closer
reading the case is *stronger* than recon-1 stated:

1. **HUD has no grow path** (fixed-capacity meshes, overflow-rejecting `addVertices`/`addIndices`).
   recon-1 listed HUD as "allowGrow per-batch" (design-1 §4 row B). The code shows the opposite:
   capacity is fixed at `makeMesh`. **A ring sized to that fixed capacity never reallocs** — this
   removes the single hardest part of `GpuRingBuffer<N>` (the `ensureCapacity` drain+remap path)
   from the *pilot*, which is exactly what you want when proving the shape. The light SSBO, by
   contrast, is genuinely grow-realloc.

2. **HUD orphan-then-draw is self-contained** (same-call orphan → bind-by-target → draw → unbind).
   There is no cross-pass / multi-draw reader of a HUD buffer, so a 3-frame ring's in-flight slots
   have no read-after-write hazard beyond what the fence already covers. The light SSBO is
   written once and **read by every lit draw for the whole frame** — its ring slot must remain the
   bound slot across all those reads, a stricter invariant.

3. **HUD is non-central, low blast radius**, and lives in `gl_utils.cpp` + the gosMesh block —
   away from the foreign-WIP files (`mech3d.cpp`, `golden-sets.json`, `assimp_importer.cpp`).

4. **The light SSBO is already a hand-tuned, sensitive path.** LIGHTSSBO-ORPHAN-1 deliberately
   orphans with `GL_STREAM_DRAW` to dodge an ~80ms NVIDIA sync stall; the split-upload path exists
   for prefix/suffix dirty optimization. A ring conversion must reproduce both *and* the grow path,
   and prove the no-stall property **on NVIDIA**. That is real follow-up work, not a pilot.

**Goal alignment:** the pilot's purpose is Vulkan-readiness *pattern proof* (the `GpuRingBuffer<N>`
shape — persistent-coherent map, per-slot align, fence-after-draw, KHR_debug label, residency
hook), not maximum first-attempt bandwidth. HUD proves that shape on the mechanically simplest live
buffer. The 1.85 MB/frame light win is the prize, but it should ride a *proven* wrapper.

### Correction to the priors (flag)
- **HUD orphan IS now hitch/owner-accounted.** recon-1 §1.3 and owner-recon-1's update-path
  clarification both said the HUD `updateBuffer` churn is "NOT hitch-accounted … invisible to Tier-1
  telemetry," making accounting-parity the HUD slice's headline risk. As of GPU-UPDATE-BUFFER-
  COUNTER-1 (`eab7924d`), `gl_utils.cpp:438` routes through `MC2_GL_BufferData_Owner(...,Hud)` →
  it IS accounted and visible in `[GPUBUF v1]`. **This further de-risks HUD-first** (the parity
  surface is smaller than the priors assumed) and gives the slice its exact regression instrument
  (the `hud` owner line dropping to ~0).
- **HUD is fixed-capacity, not allowGrow** (see Decision point 1) — overrides design-1 §4 row B's
  "DynamicMutable allowGrow" characterization for the *pilot*. The wrapper still needs `allowGrow`
  for the light SSBO; it is simply not exercised by the HUD slice.

Neither correction flips the decision; both reinforce HUD-first.

---

## Chosen slice: `GPU-BUFFER-WRAPPER-TIER0-HUD-1`

**Scope.** Replace the per-batch orphan `glBufferData` in the gosMesh path with the designed
`GpuRingBuffer<N>` (N=3, persistent-coherent map, per-slot align, fence-after-draw), wired through
the 6 `gosMesh` instances. Adopt KHR_debug labels + the `MC2_GPUBUF_RESIDENCY` hook. Keep the
public gosMesh API (`addVertices`/`addIndices`/`draw`/`drawIndexed`/`uploadBuffers`) unchanged.

**Files (expected).** `GameOS/gameos/utils/gl_utils.{h,cpp}` (or a new `GpuRingBuffer.h`),
`GameOS/gameos/gameos_graphics.cpp` (gosMesh block ~:606-808, init :4700-4710, teardown
:5126-5131). New wrapper header. **Do NOT touch** `mclib/mech3d.cpp`, `mclib/assimp_importer.cpp`,
`tests/visual/golden-sets.json` (foreign WIP). Run `slice-preflight` before coding.

**Gate.** `MC2_GPUBUF_RING` (default OFF) selecting wrapper vs legacy `updateBuffer` path, so the
pilot is A/B-comparable and reversible.

**Acceptance criteria.**
1. tier1 5/5 (`mc2_01 mc2_03 mc2_10 mc2_17 mc2_24`), 30s each, exit 0, gate OFF **and** gate ON.
2. **Byte-identical visual** vs gate-OFF baseline on the HUD-stress missions **mc2_17 and mc2_24**
   (most on-screen units/text/VFX), captured via the visual golden harness (do not edit the
   foreign-WIP `golden-sets.json` — add a dedicated baseline key).
3. `[GPUBUF v1]` (`MC2_GPUBUF_COUNTER=1`, gate ON): the `hud` owner orphan **call count drops to
   ~0** per frame (the ring memcpys into a coherent map; no per-batch `glBufferData`). `light`,
   `sp_shadow`, `gos_UpdateBuffer` lines unchanged.
4. `MC2_GPUBUF_RESIDENCY=1` lists the HUD ring buffers (tag, kind, N×perSlotBytesAligned, live
   fence count) with KHR_debug labels.
5. Debug-build assert: `endFrameFence()` called once per `beginFrame()` per ring (design §2.3) —
   no missing-fence regression.
6. No new GL errors under `MC2_GL_DEBUG_FATAL=1`; tex-unit / state-cache invariants unchanged
   (HUD draw still unbinds to 0 as today, `:760`/`:805-806`).

---

## Sequencing — do the Light SSBO SECOND

`GPU-BUFFER-WRAPPER-TIER0-LIGHT-1` is the **first high-impact follow-up** (the 1.85 MB/frame win),
gated behind HUD-1 landing:

- **Reuses the proven `GpuRingBuffer<N>`** from HUD-1, but must additionally exercise:
  - the `ensureCapacity` **grow** path (drain fences → realloc → remap → re-`glBindBufferBase`),
    mirroring `:8628/:8669`;
  - the **split-upload** prefix/suffix optimization (or a justified decision to drop it under a
    full-frame coherent ring);
  - **preservation of LIGHTSSBO-ORPHAN-1's NVIDIA no-stall property** — verified on NVIDIA, since
    the orphan exists specifically to dodge the ~80ms 1050 Ti stall (AMD-only testing is
    insufficient here);
  - the whole-frame read invariant (slot bound at base 20 must stay valid across all lit draws).
- **Acceptance:** tier1 5/5 gate-OFF+ON; byte-identical lighting on mc2_17/mc2_24 (most lights);
  `[GPUBUF v1]` `light` **bytes → ~0**; no Tracy `RenderLists.LightDataUpload` regression on
  **NVIDIA**; grow + split A/B parity.

**Rationale for the order:** HUD has no grow, no cross-pass reader, fixed capacity, lowest blast
radius → proves the ring shape cheaply. The light SSBO is the bandwidth prize but carries grow-
realloc + split-upload + a driver-specific tuning that all must survive the conversion, and a
lighting bug is whole-scene-visible. Prove the pattern on HUD, then port it to the light SSBO for
the real win.

## Cross-references
- `gpu-buffer-wrapper-tier0-recon-1.md` (live counter data, the B decision).
- `gpu-buffer-wrapper-design-1.md` §2 (`GpuRingBuffer<N>` API), §4 row B (HUD) / row C (light SSBO),
  §5 (residency hook). §3 flat-enum SUPERSEDED.
- `gpu-buffer-owner-recon-1.md` (Tier-2 ring template `gos_mech_batcher.cpp`; HUD + light SSBO rows).
- `binding-slot-occupancy.{md,json}` + `scripts/check-binding-slots.py` (slot 20 = LIGHT_DATA).
