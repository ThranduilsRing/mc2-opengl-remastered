# Vulkan Offscreen-Island Template (VULKAN-ISLAND-TEMPLATE-1)

A reusable, copy-paste checklist for building the **next** offscreen Vulkan render
island in MC2, distilled from the EdgeFog island arc so the next builder does not
rediscover the same process + bugs.

**North star:** the 8-layer Vulkan-readiness ladder — **Layer 3 = offscreen islands**
(a self-contained Vulkan render whose result is read back into the GL frame; GL still
owns the swapchain). See `docs/render-backend-seams/vulkan-readiness-audit-1.md` and
`docs/render-backend-seams/nvidia-readiness-runbook.md`.

**Reference implementation (read these before starting your own):**
- `GameOS/gameos/vulkan_edge_fog_island.cpp` — the island (whole TU under `#ifdef MC2_VULKAN_ISLAND`)
- `shaders/vulkan/edge_fog.{vert,frag}` — the ported shaders
- `GameOS/gameos/vulkan_backend_skeleton.cpp` — probe device/VMA/spv plumbing + the synthetic fixture (`mc2_vulkan_probe_edgefog_fixture`)
- Commits: `744a483c` (2a: build+port+device+readback), `be223708` (2b: row_major + ODR + pixel parity), `b1dc09d5` (synthetic fixture), `8cfbf413` (health dump)

An island is a **GL→Vulkan→GL bridge**: `glGetTexImage` scene textures → VMA staging →
`vkCmdCopyBufferToImage` → render pass → `vkCmdCopyImageToBuffer` → `glTexSubImage2D` back.
CPU readback is the **validation oracle, not production** (§5).

---

## Section outline

1. Build topology (CMake variant + ODR discipline)
2. Shader port checklist (GL uniforms → std140/descriptors; matrix + convention gates)
3. Micro-checks before parity (unproject + mismatch buckets)
4. Two-layer oracle (synthetic fixture + golden-scene bookmark)
5. Interop: readback is the oracle, not production
6. Health / observability
7. Fallback: fail-soft to GL, proven
8. Verification gates checklist (copy-paste)
9. Standing guards to run

---

## 1. Build topology

Add an OFF-by-default CMake option so the default build stays **byte-identical**.
Pattern (from `CMakeLists.txt` + `GameOS/gameos/CMakeLists.txt`):

```cmake
option(MC2_<ISLAND> "Compile the <island> Vulkan island into gameos" OFF)
if(MC2_<ISLAND> AND NOT MC2_VULKAN)
    message(FATAL_ERROR "MC2_<ISLAND>=ON requires MC2_VULKAN=ON (volk/VMA/headers).")
endif()
# TU is added ONLY under the option, and the whole .cpp is #ifdef MC2_<ISLAND>.
```

- Isolated variant build dir — **never share `build64`**. Vulkan builds use
  `build64_vulkan` (probe/skeleton); the island variant is `build64_island`. The Vulkan
  SDK loader/headers must not leak into the normal tree.
- The default (`MC2_<ISLAND>=OFF`) build compiles nothing new → byte-identical exe.

### The ODR bug — the single hardest lesson

If the island adds a **member or changes any class layout** under `#ifdef MC2_<ISLAND>`
(EdgeFog added `vkFogIsland_` + method decls to `gosPostProcess` in `gos_postprocess.h`),
the macro **must reach every TU that includes that header** — not just the TU defining the
island. In EdgeFog, `gameos_main`/`mc2` include `gos_postprocess.h` but do **not** consume
`gameos`'s PUBLIC usage-requirements (no `target_link_libraries(gameos)` edge), so:

```cmake
target_compile_definitions(gameos      PUBLIC  MC2_<ISLAND>)  # defines the member
if(MC2_<ISLAND>)
    target_compile_definitions(gameos_main PRIVATE MC2_<ISLAND>)  # else ODR skew
    target_compile_definitions(mc2         PRIVATE MC2_<ISLAND>)
endif()
```

