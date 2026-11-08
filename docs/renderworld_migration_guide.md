<!-- doc-status: current | reviewed:2026-10-11 -->
# RenderWorld Contributor Migration Guide

Onboarding doc for future contributors (and future Claude sessions) adding new
slices to the RenderWorld arc: M3 (terrain), M4 (VFX), M5 (overlay), or any
future `RenderObjectKind`. The arc shipped seven slices in one day (M1, M1.5,
M1.6, M2-pre, M2, M2.5, M2.6) and the patterns are now load-bearing. This
guide crystallizes them so the next slice author does not have to re-derive
the firewall, the handle math, the self-test harness, or the env-OFF
discipline from spec archaeology.

Authoritative cross-references:

-   `CLAUDE.md` "Active campaigns" section -- canonical one-paragraph
    description of each shipped slice, including spec/plan paths and tier1
    counters.
-   `.claude/skills/greybeard.md` -- META-FIX vs PATCH discipline.
-   `.claude/skills/adversarial-plan-review.md` -- code-grounded plan review.
-   `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md` -- the
    boundary spec (Section 12 firewall, Section 13 first-slice scope).

Slice commit anchors (use `git show <sha>` to time-travel):

| Slice  | Topic                                  | Key commit (latest in slice) |
|--------|----------------------------------------|------------------------------|
| M1     | StaticPropRenderAdapter (route-only)   | spec 2026-05-22              |
| M1.5   | ObjectID buffer substrate              | RENDER_WORLD_SELFTEST wire   |
| M1.6   | Static-prop pick (inspect-only)        | (folded into M2.6 META-FIX)  |
| M2-pre | tryGameplayPick + screenToFboPixel     | gameplay_pick.{h,cpp} add    |
| M2     | MechRenderAdapter (route-only)         | mech.cpp init/destroy wire   |
| M2.5   | Mech ObjectID substrate                | `8f8be64`                    |
| M2.6   | Mech pickup integration                | `5d413d6`                    |
| --     | DebugRenderer M1 (sibling arc)         | `c2e877e` / `286f841`        |

---

## 1. What RenderWorld owns

`RenderWorld/` is the engine-facing scene API. Everything that lives behind
the boundary is "the renderer's business" -- the caller (game side) gives it
typed engine descriptors and gets back opaque handles.

What it owns:

-   **The `RenderObjectHandle` namespace.** 20 bits index + 12 bits
    generation, packed via explicit shift/mask (NOT C++ bitfields; see
    `RenderCore/Handle.h` adversarial fix M4). Holder MUST NOT interpret
    `index` or `generation`. `Handle::invalid()` is the ONLY sentinel; do not
    overload with `-1` or `0` at the engine boundary.
-   **The `s_objectRecords` unified table.** Indexed by `handle.index()`,
    always populated. Slot recycle bumps generation; `alive=false` marks a
    retired slot.
-   **`RenderObjectKind` discriminator.** `StaticProp=0`, `Mech=1`,
    `Terrain=2`, `Vfx=3`, `Overlay=4` reserved (per
    `RenderWorld/RenderWorld.h` enum comment). Values are stable across
    releases -- never renumber; only append.
-   **`lookupAtPixel(x, y) -> LookupResult`.** Synchronous pixel-to-handle
    readback against the M1.5 `R32_UINT` attachment-2. Stalls the GPU --
    intended for click-time (~10/sec), not per-frame.
-   **`GameplaySelectionDebugState`** (single mutex-guarded slot, last-pick
    wins) and the `setLastGameplayPick` / `clearLastGameplayPick` /
    `getLastGameplayPick` triad (M2.6 META-FIX of the M1.6 per-kind state).
-   **Per-kind env gates.** `IsObjectIdBufferEnabled()`,
    `IsStaticPropPickEnabled()`, `IsStaticPropPickDebugEnabled()`,
    `IsMechPickEnabled()`, `IsMechPickDebugEnabled()`,
    `IsMechPickPierceFogEnabled()`. All process-lifetime cached --
    restart required to flip (GLSL `#ifdef` is fixed at program-load time,
    per `memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md`).
-   **The `legacy/static_prop_backend.{h,cpp}` bridge.** The ONLY engine TU
    that touches `gos_static_prop_batcher.h` for static props (M1 spec
    section 12). New slices SHOULD prefer extending RenderWorld's API to
    adding a second engine-TU that reaches into a legacy batcher.
