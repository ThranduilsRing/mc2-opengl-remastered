<!-- doc-status: current | reviewed:2026-10-11 -->
# Shader-permutation inventory — SHADER-PERMUTATION-INVENTORY-1

> **Recon + governance checker only. No shader/runtime/offline-compiler changes.**
> Companion data: [`shader-permutation-inventory.json`](shader-permutation-inventory.json)
> (the machine-readable allowlist). Governance gate:
> `scripts/check-shader-injectors.py` (wired into `check-contracts.sh` as
> `shader_injectors`).

## VERDICT: **GO**

**Yes — we can mint a finite `shaderVariantId`.** The shader-variant space is
finite and enumerable. Proceed to `PIPELINE-KEY-SCHEMA-1`.

### Headline — the suspected blocker does NOT exist
`pipeline-state-contract-recon-1.md` (and the conventional wisdom) assumed
`gosMaterialVariation` injects an **open-ended, data-driven** `#define` set per
material → non-enumerable → DEFER. **That premise is wrong.** Re-examined and
verified this session:

- `gameos_graphics.cpp:399-403` — `gosGLOBAL_SHADER_FLAGS` is exactly two flags:
  `ALPHA_TEST`, `IS_OVERLAY`.
- `GameOS/gameos/gameos_graphics_material_internal.h:413-416` — `g_shader_flags[]` = `{"ALPHA_TEST","IS_OVERLAY"}`.
- `gameos_graphics.cpp:4849-4868` — `combinations[]` is a **static 4-entry array**
  (`0`, `ALPHA_TEST`, `IS_OVERLAY`, both), looped over a **fixed 6-shader list**.
  The `defines` vector handed to `getMaterialVariation` only ever receives names
  from that 2-element array. **Nothing reads material/config data.**

The earlier string-grep "mystery ALPHA_TEST has no injector" was a false negative:
the macro name comes from the `g_shader_flags[]` array variable and is emitted as
`#define <var> = 1` (`GameOS/gameos/gameos_graphics_material_internal.h:355-359`), so a literal `"#define ALPHA_TEST"`
grep misses it. **`DATA_DRIVEN_BLOCKER` count = 0.**

## Injection mechanism
- **Primary:** `glsl_shader::makeShader` (`GameOS/gameos/utils/shader_builder.cpp:638`) glues `prefix`
  as a separate `glShaderSource` string before the file source; no `#version` in
  `.vert`/`.frag`. `makeProgram2` passes the same prefix to all stages; `prefix_`
  is cached and reused verbatim on `reload()`.
- **Material path:** `getMaterialVariation` (`gameos_graphics.cpp:346-396`) builds
  the prefix from the fixed `defines` vector + runtime-appended boolean gates, and
  already emits `unique_name_suffix_` — **an informal variant key that exists today.**
- **Compute path:** parallel hand-rolled `std::vector<const char*>` builders in
  `gpu_cull_compute.cpp` / `gpu_driven_common.cpp` / `gos_cardcloud_sim.cpp` /
  `gos_particle_bridge.cpp` (not `makeShader`).

## Macro classification (16 feature macros)

| Macro | Class | Inj | GLSL | reflect | anchor |
|---|---|:--:|:--:|:--:|:--:|
| `ALPHA_TEST` | OFFLINE_VARIANT | ✅(array) | ✅ | ❌ | ❌ |
| `IS_OVERLAY` | OFFLINE_VARIANT | ✅(array) | ✅ | ❌ | ❌ |
| `MRT_ENABLED` | OFFLINE_VARIANT | ✅ | ✅ | ❌ | ✅ |
| `TERRAIN_NORMAL_ARRAY` | OFFLINE_VARIANT | ✅ | ✅ | ❌ | ✅ |
| `MC2_SHADOW_CSM` | OFFLINE_VARIANT | ✅ | ✅ | ❌ | ✅ |
| `MC2_SHADOW_CSM_MAX` | SPECIALIZATION_CONSTANT | ✅ | ✅ | ❌ | ✅ |
| `MC2_OBJECT_ID_BUFFER` | OFFLINE_VARIANT | ✅ | ✅ | ✅ | ❌ |
| `MC2_USE_VIEW_UNIFORMS` | OFFLINE_VARIANT | ✅ | ✅ | ✅(mech) | ❌ |
| `MC2_COALESCE` | OFFLINE_VARIANT | ✅ | ✅ | ✅ | ❌ |
| `MC2_STATICPROP_PBR_SLOTS` | OFFLINE_VARIANT | ✅ | ✅ | ❌ | ❌ |
| `GPU_CULL_C1B_INDIRECT` | OFFLINE_VARIANT | ✅ | ✅ | ✅ | ❌ |
| `GPU_CULL_C2_READBACK` | OFFLINE_VARIANT | ✅ | ✅ | ✅ | ❌ |
| `READBACK_SSBO_BINDING` | SPECIALIZATION_CONSTANT | ✅ | (literal) | ✅ | ❌ |
| `ENABLE_VERTEX_LIGHTING` | DEAD_OR_STALE | ❌ | ✅ | ❌ | ❌ |
| `ENABLE_TEXTURE1` | DEAD_OR_STALE | ❌ | ✅ | ❌ | ❌ |
| `MC2_STATIC_PROP_LIGHTING` | DEAD_OR_STALE | ❌ | ✅ | ❌ | ❌ |