**Symptom if you get this wrong:** `getSceneFBO()`/`getWidth()`/`getHeight()` (or whatever
accessors sit past the new member) read the **wrong struct offset** → garbage FBO → the
visual capture path never fires (silent). Keep the gated header block to an **opaque
pointer + method decls only** so no Vulkan headers leak to consumers.

---

## 2. Shader port checklist

Port `shaders/<name>.frag` → `shaders/vulkan/<name>.frag`. **Keep the math bit-for-bit
identical**; change only uniform/sampler plumbing.

- [ ] GL default-block uniforms → a **std140 UBO** with explicit `layout(set,binding)`.
      Mirror the byte offsets in the CPU-side POD (`static_assert(sizeof(Params)==N)`).
- [ ] GL samplers → explicit `layout(set=0, binding=N) uniform sampler2D`. Wire matching
      `VkDescriptorSetLayoutBinding`s (combined-image-sampler + uniform-buffer) CPU-side.
- [ ] **Matrix layout — the 2b bug.** A std140 `mat4` defaults to **column-major**. If the
      CPU packs the *same 16 floats the GL path uploads* (row-major direct, `glUniformMatrix4fv`
      **GL_FALSE**), the shader will transpose them and unprojection breaks. Fix: qualify the
      UBO block `layout(std140, row_major)`. Concrete failure witnessed:
      `GL-Z=-15.39 vs VK-Z=669.57` before the qualifier; matched after.
- [ ] **Convention audit** — explicitly decide + comment each:
      - **Reverse-Z depth** — EdgeFog uses `[0,1]` reverse-Z: near=ndc-z `1.0`, far=`0.0`.
      - **Y-flip** — none needed when the frag reads an interpolated `[0,1]` UV (`TexCoord`)
        and you upload the depth image so texel (u,v) maps like the GL sample. **If you read
        `gl_FragCoord`, Vulkan's origin/Y differs from GL** — handle it.
      - **sRGB vs linear** — pick UBO/image formats to match GL (EdgeFog: color `RGBA16F`
        linear, depth `D32_SFLOAT`).
- [ ] Fullscreen triangle: no vertex buffer, `gl_VertexIndex`-derived UV
      (`vec2((idx<<1)&2, idx&2)`), `gl_Position = vec4(uv*2-1, 0, 1)`.
- [ ] Bake to `.spv` via the `mc2_vulkan_shaders` target (add your basenames to
      `MC2_VK_SHADER_BASENAMES`). `--target mc2` does NOT pull ALL, so
      `add_dependencies(mc2 mc2_vulkan_shaders)` under the island option.

---

## 3. Micro-checks BEFORE parity

Before running any pixel parity, do a **cheap CPU convention gate**: unproject a known
point (e.g. NDC `(0,0,0.5)`) with the exact `invViewProj` math the shader uses and compare
GL-world-Z vs VK-world-Z. EdgeFog logs this one-shot from `runEdgeFogVulkan()`:
computes `mul_rowmajor` (GL upload convention) alongside `mul_colmajor` (pre-2b bug) so a
regression re-diverges visibly. Match ⇒ the matrix/depth convention is consistent.

**Classify any diff you see into a named bucket** (do not guess — name it):

| Bucket | Tell |
|---|---|
| `matrix-layout` | row/col-major transpose; huge Z sign/magnitude flip (the 2b bug) |
| `clip-depth` | GL `[-1,1]` vs VK `[0,1]` clip; whole scene near/far inverted |
| `UV-Y` | vertical mirror; only when reading `gl_FragCoord` / sampling flipped |
| `sRGB-linear` | uniform gamma-ish brightness shift across the frame |
| `sampler-filtering` | soft edges / interpolation only where a sampler filters (use NEAREST for depth) |
| `depth-precision-reversedZ` | banding / precision loss far from camera; reverse-Z mishandled |
| `half-texel-viewport` | 1px shift; viewport/scissor or texel-center offset |
| `FMA-drift` | sub-ULP noise only; expected, must sit under the half tolerance (§4) |

