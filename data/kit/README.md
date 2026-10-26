# Approved building kit release

`industrial-kit.json` pins the 34 approved industrial remakes packaged by
`tools/scifi_kit/industrial_package.py` on `claude/kit-ingame-2`, as merged in
window 56. Each asset names its approved bake tag relative to the manifest's art
source root; every packaged GLB, compressed texture and registry file has a SHA256 and
byte count. The source bake commit records the `claude/kit-intent` revision.

The same mod now includes the 35 owner-approved stock-faithful civilian remakes
from `civ-approved.json` (the `faith` group): Liao, Steiner, neutral civic and
city blocks. The approval snapshot is packaged as `content/civ-approved.json`;
`civilian_source_bake_commit` pins their art revision. Pack2 includes the other
23 entries in that snapshot and the approved military remakes. Existing payload
bytes are retained, with all groups sharing the registry and native cook receipt.

Pack2 also pins the earlier approved military walls and barriers whose available
bakes pass the current LOD checks. Its `content/pack2-selection.json` records every
added asset, bake tag and LOD check. Its earlier fence exclusions are superseded
by Pack3 below. The owner's later instruction includes the 15 baked
generic wall/gate/post variants despite the military inventory's earlier
exclusion; the other 113 mod-only CVE-G pieces are excluded. Available animated
rebakes use the mirror-corrected `-n1` tags from `rebake-n1.json`.
The House and generic gates retain their stock animation node names and sinking leaves.
Generic run lengths and endpoints pass. The approved medium/heavy wall variants
have plan IoU 0.83 and the tunnel gates 0.67, below the strict 0.90 visual-area
threshold; these explicit owner selections retain their approved geometry.
`content/pack2-generic-footprints.json` records those limits. Gameplay geometry
continues to come from stock, independently of the visible mesh.
Earlier approved wall/barrier visual-area differences are recorded separately
in `content/pack2-footprint-evidence.json`; these approved meshes are retained.

Pack3 pins all 32 entries of `batch2-approved-candidates.json` at exactly their
listed tags (including r2), as authorized by the owner despite the snapshot's
older candidate-only note. These replace ten earlier pins and add 22 assets:
18 turrets/popups/camo variants, eight structures, three CrashShanties and three
fences. `content/pack3-selection.json` records that selection and source hashes.
The existing common IR cooker and deploy path are unchanged.

Pack4 replaces 84 building bakes with the 0.25 m visual footing course from
`claude/kit-intent` commit `24d18be6c`: 35 civilian, seven batch-2 structures,
and 42 industrial/military assets. `content/rebake-ft1.json` records the bake
mapping. Turrets retain their Pack3 tags and payload bytes; the rejected r3
turret bakes are excluded. Gameplay geometry and saved passability continue
to use the stock simulation pose.

The three approved r2 fence bakes predate the stock count-contract repair
7ca5e7b44. That contract is art-review metadata: the GLB frontend, package
validator and runtime override registry do not require or consume it. The exact
approved bakes are retained without a metadata-only rebake. Razorwire's crown
overhang and 45-degree blocking-cell lint are visual-area limits; the engine
uses stock collision, covered cells, passability and LOS for these render-only
overrides. Neither lint controls loading or binding.

Animation proof evaluates the selected packaged GLBs at every LOD against stock
name-bound clips and turret yaw/pitch, including popup rise and gate sinking.
The stock `srmcamo4.ini` closing state names absent `SRMCamoOpen.agl`; its other
states use the available `SRMCamoOpen4.agl`. This existing stock declaration
limits closing-clip coverage; the package does not change stock animation data.

`l_palacegate` is included because building overrides use the stock gameplay
shapes for collision, covered cells, passability and LOS, just like every other
building. Its visual two-cell blocking failure also occurs on the stock gate;
the remake does not change that gameplay geometry.

The release bytes live at the manifest's `store` on E:, combining the verified
industrial and civilian release with the military package from the same cooker. That
directory is the deploy source, independent of mutable
artist output and existing game installs. `MC2_KIT_STORE` may relocate those exact
bytes; hashes remain mandatory. Release updates require a new store directory
and a reviewed manifest change. The payload stays outside git because lane rules
prohibit generated files over about 1 MB. The native cache is derived, and is
never a release source.

Normal game/editor deploys install and verify the package, rebuild the standalone
importer through Ninja's dependency checks, and cook all 693 GLBs into 655 distinct
native recipes (identical small-prop LODs share a recipe). A
receipt pins recipes to package and importer hashes and verifies their output
hashes on subsequent deploys. Missing or altered source bytes fail the deploy;
missing or altered cache entries trigger cooking. Extra installed kit files fail
verification to prevent a mixed release. The deploy manifest includes kit files.

`--classic-assets`, or an install with `RemasteredBuildings=FALSE` in options.cfg
(including MC2_OPTIONS_OVERRIDE), skips kit lookup and cooking. This deploy switch
does not persist game preferences: use the game's `--classic-assets` switch or
the options.cfg setting when launching Classic. An existing kit can remain on
disk while Classic uses stock assets.