-   **The frame banner.** `frameBannerTick()` called from
    `gamecam.cpp` frame-end. Emits `[RENDER_WORLD v1] frame=N objects=T
    static_props=S mechs=M` as a monotonic 600-frame summary, always-on.

What it does NOT own:

-   Game-side identity (BattleMech*, Appearance*, recipe registry). It stores
    a `debugCookie` (opaque `uintptr_t`) for log echo and never dereferences
    it. M2.6 CRITICAL-1 found that even `partId` is reassigned post-spawn
    (`code/mission.cpp:2987`) -- the handle IS the identity. Game-side
    reverse-lookup lives in the adapter.

---

## 2. What GameAdapters may include

`GameAdapters/` is the spec section 12 carve-out: the ONLY module that may
bridge game and engine sides. Its headers MAY forward-declare game-side
types; its `.cpp` files MAY include real game-side headers AND RenderWorld
headers.

Existing examples:

-   **`GameAdapters/StaticPropRenderAdapter.h`** (M1) forward-declares
    `class Appearance`, `class TG_MultiShape`, `struct GpuStaticPropInstance`
    -- the only places those names may appear in a header outside their
    defining TUs.
-   **`GameAdapters/MechRenderAdapter.h`** (M2 / M2.6) forward-declares
    `class Mech3DAppearance` and `class BattleMech`. The `.cpp` includes
    `mech3d.h`, `RenderWorld.h`, and (M2.6) `code/mech.h` to walk
    `ObjectManager` movers for the `findMechByHandle` reverse-lookup.

Convention for a new adapter (e.g. `TerrainRenderAdapter.h` for M3):

1.  Header includes `<cstdint>` and `../RenderCore/Handle.h` only.
2.  Header forward-declares whichever game-side classes the adapter needs
    (e.g. `class TerrainBlock;`).
3.  Header exposes a small surface: `beginMission()`, `endMission()`,
    `sync*(...) -> RenderObjectHandle`, `destroy*(Handle)`, and optionally
    `find*ByHandle(Handle) -> GameType*` for the pickup arc.
4.  `.cpp` includes the real game headers AND `RenderWorld/RenderWorld.h`.
5.  Adapter is **temporary** per spec section 10 deletion criteria. Document
    the deletion trigger in the header: typically "when the game-side class
    is refactored to call RenderWorld directly, AND one release passes
    without it with tier1 5/5 + parity probe at zero pixel delta."

---

## 3. What RenderWorld must never include

The firewall is enforced by `scripts/check_boundaries.py` (run
pre-commit when any RenderCore/RenderWorld/GameAdapters file changes).

`SCOPE_DIRS` (the watched modules):

```
RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL
```

`GameAdapters` is deliberately NOT in this list -- it is the carve-out.

`FORBIDDEN_HEADERS` (no include from any SCOPE_DIRS file):

```
appear.h bdactor.h mech3d.h objectappearance.h objmgr.h mission.h warrior.h
gos_static_prop_batcher.h tgl.h msl.h GL/glew.h Stuff/Stuff.hpp
```

Note the inclusion of `gos_static_prop_batcher.h`, `tgl.h`, `msl.h`,
`GL/glew.h`, `Stuff/Stuff.hpp`. These are engine-side but transitively pull
GL / Stuff types that violate Section 12 purity (RenderCore must compile in a
hypothetical Vulkan/D3D backend). The M1 C1 fix added them after observing
they bled through `gos_static_prop_batcher.h`.

`FORBIDDEN_SYMBOLS` (case-sensitive word-boundary grep across SCOPE_DIRS):

```
Appearance BldgAppearance TreeAppearance GVAppearance Mech3DAppearance
GenericAppearance ObjectAppearance ObjectManager Mission MechWarrior
```

Catches forward-decls, function signatures, typedefs that an include-only
checker would miss.

Bypass: `scripts/check-include-firewall.allowlist` lists per-file exceptions
(the legacy bridge files). Add a file there only if there is no path
forward -- the allowlist is debt, not API.

**Reviewer-discipline gap (M2.5 MAJOR-M1 finding).** `GameOS/` is OUTSIDE
`SCOPE_DIRS`. Includes inside `GameOS/gameos/*` are NOT policed by the
firewall script. If you add a fast path that lives in `GameOS/`, the script
will not catch a stray `mech3d.h` include there. Manual review is the only
guard. When in doubt, move the fast path INTO a SCOPE_DIRS module so the
firewall covers it.

