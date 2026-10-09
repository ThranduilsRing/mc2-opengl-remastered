---
name: mc2-render-spine-advisor
description: Pre-execution advisor for MC2 render-spine specs and plans. Enforces slice classification, seam correctness, data ownership/lifetime, cardinality, authority, gate precision, and GL render-state discipline BEFORE a plan reaches the executor. Run this BEFORE adversarial-plan-review for any render-spine slice; run it INSTEAD for lighter observational/diagnostic slices where code-grounded adversarial is overkill. Distilled from recurring CRITICAL failure modes across the RenderWorld arc, FX-GPU arc, and unified-projection campaign.
---

# MC2 Render Spine Advisor

Pre-execution checklist for MC2 render-spine specs and plans. Distinct from `adversarial-plan-review`
(which grep-verifies every symbol against source) — this skill enforces *structural* correctness:
wrong classification, wrong seam, wrong lifetime, wrong authority, wrong cardinality. Those failures
are invisible to symbol-grep because the symbols exist — they're just used with the wrong semantics.

## Execution mode

**Default: run inline** in the current session (ONE lane, no parallel agents). Only spawn a
subagent if the user explicitly asks. Keep output terse -- fragments, technical terms exact.

### Mechanical scans (do these yourself with grep)

| Scan | Checks |
|---|---|
| Env gate scan | Grep `#ifdef MC2_` vs `getenv("MC2_` patterns in new code; confirm each new var appears in `docs/tier1_env_vars.md` |
| Header / seam scan | Grep new `#include` lines; flag engine headers in game-side files; flag includes not at file top |
| Naming / field scan | Grep new field names for known semantic overloads (`objectIndex`, `typeId`, `alphaClass`, `pipelineId`, `glProgramName`, `packetCount`) |
| RenderWorld firewall scan | Grep new code for: forbidden headers/symbols in SCOPE_DIRS files; raw `gl*()` calls in `code/` or `mclib/`; `layout(location=2)` in VFX shaders; `Handle::index()` / `Handle::generation()` used outside `RenderCore/`; handle bases >= `0x100000`; `glReadPixels` calls bypassing `lookupAtPixel` |

## When to use

**Run first (before adversarial-plan-review) for:**
- Any render-spine spec/plan before handing to executor
- RenderWorld arc slices (M1–M6 and beyond)
- FX-GPU / particle slices
- Static prop / DrawPacket pipeline slices
- Unified-projection / camera-state slices

**Run instead of adversarial-plan-review for:**
- Observational / diagnostic slices with no GL dispatch change
- Single-struct instrumentation slices
- Counter / logging addition slices

**Skip for:**
- Pure tooling slices (Python scripts, cmake, shader-reflect CI) — use adversarial-plan-review directly
- Slices that mirror a shipped pattern with no new seams or lifetime decisions

## Step 0 — Required classification table

Before any other check, demand (or derive from the plan) this table.
A plan that cannot produce it cleanly is not ready for execution.

```
Slice kind:
  design / observational / diagnostic / dispatch-changing / cleanup

Changes pixels?
  yes / no

Touches GL state?
  yes / no

Data source:
  CPU / GPU / mixed

Authority for comparison:
  CPU accessor / RenderSnapshot / SSBO readback / none

Cardinality:
  per-object / per-type / per-packet / per-frame / per-map

Boundary crossed:
  none / GameAdapters / RenderCore POD / batcher_* accessor

Env gates:
  list (or "none")

Hard invariants:
  list (or "none defined — FINDING")
```

If **observational/diagnostic** is declared: any item below that touches GL dispatch, shader behavior,
draw state, save/load, gameplay, or pixel output is a CRITICAL blocker.

## Fast-path six (check these first — most criticals live here)

These six rules caught the majority of real criticals across the RenderWorld / FX-GPU /
unified-projection arcs. Run them as a quick scan before the full checklist. A single violation
here is enough to return REVISE without reading further.

