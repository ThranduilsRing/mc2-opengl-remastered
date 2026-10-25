# Industrial and civilian kit release

`industrial-kit.json` pins the 34 approved industrial remakes packaged by
`tools/scifi_kit/industrial_package.py` on `claude/kit-ingame-2`, as merged in
window 56. Each asset names its approved bake tag under the intent-pass source
root; every packaged GLB, compressed texture and registry file has a SHA256 and
byte count. The source bake commit records the `claude/kit-intent` revision.

The same mod now includes the 35 owner-approved stock-faithful civilian remakes
from `civ-approved.json` (the `faith` group): Liao, Steiner, neutral civic and
city blocks. The approval snapshot is packaged as `content/civ-approved.json`;
`civilian_source_bake_commit` pins their art revision. The other 23 entries in
that snapshot are outside this release. Existing industrial payload bytes are
retained, with both groups sharing the registry and native cook receipt.

`l_palacegate` is included because building overrides use the stock gameplay
shapes for collision, covered cells, passability and LOS, just like every other
building. Its visual two-cell blocking failure also occurs on the stock gate;
the remake does not change that gameplay geometry.

The release bytes live at the manifest's `store` on E:, combining the verified
industrial release with the civilian package from the same cooker. That
directory is the deploy source, independent of mutable
artist output and existing game installs. `MC2_KIT_STORE` may relocate those exact
bytes; hashes remain mandatory. Release updates require a new store directory
and a reviewed manifest change. The payload stays outside git because lane rules
prohibit generated files over about 1 MB. The native cache is derived, and is
never a release source.

Normal game/editor deploys install and verify the package, rebuild the standalone
importer through Ninja's dependency checks, and cook all 207 native recipes. A
receipt pins recipes to package and importer hashes and verifies their output
hashes on subsequent deploys. Missing or altered source bytes fail the deploy;
missing or altered cache entries trigger cooking. Extra installed kit files fail
verification to prevent a mixed release. The deploy manifest includes kit files.

`--classic-assets`, or an install with `RemasteredBuildings=FALSE` in options.cfg
(including MC2_OPTIONS_OVERRIDE), skips kit lookup and cooking. This deploy switch
does not persist game preferences: use the game's `--classic-assets` switch or
the options.cfg setting when launching Classic. An existing kit can remain on
disk while Classic uses stock assets.
