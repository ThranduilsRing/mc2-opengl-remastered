# GPU Binding-Slot Occupancy — GPU-BINDING-SLOTS-LOCKSTEP-1

Source of truth for *which pass binds which role to which GPU buffer binding-base
slot* (SSBO / UBO), plus the C++↔GLSL lockstep contract. **This is NOT a flat
global registry.** The census proved slot numbers are intentionally *multiplexed
per pass*: the same number names different buffers in unrelated passes. A slot
number is only semantic **inside a pass/pipeline** — which is also the Vulkan
model (a descriptor-set layout is per pipeline, not global), so cross-pass reuse
is correct, not a bug.

- Machine-readable, regenerated: `binding-slot-occupancy.json` (emitted by the
  checker, do not hand-edit).
- Checker: `scripts/check-binding-slots.py` (CI/check-time only; no runtime code).
- Supersedes the older partial hand-maintained `docs/render-binding-registry.md`
  and `docs/audit-lanes/lane-D-shader-abi.md` slot notes.

## Regenerate / verify

```
py -3 scripts/check-binding-slots.py --json docs/render-backend-seams/binding-slot-occupancy.json
```

Exit 0 = PASS. The script scans C++ (`GameOS RenderCore RenderWorld GameAdapters
mclib code`) and GLSL (`shaders/`, excluding `shaders/fixtures/` and `tools/`).

## Pass/fail contract

| Class | Condition | Effect |
|---|---|---|
| **FAIL** | a binding named on BOTH C++ and GLSL (a shared `#define`, or a known C++-const↔GLSL-literal pair) has mismatched values | exit 1 |
| **FAIL** | one GLSL stage file binds two *different* blocks to the same slot **and they are not on mutually-exclusive `#if`/`#else` branches** | exit 1 |
| **WARN** | mode-alternate: two blocks share a slot but on exclusive `#if` branches (only one compiles per permutation — intentional) | informational |
| **WARN** | a GLSL block binding is a bare numeric literal not tied to a named constant (hand-lockstep surface) | informational |
| **WARN** | cross-pass reuse: a slot bound in >1 shader file (expected — multiplexing) | informational |
| **WARN** | unresolved binding token (compile-time macro injected via `makeProgram` prefix, e.g. `material_gpu.hglsl` `binding = N`) | informational |
| **PASS** | no FAILs | exit 0 |

The checker is preprocessor-branch aware: it tracks `#if/#ifdef/#ifndef` →
`#elif/#else` → `#endif` nesting and treats two blocks as coexisting only if they
are not separated by opposite branches of a common conditional. This is what lets
the intentional `gpu_cull.comp` slot-9 alternation pass.

## Shared C++↔GLSL named bindings (the true lockstep surface — FAIL on drift)

These are the only bindings defined by *name* on both sides; everything else in
GLSL is a bare literal (WARN, not lockstep-checked by name):

| Slot | Symbol | C++ | GLSL |
|---|---|---|---|
| 20 (SSBO) | `LIGHT_DATA_SSBO_BINDING` | `GameOS/include/gameos.hpp` | `36544d925:shaders/include/lighting.hglsl` |
| 1 (UBO) | `SCENE_DATA_ATTACHMENT_SLOT` | `gameos.hpp` | `36544d925:shaders/include/scene.hglsl` |
| 0 (UBO) | `LIGHT_DATA_ATTACHMENT_SLOT` | `gameos.hpp` | `lighting.hglsl` |
| 3 (UBO) | `kViewUniformsBinding` (C++ const ↔ GLSL **literal** 3) | `RenderCore/ViewUniforms.h` | `36544d925:shaders/include/view_uniforms.hglsl` (checked via known-pair) |

`READBACK_SSBO_BINDING` (14) is injected into `gpu_cull.comp` via the shader
build prefix rather than a duplicated GLSL `#define`, so it is checked on the C++
side and appears as a resolved token, not a named pair.

## Intentional multiplexing (documented — NOT failures)

Slot numbers reused across unrelated passes, by design (full per-file detail in
the JSON `occupancy` map):

- **Slot 0** — mech instances · static-prop instances · gpu-driven recipes ·
  cmd-patch header · patch-stream quad records · terrain-lighting vertex inputs ·
  card-cloud sim. (10 shader files.)