```
1. Cardinality first
   Every field and gate must name its denominator: object / type / packet / frame.
   static_props ≠ packets ≠ types ≠ candidates. Wrong denominator = wrong invariant = missed bug.
   (Caught: objectIndex=typeId, type-alpha vs packet-alpha, props vs packets in gate counters)

2. Authority first
   Every comparison must state its authority: CPU accessor / GPU readback / diagnostic only.
   Never compare a GPU-resident value without an explicit CPU-side readback.
   (Caught: SSBO field compared to CPU candidate without readback, shadow map authority confusion)

3. Slice kind first
   Plan must declare: observational / diagnostic / dispatch-changing.
   If observational or diagnostic: zero GL draw, shader, dispatch, cull, pixel, or save/load changes.
   (Caught: diagnostic compare slices accidentally mutating render path or GL state)

4. Boundary first
   Every cross-module call must name its seam: GameAdapters / batcher_* accessor / RenderCore POD.
   No internal batcher objects or packet structs crossing the boundary.
   (Caught: engine headers in game code, internal structs passed across adapters)

5. Env vars are runtime, not compile-time
   #ifdef MC2_* wrapping runtime env behavior is wrong. Use getenv() at runtime.
   Every new env var must be listed in docs/tier1_env_vars.md.
   (Caught: #ifdef gates that silently baked in behavior at compile time)

6. Never document gates that do not exist yet
   A gate in a plan is a claim that the gate will be written. If the gate is not in the plan's
   task list, it does not exist. Vague "we'll add observability later" is a missing gate, not
   a deferred one. Invariants listed in the plan must have a task that writes them.
   (Caught: SORT_VALIDATE env var documented in spec, never written; parity probes claimed
    without a task that wires them)
```

A plan that fails any of these six is not ready for execution. Return REVISE with the specific
rule and the line in the plan that violates it. Do not continue to the full checklist.

## Checklist

### 1. Scope discipline

- Slice kind must be declared. Reject any plan without an explicit classification.
- If declared observational/diagnostic: reject any GL draw, shader variant, dispatch, cull, save/load,
  gameplay, or pixel-producing work.
- First-consumer work must come AFTER producer facts are validated by a prior slice.
- "We'll verify this works as we implement" is not a gate — it's deferred scope creep.

### 2. Seam correctness

Project seam ladder (lowest → highest coupling):
1. `batcher_*` free accessor functions — preferred for batcher facts
2. `GameAdapters` — preferred game↔engine bridge for object/game ownership seams
3. `RenderCore` POD / handle types — acceptable across stable boundaries
4. Internal batcher objects / packet structs — forbidden to cross seams unless project already does

Findings:
- Plan passes internal batcher object across seam → CRITICAL
- Plan adds stub to public API when a local invalid handle is sufficient → MAJOR
- Plan includes engine headers in game-side code instead of routing through GameAdapters → MAJOR
- Plan puts `#include` inside anonymous namespace or variable block (not at file top) → MINOR

### 3. Data ownership and lifetime

Classify every new/modified field as exactly one of:

| Class | Authority | Lifetime |
|---|---|---|
| per-instance | CPU | object alive |
| per-type | CPU | map loaded |
| per-packet immutable | CPU | map loaded |
| per-packet frame | CPU/GPU | frame |
| per-frame | CPU | frame |
| per-map immutable | CPU | map alive |
| GPU authority | GPU | frame / buffer |
| CPU authority | CPU | explicit write |

Findings:
- Two different lifetime classes mixed in one struct without explicit staging rationale → MAJOR
- Type-level value derived from a representative instance (e.g., packetCount from prop[0]) → MAJOR
- Packet-level value sourced from type-level OR-reduced state → MAJOR
- GPU-authority field compared to CPU value without explicit readback → CRITICAL (see §12)

### 4. Invariants and gates

Replace every vague "looks sane" / "visually correct" gate with an exact invariant.

Required invariant forms:
```
emitted == expected
invalid == 0
overflow == 0
oldExpected == newExpected (after schema-neutral change)
mismatches == 0
```

Cardinality traps — confirm the invariant uses the right denominator:
- `static_prop_count` ≠ `packet_count` ≠ `type_count` ≠ `candidate_count`
- `candidates` may exceed `static_props` for multi-packet types
- `expected_packets` = Σ packetCount over distinct visible typeIds, NOT total prop count

