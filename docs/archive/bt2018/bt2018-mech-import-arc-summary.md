# BT2018 mech import + material arc — summary & handoff

Branch `claude/mech-node-manifest-1a` (worktree `A:/Games/mc2-mech-node-manifest-1a`),
forked off nifty `0a06a71b`. NOT yet merged to nifty (foreign ABL + MECHRESTORE WIP dirty
there). Deployed + user-verified at `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0`.

## What shipped (commit order)
1. `4c90126c` **NODE-MANIFEST-1A** — weapon/hit firepoints for imported mechs via a generated
   `bt2018_mech_package.json` "nodes" map (MC2 node name -> source joint), resolved through the
   live clip bone palette (`GetImportedNodeWorld`). Also the full 59-mech FBX→GLB→retarget→
   texture import pipeline + the `BT2018Mechs` mod.
2. `972d872d` **MATERIAL-TUNING-1** — imported-mech albedo gamma/scale knobs (Graphics Options >
   Mech > "BT2018 Imported Skin"), default gamma-neutral 1.0 + scale 1.1 (legacy Blinn ≠ PBR;
   strict sRGB decode crushed dark skins).
3. `ac51bd6b` **IMGUI-PAUSE-INPUT-FIX-1** — pause menu clickable over open ImGui windows
   (WantCaptureMouse was zeroing the click transition → "can't exit"). General engine fix.
4. `4a3f57fd` **KTX2-INFRA-1** — imported-mech albedos homogeneous full-res BC7 `.ktx2`
   (replaced `.tga`); `resetPaintScheme` gate -> `textureOrKtxSidecarExists`.
5. `bd92aba2` **MATERIAL-PACKAGE-SCAFFOLD-1** — package schema v2: `materials` + `featureBits`;
   AO source rule (dump `Texture2D/<chassis>-Base-amb.png`, base-chassis, skin-independent);
   `check_mech_packages.py` consistency checker.
6. `ae6f5203` **AO-1 data** — AO cooked to BC7 `.ktx2` (package-driven), shadowhawk `-NNN` matcher.
7. `2c8b1a60` **AO-1 engine seam** — first live material tenant. featureBit `HAS_AO` -> AO handle
   -> unit-6 bind (save/restore) -> shader gated `c.rgb *= mix(1,ao,u_aoStrength)`. Default 0.5.

## Architecture (the durable seam)
Imported mechs are a **material-package contract**, not per-symptom hacks. Feature ladder:
`bt2018_mech_package.json` → `featureBits` → texture handles → shader-gated paths → checker.
AO is the proven first tenant; normals/masks/roughness reuse the same ladder. Pipeline tools in
`tools/bt2018_import/` (convert_all, retarget, gen_mech_package, cook_ktx2_textures,
cook_ao_from_package, build_bt2018_mod, check_mech_packages).

## Earlier fixes folded in
Leg-reversal (Atlas-skeleton clip retarget), torso bounce (static restLift), firepoint
across-map (stock-frame x-negation + y/z), arm firepoints (forearms not unskinned hands),
black skin (variant-material pick + 128/ mirror).

## Open / next
### `BT2018-MECH-MATERIAL-NORMALS-RECON-1` (do BEFORE NORMALS-1; new risk class = vertex data)
Normals are NOT texture-only — they touch importer tangents → side-channel data → GPU vertex
packing → shader TBN. Recon must prove:
- Assimp `aiProcess_CalcTangentSpace` tangent QUALITY on BT meshes; detect degenerate-UV/zero tangents.
- `GpuMechVertex.tangentOct` (byte 40, mech.vert loc 5) is truly unused / zero-filled today.
- BC5 normal cook validated LINEAR (not sRGB).
- TBN handedness convention documented.
- Fallback when normal missing = existing vertex-normal path (no regression).
- stock byte-identical; imported before/after.
Then `BT2018-MECH-MATERIAL-NORMALS-1`: tangent side-channel (NOT in shared TG_TypeVertex) →
pack tangentOct → mech.vert TBN varying → mech.frag sample BC5 normal, reconstruct Z → substitute
v_normal. `HAS_NORMAL` feature bit (renderFlags bit 5), same ladder as AO.

### Merge to nifty (file-level, when foreign ABL + MECHRESTORE WIP clears) — checklist
- [ ] firepoints still on-mech
- [ ] imported AO visible
- [ ] stock mech byte-identical
- [ ] menu exit/pause fix intact
- [ ] no MECHRESTORE clobber (mech3d.cpp foreign diag)
- [ ] no ABL clobber (ablmc2/ablerr/ablxstd)
- [ ] check_mech_packages.py PASS
- [ ] smoke one BT mod mission (MC2_MOD_DEPS=BT2018Mechs) if available
