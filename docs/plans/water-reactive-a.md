# Reactive water A

Owner symptom (2026-11-08): "lightweight water physics ... not necessarily super realistic, but reactive water. Like the rocks in the water get a little different pattern, mechs in water make waves, same with vehicles".

Scope: option A only; presentation-owned analytic normal stamps. No simulation,
geometry, history texture, rock-flow, reflection-history or shoreline-band changes.

Plan: 128 stamps, oldest dropped, three-second lifetime; hashed world tiles with
eight newest candidates each and a distance cull. Visible completed mech/vehicle
wake requests supply wetness; presentation positions supply velocity. A small
water_geom.frag hook adds slopes to the material normal and restrained crest foam.

Done: clean assigned branch/base verified; study A/C and canonical rules read.
Proof first: WaterReactive/* doctests cover buffer lifecycle, visibility, capacity,
tile culling and the actual shared shader profile (symmetry, support, fade, wakes).
Next: implementation; box unit/shader/build/smoke and timed day/night A/B; GPU
cost on box; final lint/precheck and one Windows Ninja build; archive/release.
Receipts: c0a5df41 box production compile failed on private renderAnim_ access;
fixed by a const completed-presentation accessor (no class-layout change).
Evidence: E:/mc2-archive-2026-10/proofs/box/mc2-slot01/c0a5df41/.tails/build.log.
At 6e614f94: Windows mc2+viewer compile rc=0; box production compile PASS;
WaterReactive: 5/5 cases, 2133 assertions PASS; three nightly shader rows and PC
vk_shaders_compile PASS; actual production SPIR-V/CPU layout PASS (40960 bytes,
offsets 0/4096/8192, stamp stride 32). Broad box units: 3910/3911, existing VFX
oracle/Windows-path fixture failure; not repaired in this lane.
Cxx17 ratchet found two new idioms: velocity clamp and flattened tile indexing.
Replaced with std::clamp/std::mdspan, with unchanged payload/layout and math.
Next: prove corrected tip (including lint), then runtime day/night A/B and GPU
cost. Runtime/pixels/performance remain unproven. Current receipts/state:
E:/mc2-archive-2026-10/proofs/water-reactive-a/STATE.md.