Any compare slice must have categorized mismatch counters unless exactly one field is being compared.
"mismatches=N" without category hides whether the issue is geometry, alpha, type, or pipeline.

Findings:
- Gate is "no visual regression" with no measurable invariant → MAJOR
- Invariant denominator is wrong cardinality class → MAJOR
- Compare slice has only a total mismatch counter, no category breakdown → MAJOR
- Single-field compare slice has no counter at all → MINOR

### 5. Runtime gates

- Env vars are runtime gates, not compile-time defines.
- `#if` wrapping runtime env behavior is wrong unless a real compile-time feature flag exists.
- Every new env var must be documented in `docs/tier1_env_vars.md`.
- Default: off for diagnostics. Exception: explicitly declared developer-default visual features may default on after smoke gate.
- Do not add env vars to `docs/tier1_env_vars.md` unless the plan implements the gate in the same slice. A documented gate with no implementing task is a future lie.

Findings:
- `#ifdef MC2_*` wrapping runtime env var behavior → MAJOR
- New env var not listed in `tier1_env_vars.md` → MINOR (must be fixed before land)
- Env var appears in spec / docs but has no task that writes the `getenv()` call → MAJOR (fast-path six rule 6)

### 6. Logging and observability

- Project logging sink: `std::fprintf(stderr, ...)` unless local subsystem has stronger convention.
- One summary line per frame or per map-load minimum.
- Per-item verbose logs must be behind a separate verbose env gate (e.g. `MC2_<FEATURE>_VERBOSE=1`, matching the pattern `MC2_DRAW_PACKET_COMPARE_VERBOSE`).
- Per-frame spam at default log level → MAJOR (will DOS release smoke log).

### 7. Build / deploy discipline

- Config: **RelWithDebInfo**, not Release. Not Debug.
- Use canonical build/deploy scripts. Do not invent ad-hoc cmake invocations.
- Do not commit screenshots or `.claude/` artifacts (except skills/memory).
- Empty "smoke passed" commits are forbidden unless explicitly requested.

Findings:
- Plan says "build Release" → MINOR (change to RelWithDebInfo before execution)
- Plan invents a new build path instead of canonical script → MAJOR

### 8. Header / include discipline

- Includes at file top only, never inside namespace/anonymous block/variable init.
- Engine module *implementation* headers (`gos_static_prop_batcher.h`, `mech3d.h`, `tgl.h`, etc.) are forbidden in game-side code; route through GameAdapters. `RenderCore` POD/handle headers (`RenderCore/Handle.h`, `RenderCore/DrawPacket.h`, `RenderCore/PipelineRegistry.h`) are allowed when the plan explicitly names the seam being crossed.
- Prefer forward declarations in headers.
- `GameOS/` fast paths are outside the firewall script's `SCOPE_DIRS`; any plan routing through `GameOS/` requires manual include review.
- No C++20-only types (concepts, `std::span` without backport, ranges) — project baseline is C++17.

### 9. Handle / enum / registry discipline

- Strong enum IDs identify registry records; they are NOT GL object names.
  - `PipelineId` indexes `PipelineRegistry`. `PipelineDesc.glProgramName` is a field inside.
  - `objectIndex` ≠ `typeId`. `alphaClass` ≠ per-packet alpha.
- Check enum bounds before calling APIs that assert.
- Invalid handles/IDs must log+return, not crash, in release diagnostic runs.
- Generation=0 handles are debug/observational only; they are not backed by real registry entries.

### 10. GL / render-state discipline

Any render pass must:
- Save and restore ALL GL state it mutates before returning.
- Not touch draw buffers unless required; if touched, restore exactly.
- Save/restore per-attachment color masks if using `glColorMaski`.
- Not write ObjectID / depth / normal attachments unless spec explicitly requires it.

Diagnostic slices MUST NOT mutate any GL state whatsoever.

Findings:
- Diagnostic slice sets any GL state → CRITICAL
- Render pass modifies draw buffer without restore → MAJOR
- Missing color mask save/restore → MAJOR