---

## 3.5 What game-side code must never CALL directly

Game-side code (`code/`, `mclib/`) must NOT call raw OpenGL functions
(`gl*()`). Rendering routes through engine abstractions:
MeshRenderer / MaterialSystem / RenderWorld / GpuStaticPropBatcher /
GpuMechBatcher / GameAdapters.

**Diagnostic exception:** `mclib/render_contract.cpp` may call read-only
GL state queries (`glGetIntegerv`, `glGetBooleanv`) inside the
`assertPassContract` machinery gated by `MC2_RENDER_CONTRACT_ASSERT=1`.
This is the ONLY exception, enforced by allowlist.

**Enforced by:** `36544d925:scripts/check-no-raw-gl-from-game.sh` (CI / pre-commit).

**Why this matters:** Engine routing is what makes future Vulkan/Metal
migration feasible. Direct GL calls from game-side code couple game
logic to the GL API surface, defeating MeshRenderer / RenderDeviceGL
abstraction. The hypothesis was empirically verified clean at HEAD —
this section LOCKS that state.

**Adding a new diagnostic exception:** Add the file + 1-line
justification to `scripts/check-no-raw-gl-from-game.allowlist`. If the
exception is RENDERING (not just diagnostic), reject it: route through
an engine API instead.

---

## 3.6 VFX shaders are PROHIBITED from writing object IDs

VFX shaders (`shaders/particle_billboard.{vert,frag}`, any future
particle/effect shader) MUST NOT contain `layout(location=2) out uint`
declarations.

**Why:** The object-ID buffer is `R32_UINT` with last-write-wins on
integer attachments (GL 4.5 §17.3.6). Translucent/additive particle
fragments would clobber the mech/static-prop ID underneath even when
visually transparent. This breaks M2.6 mech-pick through muzzle flashes,
smoke, tracers, impacts — ANY effect rendered in front of a pickable
object.

**Correct behavior:** VFX is click-through. `lookupAtPixel` returns the
object UNDER the effect (mech, static prop, terrain). Game logic that
needs "which mech fired this explosion" looks it up at weapon/fire-event
time, NOT via GPU pixel readback (the source mech handle is already
known in CPU game state when the effect is spawned).

**Enforced by:** `scripts/check-vfx-no-objectid.sh` (CI / pre-commit).

**Allowlist:** `scripts/check-vfx-no-objectid.allowlist` — expected to
be empty. Adding an entry requires understanding why the allowlist
exists (read the file header).

**Reserved enum slot:** `RenderObjectKind::Vfx = 3` exists for handle-
range bookkeeping only. No writer should produce a Vfx handle in v1.
`kVfxHandleBase = 0x00080000u` reserved in the partition chart in §12.

**gosFX dev-override caveat:** `MC2_DISABLE_GOSFX=0` (developer override
re-enabling MLR gosFX) is BROKEN under unified-projection F1 (see
worktree `CLAUDE.md` known-issues section). Even if a future slice were
to flip the M4 prohibition (against current recon recommendation), the
substrate could not be validated under the gosFX dev-override path until
MLR retirement Slices 1-5 ship and the convention is corrected. Do NOT
use `MC2_DISABLE_GOSFX=0` as a substrate proof path for any VFX work.

---

## 4. How to add a new `RenderObjectKind`

The enum lives in `RenderWorld/RenderWorld.h`:

```cpp
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // Future: Terrain=2, Vfx=3, Overlay=4
};
```

Stable-across-releases: never renumber; only append.

Walk for adding `Terrain=2` (M3 example):

1.  **Add the enum value.** Append `Terrain = 2,` -- do not shuffle.
2.  **Allocate a handle-index base.** Static props live `[0..2641]` (tier1
    mc2_24 max). Mechs live `[0x00010000..]` (`kMechHandleBase = 0x10000`,
    65536; ~24x headroom over the static-prop max). Pick a base with at
    least one decimal-order headroom over the projected max. For terrain
    blocks the M3 v1 reservation is `kTerrainHandleBase = 0x00040000`
    (see §12 table); this leaves `[0x00010000..0x0003FFFF]` as
    mech-expansion headroom.
3.  **Decide your object-ID write mechanism** (see section 5). Uniform if
    every draw is a single object; SSBO per-instance if you batch.
4.  **Build the adapter** (see section 2). Header surface mirrors
    `StaticPropRenderAdapter.h` / `MechRenderAdapter.h`.
