# VK-MECH-MATERIAL-LIGHTING-1 — oracle findings (2026-07-05)

Commit `5bca3eaf` (branch `claude/vk-interop-bridge-1`, off `ac9e1ad1`).
Deployed `A:/Games/mc2-opengl/mc2-win64-0.4c`, exe fingerprint src_commit 5bca3eaf.

## What shipped
New gate `MC2_VK_MECH_LIGHTING` (default-OFF, requires `MC2_VK_MECH_TIER1`; visible
with `MC2_VK_MECH_MATERIAL`). VK mech island now ports the **GL DEFAULT mech shading**
(NOT PBR/GGX — `MC2_MECH_VIEWUNIFORMS` + `MC2_STANDARD_LIT_V1` are both default-OFF, so
the default deploy renders `calc_light` Lambert × albedo):
- VS: verbatim `lighting.hglsl` `calc_light` (4-param, `MC2_STATIC_PROP_LIGHTING`
  preserve-base branch, `kAmbientFloor=0.35`, `+aRGBHighlight*a` clamp) → `v_litColor`.
- FS: `albedo * v_litColor + highlight`, then per-actor haze `mix(c, fogColor, fogRGB.a)`.
- LightsData GL SSBO (binding 20) copied per-frame into a VK SSBO (binding 4) via
  `glGetBufferSubData`; fog color snapshotted at the scene-UBO upload site.

## In-engine evidence (mc2_01, fixed-step deterministic capture)
`[VK_MECH_ISLAND]` log, VK-lit run:
- island init `material=1 lighting=1`; paint-scheme albedo imported 128x128 glTex=139
- first co-write `instCount=13 boneCount=47 runs=13`, `used_vulkan=2157` frames, no fallback
- **`forbidden_getTexImage=0 forbidden_readPixels=0`** — the per-frame LightsData buffer
  copy did NOT trip the color-readback guard (buffer copy, not a color-surface readback)
- validation=off run clean, no VK_ERROR, no fence timeout, no disabling; 7900 XTX interop=1

## Parity oracle — before / after (same deterministic camera)
Substrate control (`MC2_VK_SHARED_SCENE_DEPTH=1` only, mech gates OFF) vs GL =
**0 diff on 10/11 bookmarks** (cross_map_view 4px = pre-existing baseline drift) →
shared-depth substrate byte-transparent, so any mech-run diff is mech pixels only.

Flat probe (before) → material+lighting (after) on the mech footprint:
mean per-changed-pixel shift ≈ 85 (albedo + Lambert now applied). Visual (highangle_wide,
mech box x[410..755] y[150..240], `.claude/mech-parity/mech_before_after.png`):
- **before**: flat orange probe (vUv.y gradient only)
- **after**: yellow paint-scheme texture + calc_light directional Lambert (shaded torso,
  dark blue-black leg shadows) — reads as a correct GL mech.

## ★ORACLE GAP (documented, carried in from the handoff — NOT resolved this session)
NO deterministic bookmark in the standard sets frames a mech that **GL also draws** at the
frozen replay camera. The mc2_01/mc2_24 mechs are AI units the GL visibility path
frustum-culls at the bookmark cameras (GL `mech_visible=0` for most sweep frames), while the
VK island draws every submitted instance (no island-side frustum cull — pre-existing
sub-slice-1 behavior). So every GL-vs-VK bookmark diff is a COVERAGE mismatch (VK draws a
mech over GL terrain/water), NOT a co-covered shading diff → the pixel-diff magnitude is not
a shading-parity number. The combat/werewolf bookmarks (`mc2_01_combat.json`,
`mc2_01_werewolf.json`) DON'T FIRE in smoke mode (single-pose werewolf; combat poses framed
for a live-combat camera the deterministic sim never reaches — `mech_visible=0` at those
positions).

**Parity is therefore established by CONSTRUCTION + VISUAL, not a same-pixel number:**
the VK shading is a verbatim port of the GL default path, and the GL mech color flush is
byte-identical when the gate is OFF (proven by the 0-diff substrate control). The visual
confirms textured + Lambert-lit output matching GL mech appearance.

## What is now PARITY vs still DEFERRED
PARITY (matches GL default mech):
- Paint-scheme albedo (single-material probe, copy-once slot-0)
- Per-vertex `calc_light` Lambert (all 6 light types ported; AMBIENT/INFINITE/
  INFINITEWITHFALLOFF/POINT/SPOT; TERRAIN ignored exactly as GL)
- aRGBHighlight (multiply-fold + additive, both, exactly as GL)
- Per-actor haze fog `mix(c, g_scene.fogColor, fogRGB.a)`
- lightsOut ambient-floor branch

STILL DEFERRED (all match the GL DEFAULT path being OFF too — so still parity vs default):
- Per-actor paint-scheme VK-descriptor CACHE (currently single copy-once slot-0 material;
  a mission with >1 distinct paint scheme would show the first scheme on all mechs).
  ★Biggest remaining mechanism item for full-population parity. Mechanism note below.
- StandardLit-GGX / MECH-SPECULAR / MECH-AMBIENT / IBL-SH / imported-BT normal+AO paths —
  all require `MC2_MECH_VIEWUNIFORMS` + `MC2_STANDARD_LIT_V1` (default-OFF); not the default
  deploy look. Only needed if the arc later flips those GL gates on by default.
- Island-side frustum cull (the coverage gap above) — VK draws all submitted instances.
  Pre-existing; ships all mechs the batcher submitted. Revisit before default-flip.

## Mechanism note for next session (per-actor paint cache)
`import_albedo_2d` imports ONLY `feed.runs[0].slot0GlTex` once per map. The feed already
carries `slot0GlTex` PER RUN (`Mc2MechDrawRun.slot0GlTex`, live per-actor paint this frame).
Full per-population parity = a `glTex -> {VkImage,VkImageView}` copy-once cache keyed on the
GL handle, one descriptor set (or a descriptor-indexed array) per distinct paint scheme,
bound per run in the draw loop. Same primitive as the prop-material 2D_ARRAY cache the arc
already built (P11 material). Un-lit albedo would still need lighting — already done here, so
only the texture-binding axis remains. Safe subset shipped this session = single-material +
full lighting (the correct next rung; the per-actor cache is the natural follow-on).