### 11. Snapshot / copy lifetime

- `RenderSnapshot` by value = shallow view into memory valid until next extraction.
- Emit and compare immediately after extraction, before any second extraction.
- Do not store snapshot spans beyond their frame.

Findings:
- Plan stores snapshot span across frames or re-extracts before comparing → CRITICAL

### 12. Comparison authority

- CPU fields: compare with CPU accessors.
- GPU / SSBO state: compare ONLY with explicit CPU-side readback.
- If no CPU-authoritative accessor exists for a field, defer that field to a future slice.
- "The GPU should have X" is not a comparison — it's a prayer.

Findings:
- Plan compares GPU-resident value without readback → CRITICAL
- Plan defers "we'll add the accessor later" as a post-hoc note → MAJOR (must be in scope or out of scope)

### 13. Failure behavior

- Missing optional assets or diagnostic data: no-op + log, NOT crash.
- `assert` is acceptable in Debug builds.
- Release smoke must return invalid/sentinel + increment failure counter, not abort.
- Plans must specify fallback/no-op behavior for every path that can fail.

Findings:
- Plan has no fallback for missing asset in release diagnostic path → MAJOR
- Plan uses `assert` in a code path that runs during release smoke → MAJOR

### 14. RenderWorld conventions and API firewall

Three CI-enforced firewall scripts protect the RenderWorld boundary. Any plan touching
RenderWorld-adjacent code must pass all three — simulate the check before execution, not after.

#### Include firewall (`scripts/check-include-firewall.sh`)

SCOPE_DIRS (watched engine modules): `RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL`

**Forbidden headers** — must NOT appear in any SCOPE_DIRS file (includes `GameOS/` fast paths):
```
appear.h  bdactor.h  mech3d.h  objectappearance.h  objmgr.h  mission.h  warrior.h
gos_static_prop_batcher.h  tgl.h  msl.h  GL/glew.h  Stuff/Stuff.hpp
```

**Forbidden symbols** — word-boundary grep catches forward-decls and typedefs too:
```
Appearance  BldgAppearance  TreeAppearance  GVAppearance  Mech3DAppearance
GenericAppearance  ObjectAppearance  ObjectManager  Mission  MechWarrior
```

**Carve-out:** `GameAdapters/` is the ONLY module that may include both game-side and engine-side
headers. Its `.cpp` files may include `mech3d.h`, `RenderWorld.h`, etc. — the header surface must
still use only forward-declares and `<cstdint>` + `RenderCore/Handle.h`.

**Blind spot:** `GameOS/` is OUTSIDE `SCOPE_DIRS`. The script does NOT police it. If a new fast
path lives in `GameOS/`, include discipline is enforced by manual review only. Flag any plan that
routes through `GameOS/` as requiring explicit firewall verification.

Findings:
- Plan adds forbidden header to any SCOPE_DIRS file → CRITICAL
- Plan uses forbidden symbol in a SCOPE_DIRS header → CRITICAL
- Plan routes a fast path through `GameOS/` without noting the blind spot → MAJOR
- Plan adds to `check-include-firewall.allowlist` for a rendering (non-diagnostic) reason → MAJOR
- Plan adds to the allowlist without a deletion criterion comment → MINOR

#### No raw GL from game side (`scripts/check-no-raw-gl-from-game.sh`)

`code/` and `mclib/` must NOT call raw `gl*()` functions. Routing goes through:
`MeshRenderer / MaterialSystem / RenderWorld / GpuStaticPropBatcher / GpuMechBatcher / GameAdapters`

Only allowed exception: `mclib/render_contract.cpp` calling read-only GL state queries
(`glGetIntegerv`, `glGetBooleanv`) inside `assertPassContract` gated by `MC2_RENDER_CONTRACT_ASSERT=1`.

Findings:
- Plan adds `gl*()` call in `code/` or `mclib/` for rendering (not diagnostic) → CRITICAL
- Plan adds a new diagnostic `gl*()` call in `mclib/` without adding to the allowlist → MAJOR

