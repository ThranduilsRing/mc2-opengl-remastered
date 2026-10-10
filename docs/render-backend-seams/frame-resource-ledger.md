<!-- doc-status: current | reviewed:2026-10-11 -->
# Frame Resource Ledger — MRT layout + draw-buffer / attachment ownership

**Audit class:** `FRAME_RESOURCE_LEDGER`. Source-verified 2026-06-24 against nifty
`ff3d0706` (worktree `mc2-colormask-rollout-1`). All file:line are
`GameOS/gameos/gos_postprocess.cpp` unless noted.

This is the **MRT / draw-buffer ownership** half of the audit. The **sample-safe
mid-frame copy** half is tracked by a sibling lane (VFX) in
[`.claude/FRAME-RESOURCE-LEDGER-1.md`](../../.claude/FRAME-RESOURCE-LEDGER-1.md) —
that doc owns `sceneDepthCopyTex_` / `sceneColorCopyTex_`; this doc does not
restate or supersede it. Cross-referenced below under "Mid-frame copies".

## Scene MRT framebuffer (`sceneFBO_`)

FBO setup `createFBOs()` (`:821`–`:908`).

| Attachment | Texture | Format | Created | Attached | Role |
|---|---|---|---|---|---|
| COLOR0 | `sceneColorTex_` | `RGBA16F` | `:830` | `:835` | scene HDR color |
| COLOR1 | `sceneNormalTex_` | `RGBA16F` | `:879` | `:884` | world normals (rgb) + shadow-skip flag (a) |
| COLOR2 | `sceneObjectIdTex_` | `R32UI` | `:894` | `:900` | object IDs — **gated `MC2_OBJECT_ID_BUFFER`** |
| DEPTH_STENCIL | `sceneDepthTex_` | `DEPTH24_STENCIL8` | `:868` | `:873` | immutable `glTexStorage2D`, sampleable for reconstruct |

## Draw-buffer ownership — the chokepoint

**Every `glDrawBuffers` against `sceneFBO_` routes through one helper:**
`setSceneDrawBuffers(mode, objectIdAttachmentReady)` (`:221`–`:246`).

| Mode | Draw set | Used by |
|---|---|---|
| `MainSceneMRT` (oid ON) | `{COLOR0, COLOR1, COLOR2}` | scene geometry / terrain / mech draw |
| `MainSceneMRT` (oid OFF) | `{COLOR0, COLOR1}` | same, when `MC2_OBJECT_ID_BUFFER` off / FBO not ready |
| `SingleColor` | `{COLOR0}` | all post-fx composites |

`glClearBufferuiv(GL_COLOR,2,…)` at frame-entry is only safe after a
`MainSceneMRT,true` bind installed the 3-entry list (`:215`–`:216`).

**Post-fx SingleColor sites** (all bind `{COLOR0}` only, so COLOR1/COLOR2 are NOT
draw-targeted during post-fx): `drawBoxDecals` `:1310`, `runSSAO` `:1948`,
`runScreenShadow` `:2009`, `runCloudShadow` `:2155`, `runShoreline` `:2210`,
`runEdgeFog` `:2262`, `runFogOob` `:2318`.

### Relationship to COLORMASK-ROLLOUT-POSTFX-1
The post-fx family writes `{COLOR0}` *by draw-buffer set already* — COLOR1/COLOR2
cannot be written regardless of colorMask. That is exactly why opting those rows
into `colorMask {true,false,false}` ownership (commit `e28a982d`) is a proven
visual no-op (A/B byte-identical): the colorMask is redundant belt-and-suspenders
over the draw-buffer set. Two independent mechanisms enforce the same GBuffer
protection; neither alone is load-bearing for these passes. (The colorMask is the
one that also covers a pass that forgets to call `setSceneDrawBuffers`.)

## Draw-buffer leak analysis — NO cross-frame leak

| Site class | file:line | Verdict |
|---|---|---|
| Scene MRT bind | `setSceneDrawBuffers` | re-established every `beginScene`; SingleColor left at endScene is abandoned at FBO=0 — no leak |
| Shadow FBO init ×4 | `:3347 :3674 :3755 :3806` | each unbinds FBO immediately after — no leak |
| Dynamic shadow cascade | `:3890 :3896` (`beginDynamicShadowCascade`) | unpaired BUT intentional — caller switches FBO per cascade |
| VFX ribbon flush | `gos_particle_bridge.cpp:745`→`754`→`877` | save→set `{COLOR0}`→restore full MRT — paired, safe |

Same shape as the colorMask leak class: a *set-only* draw-buffer change is healed
because `beginScene` re-binds the MRT before the next frame's first scene draw.

## Secondary FBOs

| FBO | Color | Depth | Notes | file:line |
|---|---|---|---|---|
| `shadowFBO_` | `shadowDummyColorTex_` `R8` | `shadowDepthTex_` `DEPTH_COMPONENT24` | forward-Z; dummy color = AMD raster req | `:3322`–`:3346` |
| `ssaoFBO_` | `ssaoColorTex_` `R16F` (half-res) | none | screen-space AO | `:946`–`:948` |
| `waterReflFBO_` | `waterReflColorTex_` `RGBA16F` (¼-res) | `waterReflDepthTex_` `DEPTH_COMPONENT24` | water reflection | `:981`–`:984` |
| `hzbFBO_` | per-level `hzbLevelTex_[n]` `R32F` | none | Hi-Z pyramid (reverse-Z), gated `MC2_HZB_BUILD` | `:1048` `:1467` |
| `dynamicFullMapFbo_` | `dynamicFullMapDummyColorTex_` `R8` | `dynamicFullMapTex_` `DEPTH_COMPONENT24` | CSM last cascade separate full-map | `:3804`–`:3805` |

## Mid-frame copies (owned by sibling ledger — do not duplicate)

`sceneDepthCopyTex_` (`DEPTH24_STENCIL8`, copy `:1084`) and `sceneColorCopyTex_`
(`RGBA16F`, copy `:1116`) are the sample-safe snapshots — see
[`.claude/FRAME-RESOURCE-LEDGER-1.md`](../../.claude/FRAME-RESOURCE-LEDGER-1.md).
Both `glCopyImageSubData`, lazy-alloc, immutable, freed+recreated on resize.

## Framebuffer invalidation

**None.** No `glInvalidateFramebuffer` / `glInvalidateSubFramebuffer` anywhere in
`gos_postprocess.cpp`. Frame-entry uses `glClear` (color/depth) +
`glClearBufferuiv` (COLOR2 objectId). The colorMask keystone
`glColorMask(GL_TRUE×4)` at `:1389` (COLORMASK-ROLLOUT-1) resets indexed masks
before clear. A tiled/mobile-GPU invalidation pass is a possible future seam (no
correctness need on desktop today).

## Open follow-ups (none blocking)

- `MC2_OBJECT_ID_BUFFER`-OFF path uses a 2-entry MRT — picking/objectId consumers
  must tolerate COLOR2 absent (already gated; not re-audited here).
- Invalidation hints (above) — perf-only, deferred.
