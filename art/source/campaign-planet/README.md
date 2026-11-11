# Carver V campaign globe

Rebuild: `py -3 tools/cook_campaign_planet.py` (Pillow, NumPy, OpenCV, SciPy).
This also recooks the marker atlases from part A.

The thirteen stock PNG compositions are lossless extracts from the stock TGAs.
Eleven are unique; op3_3 and op3_4 duplicate op1_5. They constrain the design,
framing, moon, screen grid, lighting and continent/cloud landmarks.

`tools/campaign_globe.py register` regenerates `registration.json`: mutual SIFT
matches, proper-rotation RANSAC, then one robust global camera/projection solve.
The sphere is perspective-projected, not orthographic. Coordinates are stock
pixels, Y down; camera vectors use Y up, Z toward the viewer. Poses map camera
to world. The fit jointly estimates disc centre, radius and radius/distance.
Alias images do not get extra votes. Rejected edges cannot constrain a pose.

The regular cook reconstructs `maps/albedo.png` (1024x512) and per-view lighting
from those poses. It removes the screen reticles before spherical projection,
excludes the moon, fits ambient/directional illumination from neutral clouds,
and weights observations by view angle and illumination. `coverage.png` records
observed texels; unobserved polar texels receive a smooth boundary extension.
The map is a composite surface/cloud design: stock does not provide reliable
cloud-free observations beneath opaque cloud, so no separate layer is claimed.

Rendering regenerates small material variations in fixed spherical coordinates,
preserves measured edge positions, and renders each view at 512x512. Screen
crosses are rebuilt geometrically; atmosphere, star field, moon and name bar
retain the stock screen-space composition. No invented labels or coastline
warps are introduced. The TGA overrides use the existing GUI sibling loader;
full-image UVs support the stock fallback and mod replacements of any size.

`tools/campaign_globe.py landmarks --out data/art/gui` reports independent SIFT
displacements between stock and downsampled renders, excluding grid and moon.
The cook pytest requires >=40 landmarks/view, mean <1 and p95 <2 stock pixels.