#### VFX shaders prohibited from writing object IDs (`scripts/check-vfx-no-objectid.sh`)

VFX / particle shaders must NOT contain `layout(location=2) out uint`. The object-ID buffer is
`R32_UINT` with last-write-wins on integer attachments (GL §17.3.6). Translucent particle
fragments writing attachment-2 would clobber mech/static-prop IDs underneath even when visually
transparent, breaking M2.6 mech-pick through muzzle flashes / smoke / tracers.

`scripts/check-vfx-no-objectid.allowlist` is expected to remain EMPTY FOREVER.

Findings:
- Plan's VFX/particle shader writes `layout(location=2)` → CRITICAL (cannot be allowlisted)

#### RenderObjectHandle API contract

- `RenderObjectHandle` is opaque. Callers MUST NOT interpret `index` or `generation` directly.
- `Handle::invalid()` is the ONLY valid sentinel. Never use `-1`, `0`, or a raw uint as sentinel.
- `RenderObjectKind` enum values are stable-across-releases: never renumber, only append.
- Handle-index bases must stay within the 20-bit mask (`0x000000..0x0FFFFE`). Any base ≥ `0x100000`
  silently truncates to index=0, colliding with static-prop slot 0.
- `RenderObjectKind::Terrain=2` and `RenderObjectKind::Vfx=3` are RESERVED with no writers in v1.
  No plan may produce handles in those ranges or write attachment-2 from VFX shaders.
- `lookupAtPixel(x, y)` is the canonical pixel→handle readback. It stalls the GPU and is intended
  for click-time (~10/sec). Never bypass with raw `glReadPixels` or per-frame calls.

Findings:
- Plan interprets `handle.index()` or `.generation()` outside of `RenderCore/Handle.h` → MAJOR
- Plan uses `0` or `-1` as a handle sentinel instead of `Handle::invalid()` → MAJOR
- Plan proposes a new handle base ≥ `0x100000` → CRITICAL (silent truncation)
- Plan produces `Terrain=2` or `Vfx=3` kind handles → CRITICAL (no writers allowed in v1)
- Plan calls `glReadPixels` instead of `lookupAtPixel` for object-ID readback → MAJOR

#### New RenderObjectKind checklist

Any plan adding a new `RenderObjectKind` must answer all five questions before spec-ready:
1. What creates/destroys the handle?
2. What kind does it report?
3. Does it write object ID? (via mechanism A — coalesce SSBO, or mechanism B — per-instance SSBO)
4. How does lookup/pick/debug consume it? (`findXByHandle` semantics)
5. What legacy fallback remains?

If any answer is "TBD" at execute-phase entry → slice is not ready.

Also required: self-test wired into `RenderWorld::init()`, gated by `MC2_X_SELFTEST=1`, emitting
`[X_SELFTEST v1] result=PASS|FAIL`. Per-mission counters for substrate-covered path and fallback
path separately. Env-OFF pixel-parity gate (tier1 5/5, zero delta vs pre-slice HEAD).

### 15. Naming / versioning

- Log tag versions must increment when schema changes (otherwise old logs parse silently wrong).
- Transitional fields must be labeled `// TRANSITIONAL: remove when <condition>`.
- Avoid semantic overload:
  - `snapshot` = per-frame data, not a saved state
  - `desc` = immutable descriptor, not a mutable record
  - `pipelineId` ≠ `glProgramName`
  - `objectIndex` ≠ `typeId`
  - `alphaClass` ≠ per-packet alpha value
  - `packetCount` from representative prop ≠ canonical type table value

## Output format (terse — caveman mode)

