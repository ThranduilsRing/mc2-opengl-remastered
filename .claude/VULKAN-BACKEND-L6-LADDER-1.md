# VULKAN-BACKEND-L6-LADDER-1 — road from region-iface entry to a real Vulkan backend

RECON + PLAN. Read-only inputs, no source touched. Worktree `A:/Games/mc2-vulkan-backend`
(`claude/vulkan-backend-l6`, base `c18618c4`). Supervised sustained lane — user reviews
between slices. This doc is the north-star ladder; the first 2 slices are fully specified so
builders can start on the supervisor's go.

Inputs read (verified this session): HANDOFF_2026_07_01_vulkan_lib_track,
`docs/render-backend-seams/{vulkan-island-template,vulkan-readiness-audit-1,postprocess-subgraph-recon-1,spirv-postprocess-family-1,spirv-consumer-pilot-recon-1}.md`,
root `.claude/{RENDER-BACKEND-BOUNDARY-RECON-1,VULKAN-DESCRIPTOR-INVENTORY-1,VULKAN-BARRIER-PLAN-RECON-1}.md`,
commits `1c1bdc38` (region-iface), `805d772f` (postprocess-vk opt-in), `85182be1`
(backend-compare-harness), `RenderCore/{RenderRegionId,IRenderBackend}.h`.

---

## 0. Current-state verdict

**Layers 1–5 COMPLETE + Layer-6 ENTRY landed and is architecturally right.** The seam is
built exactly as the boundary recon prescribed:

- `IRenderBackend::runRegion(RenderRegionId, ctx, out)` is the single generalizable entry.
  Context is **backend-neutral** (`PostprocessFogRegionContext` names resources by
  `RenderResourceId`, carries `FogParams`+dims, **no raw GLuint/VkImage**), and an
  `IResourceResolver` (owned by `gosPostProcess`) translates id→handle per frame. This is the
  correct shape and it must be preserved: **new regions add an enum value + a context struct +
  a resolver mapping, nothing else structural.**
- Selection is layered and proven: master gate `MC2_RENDER_BACKEND_REGION_IFACE` (default OFF
  → original direct-GL, byte-identical); then `MC2_POSTPROCESS_BACKEND=gl|vk` (default gl).
  `=vk` is now a **true opt-in** that runs the Vulkan subgraph directly and **fail-soft** to
  `FallbackGL` on any device/init failure. Vulkan code compiles only under `#ifdef
  MC2_VULKAN_ISLAND` → shipping `build64` is byte-identical.
- `BACKEND-COMPARE-HARNESS-1` formalizes the oracle: `--backend-a gl --backend-b vk --region
  PostprocessFog`, GL-vs-GL noise floor, **FallbackGL detection = exit 5 INCONCLUSIVE** (a
  fallback can never masquerade as proven parity). Self-tests green.

**The three recon capstones (descriptor inventory, barrier plan, boundary) are done and
authoritative** — the port design is evidence-based, not guesswork. What remains is *execution*
plus closing two known hazards before the resources they gate are ported.

**Where we are on the 8-layer north-star ladder:** L1–L5 done; **L6 = backend parity in
progress (one region live: PostprocessFog).** This doc details L6→L8.

---

## 1. Which regions convert next, and why (order = risk-ascending, dependency-honest)

Ordering axis: **self-containment + descriptor simplicity + parity-provability**, cheapest
first. The barrier plan (`VULKAN-BARRIER-PLAN-RECON-1`) and descriptor inventory
(`VULKAN-DESCRIPTOR-INVENTORY-1`) fix the difficulty ranking:

1. **PostprocessFog** — DONE (L6 entry). Fullscreen, no SSBO/material, no vertex input,
   reverse-Z depth sample only, contiguous subgraph. The template region.
2. **Postprocess-family fullscreen passes** (SSAO, CloudShadow, Shoreline, HZB-reduce, EdgeFog,
   FogOob already fused) — same shape as fog: `postprocess.vert` + one frag, COMBINED_IMAGE_SAMPLER
   inputs, single color output, **zero SSBO/material bindings**. SPIR-V for 4 of these already
   ships byte-exact (`spirv-postprocess-family-1`). **Cloud + Shoreline are FBM-noise → NOT
   byte-exact under two-compiler** — they need the perceptual-tolerance gate (see §5, §6-open).
   This family is the natural L6 body: grow `RenderRegionId` one fullscreen pass at a time,
   each behind `MC2_POSTPROCESS_BACKEND=vk`, each backend-compared.
