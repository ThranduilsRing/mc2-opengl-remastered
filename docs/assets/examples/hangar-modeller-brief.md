# Modeller brief: hangar

Keep the stock pivot (0,0,0), TG +Y up, and measured bounds {'min': [-96.0, 0.0, -96.0], 'max': [95.0, 77.92009735107422, 96.0], 'size': [191.0, 77.92009735107422, 192.0], 'footprint_wu': [191.0, 192.0], 'height_wu': 77.92009735107422, 'extent_radius_wu': 144.65136190302826}.
Nominal front is -Z; verify the hangar entrance / fence endpoints in the stock preview.
Target at most 12000 triangles and
1024 px texture edges. Preserve gameplay collision
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
