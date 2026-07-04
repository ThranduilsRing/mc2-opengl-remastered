# P4 recon — HZB island onto the zero-copy seam

**VERDICT: NOT greenfield-easy. P4 has TWO real complications, but they are NOT the ones the
roadmap predicted.**

1. **Roadmap is STALE on parity.** The roadmap (`VK-ARC-ROADMAP.md:54`) says HZB is "SCAFFOLD-only —
   needs a live backend-compare parity FIRST." **This is wrong.** Commit `92b8874c` ("L6.4
   VK-REGION-HZB-1") already ran the live backend-compare and it **PASSED**: `--region PostprocessHzb
   RESULT PASS within_floor=True exit=0`, `B(vk)=VulkanSubgraph` (non-fallback), `used_vulkan=2308
   vk_draws=11 levels=11 validation_errors=0`, tier1 5/5. HZB is a fully-wired live region (selector +
   backend + health), byte-identical shipping, exactly like SSAO/fog. **P4 step 0 (establish baseline)
   is ALREADY DONE.** Do NOT re-run it as a prerequisite; cite `92b8874c` and move to the seam.

2. **THE REAL COMPLICATION — HZB is 100% single-channel (R32F) + depth (D32), ZERO RGBA16F color.**
   Every proven seam piece (P1 edge-fog, P3 SSAO) zero-copies an **RGBA16F color** image. HZB touches
   NO color: input = **D32_SFLOAT** scene depth (`glGetTexImage(GL_DEPTH_COMPONENT)`), output = **11×
   R32_SFLOAT** pyramid-level images written back via `glTexSubImage2D(GL_RED,GL_FLOAT)`. So P4 CANNOT
   reuse the color-transport substrate the harness/self-test proved. It needs the **depth/single-channel
   transport path**, which is T0-proven-*viable* (D32 sampled VK→GL bit-exact) but **NOT yet built into
   `VkImageResource`/`GlInteropView` as a live shipping path, and NEVER built for R32F output.** This is
   the key risk. See §3.

**Gate: `MC2_VK_HZB_INTEROP` (default OFF).** Build on POST-P1.5 primitives + the 6 external exts.

---

## 1. HZB ISLAND STATE (`GameOS/gameos/vulkan_hzb_island.cpp`, 1036 lines, commit `92b8874c`)

**What it computes:** a reverse-Z hierarchical-Z depth pyramid. N sequential fullscreen draws, one per
ceil-halved mip level over ONE shared render pass + ONE pipeline (dynamic viewport). Level 0 =
1:1 reverse-Z copy of scene depth (`uReduce=0`); level k = clamped 2×2 MIN reduce of level k-1
(`uReduce=1`). SEPARATE `VkImage` per level (`:279-294`) — REQUIRED because GL's ladder is CEIL-halved
and a single mipped VkImage is floor-halved (TU header `:16-23`).

**GL inputs:** DEPTH only. `sceneDepthTex_` read via `glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT,
GL_FLOAT,...)` at **`:887`**, memcpy'd to `depthStaging` (`:892-897`), `vkCmdCopyBufferToImage` into a
`D32_SFLOAT` image at **`:926`**. `kDepthFmt = VK_FORMAT_D32_SFLOAT` (`:257`).

**Output:** 11× `R32_SFLOAT` level images. Each read back `vkCmdCopyImageToBuffer` into one shared
buffer at per-level offsets (**`:965`**), fence-waited (`:977`), then each level `glTexSubImage2D(
GL_TEXTURE_2D,0,...,GL_RED,GL_FLOAT,...)` into `hzbLevelTex_[level]` at **`:995`**. `kLevelFmt =
VK_FORMAT_R32_SFLOAT` (`:258`, matches GL `GL_R32F` exactly).

**Bridge pattern:** CPU-readback round-trip — the SAME bridge as fog/ssao pre-seam (TU header `:33-42`).
Explicitly "the VALIDATION ORACLE, not a shipping design" (`:49`). No `VkImageResource`/`GlInteropView`
anywhere in this TU yet.

**Live or scaffold?** **LIVE.** Wired identically to SSAO:
- `runHzbRegionVulkanBackend()` (`gos_postprocess.cpp:3349`) → `hzbRegionVulkanReady()` (island `:840`,
  lazy device init + size rebuild + fail-soft self-disable) → `runHzbVulkan()` (island `:868`).
- Selector = `runHzbRegionThroughBackend()` (`gos_postprocess.cpp:3382+`), sub-selector env
  **`MC2_POSTPROCESS_BACKEND=vk`** (`:3152-3157`, one selector for all regions), master gate
  `MC2_RENDER_BACKEND_REGION_IFACE`. HZB's own content gate is **`MC2_HZB_BUILD`** (default OFF) +
  `MC2_HZB_PROBE` (diagnostic CPU cross-check).
- **NO env-gate check inside the island** — `backend=vk` selection IS the enable signal (island `:836-838`,
  mirrors SSAO). Enable-guard parity: the VK path is scoped bit-for-bit to the same frames GL would shade
  (`hzbGatesLetItRun`, `gos_postprocess.cpp:3357-3367`; declines with SKIP reason `hzb_gate_off`, not
  FallbackGL).

**Device:** builds its OWN `VkInstance`/`VkDevice` (island `:435`/`:493`), api 1.1. Instance exts =
debug-utils ONLY when validation on (`:407`); device exts = **NONE** (`dci` at `:489-494` sets no
`ppEnabledExtensionNames`). Same starting point as SSAO — see §4.

---

## 2. PARITY STATUS — ALREADY GREEN (roadmap stale)

| Claim | Roadmap says | Evidence says |
|---|---|---|
| Backend-compare run? | "NOT YET RUN" | **RUN + PASS** in `92b8874c` |
| Result | — | `--region PostprocessHzb RESULT PASS within_floor=True pixel_oracle=SHARP(6) exit=0` |
| Impl | — | `B(vk)=VulkanSubgraph` (NOT FallbackGL) |
| Island health | — | `used_vulkan=2308 vk_draws=11 levels=11 fallback_reason="" validation_errors=0` |
| Fixture | — | bit-tight, **tolerance 0** (min() of floats is bit-portable — NO transcendental floor, unlike ssao/fog) |
| tier1 | — | 5/5 (mc2_01/03/10/17/24) |

**Caveat (honest framing, from the commit body):** HZB output "feeds no visible pass yet" — the pixel
oracle sees **0 visible px changed**. So the backend-compare proves **no-regression + native
non-fallback**, and the **math correctness proof is the bit-tight fixture** (per-level readback,
tolerance 0). This is already the accepted proof shape; P4 inherits it. Reproduce the SAME evidence
post-seam (0 visible px changed, non-fallback, validation 0) — do not expect the pixel oracle to
"light up."

**Baseline command (unchanged from `92b8874c`, use as the FINAL P4 confirmation):**
```
$env:MC2_RENDER_BACKEND_REGION_IFACE="1"; $env:MC2_HZB_BUILD="1"; $env:MC2_POSTPROCESS_BACKEND="vk"; $env:MC2_VULKAN_VALIDATION="sync"
py -3 scripts/run_golden_parity.py mc2_01 --backend-compare --region PostprocessHzb --exe <islandExe>
# expect: RESULT PASS within_floor=True exit=0; B(vk)=VulkanSubgraph; 0 visible px changed;
#         [VK_HZB_ISLAND_HEALTH] used_vulkan>0 vk_draws==levels fallback_reason="" validation_errors=0
```

**➡ So step 0 is NOT "prove parity first" — that's done. Step 0 is the FORMAT PROBE (§3).**

---

## 3. FORMAT COMPLICATION — THE KEY RISK (depth + R32F single-channel, no color)

The seam substrate is format-agnostic *by signature* but color-only *by proof*:
- `vkImageResourceCreate(r, device, phys, w, h, VkFormat format, usage, exportable)`
  (`vk_image_resource.h:81`) — takes a format param.
- `glInteropViewCreate(v, src, phys, GLenum glInternalFormat)` (`gl_interop_view.h:41`) — takes a GL
  internal format.
- BUT: every live instantiation (edge-fog `kColorFmt=VK_FORMAT_R16G16B16A16_SFLOAT`
  `vulkan_edge_fog_island.cpp:284`; SSAO; `vkInteropSelfTest` `gl_interop_view.h:60` = "one exportable
  **RGBA16F**"; offline harness `IMG_FORMAT=VK_FORMAT_R16G16B16A16_SFLOAT`
  `tools/vk_interop_transport_test/main.cpp:82`) is **RGBA16F color**. The RGBA16F↔GL_RGBA16F transport
  is the ONLY externally-shared-image format proven end-to-end.

**HZB's two novel formats:**

| Boundary | HZB format | GL side | Proven as shared/exportable? |
|---|---|---|---|
| Depth IN (level-0 source) | `D32_SFLOAT` (aspect DEPTH) | `GL_DEPTH_COMPONENT/GL_FLOAT` from `sceneDepthTex_` | **T0 VIABLE only** (`vk_gl_interop_spike main_depth`) — sampled VK→GL bit-exact, "keep depth dedicated-allocated". Never wired into `VkImageResource`/`GlInteropView` live. **This IS P5's job.** |
| Pyramid OUT (11× levels) | `R32_SFLOAT` (aspect COLOR) | `GL_RED/GL_FLOAT` → `hzbLevelTex_[k]` (GL_R32F) | **NOT proven at all.** No spike, no self-test, no harness for single-channel R32F export. GREENFIELD format. |

**Consequences for scoping — pick ONE and state it in the plan:**

- **The depth INPUT boundary is really P5, not P4.** HZB's input is the SAME `sceneDepthTex_` that
  edge-fog/SSAO read back — P5 ("Depth zero-copy for the fog/postprocess regions") is explicitly the
  slice that adds the depth `VkImageResource` (dedicated-allocated) + depth `GlInteropView` import. If
  P4 does its own depth import it **duplicates and pre-empts P5**, and eats the "dedicated-allocated
  depth" T0 constraint alone. **Recommendation: P4 should NOT build the depth-in boundary; it should
  consume the shared depth image P5 produces.**

- **The R32F OUTPUT boundary is P4's genuinely-new work** and is the harder half: 11 separate exportable
  R32F images (one per ceil level), each imported into the corresponding `hzbLevelTex_[k]` as a shared
  image, replacing the `vkCmdCopyImageToBuffer` (`:965`) + `glTexSubImage2D` (`:995`) round-trip. This
  needs: (a) `VkImageResource` proven for `VK_FORMAT_R32_SFLOAT` + `GL_R32F`; (b) the offline harness
  parameterized to R32F (`IMG_FORMAT`, `GL_INTERNAL_FMT`, `glReadPixels` type, golden — harness
  README `:81-84` documents exactly these knobs, so it IS parameterizable); (c) N-image variant (loop
  the single-image pattern 11×) — mechanical, not novel.

**➡ SEQUENCING RULING (blunt): P4 should come AFTER P5.** P5 builds + proves the depth shared-image
(D32, dedicated-allocated) that HZB's level-0 seed pass samples. Then P4 = "consume P5's shared depth as
level-0 source + add the 11× R32F shared-output images." Doing P4 before P5 forces P4 to build BOTH novel
format paths (depth import AND R32F export) in one slice — larger, and it steals P5's depth deliverable.
**If forced to keep P4 before P5:** split it — P4a = R32F single-channel transport probe + harness param
+ ONE-level output-only seam (depth stays readback, exactly as edge-fog kept depth readback); P4b (or
folded into P5) = the depth-in boundary. Ship P4a first; it's the honest minimal rung.

---

## 4. DEVICE / EXTS / GATE

- **Own device.** HZB builds its own instance (`:435`) + device (`:493`) with **empty device exts** and
  instance exts = debug-utils only. IDENTICAL to SSAO's starting state.
- **Must add the 6 external-memory/-semaphore Win32 exts conditionally (gate ON)**, mirroring edge-fog:
  instance caps (`VK_KHR_external_memory_capabilities` + `..._semaphore_capabilities` +
  `get_physical_device_properties2`) and device (`VK_KHR_external_memory` + `..._win32` +
  external-semaphore-win32). Add ONLY when `MC2_VK_HZB_INTEROP` is ON so gate-OFF = no probe =
  byte-identical (SSAO recon blocker #6). LUID match runs in `glInteropViewCreate` on the island's own
  device; single-GPU auto-passes, multi-GPU fail-soft to readback.
- **Gate `MC2_VK_HZB_INTEROP` (default OFF).** Distinct from `MC2_HZB_BUILD` (content) and
  `MC2_POSTPROCESS_BACKEND` (backend select). Register + allowlist (`run_smoke.py`) + document
  (`docs/tier1_env_vars.md`, gated-layout-macros.md). Gate-OFF path keeps the current readback bridge
  verbatim → shipping `build64` byte-identical; assert exe md5-identity bracketed (LM8).

---

## 5. ORACLE + FORBIDDEN-COUNTERS + TASK BREAKDOWN

**Oracle (two-tier, both already exist for HZB):**
1. **Offline first (seconds):** the R32F-parameterized transport harness (`vk_interop_transport_test`,
   set `IMG_FORMAT=VK_FORMAT_R32_SFLOAT`, `GL_INTERNAL_FMT=GL_R32F`, aspect COLOR) — proves the R32F
   transport is transparent for a single level before any game launch. PLUS keep the existing bit-tight
   `hzb_fixture` (tolerance 0, per-level readback) green — that's the math proof.
2. **Game golden-parity FINAL:** the `92b8874c` command in §2, unchanged. Expect same evidence
   (PASS, 0 visible px changed, non-fallback, validation 0), now with forbidden-counters == 0.

**Forbidden-counters (interop path → 0), scoped to whichever boundary the slice ships:**
- OUTPUT boundary (P4a): `vkCmdCopyImageToBuffer` (island `:965`) AND `glTexSubImage2D` (island `:995`)
  → 0 (replaced by shared R32F images + `glInteropWaitFromVk`).
- INPUT boundary (P5 / P4b): `glGetTexImage(depth)` (island `:887`) AND `vkCmdCopyBufferToImage(depth)`
  (island `:926`) → 0. **Keep these NON-zero (readback) in an output-only P4a**, exactly as edge-fog/
  SSAO kept depth readback.

**Task breakdown (mirror edge-fog T3-T5 / SSAO):**
- **T0 (new type checkpoint — DO THIS FIRST):** R32F single-channel transport probe. Parameterize
  `vk_interop_transport_test` to R32F; add an R32F case to `vkInteropSelfTest` (currently RGBA16F only).
  Prove `VkImageResource(VK_FORMAT_R32_SFLOAT)` ↔ `GlInteropView(GL_R32F)` round-trips bit-exact,
  validation-clean. This retires the §3 key risk. Update the Tooling Ledger row.
- **T3a:** add the 6 exts + `MC2_VK_HZB_INTEROP` to the HZB device (conditional, gate-ON).
- **T3b:** create 11× exportable R32F `VkImageResource` (one per level) as the render-pass color
  attachments (repoint each level's framebuffer view), + 11× `GlInteropView` over `hzbLevelTex_[k]`.
- **T4 (output seam):** drop `vkCmdCopyImageToBuffer` (`:965`) + `glTexSubImage2D` (`:995`); after
  submit, `glInteropWaitFromVk` per level so the GL probe/consumer reads the shared images. Depth-in
  STAYS readback (P4a) → defer to P5/P4b.
- **T5:** forbidden-counter hard-asserts (output pair == 0) + health (`vk_draws==levels`,
  `fallback_reason=""`, `validation_errors=0`) + region non-fallback.
- **T6:** gate register/allowlist/doc; exe md5-identity gate-OFF (LM8); offline R32F harness + fixture
  green; game golden-parity FINAL (§2). STOP-gate: any failure → `MC2_VK_HZB_INTEROP` default-OFF
  forever.

---

## 6. CONFLICTS WITH IN-FLIGHT P2 (subgraph) / P3 (SSAO)

**Clean — HZB is its own TU.** `vulkan_hzb_island.cpp` is a separate translation unit with its own
anonymous-namespace `ensure_volk_initialized()`, its own `g_health` (named `HzbIslandHealth`,
distinct symbol — TU header `:75-79` calls this out explicitly to avoid collision with the other
islands' health), its own device, its own extern-C health getter (`mc2_vulkan_hzb_island_health`),
its own region selector/health (`g_hzbRegion` / `mc2_render_backend_hzb_region_health`). No shared
mutable state with the fog subgraph (P2) or SSAO island (P3).

**Only shared touchpoints (all additive, non-conflicting):**
- `MC2_POSTPROCESS_BACKEND` selector env — READ-shared (one selector picks backend for every region);
  P4 adds no new selector logic.
- The interop substrate headers (`vk_image_resource.h` / `gl_interop_view.h`) — P4's **T0 adds an R32F
  case to `vkInteropSelfTest`**; that's a shared-file additive change. Coordinate a touch of
  `gl_interop_view.cpp` self-test if P3 is mid-edit there — otherwise clean.
- P5 (depth) — the ONE real sequencing dependency (§3), not a conflict: P4 should consume P5's shared
  depth. **Recommend P5 before P4.**

---

## Tooling-ledger delta (per roadmap standing order)
| Tool | Covers | Status |
|---|---|---|
| `vk_interop_transport_test` (R32F) | single-channel R32F transport transparency | ❌ **build at P4 T0** (param exists, README `:81-84`) |
| `vkInteropSelfTest` (R32F case) | in-engine R32F export round-trip | ❌ **add at P4 T0** (currently RGBA16F only) |
| `hzb_fixture` (bit-tight, tol 0) | HZB math correctness | ✅ exists (`92b8874c`) — keep green |
| depth shared-image (D32, dedicated) | HZB level-0 seed input | ⏳ **P5 deliverable** — P4 consumes it |
