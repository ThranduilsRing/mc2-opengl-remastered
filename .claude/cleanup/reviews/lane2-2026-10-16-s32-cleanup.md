# Lane 2 session 32 cleanup review (c6f7dba41..f170964d8)

Read-only review of the row 25 and row 26 VK ports, the empty provers parity section, the tracy wording, and the gaps/state/callers updates. No builds or launches.

## Verdict

PASS with should-fixes. The VK math matches GL term for term: the same inset formula, band widths, fade, slope guard, affinity, wet/foam colours, and fbm calls (both backends include the shared `shaders/include/noise.hglsl` and use the same world-XY inputs). There is no real port gap behind the low row-26 per-pixel fidelity (see S1).

## Must-fix

None.

## Should-fix

- **S1 (row 26 proof wording):** the foam term is animated (`fbm(xy*0.09 + time*0.15)`, `fbm(xy*0.20 - time*0.22)`). GL and VK captures taken at different shader-clock values put the flecks on different texels, which explains "flecks land on neighbouring texels" without any math difference. The rest of the residual comes from inputs both shaders inherit upstream, not from this port:
  - VK `pureConcrete` is a bool, while GL uses a float that includes `cementTransAlpha`.
  - VK `mv_weights(base)` stands in for GL `matWeights`, which can come from the control map.
  - Upstream `lit`/`N` differ between the backends.

  Record this cause in the row 26 note in `VK-PARITY-GAPS.md` so a later reader does not reopen it as a port defect. A fixed-clock GL/VK pair, if one is ever cheap, would make the foam term comparable texel by texel.
- **S2 (gaps Total line):** it still says "Total: 26 rows" after row 27 was added. It should say 27.

## Nits

- **N1:** the lane2-state design note says the pad went "61 -> 48". The diff shows `_pad[62]` -> `_pad[48]`: 14 new fields, so 62 -> 48.
- **N2:** GL `bindShorelineMask` used to cache the knobs in function statics. It now calls `gos_TerrainShorelineKnobs()` every frame. The output is the same because the gates are per-process, and dropping the statics is the right direction. The cost is a few gate reads per frame, which is negligible.
- **N3:** the VK diag-connect cement branch sets `pureConcrete` but not `cementHit`, whereas GL sets both (:1033). The wet band result is still identical because `wetAmt *= (1 - F3pc)` zeroes it. Setting `cementHit` there too would keep the bool honest for future readers.
- **N4:** the descriptor pool size comment in `vulkan_terrain_island_island.cpp` runs past about 200 columns.

## Checked-OK

1. **Row 25 math:** `0.5 / max(texW / gridSide, 1.0)` and the clamp to `[inset, 1-inset]` are applied to the sample UV only, with `cTileUV` kept for the transition mask. This holds at both VK cement sites (main and diag) and matches GL :866/:1025. GL's third site (:1094, runway/road blend) has no VK twin and is correctly tracked as new row 27.
2. **Row 26 math:**
   - The macro normal (`mv_smoothTerrainNormal`) is identical to GL `smoothTerrainNormal`.
   - `sinSlope` floor 0.03, jitter `fbm(xy*0.055,2)*jitter`, the 1.5 wu `aboveFade` on the un-jittered distance, mask UV/bounds/valid-alpha, slope 0.940/0.978, luma smoothstep 0.08/0.26, wet `vec3(1.16,1.06,0.78)*luma*0.9` at 0.85, and foam coverage/`vec3(0.92)` are all the same as GL.
   - The noise source and UV space are the same.
3. **GL behaviour:** the only GL change is moving the knob reads into the shared producer. It produces the same values, the same `isSet` alias precedence for `_RUN`/`_HEIGHT`, and the same 0 water elevation when post-process is null. The GL tier1 run (5/5 PASS, fingerprint) agrees.
4. **20f3c0575 (`cementHit`):** a function-local `bool` hoisted from the `useCement` block to `main()` scope next to `pureConcrete`. It is a clean scope, not a global.
5. **dup_lines exclude:** the one glob `shaders/vulkan/include/*_frame_ubo_vk.glsl` covers only `gpu_abi_gen.py` output, which carries the "GENERATED ... DO NOT EDIT" header. It has a WHY comment and sits under the file's own "machine-written only" rule.
6. **Comments and code:**
   - Comments are WHAT+WHY, with no journal narration in code. The removed GL statics shrink static-duration state, and none were added.
   - `import_shore_mask` treats a null/zero-size/version-0 feed as "no sidecar" (the documented absence, not malformed input). It logs both a successful import and an upload failure, falls back to the dummy, and does not retry a failed version every frame.
   - The UBO layout (gen header offsets 776..828, 832 B within the 1024 B slot) and binding 14 (layout, pool +2, write) are consistent.
7. **Gaps vs state:** the gaps rows 20/25/26/27 and the PARITY STATE line match the lane2-state session 32 numbers: 1,664 of 1,667 px, err 0.45/33.51, 22.48 -> 9.97 at 205952, row 20 PASS 63 fps with validation 0, and row 27 open with owner lane 2. The only mismatches are S2 and N1.
8. **Doctest:** `test_terrain_field_store.cpp` asserts that reloading identical mask bytes strictly bumps `version`. This is the real contract the VK island's copy-once re-import relies on, and a dedupe-by-content regression would break it.