3. **Composite / backbuffer edge** — the FBO-0 output. Already contract-declares
   `colorFinalLayout=Present`. Converting this couples the region path to the engine-owned
   swapchain (L5) → first "Vulkan writes the actual presented image" milestone. Medium risk
   (present integration), small surface (one fullscreen blit).
4. **Shadow pass** — depth-only, but introduces the **compare-sampler** (`VK_COMPARE_OP_LESS`,
   Poisson PCF), the CSM **2D_ARRAY** hard case, and per-frame instance/bone ring SSBOs. First
   region with real vertex geometry + rings. High descriptor cost.
5. **Terrain solid** — tessellation/compute-driven, indirect draw, terrain-lighting
   compute→draw SSBO barrier, the **SSBO:2 collision** (open hazard #2) lives here. Highest
   descriptor + barrier complexity; the recon flags it "port last among geometry."
6. **Water** — depth-fudge lockstep, cross-frame reflection RT (External lifetime), thin-record
   ring. Order-sensitive; depends on terrain being solid first.
7. **StaticProp + Mech opaque** — MRT G-buffer (MainColor+MainNormal+SceneObjectId),
   ViewUniforms UBO ring, material-table SSBOs. **Depth-EQUAL commutativity hazard**
   (StaticProp↔Mech, ORACLE-STRENGTHEN-1) means pass order must be preserved, not a barrier.
8. **UI / 2D blit** — legacy `gl_utils.cpp` immediate-mode; **LAST**. Confirm liveness first
   (likely debug-only; delete-not-port candidate per readiness audit dim G).

Guiding rule (from boundary recon, keep it hard): **no pass logic in backend files; no GL/VK
calls in agnostic files.** Every region routes through `runRegion` with a neutral context.

---

## 2. The 2 OPEN hazards — fix order + plan

Both are **prerequisites gated by the region that first exercises them**, not blanket blockers.

**H1 — island readback READ_AFTER_WRITE (`vkCmdCopyImageToBuffer` under sync-validation).**
Latent today (parity byte-identical, the fence covers it) but sync-validation flags it. **Fix
FIRST and cheaply** — it is the smallest slice, it hardens the harness oracle every later
region relies on, and it is confined to the island readback path (EdgeFog/OOB islands +
subgraph). Add a `VkImageMemoryBarrier` (`COLOR_ATTACHMENT_OUTPUT→TRANSFER` :
`COLOR_ATTACHMENT_WRITE→TRANSFER_READ`, layout `COLOR_ATTACHMENT_OPTIMAL→TRANSFER_SRC_OPTIMAL`)
before the copy-to-buffer. Acceptance: sync-validation errors 0, all 6 fog bookmarks still
byte-identical. **This is Slice 1 (see §7).** Note: readback is the *oracle*, not production
(§5) — but the harness must be sync-clean to be trusted for every subsequent region.

**H2 — SSBO:2 terrain-mask-solid collision (LightingBuf@2 vs ThinRecordBufFrag@2, same
pipeline).** A real same-pipeline binding collision — two logical resources on binding 2 in one
program. Benign in GL (disjoint use), a **hard blocker for the Vulkan terrain region**
(descriptor sets require distinct `(set,binding)`). **Fix JUST BEFORE the terrain region
(ladder step 5), not now** — it is a GL-side binding-slot split (assign the two resources
distinct bindings + update the two shaders + `check-binding-slots.py`), provable byte-identical
in pure GL with the existing golden oracle, and doing it early risks bit-rot against concurrent
terrain-v2 work on other branches. Same treatment for the related slot-14 (cull-readback vs
particle) and slot-20 (LightData vs SurfaceVertexBuf) multi-binders when their regions arrive.
Descriptor inventory §"hard cases" is the authoritative untangle list.

**Fix order: H1 now (Slice 1) → H2 at terrain (step 5).** Binding-20 split judged benign
(disjoint pipelines) — leave it; only split when the Vulkan geometry region actually shares a
descriptor set across those passes.

---

## 3. Resource ownership — is GPU-BUFFER-OWNER a per-region prerequisite?

**Yes, per-region and incremental — not a blanket prerequisite arc.** Vulkan needs every
resource a region touches to be an **owned, registered handle** (so the resolver can map
id→`VkBuffer`/`VkImage`). Today ~70% of GPU resources are raw file-static `GLuint` globals
(`s_heightSsbo`, `g_recipeSSBO`, `s_mechMaterialSsbo`, rings, batcher instance SSBOs, view
UBO). The right sequencing:

- **Postprocess family (steps 1–3) needs almost NO ownership work** — its inputs are FBO-backed
  images (`MainColor`/`MainDepth`/`MainNormal`) already carrying `RenderResourceId`, resolved
  through the existing resolver. This is why postprocess is the correct L6 body: it exercises
  the backend seam without dragging in the buffer-owner arc.
- **Geometry regions (steps 4–7) each require their SSBOs to be owned+registered BEFORE they
  can port.** Make GPU-BUFFER-OWNER a **just-in-time dependency of each geometry region**, not
  a big-bang migration: e.g. Shadow needs mech/prop instance+bone rings owned; Terrain needs
  height/type/cement + recipe/thin owned; Mech/StaticProp need ViewUniforms UBO + material
  tables owned. The `GpuBufferOwner` handle-split template is already correct — the boundary
  recon's recommendation stands: **rename `glName:uint32_t`→`backendHandle` (widen to 64-bit
  for `VkBuffer`), keep the record GL-free.** Do that rename as a standalone prep slice early
  (it is mechanical + byte-identical) so every later region inherits it.