5.  **Wire `registerX` / `destroyX` into RenderWorld.** These set
    `RenderObjectRecord.kind = RenderObjectKind::Terrain`. Bump
    `getCountForKind` accumulators so `frameBannerTick()` reports
    `terrain=T` alongside `static_props=S mechs=M`.
6.  **Wire the per-mission lifecycle.** `beginMission` adjacent to existing
    `StaticProp::beginMission` / `Mech::beginMission` at
    `code/mission.cpp` (see CLAUDE.md M2 entry for the exact wiring
    pattern). `endMission` does the safety-sweep force-clear and warns on
    leaked handles.
7.  **Add a self-test.** See section 9.
8.  **Update the firewall allowlist** if the adapter `.cpp` needs an entry.

### The five questions every new RenderObjectKind asks

Adopt these verbatim at spec time:

1.  What creates/destroys the handle?
2.  What kind does it report?
3.  Does it write object ID? (M1.5 substrate yes/no, and via which mechanism)
4.  How does lookup/pick/debug consume it? (`findXByHandle` semantics)
5.  What legacy fallback remains? (M2.5 MLR-mech gap is the canonical
    example: shipped because tier1 empirically `mlr_mech_draws=0`)

The five questions force the spec author to think through the same surface
as the existing two kinds. If any answer is "TBD" at execute-phase entry,
the spec is not ready.

---

## 5. How to add an object-ID writer

The M1.5 substrate provides `R32_UINT` `GL_COLOR_ATTACHMENT2` on the main
scene FBO. Any fragment that writes to `layout(location=2) out uint v_objectId`
participates. Gating is at three levels:

-   **Substrate gate:** `MC2_OBJECT_ID_BUFFER=1` (cached;
    `IsObjectIdBufferEnabled()`). Master switch -- attachment exists, draw
    buffers include location 2.
-   **GLSL gate:** `#ifdef MC2_OBJECT_ID_BUFFER` injected at `makeProgram()`
    time. C++ side reads env and prepends `"#define MC2_OBJECT_ID_BUFFER 1\n"`
    to the shader prefix (see `gos_static_prop_batcher.cpp:510-521` and
    `gos_mech_batcher.cpp:loadProgramsIfNeeded()`).
-   **CPU prep:** Filling the `objectIdRaw` field on the per-instance SSBO
    is unconditional per Q3 (M2.5 decision) -- the env gate guards SHADER
    OUTPUT, not CPU work. This keeps the binary path stable.

Two existing examples; pick the one that matches your draw shape.

### Mechanism A: coalesce SSBO + per-draw fill (M1.5 static-prop)

Static props use the coalesce path: one big SSBO of `PerDrawEntry`. M1.5
renamed `_pad0 -> objectIdRaw` and added a three-owner chain:

1.  Registry: `getRecipeIndexForType(typeID) -> int32_t`.
2.  RenderWorld: `objectIdRawForStaticPropRecipe(recipeIndex) -> uint32_t`
    (centralizes Handle encoding; returns 0 for `recipeIndex < 0`).
3.  Batcher: `PerDrawEntry.objectIdRaw = handle.raw()`.

Use this when your draw is "one object per draw entry" (decals, building
recipes).

### Mechanism B: per-instance SSBO + flat varying (M2.5 mech)

Mechs grow `GpuMechInstance` (std430) 48B -> 64B with an `objectIdRaw` field
+ 3 generic `_padN` reserved uints. Vert reads `flat out uint v_objectIdRaw`;
frag writes location=2 under `#ifdef`. Submit site at
`mclib/mech3d_diag.cpp:65` unconditionally fills
`desc.objectIdRaw = getRenderWorldHandle().raw()`.

Use this when your draw is "many instances per draw call" (mechs, particles,
projectiles).

### Choice point

-   Single-mesh / single-recipe per draw -> coalesce SSBO (mechanism A).
-   Instanced batch (one shader call, many objects) -> per-instance SSBO
    (mechanism B).
-   Push constants / uniforms are NOT recommended -- they break the binary
    path stability and you cannot mix env-OFF and env-ON cleanly without a
    second program object.

### Observability

Each substrate slice ships per-mission counters. Example from M2.5 (split
across two lines per the M1 amendment):

```
[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N
[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M
```

