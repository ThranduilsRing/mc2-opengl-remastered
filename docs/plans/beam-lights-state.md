# beam-lights

Assignment: slot07, claude/beam-lights, base 8fc0c6c8957673ab0be789403a10b91b852c59b2.

Current truth: the May memory claiming no dynamic emitters is stale. WeaponBoltType
reads PointLight/LightRGB/Intensity/MaxRadius/OutRadius from bolt effect FIT data;
non-beams register a moving point light. ExplosionType independently supplies RGB,
intensity and evolving radii (carnage.cpp; owned by lighting-hud-residuals).
Camera has 1024 world slots; the frame pool defaults to 256. Terrain consumes
tlp_terrainPoolLight; props and mechs consume olp_objectPoolLight (256 prefix).
Lasers previously never entered the non-beam point-light registration branch.

Implemented: frozen bolt packets carry energy classification and remaining light
lifetime; shared pool gets up to 32 modest beam samples, 1..6 along each segment,
24-unit merging with max radiance, padded screen rejection, 160ms fade and brighter
endpoint impact. Non-beam energy/PPC uses its moving bolt segment. Existing world
lights retain priority; beam lights use remaining capacity, no shader/ABI changes.

Integration correction: terrain geometry packs before projectile extraction.
Append beam samples at extractRenderFrame after extractRenderList, then refresh
the shared pool/grid snapshot before the frame is frozen. Packing from the
earlier terrain stage produced a pixel-identical A/B and was rejected.

Next: targeted doctests, box build/unit/smoke + night VK baseline/changed capture
using existing force-fire fixture, inspect montage; one final Windows Ninja build
and lint. Commit locally, record exact receipts and any missing proof. No perf claim.
