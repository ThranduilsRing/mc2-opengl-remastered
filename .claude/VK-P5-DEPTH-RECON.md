# P5 recon — depth zero-copy for the postprocess regions (edge-fog + fog subgraph)

**VERDICT: NOT the straightforward "T0 de-risked it" slice the roadmap assumes. T0 proved the
WRONG FORMAT.** The primitives (`VkImageResource`/`GlInteropView`) already handle depth aspect — that
part is genuinely done. But T0 exported **`D32_SFLOAT`**, while the engine's actual scene depth texture
`sceneDepthTex_` is **`GL_DEPTH24_STENCIL8`** (`gos_postprocess.cpp:933`). You cannot zero-copy-alias a
D24S8 GL texture as a D32_SFLOAT VK image — different bit layouts, and the current island readback is a
*format-converting* `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)`, which zero-copy cannot reproduce. P5's
hard part (a D24S8/packed-depth-stencil external import, or a proven equivalent) is **UN-de-risked**.
Everything else is a clean copy of the edge-fog/P2 color template. **Recommend a P5-T0' spike (D24S8
import) BEFORE committing the slice** — mirror `main_depth.cpp`, swap the format, keep it dedicated.

Base worktree: `A:/Games/mc2-vk-interop-bridge` @ branch `claude/vk-interop-bridge-1`, HEAD `6d60ed23`.
Relevant SHAs: `f1f1b324` (P1 seam), `1fcfbc79` (P1.5 VK→GL sem), `790bfd6f` (transport harness),
T0 depth spike shipped alongside color spike (`tools/vk_gl_interop_spike/main_depth.cpp`).

---

## 1. Depth boundary call sites (the forbidden-counter targets)

Depth is **INPUT-ONLY** to both passes — GL renders scene depth, VK samples it, VK never writes depth
back. Confirmed one-directional GL→VK (no VK→GL depth output anywhere). This is *simpler* than color's
round-trip: depth needs only the `doneSem`/`glInteropSignalToVk` (GL→VK) half; it does **not** need the
`vkToGlSem` back-channel for correctness (nothing waits VK's depth on the GL side).

### edge-fog (`GameOS/gameos/vulkan_edge_fog_island.cpp`)
Two code paths, depth round-trips in BOTH:
- **Readback path** `renderReadback` (approx :1030–1240):
  - `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` — **:1096** (`++g_depthGetTexImage` :1097)
  - `vkCmdCopyBufferToImage(depthStaging → depthImage)` — **:1171** (`++g_depthCopyBufferToImage` :1172)
- **Interop path** `renderInterop` (approx :1245–1480):
  - `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` — **:1278** (`++g_depthGetTexImage` :1279)
  - `vkCmdCopyBufferToImage(depthStaging → depthImage)` — **:1355** (`++g_depthCopyBufferToImage` :1356)
- Depth counters **already exist** and are surfaced in the `[VK_EDGE_FOG]` health line:
  `depth_copyBufToImg` / `depth_getTexImage` (:1474–1478). P5 drives these to **0** on the interop path.
- Depth image + staging + scratch: `depthImage`/`depthAlloc`/`depthView` (:322–323), `depthStaging`
  (:329), `depthScratch` (:357), created `make_image(kDepthFmt, SAMPLED|TRANSFER_DST)` (:685),
  view `VK_IMAGE_ASPECT_DEPTH_BIT` (:706), staging buffer (:743). `kDepthFmt = VK_FORMAT_D32_SFLOAT` (:283).

### fog subgraph (`GameOS/gameos/vulkan_postprocess_subgraph.cpp`)
Two code paths, depth round-trips in BOTH; **depth is UN-counted here** (no forbidden counter on it —
P5 must add depth counters, mirroring edge-fog):
- **Readback path** (approx :1120–1240): `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` **:1132**;
  `vkCmdCopyBufferToImage(depth)` **:1221**.
- **Interop path** (approx :1340–1520): `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` **:1351** (no
  `++` — see comment :1322 "DEPTH still round-trips … P5"); `vkCmdCopyBufferToImage(depth)` **:1441**.
- Depth image/staging: `depthImage` (:339), `depthStaging` (:346), `depthScratch` (:376),
  `make_image` (:697), view `ASPECT_DEPTH` (:717), staging (:752), `kDepthFmt` D32_SFLOAT.

**Input source (both):** `sceneDepthTex_`, the GL-owned scene depth texture, exposed via
`GosRenderExecutor::getSceneDepthTexture()` (`gos_postprocess.h:106`). Bound at edge-fog :1095/:1277,
subgraph :1131/:1350. Guarded non-zero: `sceneDepthTex_ == 0 → return false` (edge-fog :1038,
subgraph :1074).

---

## 2. Format / alloc — **the load-bearing finding**

- **T0 proved `D32_SFLOAT`** SAMPLED external export, dedicated-allocated, `GL_DEPTH_COMPONENT32F`
  import, bit-exact, validation-clean (`T0-depth-spike-result.log`: `dedicated-required=1`, depth[0]
  round-trips exactly, 0 val errors). The island's `kDepthFmt` is D32_SFLOAT to match T0.
- **BUT the engine's real `sceneDepthTex_` is `GL_DEPTH24_STENCIL8`** (`gos_postprocess.cpp:933`
  `glTexStorage2D(..., GL_DEPTH24_STENCIL8, ...)`; confirmed again :951, :1004, :1249, :4185). The
  current island bridges the format mismatch *for free* because `glGetTexImage(GL_DEPTH_COMPONENT,
  GL_FLOAT)` **converts** D24S8 → normalized float32 on read, then uploads into a D32_SFLOAT image.