Two counters because the substrate-covered path (`N`) and the fallback path
(`M`) must be measurable separately. M2.5 shipped because tier1 5/5 showed
`M=0`; if a new slice cannot empirically prove the fallback is rare, it
needs a real fallback contract.

---

## 6. How to add a gameplay/editor pickup consumer

The M2-pre extraction made pickup additive: any new kind adds a small
caller block, not a new spine.

### Shared spine: `tryGameplayPick(request) -> Result`

`code/gameplay_pick.{h,cpp}` (M2-pre) hosts:

-   `GameplayPickRequest` / `GameplayPickContext` / `GameplayPickResult` with
    `Outcome { skipped, gated, miss, hit }`.
-   `tryGameplayPick(req)` dispatcher: env-substrate gate + per-gesture
    gates + mover-first short-circuit + viewport query + bounds + coord
    scale + `lookupAtPixel`.
-   `screenToFboPixel(...)` pure coord transform.

A new pickup consumer (e.g. terrain pick at M3.6) writes:

1.  A category env-flag gate (`MC2_TERRAIN_PICK=1`) checked at the call
    site.
2.  A `GameplayPickRequest` build (gesture, mouse coords, viewport context).
3.  A call to `tryGameplayPick(req)`.
4.  A switch on `result.outcome`. On `hit`, kind-guard against
    `RenderObjectKind::Terrain` before consuming kind-specific fields. On
    `miss` with `MC2_TERRAIN_PICK_DEBUG=1`, emit the diagnostic.

### Log schema: `[GAMEPLAY_PICK v1]`

M2.6 META-FIX retired the M1.6 per-kind log format. All new pickup consumers
MUST emit:

```
[GAMEPLAY_PICK v1] hit  kind=<X> handle=N idx=N gen=N <kind-specific> screen=(x,y) gl=(x,y)
[GAMEPLAY_PICK v1] miss kind=<X> screen=(x,y) gl=(x,y) reason=<...>
```

Reuse `setLastGameplayPick(kind, lookupResult, mouseX, mouseY, glX, glY)` to
populate the single-slot debug state. The state struct currently carries
`recipeIndex` for the StaticProp payload; future kinds add a tagged-union
payload field at THAT slice -- do not widen prematurely.

### Reverse-lookup: handle -> game object

Adapter responsibility. Two existing patterns:

-   **Static prop (M1.6):** O(1) via `handleToRecipeIndex` table maintained
    in RenderWorld at upsert time.
-   **Mech (M2.6):** O(N) linear scan over `ObjectManager` movers (N <= 50;
    tier1 max 46). `GameAdapters::Mech::findMechByHandle` lives in the
    adapter, not in RenderWorld (RenderWorld cannot include `objmgr.h`).
    The CRITICAL-1 reason: there is no stable game-side cookie at
    syncSpawn -- `partId` is reassigned post-init. Handle IS the identity.

When picking your reverse-lookup mechanism, ask: is there a stable game-side
cookie usable at spawn time that survives mid-mission reshuffles? If yes,
O(1). If no, O(N) scan in the adapter; document the cap.

---

## 7. How to debug a pixel -> handle -> object path

End-to-end walk for a Shift+click on a mech (M2.6 path):

1.  **Win32 mouse -> FBO pixel.** `MissionInterfaceManager` receives
    `mouseXPosition / mouseYPosition` in UI-canvas viewport space (already
    multiplied by `viewMulX` from the HUD-scene-split work). The shared
    `screenToFboPixel(...)` in `code/gameplay_pick.cpp` queries
    `gos_GetViewport()`, scales, then GL y-flips.
2.  **Substrate gate.** `RenderWorld::IsObjectIdBufferEnabled()` returns
    false -> `outcome=gated`. Returns true -> proceed.
3.  **Per-kind gate.** `RenderWorld::IsMechPickEnabled()` etc. False ->
    `outcome=gated`.
4.  **Mover-first short-circuit.** If the legacy Shift+mover-toggle
    consumed the click, `outcome=skipped` (recorded via the
    `moverSelectedThisFrame` observable set at the 4 `setSelected(true)`
    writer sites: `code/missiongui.cpp:1460/1487/1690/1705`). The sibling
    `setSelected(false)` sites at `:1462/:1483/:1692/:1701` are deliberately
    NOT instrumented.
5.  **`lookupAtPixel(glX, glY)`.** Synchronous readback against
    attachment-2 of the prior frame. Returns `LookupResult{isValid=false}`
    on background pixel (raw==0) or generation mismatch (stale-pixel-after-destroy).
