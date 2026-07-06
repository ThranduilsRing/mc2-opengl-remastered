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

## ★ORACLE GAP — RESOLVED 2026-07-05 (per-actor scheme cache session)
The gap is CLOSED via the natural game-camera FREE-CAPTURE route (not a bookmark). At
mission start the game camera centers on the PLAYER lance, which GL DOES draw. Running
free-capture (`MC2_VISUAL_CAPTURE_FRAME=200`, no bookmark override, fixed-step + seed)
on mc2_01 frames 4 player mechs GL draws — the FIRST co-covered GL-vs-VK mech frames.
Driver: `.claude/mech-parity/freecap_driver.py`; diff: `.claude/mech-parity/diff_region.py`;
captures in `.claude/mech-parity/freecap/mc2_01_f200/`.

★HEADLINE — co-covered mech parity (mc2_01 f200, mech region x[400..620] y[100..200],
same natural camera, substrate whole-frame control ~0 so this IS a shading number):
- BEFORE (gl vs vk_flat):          6.91% covered, mean-over-covered ALL = **61.96**/ch (flat orange probe)
- AFTER  (gl vs vk_mat+scheme):    5.03% covered, mean-over-covered ALL = **28.33**/ch (textured+lit+scheme-cache)
- Whole-frame AFTER: 0.23% covered; substrate control (gl vs vk_flat non-mech) ~0.
Material+lighting+scheme-cache HALVED per-covered-pixel error (61.96→28.33) and cut covered
count (1521→1107). Residual ~28/ch = edge/silhouette AA + no island frustum cull (VK draws
all submitted instances) + minor lighting approx — NOT a hue shift (visual confirms).
Scheme cache PROVEN: log shows 2 distinct schemes cached (glTex=139 scheme1, glTex=138
scheme2); stacked crop `stack_gl_flat_mat.png` shows VK-mat yellow paint + Lambert matching
GL (was flat orange). Before this fix, only glTex=139 would show on all mechs.

--- (historical gap description, now superseded by the free-capture route above) ---
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

DONE 2026-07-05:
- Per-actor paint-scheme VK-descriptor CACHE (commit c3f683f2). glTex-keyed cache of
  {VkImage,view,descriptorSet}, kMaxSchemes=32, schemes[0]=dummy white. Draw loop binds
  the set matching run.slot0GlTex. Proven: 2 distinct schemes cached + rendered on mc2_01
  (glTex 139/138). See HEADLINE above. Full-population parity mechanism now shipped.

STILL DEFERRED (all match the GL DEFAULT path being OFF too — so still parity vs default):
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
