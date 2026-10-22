# Lighting L0 evidence

Owner-approved design: `E:/mc2-archive-2026-10/plans/lighting-rework-design-2026-10-21.md`.
Evidence root: `E:/mc2-archive-2026-10/proofs/lighting-L0/`.
Install: `E:/mc2-lane4` only. Baseline engine: `a7600589427e`.
Build: existing `build64-ninja`, RelWithDebInfo. No lighting behavior changes in L0.

## Source census

Reproduce the physical-source census with:

```powershell
py -3 tools/lighting_source_census.py --root E:/mc2-lane4/data --root E:/mc2-lane4/mods --out E:/mc2-archive-2026-10/proofs/lighting-L0
```

The CSV records path, size, SHA256, container format evidence and conservative
lighting provenance. It contains 16,662 loose sources, including 193 `.burnin`
files. `archive-texture-members.csv` separately inventories 2,956 texture members
of the install's FSTs, including 40 `.burnin` members. These are physical inventories,
not a claim that every entry is selected by the current mod mount.

| Source/consumer | Existing encoding and lighting interpretation |
|---|---|
| Terrain `.burnin` | Prelit. `mclib/terrtxm2_bake.cpp` multiplies RGB bytes by mission light RGB; `terrtxm2_load.cpp` probes burned sources and sidecars. Never infer unlit albedo or divide by guessed illumination. |
| Terrain atlas/cooked macro color | `vulkan_terrain_island_images.cpp` preserves atlas sampling space: BC7 sRGB/UNORM or RGBA8 UNORM; cooked color is RGBA8 sRGB when the input is sRGB, otherwise UNORM. Storage conversion does not remove prelighting. |
| Cement/road and detail normals | RGBA8 UNORM in the terrain island. Cement color has legacy authored semantics; normals are data. |
| Legacy props, mechs, foliage, mod art | Painted/prelit status unknown unless independently validated. Compatibility classification, never automatically unlit based on a filename or sRGB tag. |
| HDR skies | HDR containers/BC6H sidecars; existing sky, SH and water consumers have different scale/output conventions. The sole default SH set is daylight-derived, including at night. |
| Scene output | RGBA16F target; legacy vertex clamps, private water mapping and post grading remain. No claim of a unified scene-linear contract yet. |

KTX2 vkFormat counts: 0=100, 43=100, 143=8, 145=464, 146=4,931.
Format 0 needs DFD/transcoding interpretation; it is not declared linear or sRGB
by this census. Embedded GLB images and PAK-internal textures need later material
metadata inspection; they are not silently classified as unlit.

## Proof and limitations

`unit-baseline.txt` records successful doctest, engine-linked and Python harnesses.
`contracts.txt` records the static contracts. The crossfade ownership failure is
pre-existing: `preexisting-contract-parent.txt` reproduces the same orphan
`tests/unit/test_music_crossfade.cpp` assertion at unchanged parent `a76005894`,
with the census held in temporary commit `0dfb6d512` and no stash used. That
untracked test was never changed, staged, deleted or committed.

The new census's caller-registration failure is owned by L0 and is repaired by
the manual-tool registration and regenerated `tests/callers.json`.

The original deploy refusal was resolved only by the owner's explicit approval
of `--allow-new-target` for exactly `E:/mc2-lane4`; see `deploy-baseline.txt`.
All captures use the background harness and `--no-kill`, with serialized host
leases. Captures use the Vulkan present readback, fixed simulation clock and
declared diagnostic settings recorded in their sidecars.

The initial `baseline/` and `hangar-probe/` directories preserve rejected framing
attempts. Close hangar framing uses the actual type-559 Hangar position decoded
from stock mission packet 1, rather than the old building-cluster centroid.
The accepted frozen poses and viewed images are recorded in the archive report.
`docs/lighting-L0-manifest.json` pins their hashes, the census and the GPU/VRAM
ledger. `review.md` in the archive records the viewed subjects and limitations.
Four runs per mission at native 1080p and 4K are retained in `perf-steady/`.
Several post-settle CPU trace windows are short or absent under shared host
load; the ledger reports actual coverage rather than claiming qualified tails.
Sampled process residency is 1820/1825 MiB at 1080p and 2135/2139 MiB at 4K
(day/night). This is the high-settings baseline, not potato qualification.

The render dump's legacy `build` block reports `Release/unknown` even for the
RelWithDebInfo Ninja build. Use the capture/deploy fingerprint and CMake cache
for build identity. `renderResources` is a partial logical ledger, not peak
physical residency; GPU budget/process/VMA counters are recorded separately.

GPU timestamp receipts are window means. They cannot supply per-frame GPU
p50/p95/p99. Load spikes, destruction spikes, missing diagnostic decompositions
and release-budget qualification must remain explicitly unmeasured where the
current telemetry cannot establish them. L0 makes no performance-budget pass.