6.  **Kind dispatch.** `result.kind == Mech` ->
    `GameAdapters::Mech::findMechByHandle(result.handle)` -> `BattleMech*`
    or nullptr on stale handle.
7.  **Fog predicate (mech-specific).** Respect `ShowMovers` + multiplayer
    -defeat carve-outs (full predicate from `code/missiongui_update.cpp:534-540`).
    `MC2_MECH_PICK_PIERCE_FOG=1` is the debug-only bypass.
8.  **`[GAMEPLAY_PICK v1] hit kind=Mech handle=N idx=N gen=N mech=PTR
    screen=(x,y) gl=(x,y)`** emitted unconditionally on hit;
    `setLastGameplayPick(Mech, result, ...)` updates the single-slot state.

If the log is silent on what you expect to be a hit:

-   Look for the 600-frame `[RENDER_WORLD v1] frame=N objects=T mechs=M`
    summary. If `M=0`, you have a registration leak.
-   Run with `MC2_OBJECT_ID_BUFFER=1` and the per-mission counter
    `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N`. If
    `N=0`, the substrate is dark -- check the GLSL `#ifdef` prefix
    injection at `makeProgram()` time and that the env flag was set BEFORE
    process start (cached at first call).
-   `MC2_MECH_PICK_DEBUG=1` for verbose miss/gated/stale-handle diagnostics.

---

## 8. How env-OFF must behave

The substrate gate is the master switch. Default-off MUST be pixel-identical
to the pre-slice HEAD.

Discipline:

-   **Default OFF.** Every new env var defaults off. Document the default in
    the header alongside the accessor.
-   **Pixel-parity gate.** Tier1 5/5 env-OFF compared to the pre-slice
    parent commit MUST show zero pixel delta. If the substrate adds a draw
    buffer or fragment write, the env-OFF branch MUST NOT advertise it to
    GL (no `glDrawBuffers` location, no `#define`, no per-instance fill if
    it bleeds through to a side effect).
-   **Process-lifetime caching.** All env accessors cache on first call.
    Flipping the env var at runtime does not work (GLSL `#ifdef` is fixed
    at program-load time). Document this in the header.
-   **Gate stacking.** Substrate gate AND per-kind gate. M2.6 enforces
    `MC2_OBJECT_ID_BUFFER=1` AND `MC2_MECH_PICK=1`. If a dev enables only
    the substrate, click behavior is unchanged but the readback is
    inspectable via the debug API + log.
-   **Self-test gate.** A separate `MC2_X_SELFTEST=1` env that runs the
    synthetic validator at `RenderWorld::init()` and prints
    `[X_SELFTEST v1] result=PASS|FAIL ...`. Separate from the runtime
    enable -- so CI can gate on the self-test without enabling the user-
    facing behavior.

Tier1 5/5 PASS env-OFF AND env-ON is the ship gate. A slice that passes
env-ON but regresses env-OFF cannot ship; the substrate has bled.

---

## 9. Self-test pattern

Each substrate slice ships a `Run*SelfTest()` validator wired into
`RenderWorld::init()` (or the adjacent adapter lifecycle hook) and gated by
an `MC2_X_SELFTEST=1` env var. Three live examples:

-   **M1.5:** `RunSubstrateSelfTest()` -> `[RENDER_WORLD_SELFTEST v1]
    result=PASS step=all`. Validates record-table generation/alive lifecycle.
-   **M2-pre:** `RunGameplayPickSelfTest()` -> exercises 8 synthetic
    `GameplayPickRequest` inputs (gesture-gate fails + mover-gate +
    off-screen + clean spine) and asserts outcomes match expected.
    `MC2_GAMEPLAY_PICK_SELFTEST=1`.
-   **M2.5:** `RunMechObjectIdSelfTest()` -> `[MECH_OBJECT_ID_SELFTEST v1]
    result=PASS`. `MC2_MECH_OBJECT_ID_SELFTEST=1`.
-   **M2.6:** `RunMechPickSelfTest()` hosted in
    `GameAdapters/MechRenderAdapter.cpp` (firewall: RenderWorld cannot
    include game headers; the adapter can). `MC2_MECH_PICK_SELFTEST=1`.

Wire order in `RenderWorld::init()`: substrate -> gameplay-pick spine ->
per-kind. New slice appends to the tail. A failing self-test is the canary
that fires before the user notices.

Output schema (load-bearing for grep):

