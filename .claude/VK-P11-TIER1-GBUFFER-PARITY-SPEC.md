# VK-P11 prop-color TIER-1 (parity) — build spec

**Depends on:** `MC2_VK_SHARED_SCENE_DEPTH` substrate landing (shared D32S8 scene depth). Verified @ `85513044`.
**Size:** L→XL. Not "add an attachment" — flips Tier-0's side-image+composite to IN-PLACE rendering into
shared GBuffer attachments (because GBuffer1 can't be alpha-composited).

## GL prop color MRT (what VK must reproduce bit-adjacent)
`shaders/static_prop.frag:148-154` outputs:
- `location=0` FragColor — lit color → scene `COLOR_ATTACHMENT0 sceneColorTex_` **RGBA16F** (`gos_postprocess.cpp:892-899`)
- `location=1` GBuffer1 — → `COLOR_ATTACHMENT1 sceneNormalTex_` **RGBA16F** (`:963-970`)
- `location=2` objectId `uint` — → `COLOR_ATTACHMENT2 sceneObjectIdTex_` **GL_R32UI**, gated
  `RenderWorld::IsObjectIdBufferEnabled()` / `MC2_OBJECT_ID_BUFFER` (`:977-988`, default-OFF).
- Depth `sceneDepthTex_` DEPTH24_STENCIL8 → shared D32S8 after substrate.
- Draw-buffer set chokepoint `setSceneDrawBuffers()` (`:224-261`): `{0,1}` or `{0,1,2}` when objectId on.

## GBuffer1 encoding = THE parity contract (`shaders/include/render_contract.hglsl`)
RGBA16F: `.rgb = normal*0.5+0.5`, `.a = shadow-mask class`. Prop production path
(`static_prop.frag:497`) = `rc_gbuffer1_screenShadowEligible_noSelfShadow(normalize(v_normal))` →
`vec4(N*0.5+0.5, 0.25)`. ★`a=0.25` = static-prop class (static-map shadow OK, no dynamic-cascade
self-shadow); reader `shadow_screen.frag` gates `rc_isStaticPropNoSelfShadow` (`0.1<a<=0.5`).
★★`v_normal` is **Stuff-space, NOT GL-space** — the vert GL→Stuff swap applies to POSITIONS only; the
GBuffer normal is written un-swapped (only the local PBR block swaps). VK MUST reproduce the un-swapped
normal + exact `0.25` in RGBA16F, or shadow classification + normal-based post passes break.

## Tier-0 → Tier-1 (two axes)
1. **Shared-depth TEST** — drop the VK-owned local D32 (`kDepthFmt` @ island `:169`); import the shared
   scene depth (D32S8) as a VK depth attachment and depth-TEST (GEQUAL reverse-Z far=0 — already matches;
   no convention flip). D32S8↔D32S8 both sides (substrate provides it). Props then occlude vs terrain.
2. **Full MRT out, IN-PLACE** — the crux. GBuffer1 is NOT alpha-blendable → premult-over (Tier-0's
   `glBlendFuncSeparate(ONE,ONE_MINUS_SRC_ALPHA)` @ island `:701`) is WRONG for it. Must make
   `sceneColorTex_` + `sceneNormalTex_` themselves GL↔VK **shared images**, VK renders the prop pass
   depth-tested writing directly into the shared color+normal attachments in-place (no composite), only
   overwriting where it wins the depth test. Renderpass grows 1→2 color attachments (both exportable).
   objectId (att2) = separate follow-on gated on `MC2_OBJECT_ID_BUFFER`.

## Top parity risks
1. **Normal space/encoding** — Stuff-space un-swapped normal + `0.25` alpha in RGBA16F; any half-float
   delta or accidental GL-swap breaks shadow_screen + post passes.
2. **Depth precision at prop/terrain contact** — must be D32↔D32 (not D24 vs D32) or occlusion shimmers
   at the seam (the exact thing Tier-1 fixes).
3. **In-place vs composite** — must NOT re-clear the shared normal buffer (`clearGBuffer1()`
   `gos_postprocess.cpp:2323`); must depth-test so props only overwrite terrain GBuffer1 where they win.

## Sequencing
Substrate (MC2_VK_SHARED_SCENE_DEPTH) → decide/standardize shared depth = D32S8 → Tier-1 = shared-depth
test + convert scene color/normal FBO attachments to interop-shared + VK emits 2-output GBuffer in-place.
Terrain P10 + mech P12 inherit this same shared-GBuffer-attachment mechanism.