Cheap early ownership wins (from MEMORY GPU-BUFFER-OWNER-RECON): view-UBO (1 site) +
LOD-chunk type/cement SSBOs. Fold these into the geometry-region prep slices that need them.

---

## 4. Interop cost while mixed GL/VK — readback bridge vs shared images

Today's islands use **CPU-readback bridges** (`glGetTexImage`→staging→VK→`glTexSubImage2D`) —
the H1 hazard. This is the **validation oracle, not production** (island template §5): it
stalls both pipelines (~59 fps vs 79 GL measured). Strategy for the mixed era:

- **Keep readback bridges ONLY for offscreen parity-proof islands.** Never ship a readback in a
  production region path.
- **Production interop = the engine-owned swapchain (L5 done) + eventually shared images.**
  Once the Composite/backbuffer edge (step 3) routes through Vulkan-owned present, the
  postprocess family can render into a **Vulkan-owned intermediate** (the subgraph already does
  this — fused fog = 1 render pass, Vulkan-owned color, no internal readback) and present
  directly, avoiding the round-trip. The subgraph pattern is the production template; the
  island readback is the oracle template.
- **GL↔VK zero-copy shared memory (external semaphores / `GL_EXT_memory_object`) is deferred to
  L7** — only needed when a Vulkan region must feed a still-GL region *within the same frame*
  (mixed frame). Avoid it as long as regions convert in contiguous blocks (postprocess → then
  present → then geometry). The barrier plan already enumerates the intra-Vulkan barriers each
  region needs once it is native; the R-A-W (H1) is the first of these.

**Recommendation:** treat "mixed frame with a GL region downstream of a VK region" as the
thing to *avoid scheduling* until L7 — convert regions so the Vulkan block sits at the frame
boundary (postprocess+present), growing backward into geometry, so no VK→GL in-frame handoff is
needed and no shared-image interop is required before the full-frame milestone.

---

## 5. Shader porting strategy — recommendation