```
[X_SELFTEST v1] result=PASS|FAIL [optional details]
```

`v1` is the schema version. Bump it (`v2`, `v3`) when the field set changes
in a breaking way; readers grep for `\[X_SELFTEST v[0-9]+\]`.

---

## 10. Firewall summary

`scripts/check_boundaries.py` is grep-based, comment-aware (single
-line `//`, `/*`, `*` opens), allowlist-supported
(`scripts/check-include-firewall.allowlist`).

-   `SCOPE_DIRS` (watched): `RenderCore RenderWorld Visibility MeshRenderer
    MaterialSystem DebugRenderer RenderDeviceGL`. New SCOPE_DIRS modules are
    forward-compatible -- the `[ -d ] || continue` guard means no script
    edit needed when you add a new module skeleton.
-   `GameAdapters/` is the carve-out -- never add it to `SCOPE_DIRS`.
-   `GameOS/` is OUTSIDE `SCOPE_DIRS`. Includes there are unpoliced;
    reviewer discipline is the only guard (M2.5 MAJOR-M1 finding).
-   Forbidden headers + forbidden symbols are case-sensitive word-boundary
    grep. Comments are stripped before matching.
-   Run pre-commit when any RenderCore/RenderWorld/GameAdapters file
    changes.

If you must add an allowlist entry: include a comment line documenting
which slice introduced the entry and the deletion criterion. Allowlist is
debt.

---

## 11. Build-dir trap warning

The root checkout (`A:/Games/mc2-opengl-src/build64/`) is `terrain-pbr-mod`
branch and is STALE. Always build the worktree:

```
A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/
```

M2.5 T5 burned a session on a false-alarm: a self-test failure that was
actually root-build pollution leaking into the deploy path. Symptom: env
flag set, log empty, no clear shader-prefix bug. Diagnosis: wrong `mc2.exe`
deployed.

Pre-flight every build with the canonical `/mc2-build` skill (which pins
the worktree path) or set:

```
cmake --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo
```

ALWAYS `--config RelWithDebInfo`. Release crashes with `GL_INVALID_ENUM`
(see CLAUDE.md inline rule).

---

## 12. The handle-base partitioning pattern

The unified `s_objectRecords` table is indexed by `handle.index()` (20 bits,
`[0..1048575]`). Different kinds allocate disjoint index ranges so a stale
handle's index alone reveals what kind it was meant to be (useful in logs
even when generation is wrong).

Allocations (corrected per M3 + M4 sidecar resolutions; supersedes earlier
TBD draft):

| Kind        | Handle base           | Slot count | Notes |
|-------------|-----------------------|-----------:|-------|
| StaticProp  | `0x00000..0x0FFFF`    | 64k        | Max observed mc2_24 = 2641 |
| Mech        | `0x10000..0x3FFFF`    | 192k       | Max observed mc2_24 = 46 |
| Terrain     | `0x40000..0x7FFFF`    | 262k       | Reserved (M3); no writer in v1 |
| Vfx         | `0x80000..0xBFFFF`    | 262k       | Reserved (M4); PROHIBITED writers per §3.6 |
| Overlay     | `0xC0000..0xFFFFE`    | 262k-1     | Deferred indefinitely (M5); slot may be reclaimed |
| Sentinel    | `0xFFFFF`             | 1          | Reserved bug-bait; never allocate |

20-bit index mask = `0xFFFFF` (per `RenderCore/Handle.h:34`). Bases above
`0xFFFFF` SILENTLY TRUNCATE to `index=0`, colliding with static-prop slot 0
— do NOT propose any base `>= 0x100000`. A prior draft hit this trap with
`0x200000`.

Allocation rule: pick a base with at least one decimal order of magnitude
headroom over the projected max. Power-of-two bases let you visually
disambiguate index ranges from a single log line.

When you allocate a base, document it in `RenderWorld/RenderWorld.cpp`
alongside `kMechHandleBase` so the next contributor can find the convention.

---

## 13. META-FIX vs PATCH discipline

Before proposing or writing any fix: run the `greybeard` skill
(`.claude/skills/greybeard.md`). Every fix must carry an explicit ruling:

-   **META-FIX:** Retires the bug *class*. Substitutive: the old shape
    disappears from the codebase. M1.5 C1
    (`setSceneDrawBuffers(SceneDrawBufferMode, bool)` centralizing
    scene-FBO draw-buffer policy across 5 sites) is canonical.