---

## 4. Two-layer oracle

Two independent proofs. Ship both.

### (a) Synthetic headless fixture — proves the shader MATH

Render the **shipped `.spv`** on KNOWN inputs offscreen (no game), against a faithful CPU
reimplementation of the frag; per-pixel compare. Reference:
`mc2_vulkan_probe_edgefog_fixture()` in `vulkan_backend_skeleton.cpp` (64×64, row-major
`invViewProj` with known world reconstruction, banded synthetic depth exercising every
branch — void / mid / water-skip + a column sweep through inner-ramp / edge / outside-fill).
Reuses the probe device/VMA/spv plumbing; harness asserts `edgefog_fixture=1` strict.

**Tolerance = `1/1024` (RGBA16F half precision), justified.** A `RGBA16F` attachment has a
10-bit mantissa; a single round-on-store gives ~`0.0004` max abs diff. EdgeFog measured
`max_abs_diff=0.00044477`, `pixels_beyond_tol=0`. Set tol to the half-precision floor, **not**
to zero — a byte-exact bar would false-fail on legal FP16 rounding.

### (b) Golden-scene bookmark parity — proves INTEGRATION

`scripts/run_golden_parity.py <scene> MC2_<ISLAND_GATE>` runs the deployed exe with the gate
OFF vs ON and floors the diff. The **pixel oracle** is `run_visual_capture.py` (pinned
bookmark pose + parked cursor + `MC2_SMOKE_FIXED_TIMESTEP=1` ⇒ byte-stable per-bookmark
sha256), NOT golden_scene's fly-through pixel_hash. Structural fields (registry_hash,
pass_counters, render_health, exe_md5, gate_set) come from `golden_scene.py`.

- [ ] Add a bookmark framing the island's output with **≥2 poses** to
      `tests/visual/bookmarks/<scene>.json` (see `mc2_01.json`), so the changed pixels are
      actually in frame.
- Noise floor is built from **N≥2 OFF captures** (default `--n-floor 3`); bookmarks that
  drift OFF-vs-OFF (first-run shader-compile / texture-residency / streaming warmup) are
  recorded non-exact and cannot fail an ON candidate. If ALL pixel bookmarks drift
  OFF-vs-OFF, the harness says so — the oracle is too blunt for that scene.
- Exit 0 iff OFF vs ON is within-floor for every field incl. every exact `pixel_sha`.

---

## 5. Interop: CPU-readback is the VALIDATION ORACLE, not production

The `glGetTexImage`/`glTexSubImage2D` round-trip and the fence-wait are a **correctness
harness**, not a shipping design. They stall the GL pipeline and the CPU. State this
explicitly in the island's header comment. Production zero-copy interop (GL/VK shared
memory, external semaphores) is a **later layer** — an island proves the *shader + math*
port under a gate; it does not commit MC2 to the readback path.

---

## 6. Health / observability

Ship a process-lifetime health struct + an `extern "C"` POD getter consumed by
`debug_state_dump.cpp` (survives island teardown; the shutdown dump still sees last values).
Generalize the EdgeFog fields (`IslandHealth` in `vulkan_edge_fog_island.cpp`):

- `vulkan_available` (volkInitialize succeeded), `island_build_enabled` (TU compiled),
  `island_runtime_gate` (env=1)
- `device_name` (`VkPhysicalDeviceProperties.deviceName`)
- `validation_errors`
- `<op>_attempted` (frames the island path was entered) vs `<op>_used_vulkan`
  (frames actually composited via Vulkan) — the delta tells you fallbacks happened
- `fallback_reason` ("" when healthy; else `no_vulkan_runtime` / `spv_load_failed` /
  `device_init_failed` / `fence_timeout` / …)