**Hand-mirrored `.frag` pairs (today's `shaders/vulkan/*.frag`) do NOT scale — retire them in
favor of the offline-bake-from-one-source path that already exists.** Evidence:

- `OFFLINE-SHADER-VARIANT-BUILD-1` shipped: a glslang `→ .spv` step compiling **without**
  `--auto-map-bindings` (preserving explicit `layout(binding=)`), one `.spv` per (shader,
  define-set) keyed by `shaderVariantId = hash{base + sorted-define-set}`, deployed under
  `shaders/spv/`.
- `SPIRV-POSTPROCESS-FAMILY-1` shipped 4 postprocess members (ssao, fog_oob, edge_fog,
  hzb_reduce) **byte-exact GL-vs-SPIR-V** — proving the single-source model works for
  numerically-stable shaders.
- The runtime consumer seam is proven (`SPIRV-CONSUMER-PILOT`): `glShaderBinary` +
  `glSpecializeShader` behind `GLEW_ARB_gl_spirv`, GLSL hot-reload fallback intact by
  construction.

**⚠ WAVE-3 CORRECTION (VK-REGION-SSAO-1 finding, verified via spirv-dis): the premise
"the Vulkan backend consumes the SAME `.spv` artifacts the SPIR-V-GL consumer does" is
WRONG as stated for the artifacts that exist today.** The current `shaders/spv/*.spv`
bakes are compiled from GLSL sources that carry NO explicit `layout(binding=)` and keep
loose default-block uniforms, so:

1. **glslang free-numbers the samplers** — `ssao.frag.default.*.spv` decorates BOTH
   `sceneDepthTex` and `sceneNormalTex` to `(set=0,binding=0)` (verified spirv-dis: two
   `OpDecorate … Binding 0` on distinct sampler ids). That is legal for the GL
   `glShaderBinary`/`glSpecializeShader` consumer (which resolves samplers by reflection,
   not descriptor binding) but a **genuine descriptor collision** for any Vulkan pipeline.
2. **Loose default-block uniforms cannot exist in Vulkan-target SPIR-V at all** — Vulkan
   requires every uniform inside a block (UBO / push constant). The GL-consumer bakes
   target `opengl`, not `vulkan`; they are not even the right target environment.

SSAO was therefore hand-ported (`shaders/vulkan/ssao{,_apply}.{vert,frag}`, same
discipline as edge_fog/fog_oob). Hand-mirroring is the **stopgap, not the strategy** —
the scalable fix is to push an explicit-binding discipline INTO the single GLSL source +
the offline glslang bake (see §5a), so future regions consume bakes with zero
hand-porting. `#define`-permutations become `constant_id` specialization constants where
they size arrays (e.g. `MC2_SHADOW_CSM_MAX`). **spirv-cross is NOT needed** (we own the
GLSL source; we do not reverse-engineer). Two caveats to encode as rules:

1. **FBM-noise shaders (cloud, shoreline) are NOT byte-exact** across the driver-GLSL vs
   glslang-SPIR-V compilers (ULP drift through `smoothstep` edges). They require a
   **perceptual-tolerance visual gate (SSIM/threshold)**, not the byte-exact sha256 oracle —
   this is the one net-new proof tool the ladder needs (see §6-open). Until it exists, cloud +
   shoreline stay on GL even when their neighbors go Vulkan.
2. Migrate the fog islands' hand-written `shaders/vulkan/edge_fog.{vert,frag}` onto the baked
   `edge_fog.frag.spv` artifact as a consolidation slice, proving the two sources produce
   byte-identical fog output — retires the fork. (Now conditional on the §5a bake plan:
   the artifact must first be re-baked with explicit bindings + params-UBO form; L6.5
   retires the whole `shaders/vulkan/{edge_fog,fog_oob,ssao,ssao_apply}` hand-mirror set
   together.)

### 5a. Postprocess-region family set/binding convention (NORMATIVE — wave-3 amendment)

One convention **per region family**. The postprocess-family standard below is exactly
what `vulkan_postprocess_subgraph.cpp`, `vulkan_ssao_island.cpp`, and every hand-ported
`shaders/vulkan/*.frag` ALREADY expect — future offline bakes must decorate to it, and
new fullscreen regions must not invent their own:

| set | binding | resource | rule |
|---|---|---|---|
| 0 | `0 … N-1` | `sampler2D` / `usampler2D` (COMBINED_IMAGE_SAMPLER) | one per GL sampler uniform of the pass, in **GL texture-unit slot order**: unit 0 → binding 0, unit 1 → binding 1, … (N = the pass's sampler count) |
| 0 | `N` | region params UBO — `std140`; **`row_major` qualifier mandatory** when the block holds mat4s the GL path uploads row-major/`GL_FALSE` (the 2b bug) | ALL loose GL uniforms of the pass gathered into ONE block, member order = GL declaration order, byte offsets mirrored in a CPU POD with `static_assert(sizeof==…)` |
| — | — | push constants | NOT used by this family (zero push-constant ranges; per-draw variation goes through the UBO) |
| — | — | vertex input | none — fullscreen triangle from `gl_VertexIndex`; interface is `location=0 vec2 TexCoord` only |

Conforming instances (audited 2026-07-01): `edge_fog` (1 sampler → UBO@1), `fog_oob`
(1 sampler → UBO@1), `ssao` (2 samplers → UBO@2), `ssao_apply` (1 sampler → UBO@1).
`scripts/check-vulkan-bindings.py` is the enforcement gate — extend its expectations as
each region lands.

**Variants:** `#define`s that only size arrays or select constant branches (e.g.
`MC2_SHADOW_CSM_MAX`) become `layout(constant_id=K)` specialization constants — K
allocated per shader starting at 0, documented in the shader header. Semantics-changing
define-sets keep one `.spv` per set keyed by `shaderVariantId`
(hash{base + sorted-define-set}) exactly as OFFLINE-SHADER-VARIANT-BUILD-1 does.

**Bake plan (replaces the "reuse the existing GL bake" premise):** ONE GLSL source per
pass, TWO bake targets from that same source — `--target-env opengl` for the GL SPIR-V
consumer, `--target-env vulkan1.1` for the backend. Precondition, folded into each
region's conversion slice (not a big-bang): the GL source gains (a) explicit
`layout(binding=)` on samplers per the table, (b) its loose uniforms gathered into the
params UBO. That is a GL-side change (glUniform* upload → UBO upload) proven
byte-identical by the golden oracle at conversion time; until a region converts, its GL
source stays untouched. Geometry-region families (Shadow / Terrain / Mech opaque) get
their OWN convention table (per-frame view UBO set, material+instance SSBO set, texture
set) specced at the L7.2 recon — do NOT stretch this postprocess table over them.

---

## 6. Milestone ladder (L6 → L8), one line each

Each region slice = opt-in behind `MC2_POSTPROCESS_BACKEND=vk`-style selection + fail-soft +
the two-layer proof (synthetic `.spv`-vs-CPU fixture + golden backend-compare) + the island
template checklist. `size` = S/M/L; `risk` L/M/H; `proof` = harness; `gate` = env selector.

| # | Milestone / slice | size | risk | proof method | gate |
|---|---|---|---|---|---|
| L6.0 | **island readback R-A-W barrier (H1)** | S | L | sync-validation 0 + 6 fog bookmarks byte-identical | (fixes existing island path) |
| L6.1 | **backendHandle rename** (`GpuBufferOwner.glName`→64-bit `backendHandle`) | S | L | build byte-identical, unit suite green | none (mechanical) |
| L6.2 | **SSAO region** through runRegion (fullscreen, no SSBO) — **LANDED `af5f9cdc`** (hand-ported shaders per §5 correction; deploy spv gap fixed `689623a9`; acceptance debt = synthetic fixture + tier1 re-run, closed in wave 3) | M | L | synthetic fixture + backend-compare `--region PostprocessSSAO` (+`MC2_SSAO=1`) | `MC2_POSTPROCESS_BACKEND=vk` |
| L6.3 | **CloudShadow + Shoreline regions** (FBM) + **perceptual-tolerance gate** | M | M | new SSIM harness (byte-exact impossible) | vk + tolerance policy |
| L6.4 | **HZB-reduce region** (per-mip intra-pass barriers, B13) | M | M | fixture + compare; per-mip barrier enumerated | vk |
| L6.5 | **fold fog+ssao `.frag` forks onto explicit-binding baked `.spv`** (retire hand-mirror; requires §5a bake plan) | S | L | fog/ssao output byte-identical GL vs baked-spv | none |
| L6.6 | **Composite/backbuffer region → Vulkan-owned present** (couples to L5 swapchain) | L | M | present 16 frames, validation 0, backend-compare | vk |
| L7.1 | **SSBO:2 / slot-14 / slot-20 binding split (H2)** — GL-side, pre-terrain | M | M | golden byte-identical (pure GL) + check-binding-slots | none |
| L7.2 | **Shadow region** (compare-sampler + CSM 2D_ARRAY + instance/bone rings owned) | L | H | fixture (compare-sampler) + backend-compare shadow bookmarks | vk-shadow |
| L7.3 | **Terrain solid region** (indirect + terrain-lighting compute→draw barrier) | L | H | compare + compute→draw barrier proof | vk-terrain |
| L7.4 | **Water region** (depth-fudge lockstep + cross-frame reflection) | L | H | water depth bookmarks + reflection parity | vk-water |
| L7.5 | **StaticProp + Mech opaque** (MRT G-buffer, ViewUniforms ring, depth-EQUAL order-preserve) | L | H | MRT parity, order-preserve assertion (no reorder) | vk-geo |
| L7.6 | **descriptor-set consolidation** (retire per-pass slot multiplexing → set0/1/2/pre-pass) | L | H | validation-layer clean, all regions still parity | — |
| L8.0 | **UI / gl_utils 2D** (confirm-live-first, likely delete) | S | L | UI bookmarks / dead-code proof | vk-ui |
| L8.1 | **default-flip** MC2_POSTPROCESS_BACKEND=vk default, full-frame Vulkan soak | M | H | 30s soak all tier1 + backend-compare full-frame | flip default |

**"Full Vulkan frame" milestone definition (what remains after L5's engine-owned present):**
every region in `kFramePassOrder` (Shadow→StaticProp→Mech→Terrain→Overlay→Decal→Water→Veg→VFX→
UI→PostProcess) runs through a Vulkan `runRegion` impl **within one Vulkan command
buffer**, no CPU readback bridge in any production path, all inter-pass edges expressed as
`VkPipelineBarrier`/render-pass dependencies (the barrier plan's B1–B17), present via the
engine-owned swapchain. Reached at **L8.1** (default-flip). L5 gave us present; L6–L7 give us
the regions; L8 fuses + flips.

---

## 7. First 2 slices — fully specified (builder-ready on supervisor's go)

### Slice 1 — `VK-ISLAND-READBACK-BARRIER-1` (closes open hazard H1)

**Goal:** make the island/subgraph readback sync-validation-clean by adding the missing
`COLOR_ATTACHMENT→TRANSFER_READ` image barrier before `vkCmdCopyImageToBuffer`. Hardens the
oracle every later region depends on. No behavior change (parity already byte-identical; the
fence already makes it correct — this satisfies the *validator*).

**Files (worktree `A:/Games/mc2-vulkan-backend`, all under `#ifdef MC2_VULKAN_ISLAND`):**
- `GameOS/gameos/vulkan_edge_fog_island.cpp` — before the `vkCmdCopyImageToBuffer` in the
  island render, insert a `VkImageMemoryBarrier`: `srcStageMask=COLOR_ATTACHMENT_OUTPUT`,
  `dstStageMask=TRANSFER`, `srcAccessMask=COLOR_ATTACHMENT_WRITE`,
  `dstAccessMask=TRANSFER_READ`, `oldLayout=COLOR_ATTACHMENT_OPTIMAL`,
  `newLayout=TRANSFER_SRC_OPTIMAL`, on the render-target color image.
- `GameOS/gameos/vulkan_oob_fog_island.cpp` — same barrier (mirror island).
- `GameOS/gameos/vulkan_postprocess_subgraph.cpp` — same barrier before its
  copy-to-buffer if the subgraph's readback path (oracle mode) does one; if the native subgraph
  keeps the color Vulkan-owned with no readback, confirm + add a code comment that no barrier is
  needed there (only the island oracle path reads back).
- (No RenderCore change — barriers are backend-internal.)

**Build:** `build64_island` (`MC2_VULKAN_ISLAND=ON`) only; shipping `build64` untouched.
**Acceptance:**
1. `scripts/run_vulkan_probe.py --validation sync` → **sync validation_errors=0** (was: R-A-W
   flagged).
2. `scripts/run_golden_parity.py <scene> MC2_VULKAN_EDGE_FOG_ISLAND` and `…_OOB_FOG_ISLAND` —
   all bookmarks still **byte-identical** (the barrier is a no-op for correctness).
3. `run_golden_parity.py --backend-compare --region PostprocessFog` still byte-identical /
   non-fallback where the subgraph runs.
4. `build64` default build byte-identical (exe_md5 unchanged) — trivially true (island-only).
**Size:** S (≤3 files, ~10 lines each). **Risk:** L. **Gate:** none new (fixes island path).
**Preflight:** `repo_query.py slice-preflight --slice VK-ISLAND-READBACK-BARRIER-1 --symbols
"runEdgeFogVulkan,runFogOobVulkan" --paths GameOS/gameos/vulkan_edge_fog_island.cpp
GameOS/gameos/vulkan_oob_fog_island.cpp --base c18618c4`.

### Slice 2 — `VK-REGION-SSAO-1` (first NEW region through `runRegion`)

**Goal:** prove a *second* region routes through the backend seam — SSAO, the next-simplest
fullscreen postprocess pass (reads `MainDepth`+`MainNormal`, writes `MainColor` via a half-res
`SsaoOcclusion` intermediate; **no SSBO/material bindings**). Establishes the
grow-one-region-at-a-time pattern beyond fog.

**Files:**
- `RenderCore/RenderRegionId.h` — add `RenderRegionId::PostprocessSSAO` enum value; add a
  backend-neutral `PostprocessSSAORegionContext` (RenderResourceId depth/normal/color + dims +
  SSAO params: radius/bias/intensity/kernel — the same numeric content the GL
  `runSSAO()` uploads; NO handles) + `region_id_to_string` case. Keep GL-free.
- `RenderCore/IRenderBackend.h` — `runRegion` already takes an id; if the context type must be
  distinct per region, either add an overload or (preferred, minimal) a small tagged-union /
  second `runRegion(RenderRegionId, const PostprocessSSAORegionContext&, RegionOutput*)`
  following the exact fog precedent. Decide with supervisor (open ruling R3).
- `GameOS/gameos/gos_postprocess.cpp` — (1A) extract inline `runSSAO()` into
  `runSSAORegionGL()` pure code-move (byte-identical); (1B) route through
  `runSSAORegionThroughBackend()` under `MC2_RENDER_BACKEND_REGION_IFACE`, selected by
  `MC2_POSTPROCESS_BACKEND`; (1C) Vulkan impl under `#ifdef MC2_VULKAN_ISLAND` — a new
  `vulkan_ssao_island.cpp` following `vulkan-island-template.md` (SSAO is two internal passes:
  occlusion→apply; use one Vulkan-owned intermediate for `SsaoOcclusion`, sequential draws, the
  SSAO intra-pass barrier B14). Resolver already maps MainDepth/MainNormal/MainColor.
- `GameOS/gameos/vulkan_ssao_island.cpp` — NEW (island TU, whole file `#ifdef
  MC2_VULKAN_ISLAND`). Ports `shaders/ssao.frag` (already baked to `.spv` in the SPIR-V family —
  **reuse the baked artifact, do not hand-mirror**). Health struct + fail-soft per template.
- `shaders/vulkan/ssao.{vert,frag}` — NOT created; consume the existing baked
  `shaders/spv/ssao.frag.spv` (§5 recommendation — this slice is also the first proof that a
  region consumes the shared bake rather than a hand-mirrored pair).
- `GameOS/gameos/debug_state_dump.cpp` — extend the `render_backend_region` health block to
  carry the SSAO region's impl/fallback (or generalize to per-region rows).
- `CMakeLists.txt` + `GameOS/gameos/CMakeLists.txt` — add `MC2_VULKAN_SSAO_ISLAND`-style
  wiring OR fold under existing `MC2_VULKAN_ISLAND` (prefer folding — one island option).
  Apply the ODR discipline (template §1): the region enum lives in GL-free RenderCore so no
  layout-macro skew, but if any `gosPostProcess` member is added under the island macro,
  propagate to `gameos_main`+`mc2`.
- `scripts/run_golden_parity.py` — register `--region PostprocessSSAO` in the backend-compare
  region map.
- `docs/tier1_env_vars.md` — register the SSAO region selection (SSAO default-OFF: the golden
  scene must force `MC2_SSAO=1` so the region actually runs and is in-frame).

**Build:** `build64` (GL, byte-identical, SSAO GL path unperturbed) AND `build64_island`.
**Acceptance (island template §8 checklist):**
1. **BYTE-IDENTICAL OFF** — `build64` exe_md5 unchanged; `MC2_RENDER_BACKEND_REGION_IFACE=0`
   SSAO output byte-identical to baseline.
2. **GL-vs-GL** — `backend=gl` (region-iface ON) vs baseline SSAO byte-identical (pure code-move
   proof, 1A).
3. **SYNTHETIC FIXTURE** — headless `ssao.frag.spv`-vs-CPU fixture: pixels_beyond_tol=0,
   max_abs_diff < 1/1024, validation_errors=0.
4. **BACKEND-COMPARE** — `run_golden_parity.py --backend-compare --region PostprocessSSAO`
   (forcing `MC2_SSAO=1`): GL-vs-VK within-floor for every pixel_sha; **NOT FallbackGL** (exit
   ≠ 5).
5. **VALIDATION CLEAN** — probe with validation sync: 0 errors (uses the Slice-1 barrier
   pattern for the SSAO intermediate).
6. **FALLBACK PROVEN** — force spv-load failure → region_impl=FallbackGL, no crash, output ==
   GL, golden still within-floor.
7. **tier1 all-OFF 5/5** (canonical smoke command, default gates).
**Size:** M. **Risk:** L (fullscreen, no SSBO, baked shader exists). **Gate:**
`MC2_POSTPROCESS_BACKEND=vk` + `MC2_SSAO=1`.
**Preflight:** slice-preflight on `runSSAO` / `gos_postprocess.cpp` / `RenderRegionId.h` base
`c18618c4`; if `runSSAO` changed on base..HEAD, re-recon.

---

## 8. Open rulings for the supervisor

1. **Region-context typing (R3, blocks Slice 2 shape):** each region needs its own neutral
   context struct (fog has `PostprocessFogRegionContext`). Do we (a) add a `runRegion` overload
   per context type, (b) a tagged union / `std::variant` of contexts, or (c) a base
   `RegionContext` + downcast? Recommend **(a) overload per region** — matches the fog precedent,
   keeps each context POD + GL-free, grows one method at a time (same discipline as the op
   list). Confirm before Slice 2.
2. **Island option granularity:** one umbrella `MC2_VULKAN_ISLAND` for all region islands, or
   per-region CMake options (`MC2_VULKAN_SSAO_ISLAND`, …)? Recommend **one umbrella** (fewer
   ODR surfaces, all-or-nothing island build) with per-region *runtime* selection via
   `MC2_POSTPROCESS_BACKEND`. Confirm.
3. **Perceptual-tolerance gate timing (blocks L6.3):** cloud/shoreline are FBM-noise → byte-exact
   impossible. Build the SSIM/threshold visual-diff harness as a dedicated slice **before**
   attempting those two regions, or defer both cloud+shoreline to the very end and keep the
   byte-exact oracle pure for everything else? Recommend **defer** cloud+shoreline (they are
   default-OFF anyway) and keep the byte-exact oracle for all mandatory regions — build the
   perceptual gate only if their Vulkan port becomes a priority.
4. **backendHandle rename scope (L6.1):** rename `glName`→`backendHandle` now (mechanical,
   byte-identical, every later region inherits it) vs defer until the first geometry region
   needs 64-bit handles? Recommend **now** — it is cheap and prevents a churny rename mid-geometry.
5. **GPU-BUFFER-OWNER as JIT-per-region vs a dedicated prep arc:** confirm the §3 recommendation
   to make buffer-ownership a just-in-time dependency of each geometry region (steps 4–7) rather
   than a separate up-front migration. This keeps postprocess (L6) unblocked and defers the big
   ownership work to when a region actually needs it.
6. **Concurrency with terrain-v2:** base `c18618c4` merged terrain-v2 day-1. The SSBO:2 / slot
   splits (H2, L7.1) touch terrain shaders that other branches are actively changing. Confirm we
   fence the terrain region (steps 5, L7.1–L7.3) behind a rebase/coordination checkpoint so the
   binding split doesn't collide with concurrent terrain work.
