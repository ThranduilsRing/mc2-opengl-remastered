# Lane 2 session 31 cleanup review (39fc9203f..0ae49ae96)

## Verdict
PASS. 0 must-fix, 2 should-fix, 3 nits. Read-only; nothing built or launched.

## Must-fix
None.

## Should-fix
- S1. `docs/tracy-harness.md` still says "per arm" in the threshold table lead-in ("Thresholds (on frame-count drop, per arm)", ~line 153) and "baseline.json keeps them per arm" (~line 65), after the doc was cut to one arm. Reword or the doc contradicts its own "One arm" section.
- S2. The VK-PARITY-GAPS "Total" line dropped the "unverified: #8, #10, #11 residual, #17, #19" tail instead of updating it. Rows 8/10/17/19 are now claimed closed in the PARITY STATE header; either restate which of them remain unverified or say none do.

## Nits
- N1. State file lists tracy nits N2 (`baseline --arms` help) and N4 (docstring "GL-side frame") as open, but the doc scope note was already reworded in this range; confirm N4 is only the docstring.
- N2. `MC2_MECH_SURFACE_MATERIAL` and `MC2_LIGHT_BILLBOARD_FACING` have an empty default in gate_table.json (string/mode gates). "Default OFF" is correct by reading the code path, but the table does not say it; one-line note in the allowlist would make the label checkable.
- N3. PORTED_AS "fails loud" works only through the allowlist: a vanished identifier turns the pair NOT_PORTED, which fails as a new gap because the entry was deleted. Fine, but the comment could say that is the mechanism.

## Checked-OK
1. TEMP pairs cancel: `git diff f3b3f3d13^ 1e9740df1` and `git diff 409b7242e^ b44a12842` are both empty. (The literal `39fc9203f..b44a12842` diff is 34 files only because of the nifty merge.)
2. PORTED_AS: `forms.append` adds the VK identifier into the same token search as the stem forms, so it is re-evaluated each run against the twin family. Both identifiers exist today: `edgeFeatherStrength` in terrain_frame_ubo_vk.glsl / terrain_tier1.frag, `LSVK_KNOB_BANDFIX_V2` in lighting_shadow_vk.glsl / terrain_tier1.frag. The new test asserts PORTED/HIGH/"ported-as row 0:..." with the name present and NOT_PORTED when absent.
3. Gate defaults (Gates/gate_table.json): CEMENT_ATLAS_SEAM_FIX True, TERRAIN_SHORELINE True (rows 25/26 default-ON, correct); spot checks STANDARD_LIT_V1, TERRAIN_MACRO_VARIATION, TERRAIN_SLOPE_BIAS, TERRAIN_VISUAL_DISPLACE, SSAO, CLOUD_SHADOW all False; BACK_FILL 0; TERRAIN_LOD_CHUNK True (backs the "legacy terrain program" labels). The first allowlist section has 9 pairs, matching the state's "9 of 68 NOT_PORTED are rows 25/26".
4. PARITY STATE paragraph, row 3/24 numbers and BUGS 43 extension agree with lane2-state s31 (52,553 px, 1,041,136 / 1,042,012 / 1,039,512, 0.95 on 49.33, 0.551 median, commits f3b3f3d13/409b7242e). Open set 18, 25, 26 consistent with the Total line.
5. Comments in tools state WHAT+WHY (PORTED_AS, SELFCHECK_SIZES measured 33.0% vs 8% bar); no journal comments.
6. New tests assert refusals: three `pytest.raises(SystemExit, match=...)` cases (no backend receipt, fail-soft REFUSING, `_measure_pose` with stubs) plus the PORTED_AS in/out test.
7. docs/tracy-harness.md: index eol `i/lf w/lf`, 0 CR bytes before and after, UTF-8; diff is 8+/7- of content lines only, no line-ending churn.
