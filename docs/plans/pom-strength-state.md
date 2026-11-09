# POM strength

Assignment: A:/Games/mc2-slot02, claude/pom-strength; requested base
8fc0c6c89, starting tip b934d6ef8 (material footprint fade already present).

Plan (<=150 calls): inspect 30, implement 50, prove 40, hand back 20.
Found: MATV_POM_SCALE 0.02; material multiplier (1,1,2.5,1);
8..16 layers / 16 march iterations; denominator floor 0.001 and no offset cap.
The caller and march consistently use (east,up,north). Initial suspicion of
an axis mismatch was incorrect; retain that layout. Reduce scale, clamp the
grazing denominator, and cap displacement to 0.25 of the filtered rock texel
footprint AND 0.25 screen pixels (directional inverse UV Jacobian).
Keep the existing footprint and scatter fades; default ON, no new gate.

Baseline instrumentation: existing debug modes extended with 30..39.
Red marks offsets >= 4^(mode-30) world units; green >= 0.25*4^(mode-30)
screen pixels; blue marks nonzero offsets. Thresholds give max brackets
without trying to decode tonemapped floating-point colors. Pixel distance
uses the inverse continuous rock UV Jacobian, world distance the rock scale.
Normal baseline/final captures use the six matching pan/zoom poses and night.

Baseline commit: 6c2076b2e (only diagnostic/poses, original POM unchanged).
Box attempt failed before build/launch: native scp connection closed; native
ssh reports UNPROTECTED PRIVATE KEY FILE / bad permissions on the configured
C:/SPB_Data/.ssh/mc2farm_ed25519 and Permission denied (publickey). Per lane
rules, do not bypass the refusal. No captures or runtime measurements exist.

Implemented: scale 0.002 (10x reduction); removed the dirt 2.5x multiplier;
view-up floor 0.35; ceil 8..16 layers prevents fractional final-step overshoot;
ray capped BEFORE height sampling; return the small offset directly to avoid
large-UV subtraction cancellation. Original footprint/scatter fades retained.
Next: covering offline tests + shader rows/lint and one Windows compile;
record receipts/cleanup. Box build/unit/smoke/captures and compare-only remain
NOT RUN until SSH access works. Do not claim visual stability without them.
