# Modeller brief: wirefence

Keep the stock pivot (0,0,0), TG +Y up, and measured bounds {'min': [-0.15759999677538872, 0.0, -0.4255000092089176], 'max': [0.20360000804066658, 15.969900131225586, 42.91749834641814], 'size': [0.3612000048160553, 15.969900131225586, 43.34299835562706], 'footprint_wu': [0.3612000048160553, 43.34299835562706], 'height_wu': 15.969900131225586, 'extent_radius_wu': 45.79291241534433}.
Nominal front is -Z; verify the hangar entrance / fence endpoints in the stock preview.
Target at most 494 triangles and
512 px texture edges. Preserve gameplay collision
and fence tiling endpoints; bake scene transforms and use identity nodes.
Rebuild silhouette, roof seams, posts and damage rubble before adding small detail.
Provide intact and destroyed states, UVs, albedo and authored normals; lower LODs
must preserve footprint and silhouette. Use opaque structural posts plus a sparse
alpha lattice for wire fences, rather than thousands of tiny wire cylinders.
Current runtime activates intact staticProp replacement only; destroyed geometry
stays stock, while this texture package reworks both states via stock TGL clones.
Tier 256 retains the enlarged image; lower tiers halve each dimension per step.
Material authoring belongs to the Blender/PBR pipeline. Lanczos interpolation
adds pixels but cannot recover missing detail.