- **Zero-copy cannot convert.** A shared image aliases the SAME memory bytes. To zero-copy the scene
  depth you must import `sceneDepthTex_`'s actual storage — i.e. create the exportable image as
  `VK_FORMAT_D24_UNORM_S8_UINT` (or `X8_D24_UNORM_PACK32`) and import into GL as
  `GL_DEPTH24_STENCIL8`, matching bit layout. **T0 did NOT prove this format is exportable / importable
  on the 7900 XTX.** On AMD, D24S8 external export is historically less reliable than D32_SFLOAT.
- **T0 dedicated finding still holds and still applies:** external depth is `dedicated-required=1`; the
  `VkImageResource` create path *always* dedicate-allocates (`vk_image_resource.cpp:117–124`, `ded`
  chained whether or not exportable), so DEDICATED is satisfied. Good.
- **Direction note (worth flagging):** T0 is VK-writes / GL-reads (VK→GL). **P5 is the opposite**: GL
  writes/owns the depth, VK samples it (GL→VK). GL→VK is the direction the *color input* already proves
  live (`f1f1b324`), so the direction itself is de-risked — only the *format* is not.

### Options for the format gap (pick in the P5-T0' spike)
1. **Import D24S8 as-is** (`D24_UNORM_S8_UINT` VK ↔ `GL_DEPTH24_STENCIL8` GL). Truest zero-copy.
   Risk: AMD external-export support for packed depth-stencil; depth-stencil aspect handling in the
   sampler (frag samples `.r` depth only — needs `VK_IMAGE_ASPECT_DEPTH_BIT` view over a DS image, which
   `aspectFor()` already returns DEPTH|STENCIL for D24S8 → **would need a DEPTH-only view override**).
2. **Change the engine scene depth attachment to D32F** — out of scope / too invasive (touches the whole
   GL depth pipeline, water reflection, shadow copy `getSceneDepthCopyTexture` :114). Reject.
3. **Keep depth on readback for P5, ship nothing** — i.e. P5 is blocked until the D24S8 spike passes.
   This is the honest fallback if the spike fails (STOP-gate discipline).