```
VERDICT: EXECUTE | REVISE | BLOCK

CLASS:
  kind=[design|observational|diagnostic|dispatch|cleanup]
  pixels=[yes|no]  gl=[yes|no]  src=[CPU|GPU|mixed]
  authority=[accessor|snapshot|readback|none]
  cardinality=[object|type|packet-imm|packet-frame|frame|map]
  boundary=[none|GameAdapters|RenderCore-POD|batcher*]
  gates=[list or none]
  invariants=[list or MISSING]

FAST-6:
  1.cardinality  [✓ | ✗ reason]
  2.authority    [✓ | ✗ reason]
  3.slice-kind   [✓ | ✗ reason]
  4.boundary     [✓ | ✗ reason]
  5.env-runtime  [✓ | ✗ reason]
  6.gates-exist  [✓ | ✗ reason]

CRITICALS: N
  C1. §rule → fix
  C2. §rule → fix

MAJORS: N
  M1. §rule → fix

MINORS: N
  m1. §rule

GATES: [revised exact invariants only — omit if none]
GREP:  N sym / M sites / K writers / src:yes|no
SKIP:  [missing context, skipped checks, or "none"]
```

No prose in findings. Each finding: one line. `§rule` = section number from checklist.
`fix` = mechanical action (rename / add task / change type), not architectural advice.
Architectural decisions that need sign-off: append as `ARCH: [item]` after SKIP.

## The one rule that prevents most criticals

> **Never promote a transitional convenience into a canonical field unless its ownership, lifetime, authority, and cardinality are all correct.**

Examples of this failing:
- `typeId` used as `objectIndex`
- `alphaClass` used as per-packet alpha
- `PipelineId` treated as `glProgramName`
- Representative prop's `packetCount` used as canonical type table value
- GPU cull state compared to CPU without readback

## Recurring CRITICAL patterns (distilled from RenderWorld / FX-GPU / unified-projection arcs)

| Pattern | Example | Fix |
|---|---|---|
| Cardinality mismatch | `static_props == packets` assumed | Σ packetCount over distinct typeIds |
| Lifetime mismatch | Per-map immutable field written per-frame | Separate per-frame from per-type struct |
| Authority mismatch | CPU candidate vs GPU SSBO compared directly | Add CPU-authoritative accessor or defer |
| Seam mismatch | Internal batcher object passed to game side | Use `batcher_*` free accessor |
| Diagnostic becoming behavior | Compare slice sets GL state | Zero GL mutation in diagnostic path |
| Env gate confusion | `#ifdef MC2_VERBOSE (gate since removed)` for runtime env var | `getenv("MC2_VERBOSE")` at runtime |
| Release config drift | `cmake --config Release` | Always `RelWithDebInfo` |
| Semantic overload | `objectIndex` stored in `typeId` field | Rename + separate lifetimes |
| Error handling mismatch | `assert` in release diagnostic | `if (!x) { ++failures; return sentinel; }` |
| Include / firewall drift | `#include <gosRenderer/batcher.h>` in game cpp | Route through GameAdapters |
| DrawPacket semantic drift | `objectIndex` used as `typeId` in `RenderCore::DrawPacket` | Candidate structs carry `typeId`/`globalPacketIdx`; canonical DrawPacket must not alias them |
| DrawPacket pipeline overload | `DrawPacket.pipelineId` set to `glProgramName` | `pipelineId` = `static_cast<uint32_t>(PipelineId)` — never a GL program name |
| Pipeline selection cardinality | Pipeline selected once per type instead of per packet | If packet has material flags, pipeline selection must be per-packet |

## Relationship to other skills

- Run **this skill first** to check structural correctness.
- Run **`adversarial-plan-review`** after to grep-verify cited symbols against source.
- If slice is observational/diagnostic only and small, this skill may be sufficient alone.
- If slice is architectural-endpoint or retires a contract, both skills are mandatory.

## Origin

Created 2026-05-25. Distilled from recurring CRITICAL findings across:
- RenderWorld arc (M1–M5): seam and lifetime failures
- FX-GPU arc (B1–B3): authority mismatch (Stuff→MC2 axis swap, age=0 curve trap), lifetime confusion
- Unified-projection campaign: cardinality, GPU-authority comparison without readback, env gate confusion
- Static prop DrawPacket arc: cardinality mismatch (props vs types vs packets vs candidates), seam violations

The structural failures in this checklist are invisible to symbol-grep (the adversarial-plan-review
technique) because the *symbols exist* — they're used with wrong lifetime/authority/cardinality
semantics. This skill fills the gap between "all symbols verified" and "plan is actually correct."