(`MC2_PI` = self-defined `#ifndef` fallback constant, not a variant axis.
`#version 430` carries no variant identity. 8 include-guard self-tokens excluded.)

Counts: 11 `OFFLINE_VARIANT`, 2 `SPECIALIZATION_CONSTANT`, 3 `DEAD_OR_STALE`,
0 `UNIFORM_OR_MATERIAL_FIELD`, **0 `DATA_DRIVEN_BLOCKER`**. Per-macro evidence
(injector file:line, notes) in the JSON.

## The finite variant space (what `shaderVariantId` keys over)
- **Base materials:** 6 shaders × `combinations[4]` = `{none, ALPHA_TEST, IS_OVERLAY, both}` — static.
- **Material-appended gates** (boolean, runtime-selected but enumerable): `MRT_ENABLED`,
  `TERRAIN_NORMAL_ARRAY`, `MC2_SHADOW_CSM` (+ `MC2_SHADOW_CSM_MAX` small int).
- **Batcher programs** (separate, not via gosMaterialVariation): static_prop
  `{COALESCE × OBJECT_ID × USE_VIEW_UNIFORMS × PBR_SLOTS}`, mech `{OBJECT_ID × USE_VIEW_UNIFORMS}`,
  gpu_cull `{C1B × C2}`.

All boolean toggles + one small int. The *realized* set is far smaller than the
multiplicative bound (e.g. `COALESCE` only on static_prop, `MRT` only on the
material/terrain path).

## Existing tooling coverage
- **reflect.py `SHADER_VARIANTS`** already compiles + reflects: static_prop.{frag,vert}
  (default/coalesce/objectid/both), mech.{frag,vert} (default/objectid/viewuniforms),
  shadow_static_prop.vert, gpu_cull.comp (default/c1b/c2_readback), cardcloud_sim.comp.
  `KNOWN_VARIANT_MACROS = {MC2_COALESCE, MC2_OBJECT_ID_BUFFER}` only.
- **matrix-harness `ANCHORS`** = `MRT_ENABLED, TERRAIN_NORMAL_ARRAY, MC2_SHADOW_CSM,
  MC2_SHADOW_CSM_MAX` (hard-fail if inject/guard disappears; all else WARN-only).

## Governance checker (`check-shader-injectors.py`)
The JSON inventory is the **allowlist**. The checker collects every C++-injected
shader define (literal `"#define X"` + the `g_shader_flags[]` array path) and
**FAILs on any injected macro with no inventory entry** — "no new runtime define
injector without an inventory entry." WARNs (never fails) on mystery GLSL guards
(no injector, no entry) and on injectors of macros marked `DEAD_OR_STALE`.

- Current tree: **PASS** (12 injected macros, 15 GLSL guards, 0 fail / 0 warn).
- Planted ungoverned injector: **FAIL** (proven).

## Gaps (documented, no fix in this slice)
- 3 `DEAD_OR_STALE` macros with no injector — `ENABLE_VERTEX_LIGHTING`,
  `ENABLE_TEXTURE1`, `MC2_STATIC_PROP_LIGHTING` — confirm + prune in a separate
  cleanup slice (the checker WARNs if any later gets an injector).
- `MC2_STATICPROP_PBR_SLOTS` covered by neither reflect.py nor matrix anchors
  (reflection gap); `MC2_USE_VIEW_UNIFORMS` reflected for mech only, not static_prop.

## Exclusions honored
No shader edits, no offline-compiler change, no SPIR-V consumer, no PSO key, no
material-unify, no `gosMaterialVariation` behavior change, no runtime define
removal. Foreign WIP (`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.

## Next
`PIPELINE-KEY-SCHEMA-1` — define `shaderVariantId` over the `OFFLINE_VARIANT` set
+ `SPECIALIZATION_CONSTANT` params, then extend the (deferred) PSO-key contract.
Variant identity is now finite; the biggest PSO blocker is resolved into a
governed list.
