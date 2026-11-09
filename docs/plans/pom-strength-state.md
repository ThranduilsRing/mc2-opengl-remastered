# POM strength

Assignment: A:/Games/mc2-slot02, claude/pom-strength; requested base
8fc0c6c89, starting tip b934d6ef8 (material footprint fade already present).

Plan (<=150 calls): inspect 30, implement 50, prove 40, hand back 20.
Found: MATV_POM_SCALE 0.02; material multiplier (1,1,2.5,1);
8..16 layers / 16 march iterations; denominator floor 0.001 and no offset cap.
The caller supplies tangent (east,north,up), but the march divides by north
and offsets east/up. Correct to xy/z, reduce scale, clamp grazing denominator,
and cap displacement to 0.25 of the filtered rock texel footprint.
Keep the existing footprint and scatter fades; default ON, no new gate.

Baseline instrumentation: existing debug modes extended with 30..39.
Red marks offsets >= 4^(mode-30) world units; green >= 0.25*4^(mode-30)
screen pixels; blue marks nonzero offsets. Thresholds give max brackets
without trying to decode tonemapped floating-point colors. Pixel distance
uses the inverse continuous rock UV Jacobian, world distance the rock scale.
Normal baseline/final captures use the six matching pan/zoom poses and night.

Next: baseline box capture + diagnostic thresholds; implement bounded POM;
box build/unit/shader rows/lint/smoke + captures; compare-only and inspect;
one final Windows Ninja compile; record receipts and cleanup.