**Recommendation:** run P5-T0' (D24S8 external import spike) FIRST. If it passes → option 1, change
`kDepthFmt` to D24S8 + force a depth-aspect-only view. If it fails → P5 does not ship; depth stays
readback; revisit when/if the engine depth format changes.

---

## 3. Primitive gaps (VkImageResource / GlInteropView for depth)

Mostly **already handled** — the primitives were written depth-aware from the start:
- `vk_image_resource.cpp:aspectFor()` (:37–44) maps D32/D16/X8D24 → DEPTH, D24S8/D32S8 → DEPTH|STENCIL,
  else COLOR. The ImageView subresource uses it (:158). ✅ depth aspect handled.
- Create path always dedicate-allocates (:117–124). ✅ satisfies T0 depth constraint.
- Export sanity uses `vkGetPhysicalDeviceImageFormatProperties2` with the actual format+usage (:61–83),
  so a non-exportable depth format **fail-softs** (returns false) rather than crashing. ✅.

**Minimal additions P5 needs:**
- `glInteropViewCreate(..., GLenum glInternalFormat)` already takes the GL internalformat as a param
  (`gl_interop_view.h:42`) — pass `GL_DEPTH_COMPONENT32F` (option-2/T0 format) or `GL_DEPTH24_STENCIL8`
  (option-1). No signature change. ✅.
- **If option 1 (D24S8):** need a **depth-aspect-only** ImageView for sampling. `aspectFor(D24S8)`
  returns DEPTH|STENCIL, which is **invalid for a sampled image view** (a sampled DS view must pick one
  aspect). → add an optional `VkImageAspectFlags viewAspectOverride` param to `vkImageResourceCreate`,
  or a dedicated `depthSampleView`. Small, contained.