-   **PATCH (justified):** Local symptom fix with a named META-FIX deferred
    to a stated trigger. M1.6 was a justified PATCH; M2-pre IS the deferred
    META-FIX. The justification MUST name the trigger.

A patch with no named META-FIX and no debt justification is NOT allowed.
Documented history of additive slices netting ~0ms perf:
`memory/feedback_offload_must_be_substitutive_not_additive.md`.

Dispatch prompts to subagents MUST include "run the greybeard skill"
verbatim.

---

## 14. Adversarial review pattern

For any RenderWorld slice (architectural endpoint, legacy retirement,
SSBO schema, perf gate >=30%): the spec/plan pipeline is

```
spec draft
  -> internal adversarial-plan-review (code-grounded; grep every cited symbol)
  -> revision (each finding addressed; CRITICAL/MAJOR/MINOR triaged)
  -> plan draft
  -> internal adversarial-plan-review
  -> revision
  -> external review checkpoint (codex / external greybeard)
  -> revision
  -> execute (subagent-driven-development skill)
```

Why each step:

-   **Internal adversarial review (spec):** Catches firewall violations,
    handle math errors, env-OFF parity gaps before they reach plan
    granularity.
-   **Internal adversarial review (plan):** Catches per-task ordering bugs,
    missing self-test wiring, missing parity probe, undocumented
    assumptions.
-   **External review checkpoint:** Catches main-agent staleness. M2.6
    received external-greybeard fixes (visible in commit `64c13f1`) that
    reshaped the spec before execute.
-   **Execute via subagent-driven-development:** Tasks are independent
    enough to parallelize; each subagent gets isolated context and reports
    back with `--keep-logs` artifacts.

Dispatch prompts for review MUST include "use the adversarial-plan-review
skill" verbatim. Always dispatch without asking
(`memory/feedback_always_dispatch_adversarial_review.md`).

---

## 15. Cheat sheet: a new slice end-to-end

Working order for "add `Terrain=2` (M3) pickup":

1.  Read `CLAUDE.md` "Active campaigns" for all M1..M2.6 entries.
2.  Pick handle-base (M3 v1 landed `kTerrainHandleBase = 0x00040000`).
3.  Pick object-ID write mechanism (terrain blocks are batched -> SSBO
    per-instance, mechanism B).
4.  Spec draft:
    -   Section 12 firewall compliance statement.
    -   Five questions answered.
    -   Self-test design.
    -   Env-OFF parity argument (what new draw state exists; why default-off
        is pixel-identical).
    -   Deletion criteria for the temporary adapter.
5.  Adversarial-plan-review on spec.
6.  Revise; if any CRITICAL remains unaddressed, do not advance.
7.  Plan draft (task-level granularity, 4-10 tasks).
8.  Adversarial-plan-review on plan.
9.  Revise.
10. External review checkpoint.
11. Revise.
12. Execute (subagent-driven, `--keep-logs`).
13. Tier1 5/5 env-OFF AND env-ON.
14. Run `scripts/check_boundaries.py`.
15. Greybeard ruling (META-FIX vs PATCH) on the shipped diff.
16. Update `CLAUDE.md` "Active campaigns" with a one-paragraph SHIPPED
    entry mirroring the M2.6 entry style.
17. Add MEMORY.md handoff if the slice introduces new durable lessons.

### C++17 language features

The project is C++17 project-wide since 2026-05-24 (commit `5c03835`).
For which C++17 features are allowed immediately, which need caution,
and which to avoid for now, see `docs/cxx17-coding-rules.md`. Rule of
thumb for RenderWorld slices: the runtime renderer is hot and audited
-- do not modernize it for its own sake. Cold-path tools and tests are
fair game.

---

## 16. References

-   `CLAUDE.md` -- "Active campaigns" section (canonical slice index)
-   `RenderCore/Handle.h`, `RenderWorld/RenderWorld.h` -- public API
-   `GameAdapters/StaticPropRenderAdapter.h`,
    `GameAdapters/MechRenderAdapter.h` -- adapter convention
-   `scripts/check_boundaries.py` -- the firewall script itself
-   `.claude/skills/greybeard.md` -- META-FIX discipline
-   `.claude/skills/adversarial-plan-review.md` -- review skill
-   `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md` --
    boundary spec
-   `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`
    -- the spine extraction
-   `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md`
    -- the META-FIX of M1.6's per-kind state
