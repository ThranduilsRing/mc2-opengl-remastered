# Lighting L1: compatibility environment

Design: `E:/mc2-archive-2026-10/plans/lighting-rework-design-2026-10-21.md`.
Proof: `E:/mc2-archive-2026-10/proofs/lighting-L1/`.
Baseline: `docs/lighting-L0-manifest.json`, engine `a7600589427e`.
Only L0 and L1 are implemented. No shader, radiometric library, GPU ABI,
environment cook or material-relighting change is part of this packet.

`MissionEnvironment` is a CPU record of the current mission camera inputs,
effective terrain direction, profile values, sky/SH identities and explicit
legacy composition policies. Camera/terrain directions retain MC2 Z-up;
the existing generated SH convention is explicitly labelled Y-up. Authored
color bytes keep their legacy interpretation. The record makes no assertion
that legacy colors or the completed legacy shading are physical radiance.

The loader accepts unversioned/version-1 profiles and version 2. Shipped data
now declares `schemaVersion: 2` and `colorContract: legacy-authored-v1` while
retaining its numeric values and key vocabulary. Exposure's existing multiplier
is applied directly; EV is derived metadata, never an exp2/log2 round trip.
Unsupported schema/color contracts, malformed JSON, invalid numeric types,
non-finite/out-of-range values and inverted classifier thresholds diagnose and
fall back to valid defaults/layers. Unknown fields survive saves but do not
silently acquire new rendering semantics.

Every `visualTuning_applyProfile` constructs a fresh record and applies every
supported tuning value. The existing mission-init and save-load calls therefore
reset omitted fields; the next camera publication supplies that mission's FIT
lighting. Profile defaults, its mission entry, then explicit session profile
and supported per-value session overrides resolve in order. FIT lighting and
profile tuning remain disjoint legacy inputs during L1. The new physical groups
and authoring behavior belong to later packets, not implicit version-2 keys.

The adapter reads through the real `File` resolver, including FST fallback.
It uses the existing mount index: active mod, then current dependency priority,
then base data. The actual engine gives the first declared dependency priority;
L1 preserves that behavior. Profiles are whole-file mount overrides, not a new
field-merging rule between different mod mounts.

Save targets the resolved active editable file or explicit session file.
Dependencies, FST members and base fallback while a mod is active are protected.
Create an active override before editing an inherited profile. Save re-reads
the provider, preserves unrelated entries/fields, migrates supported legacy
metadata, and replaces via a temporary file. Invalid existing files are not
overwritten. This packet does not add an editor UI or automatic override-file
creation.

Gamecam publishes the same legacy flat light, night state and daylight level,
then reads them from the record for the existing renderer setters. It retains
both camera and effective terrain directions rather than changing their legacy
relationship. Sky selection, SH strength, lamp SUM/MAX/compression, pad spill,
atmosphere, clouds, grading, clamps and prelit art interpretation remain on the
compatibility path. There is no new feature gate or GPU resource allocation.

The debug dump's `missionEnvironment` block records a canonical-JSON FNV-1a-64
effective hash, contributing profile providers, diagnostics, sky/SH identity,
color contract and `legacy-no-environment-cook` cache status. Hashes exclude
provider metadata and source schema version so a value-preserving migration
does not change the effective value hash. The legacy dump's old build identity
block remains `Release/unknown`; use the deploy/capture fingerprint for identity.

## Validation

`profile-tests-mounted.txt`: nine focused cases, 94 assertions passing, including
the real VFS, dependency order, active-only save, session precedence, mission
reset, exact stock values, malformed/versioned inputs and preservation on save.
`unit-integrated.txt` and `contracts-integrated.txt` contain the full tier runs.
The contract's crossfade orphan is pre-existing and reproduced at unchanged
parent `a76005894` in the L0 proof. The JSON isolation rule was extended for
the private profile implementation and its focused test; no public header
exposes JSON-library types.

Visual and smoke results, capture fingerprints, matched full frames/crops and
the viewed montage are recorded in the final archive review. MAD is descriptive
only and never an acceptance threshold.

For L2: shared units/coordinate adapters, BRDF/environment/local-light library
and generated CPU/GPU ABI remain open. Sky cooking, asset-specific unlit/prelit
metadata, physical relighting, common atmosphere/output and release-budget
qualification remain in their later design packets. L0's short CPU trace
windows, missing per-frame GPU tails and missing destruction/staging peaks
are explicit measurement limits, not performance passes.