- **GL depth import correctness:** `glTextureStorageMem2DEXT` with the depth internalformat over
  imported OPTIMAL-tiled memory — T0 proved it for `GL_DEPTH_COMPONENT32F`; **UNPROVEN for
  `GL_DEPTH24_STENCIL8`** (P5-T0' must confirm). Note GL depth-stencil import may need
  `GL_DEPTH_STENCIL` / `GL_UNSIGNED_INT_24_8` semantics.
- No new semaphore slots: depth is input-only, reuses `doneSem`/`glInteropSignalToVk` (GL→VK). The
  `vkToGlSem` back-channel is unused for depth. ✅ simpler than color.

`main_depth.cpp` is the exact reference; the only line that changes for the spike is `IMG_FORMAT`
(:46) + the GL internalformat at `glTextureStorageMem2DEXT` (:472) + the readback verify aspect.

---

## 4. Tooling checkpoint (NEW TYPE = depth) — **P5 task T0'** (do first)

Per the roadmap's TOOLING-REFRESH standing order, first-of-a-new-type ⇒ build/extend the offline oracle
BEFORE grinding the slice.
- **Extend `tools/vk_gl_interop_spike/main_depth.cpp`** → add a D24S8 (`D24_UNORM_S8_UINT` /
  `GL_DEPTH24_STENCIL8`) variant, or parameterize `IMG_FORMAT`. Exit 0 = D24S8 depth zero-copy viable.
  This is the *decisive* de-risk; ~1 hour, no game launch.
- **Parameterize `tools/vk_interop_transport_test`** to a **depth (D32 and D24S8) case**: render a
  fixed golden depth field BOTH ways (readback-bridged vs interop-shared `VkImageResource`) and assert
  the sampled result byte-identical + forbidden depth-counters == 0 + validation-clean. Proves transport
  transparency for the depth sampler offline. (`main.cpp` + `CMakeLists.txt` already present at
  `tools/vk_interop_transport_test/`.)
- **Parameterize the in-engine `vkInteropSelfTest`** (`gl_interop_view.h:66`) to also exercise a D32/
  D24S8 exportable resource at island init (behind `MC2_VK_INTEROP_SELFTEST`), so a live device that
  can't do depth external import fails LOUD before the frame loop, not mid-frame.

**Only after T0' passes** does the slice proceed. If T0' fails on both D32-substitution and D24S8, P5 is
blocked (STOP-gate) — do not paper over with a readback that pretends to be interop.

---

## 5. Gate — recommendation: **EXTEND the existing per-region gates, no separate depth sub-gate**

Edge-fog uses `MC2_VK_INTEROP_BRIDGE`; subgraph uses `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP`
(both documented `docs/tier1_env_vars.md:63–64`). Depth-on should be **implied by color-on within each
region** — there is no scenario where you want color zero-copy but depth readback in the *same* region
(the whole point of P5 is to finish that region's tail). A separate `MC2_VK_INTEROP_DEPTH` would create a
2×2 matrix (color×depth) that multiplies test surface for zero product value.

**Recommend:** P5 extends each region's existing gate to *also* zero-copy depth when that gate is ON.
Byte-identical gate-OFF is preserved (the depth `VkImageResource` + external probe are created only
inside the same `useInterop` branch that already guards color). Keep ONE optional escape hatch:
`MC2_VK_INTEROP_DEPTH_FORCE_READBACK=1` (default off) to force depth back onto readback *while color
stays zero-copy* — purely a **fault-isolation debug knob** for the STOP-gate, not a shipping mode. This
keeps the shipping matrix at "gate ON = full region zero-copy" while giving a bisect lever if depth
regresses.

---

## 6. Oracle

Same edge bookmarks as P1/P2 (`tests/visual/bookmarks/*.json`, mc2_01 edge set: oblique_fog /
oblique_horizon / cliff_side_on / cross_map_view + edge_fog_boundary_south /
edge_fog_corner_southwest), fog_oob off. Additions vs P1/P2:
- **Assert DEPTH forbidden-counters == 0** on the interop path, in BOTH regions:
  edge-fog `depth_getTexImage` & `depth_copyBufToImg` → 0 (counters exist); subgraph — **add** the depth
  counters first, then assert 0. Mirror the existing color hard-assert (edge-fog :1433–1442, subgraph
  :1519–1528) so a stray depth copy fail-softs + logs LOUD.
- Still **byte-identical** vs each region's OWN readback baseline (so any AMD-GLSL/glslang noise
  cancels), and `MC2_VULKAN_VALIDATION=sync` clean.
- Offline transport harness (§4) FIRST (seconds), golden-parity as FINAL confirm only.

**R-A-W / layout considerations for a SHARED depth image** (new vs color):
- Depth has region-specific layouts. GL leaves the imported depth in `GL_LAYOUT_DEPTH_READ_ONLY_EXT`
  (or `GENERAL`); VK's barrier `oldLayout` must match what `glInteropSignalToVk(glDstLayout, parity)`
  hands over. For a **sampled** depth-in, target `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (or
  `DEPTH_STENCIL_READ_ONLY_OPTIMAL` if using the DS aspect), NOT `TRANSFER_DST` (no copy anymore).
- The current interop path's depth barriers (edge-fog :1353–1357, subgraph :1439–1442) go
  UNDEFINED→TRANSFER_DST→SHADER_READ_ONLY around the copy; P5 **deletes the copy** and replaces these
  with a single acquire barrier `<GL layout>→SHADER_READ_ONLY_OPTIMAL` keyed to the semaphore wait — no
  `UNDEFINED` (that would discard GL's contents). Getting the `oldLayout` right is the #1 validation-sync
  risk; `sync` validation will catch a wrong acquire.
- Depth is **read-only in VK** → no write hazard, no VK→GL export → strictly simpler than the color
  round-trip's R-A-W. The only hazard is the GL-produce → VK-sample ordering, covered by the existing
  `doneSem` GL→VK semaphore (same as color input).

---

## 7. Task breakdown + risks + in-flight conflicts

### Tasks (mirror edge-fog/P2 depth of the color template)
- **T0' (DECISIVE, do first):** D24S8 depth interop spike (extend `main_depth.cpp`) + parameterize
  `vk_interop_transport_test` + `vkInteropSelfTest` to depth. Gate the whole slice on T0' passing.
- **T1:** decide format (D24S8 zero-copy vs blocked) from T0' result.
- **T2 (edge-fog):** add exportable depth `VkImageResource` + `GlInteropView` (depth-aspect view /
  internalformat), created inside the existing `useInterop` branch; repoint the frag's depth sampler at
  the shared depth image. Drop `glGetTexImage(DEPTH)` :1278 + `vkCmdCopyBufferToImage(depth)` :1355 on
  the interop path; replace barrier with a single acquire.
- **T3 (subgraph):** same, at :1351 / :1441; **add depth forbidden-counters** first (subgraph lacks
  them), then assert 0.
- **T4:** forbidden-counter hard-asserts (depth==0 both regions) + health-line fields + fail-soft LOUD
  log. `MC2_VK_INTEROP_DEPTH_FORCE_READBACK` debug escape.
- **T5:** offline transport harness green → golden-parity (edge bookmarks, both regions), STOP-gate.
- **T6:** docs (`tier1_env_vars.md` — update the "DEPTH still round-trips" clause on both gates),
  gate-registry / allowlist re-check.

### Risks (ranked)
1. **D24S8 external export unsupported on AMD** (HIGH, decisive) — T0 only proved D32F. If the spike
   fails, P5 is blocked. This is the whole risk of the slice; everything else is mechanical.
2. **Depth-stencil aspect view for sampling** (MED) — `aspectFor(D24S8)` returns DEPTH|STENCIL; a sampled
   view needs DEPTH-only. Needs the small `viewAspectOverride` primitive addition (§3).
3. **Acquire-layout / sync validation** (MED) — wrong `oldLayout` on the depth acquire → `sync`
   validation error or garbage depth. Mitigated by `sync` validation in the oracle.
4. **Subgraph lacks depth counters** (LOW) — add them; trivial but must not be forgotten or the oracle
   is vacuous.

### Conflicts with in-flight P2 / P3
- **P5 touches BOTH the edge-fog island AND the subgraph depth paths.** P2 (subgraph color interop,
  `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP`) has **committed changes in the same file**
  (`vulkan_postprocess_subgraph.cpp`) and the same `useInterop` branch P5 extends. **Coordinate: P5 must
  rebase on P2's landed subgraph interop** before touching subgraph depth — do NOT start subgraph-depth
  until P2's color interop is committed to avoid a merge on the interop branch structure. The edge-fog
  half of P5 has no P2 dependency and can proceed independently first.
- **P3 (SSAO, `MC2_VK_SSAO_INTEROP`)** is a *different* island (`vulkan_ssao_island.cpp`) with its OWN
  device; no file overlap with P5. But P3's recon (`VK-P3-SSAO-RECON.md`) explicitly flags that "depth
  readback becomes the bottleneck once color is zero-copy" and names P5 as its natural follow — SSAO
  reads full-res depth. **If SSAO later wants the SAME shared depth image, P5's depth `VkImageResource`
  should be built as the shared `sceneDepthTex_` bridge the roadmap describes (the thing P4 HZB also
  consumes as level-0), not a per-island private depth.** For P5 scope, keep it per-region (edge-fog +
  subgraph) but design the depth resource so it's promotable to a single shared `sceneDepthTex_` VK image
  later (P4/HZB consumer). Note this as a forward-design constraint, not P5 work.
- **P4 (HZB)** consumes P5's shared depth as HZB level-0 (`VK-P4-HZB-RECON.md`). P5 building a clean
  shared depth image is the prerequisite; keep the format decision (§2) compatible with what HZB samples
  (HZB is D32 depth-in per P4 recon — another reason the D24S8-vs-D32 question is arc-wide, not local).