- timings: `copy_us`, `render_us`, `readback_us`

Emit a `[VK_ISLAND_HEALTH] …` line on teardown AND surface the same fields in the JSON
debug-state dump.

---

## 7. Fallback: fail-soft to GL, and PROVE it

- Every Vulkan error path returns false / sets `fallback_reason` and **falls back to the GL
  path** — never crash. This is also the OpenGL-user-without-a-Vulkan-runtime path.
- Init failure ⇒ set `disabled=true` once, never retry (GL for the rest of the run).
- Lazy init on first use; rebuild + resize on framebuffer size change.
- **Prove fallback:** run with the island gate ON on a machine / config where Vulkan is
  unavailable (or force a spv-load / device-init failure) and confirm the frame still renders
  via GL, `fallback_reason` is set, and `<op>_used_vulkan==0`. A golden-parity run with the
  gate ON must still be within-floor (GL path ran) — that *is* the fallback proof.

---

## 8. Verification gates checklist (copy-paste for the next island)

```
[ ] BYTE-IDENTICAL OFF   — default build (MC2_<ISLAND>=OFF) exe byte-identical to baseline
                           (exe_md5 unchanged in golden_scene manifest)
[ ] SYNTHETIC FIXTURE    — headless .spv-vs-CPU fixture PASS: pixels_beyond_tol=0,
                           max_abs_diff < 1/1024 (RGBA16F half floor), validation_errors=0
[ ] MICRO-CHECK MATCH    — unproject known point: GL-world-Z ~= VK-world-Z (row_major),
                           NOT the col-major pre-fix value
[ ] CAPTURE-LIVENESS     — run_golden_parity preflight: engine capture path fired,
                           bookmark shas produced (no ABORT reason=capture-liveness)
[ ] GOLDEN PARITY        — run_golden_parity OFF vs ON WITHIN-floor for every exact
                           pixel_sha + structural field; pixel_oracle reported SHARP
[ ] VALIDATION CLEAN     — run_vulkan_probe with validation ON: zero validation-layer errors
[ ] HEALTH DUMP SANE     — vulkan_available=1, device_name set, used_vulkan>0 (island ran),
                           fallback_reason="" on the happy path
[ ] FALLBACK PROVEN      — Vulkan-unavailable / forced-failure: renders via GL, no crash,
                           fallback_reason set, used_vulkan=0, golden parity still within-floor
```

---

## 9. Standing guards to run

Run these on every island slice (they are also CI check-scripts):

- `scripts/check-vulkan-bindings.py` — Vulkan set/binding layout consistency
- `scripts/check-binding-slots.py` + `scripts/check-sampler-bindings.py` — GL-side binding / sampler-slot occupancy (the island still touches GL textures)
- **gated-layout-macro guard** — verify the island macro is defined on *every* TU that
  includes the layout-changed header (the ODR guard from §1); if no dedicated check-script
  exists yet, assert it in the slice preflight and CMake comments.
- `scripts/run_vulkan_probe.py [--validation sync]` — probe tokens all 1, zero validation errors (also builds/asserts the synthetic fixture `edgefog_fixture=1`)
- `scripts/run_golden_parity.py <scene> MC2_<ISLAND_GATE>` — the integration parity oracle (§4b)

**Subgraph rule:** a Vulkan subgraph may only fuse passes CONTIGUOUS in canonical order;
adding a non-contiguous pass is a reorder experiment (scheduler/oracle + parity proof), not
a subgraph expansion — see `docs/render-backend-seams/subgraph-contiguity-rule.md`
(enforced by the `RenderCore/postprocess_subpass_order.h` constexpr guard).

Cross-links: `docs/render-backend-seams/vulkan-readiness-audit-1.md` (layer ladder) ·
`docs/render-backend-seams/nvidia-readiness-runbook.md` · `docs/tier1_env_vars.md`
(register `MC2_<ISLAND>` + `MC2_<ISLAND_GATE>`).