- **Slot 1** — bone data (mech) · lighting inputs · cmds (cmd-patch) · recipe
  (thin) · static-prop colors · `mesh_data`/`SceneData` (UBO namespace).
- **Slot 2** — terrain thin-record · TL light outputs · gpu-driven handle-LUT ·
  mask lighting · mech material table · static-prop per-type · `CULL_UBO_BINDING`
  (UBO namespace — numeric overlap with SSBO 2, separate GL target, not a clash).
- **Slot 5** — `kWaterRecipeSsboBinding` vs MaterialTable (building/static-prop PBR).
- **Slot 6** — `kWaterThinSsboBinding` vs gpu-driven bucket header.
- **Slot 7** — `kMechMaterialTableBinding` vs water per-cmd vs gpu-driven canary
  vs cull-patch CmdToBucket.
- **Slot 8** — substrate records vs gpu-driven cmd buffer.
- **Slot 9** — `DEBUG_SSBO_BINDING` (C1a) vs VisibleIds (C1b) — **mode-alternate
  in one file** (`#ifdef GPU_CULL_C1B_INDIRECT`), checker WARN not FAIL.
- **Slot 11** — `BUCKET_CAPS_BINDING` (cull main) vs `INDIRECT_CMD_BINDING`
  (patch) — different shaders.
- **Slot 12** — `ACTOR_VIS_BINDING` (cull) vs veg block-vis.
- **Slots 14/15/16** — cull readback / permutation / base-instance vs particle
  billboard & tube-ribbon (pos/color/uv).
- **Slot 20** — `LIGHT_DATA_SSBO_BINDING` vs terrain surface VB.

## Slot-14 particle/readback sharing — ALREADY MITIGATED (verified 2026-06-22)

The particle bridge shares SSBO slot 14 with the GPU-cull readback ring
(`READBACK_SSBO_BINDING`). An older lane-D audit note recommended an explicit
unbind; that fix (**"D-01"**) already landed. Verified: every particle-bridge
slot-14 bind has a matching, unconditionally-reachable unbind-to-0 —
`gos_particle_bridge.cpp` 499→537, 753→791, and 1045→1150 (the last cites D-01,
mirroring `gpu_cull_compute.cpp:1187`). Both consumers bind-then-unbind-to-0
symmetrically, so no later consumer observes a stale binding. The proposed
GLSTATE-SSBO-SLOT14-PARTICLE-UNBIND-1 slice is therefore **MOOT** — no live leak
remains. (A restore-previous guard would diverge from the established
symmetric-unbind-to-0 convention for no behavior gain.)

## Vulkan-prep shared-binding report (VULKAN-BINDINGS-CHECK-1)

Under GL the multiplexing above is legal (a slot is per-pass). Under Vulkan a
descriptor-set layout pins a fixed `(set, binding)`; a slot occupied by DISTINCT
live resources that a per-pipeline layout cannot isolate is a descriptor
collision that must be split before descriptor assignment.

`scripts/check-vulkan-bindings.py` converts that recon finding into a standing
report over this JSON:

```
py -3 scripts/check-vulkan-bindings.py            # report (exit 0)
py -3 scripts/check-vulkan-bindings.py --strict   # exit 1 if any collision (future gate)
```

- **VULKAN-COLLISION** — the two KNOWN Vk-prep TODOs: SSBO **slot-14**
  (`ReadbackBuf` / `Particles` / `RibbonPos`) and SSBO **slot-20**
  (`SurfaceVertexBuf` / `LightsData`). Tracked as debt; report-only until split.
- **benign shares** — the intentional per-pass multiplexing (per-pipeline
  descriptor-set isolatable) and the slot-9 mutually-exclusive `#if` branches.
- Any NEW distinct-role slot share (not in the curated classification) is
  surfaced on stderr so it cannot slip; `--strict` fails on it. It is
  deliberately NOT wired into `check-contracts.sh` (would false-fail on the
  known-present collisions).

## Scope boundaries (this slice)

CI/check-time only. No flat global enum, no descriptor abstraction, no
GpuBuffer/Ring wrapper, no replacing the scattered constants, no shader edits, no
binding renumbering. Migrating the scattered constants to a generated header is a
future per-subsystem adoption step (see `gpu-buffer-wrapper-design-1.md` §4).
