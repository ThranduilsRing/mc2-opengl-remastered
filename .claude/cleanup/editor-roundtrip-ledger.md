# Editor round-trip ledger (E-W)

Summary: 24 byte-identical, 0 lossless-only, 0 semantic; 0 blocked of 24.

Owner finding: dev `mc2-win64-v0.4` AND golden `mc2-win64-v0.4c` ship non-stock loose `data/art/Buildings.csv`, SHA-256 `86153dd5c4793672d35033ce4e8e90a871df8b1e131ccde7e8d61ab1cf083ae9`. Neither install was changed. The stock art.fst entry is `3a1b449d5e4336c7e2345ebd577181fcc7f9f41107cbc1ffa6da7e0ff9b6ebce`.

Dependency manifest: `scratch/editor-smoke/2026-10-13T14-20-40/dependencies.json`; SHA-256 `e0d9806424c5dddedef539b9293daca56036e68809cb779d967248e820b4f741`. Frozen exact-byte baseline: `tests/smoke/editor/roundtrip-baseline.json`; changes fail unless explicitly rebaselined.

Stock inputs come from mission.fst; all resolved archive entries and loose data are fingerprinted, and non-stock loose overrides are rejected. Each row loads the stock PAK through -mission and writes a separate PAK/FIT through -smoke-save (quick save). This proves the FIT writer and editor reload payload, not a full MOVE/tac-map export.

Diff size = unequal bytes at matching offsets + the absolute length difference. Byte equality is unmasked; the field census explains changes without blessing them.

Evidence: `scratch/editor-smoke/2026-10-13T14-20-40` (input FIT, written FIT, unified text diff, SHA-256 JSON and report).

Campaign representatives: group 0: mc2_01; group 1: mc2_02; group 2: mc2_03; group 3: mc2_04; group 4: mc2_05; group 5: mc2_06; group 6: mc2_10; group 7: mc2_11; group 8: mc2_14; group 9: mc2_18; group 10: mc2_19; group 11: mc2_23; group 12: mc2_24.

| Mission | Byte-identical yes/no | Diff size | Explanation |
|---|---|---:|---|
| mc2_01 | yes | 0 | Exact byte match. |
| mc2_02 | yes | 0 | Exact byte match. |
| mc2_03 | yes | 0 | Exact byte match. |
| mc2_04 | yes | 0 | Exact byte match. |
| mc2_05 | yes | 0 | Exact byte match. |
| mc2_06 | yes | 0 | Exact byte match. |
| mc2_07 | yes | 0 | Exact byte match. |
| mc2_08 | yes | 0 | Exact byte match. |
| mc2_09 | yes | 0 | Exact byte match. |
| mc2_10 | yes | 0 | Exact byte match. |
| mc2_11 | yes | 0 | Exact byte match. |
| mc2_12 | yes | 0 | Exact byte match. |
| mc2_13 | yes | 0 | Exact byte match. |
| mc2_14 | yes | 0 | Exact byte match. |
| mc2_15 | yes | 0 | Exact byte match. |
| mc2_16 | yes | 0 | Exact byte match. |
| mc2_17 | yes | 0 | Exact byte match. |
| mc2_18 | yes | 0 | Exact byte match. |
| mc2_19 | yes | 0 | Exact byte match. |
| mc2_20 | yes | 0 | Exact byte match. |
| mc2_21 | yes | 0 | Exact byte match. |
| mc2_22 | yes | 0 | Exact byte match. |
| mc2_23 | yes | 0 | Exact byte match. |
| mc2_24 | yes | 0 | Exact byte match. |

## Pinned before class comparison

Pinned commit: `73b111f33`. That record used a non-stock loose catalog; equal mission input hashes permit a class comparison, not a stock-environment claim. mc2_22 has no pinned comparison.

| Mission | Qualification | Disappeared classes | Remaining / introduced |
|---|---|---|---|
| mc2_01 | same input; historical non-stock catalog | Cameras, NavMarker coordinates, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_02 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_03 | same input; historical non-stock catalog | Cameras, Part CSV spelling, Part control/profile | - |
| mc2_04 | same input; historical non-stock catalog | Cameras, NavMarker coordinates, Objective ModelType, Part CSV spelling, Part control/profile | - |
| mc2_05 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_06 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_07 | same input; historical non-stock catalog | Cameras, Part CSV spelling, Part control/profile | - |
| mc2_08 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part control/profile | - |
| mc2_09 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part CSV value, Part control/profile | - |
| mc2_10 | same input; historical non-stock catalog | Cameras, Part CSV spelling, Part control/profile | - |
| mc2_11 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_12 | same input; historical non-stock catalog | Cameras, Part control/profile | - |
| mc2_13 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part control/profile | - |
| mc2_14 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_15 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_16 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_17 | same input; historical non-stock catalog | Cameras, NavMarker coordinates, Part CSV spelling, Part control/profile | - |
| mc2_18 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part control/profile | - |
| mc2_19 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV spelling, Part CSV value, Part control/profile | - |
| mc2_20 | same input; historical non-stock catalog | Cameras, NavMarker coordinates, Part CSV spelling, Part CSV value, Part control/profile | - |
| mc2_21 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part control/profile | - |
| mc2_22 | no pinned comparison | - | - |
| mc2_23 | same input; historical non-stock catalog | Cameras, Objective ModelType, Objective condition coordinates, Part CSV spelling, Part CSV value, Part control/profile | - |
| mc2_24 | same input; historical non-stock catalog | Cameras, Objective condition coordinates, Part CSV value, Part control/profile | - |

## Writer findings

### E-W authored-state repair

E0's 0/24 identical and 99 changed fields have become 24/24 identical and zero
changed, added or removed fields in the E-W census. Every mission has zero
differing bytes; there are no remaining differences to justify or defer.
The per-mission comparison above lists every disappeared pinned-before class;
all 23 recorded inputs have matching stock hashes, and mc2_22 has no pinned
before record. The pinned files were read only and remain unchanged.

The typed document values in `editor/AuthoredMissionState.h` are owned by each
Unit, objective and marker. They survive copies and undo. The mission camera
owns its authored zoom independently of viewport limits. Save consults these
values; it does not consult the catalog for stored control/profile/CSV or model
definition fields. Newly placed units and explicitly selected objective models
receive a catalog definition at authoring time.

| Rewrite class | Repair and writer provenance |
|---|---|
| Part control/profile | `Unit::load` retains ControlDataType and ObjectProfile. `editor/EditorObjects_unit.cpp:167` and `:168` write the authored values. |
| Part CSV spelling/value | CSVFile retains its exact case and value; `editor/EditorObjects_unit.cpp:169` writes it. Asset-provider profile/CSV relationships also use bounded document strings. |
| Objective ModelType | The complete ObjectiveModel/ModelType/ModelScale definition survives Read, assignment and Save, including unknown catalog models. `editor/Objective.cpp:565`, `:566` and `:572` write the authored definition. |
| Cameras | `editor/EditorCamera_document.cpp:82` and `:86` write authored NewScale/ZoomMax. Renderer initialization and viewport navigation do not own these mission values. Other camera controls retain their existing authoring surfaces. |
| NavMarker coordinates | FIT xPos/yPos/radius are read after binary building placement, matched to the marker by position. `editor/EditorObjects_pilot.cpp:530` and `:531` retain FIT precision until a transform changes that axis; radius is preserved too. Binary marker loads keep their authored free positions instead of applying brush snapping (`editor/EditorObjectMgr_io.cpp:166`), so transformed markers also save/reload losslessly. |
| Objective condition coordinates | Stored FIT target positions are distinct from resolved object positions. `editor/Objective_conditions.cpp:108` and `:109` (units), `:242` and `:243` (structures) preserve unchanged axes and follow intentional transforms. Replacing a target resets its authored reference. |

“Refresh mission definitions from catalog” is a named Command Palette command
(`editor/MissionDefinitionCommands.cpp:79`), with document-only snapshots for
all units/alternatives and the objectives. Undo/redo restores exact authored
definitions without re-baking unit visuals. “Use current zoom for mission
camera” (`:95`) explicitly authors zoom and has its own undo/redo snapshot.
Unit copies no longer reset VariantNumber after copying it; such a reset would
lose authored state during existing editor undo actions.

Strict readers reject absent/malformed required fields, trailing numeric junk,
non-finite coordinates, invalid model/radius bounds and malformed strings.
They emit one `[EDITOR_DOCUMENT]` rejection line and return false. Rejections
propagate through the Unit/objective collections and clear partial document
state, with an inactive viewport. Unresolved Part objects reject the load
instead of being silently omitted from a subsequent save. No static-duration
document or command owners were added.

The `MissionDefinitions` action-handler workflow deliberately authors
non-catalog ControlDataType/ObjectProfile/CSVFile, ObjectiveModel/ModelType/
ModelScale and camera zoom in a private fixture. It requires preserved save/
reload, coordinate transform/undo and marker transform/save/reload, both named commands with undo/redo,
refreshed save/reload, and seven malformed-content rejection controls.
The runner requires this eighth panel fact; absent or unsuccessful proof fails.
ObjectivesSummary and CampaignOverview remain honestly model-only.

### E-W proof commands and sequencing

- `py -3 tools/build_quiet.py --target EditRel`: final RelWithDebInfo build
  PASS, exit 0, 162.2 seconds, 51 compiler warnings reported by the quiet
  wrapper. The final build and private scratch executable both have SHA-256
  `ecbc1116c09386806f3a096f5a75759fee6d842e222e6d74c22c0fed86e55afb`.
  Editor builds finished before any editor launch; no editor relink occurred
  during a running editor process.
- `py -3 tools/build_contract_harnesses.py --run --unit`: final repeat PASS,
  20/20 contract harnesses, 2,794 doctest cases, zero unexpected failures.
  The printf corpus deliberately failing assertion is the existing negative
  control and doctest reports SUCCESS.
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py -q -p no:cacheprovider -p no:tmpdir`:
  PASS, 29 tests and 32 subtests. New controls require the definition workflow
  and distinguish equal-input pinned deltas, different inputs and missing
  mc2_22 evidence.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case workflows --case gen_save_load --keep-logs --exit-sec 3 --timeout 300`:
  PASS 3/3 in `scratch/editor-smoke/2026-10-13T13-54-49`. Eight panel facts
  green, and random terrain 5/size 0 generated, saved and reloaded. This run
  preceded the final marker free-position reload extension.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case workflows --case stock_roundtrip --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 300`:
  final pre-freeze census in `scratch/editor-smoke/2026-10-13T14-11-00`:
  all 24 comparisons are byte-identical and all eight workflow facts PASS.
  Marker transform/save/reload, coordinate transform/undo, both commands,
  authored/refreshed reload and all seven rejection controls report `ok=1`.
  Exit 1 and the 24 `roundtrip_baseline_changed` rows are EXPECTED: the
  unchanged E0 floor said `differs`. The workflow row passed; no flake or
  runtime failure was blessed. Only after this census and the pinned class
  comparison was the explicit rebaseline started.
- `git diff --check`: PASS; edited/new files retain LF.
- `py -3 scripts/check-background-launch.py`: FAIL solely for the existing
  stale `docs/background-launch-inventory.md`. No new launch-policy violation
  was reported. That generated inventory is outside this task's edit scope.
- `bash scripts/check-contracts.sh`: NOT RUN; Git Bash is forbidden by the
  task sandbox. The orchestrator must run the full board and refresh the
  generated inventory. Git index writes/staging/commits were not attempted.

Earlier red diagnostics are retained as diagnostics: the first definition
fixture parser confused `f[3]` with a section, then the nav reference probe
wrote a duplicate empty header. Both were corrected before the green final
workflow. `13-42-00` already measured 24/24 exact bytes but correctly failed
the unchanged baseline and the then-broken coordinate probe. Neither red
report was used to freeze the baseline.

### E-W baseline freeze and printed delta

After the complete final pre-freeze proof, the following command froze the
baseline with a second set of 24 fresh comparisons:

```text
py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case workflows --case stock_roundtrip --rebaseline --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 300
```

PASS 25/25, no flaky passes, in `scratch/editor-smoke/2026-10-13T14-20-40`.
All 24 inputs and outputs are byte-identical; all eight workflow panels are
green. Archive identities and the two stock-derived allowances did not change.
Each outcome changed from `differs` to `identical`; every output hash now equals
its unchanged stock input hash. No source change occurred between the final
pre-freeze proof and the freeze.

`py -3 scratch/verify_editor_frozen.py --evidence scratch/editor-smoke/2026-10-13T14-20-40`:
PASS. This transient verifier re-compared the actual FIT bytes, matched every
input to mission.fst, checked all 24 rows through the frozen gate without
rebaseline, graded all eight workflow panels and marker reload, and re-hashed
all eight archives. Final pytest remained 29 tests / 32 subtests green.

Exact printed delta:

```text
[REBASELINE] mc2_01: {"input_sha256": "95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958", "outcome": "differs", "output_sha256": "61ba4897aab8db9c9e7e07cbe2f78c06a21263d16bd6f642d16265e1ff724ffc"} -> {"input_sha256": "95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958", "output_sha256": "95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958", "outcome": "identical"}
[REBASELINE] mc2_02: {"input_sha256": "e860d257d76a3fa506d6eba24b0345d7f6b9fda4be588efd486b91e3fd8e1835", "outcome": "differs", "output_sha256": "377bad94295b402ef71b663a411ae27cd87770e96cd003fa4077acf441fbb93d"} -> {"input_sha256": "e860d257d76a3fa506d6eba24b0345d7f6b9fda4be588efd486b91e3fd8e1835", "output_sha256": "e860d257d76a3fa506d6eba24b0345d7f6b9fda4be588efd486b91e3fd8e1835", "outcome": "identical"}
[REBASELINE] mc2_03: {"input_sha256": "e874277bc7a6de6ed4e682f07c2cbb471a0daa5d80fa753df9004b842446e1e5", "outcome": "differs", "output_sha256": "3be94e32930aa742147fe8e42f688e4c36a2c08a04a8f490f6e929cdff33b76f"} -> {"input_sha256": "e874277bc7a6de6ed4e682f07c2cbb471a0daa5d80fa753df9004b842446e1e5", "output_sha256": "e874277bc7a6de6ed4e682f07c2cbb471a0daa5d80fa753df9004b842446e1e5", "outcome": "identical"}
[REBASELINE] mc2_04: {"input_sha256": "90ab1204542efcbb4c1b7bed277b9ebf4849123701ceb2238300b43481417a4d", "outcome": "differs", "output_sha256": "f76bbc113f65bb225f01098c6b81892ae98dde36ee7affd1f3f70b988ac99abd"} -> {"input_sha256": "90ab1204542efcbb4c1b7bed277b9ebf4849123701ceb2238300b43481417a4d", "output_sha256": "90ab1204542efcbb4c1b7bed277b9ebf4849123701ceb2238300b43481417a4d", "outcome": "identical"}
[REBASELINE] mc2_05: {"input_sha256": "d2d14790a3420b483a3a4b28e88853e16f67b6aafb6a8c5cecd715c68df23852", "outcome": "differs", "output_sha256": "61e33600a86c31e35dade90af8962fe321bc7cdae9c6ff207a48306459f7a1cd"} -> {"input_sha256": "d2d14790a3420b483a3a4b28e88853e16f67b6aafb6a8c5cecd715c68df23852", "output_sha256": "d2d14790a3420b483a3a4b28e88853e16f67b6aafb6a8c5cecd715c68df23852", "outcome": "identical"}
[REBASELINE] mc2_06: {"input_sha256": "9c13410dd984297d6b01ec55410280e61257bc594aa787eb6b0facb897dc9e2d", "outcome": "differs", "output_sha256": "c8c39c49784b008cbdf97b573f10fb8ea5be07461e5966609a144b12470eafdd"} -> {"input_sha256": "9c13410dd984297d6b01ec55410280e61257bc594aa787eb6b0facb897dc9e2d", "output_sha256": "9c13410dd984297d6b01ec55410280e61257bc594aa787eb6b0facb897dc9e2d", "outcome": "identical"}
[REBASELINE] mc2_07: {"input_sha256": "d4382286d80902cd25c63b48b98d81b1d7157a5cddd20e9f6a24afe102abfec8", "outcome": "differs", "output_sha256": "35cc79c438c836edc274c47aecacfa239ff3e482d0a6439f1e28831ce3b17af5"} -> {"input_sha256": "d4382286d80902cd25c63b48b98d81b1d7157a5cddd20e9f6a24afe102abfec8", "output_sha256": "d4382286d80902cd25c63b48b98d81b1d7157a5cddd20e9f6a24afe102abfec8", "outcome": "identical"}
[REBASELINE] mc2_08: {"input_sha256": "805e6e7e26e1790959daf1ced07eac448bd644204ca725c170dc463faff00392", "outcome": "differs", "output_sha256": "c1645372d532c93de5d9f15d71d1ebf807d052ce06fc8bc557f76057e20db915"} -> {"input_sha256": "805e6e7e26e1790959daf1ced07eac448bd644204ca725c170dc463faff00392", "output_sha256": "805e6e7e26e1790959daf1ced07eac448bd644204ca725c170dc463faff00392", "outcome": "identical"}
[REBASELINE] mc2_09: {"input_sha256": "6f5dc64190b090d871bef3932025e143a5fb5ca15fa7fd548ad2feeb59b02738", "outcome": "differs", "output_sha256": "9235f1a1147a83bc58c32a58fb8670cfe9420cfadb8d731f3b1a3251f4188365"} -> {"input_sha256": "6f5dc64190b090d871bef3932025e143a5fb5ca15fa7fd548ad2feeb59b02738", "output_sha256": "6f5dc64190b090d871bef3932025e143a5fb5ca15fa7fd548ad2feeb59b02738", "outcome": "identical"}
[REBASELINE] mc2_10: {"input_sha256": "885244171387ccdf64f1414de2d2009ac67bf35002fb7fe689342f1d0f9c656b", "outcome": "differs", "output_sha256": "721cad800e23fc49eac001dffc4ad22936ae96455d16e69ecac4b7cbdee6b6b9"} -> {"input_sha256": "885244171387ccdf64f1414de2d2009ac67bf35002fb7fe689342f1d0f9c656b", "output_sha256": "885244171387ccdf64f1414de2d2009ac67bf35002fb7fe689342f1d0f9c656b", "outcome": "identical"}
[REBASELINE] mc2_11: {"input_sha256": "505afc9cd374a99e35eb5971b8307d6ff5ae6b34ad9462152fe442da7066e004", "outcome": "differs", "output_sha256": "715c623834040d807d067c8e25fa4fc123fabf667ad6bbc3c44e31f07a43c450"} -> {"input_sha256": "505afc9cd374a99e35eb5971b8307d6ff5ae6b34ad9462152fe442da7066e004", "output_sha256": "505afc9cd374a99e35eb5971b8307d6ff5ae6b34ad9462152fe442da7066e004", "outcome": "identical"}
[REBASELINE] mc2_12: {"input_sha256": "e1634e979f32ee6b40cb9794a96132c4bb417a4e2424f3a4978c7457dce9d9b2", "outcome": "differs", "output_sha256": "06eca63c51243d0c63a6b739fc8c3402e51dd6130447707409fb1ef960a78e91"} -> {"input_sha256": "e1634e979f32ee6b40cb9794a96132c4bb417a4e2424f3a4978c7457dce9d9b2", "output_sha256": "e1634e979f32ee6b40cb9794a96132c4bb417a4e2424f3a4978c7457dce9d9b2", "outcome": "identical"}
[REBASELINE] mc2_13: {"input_sha256": "3640d3fd1b71cea70a9662fef494665706a41f89aee6cb27708e0b72dfe4ac83", "outcome": "differs", "output_sha256": "4178a55985b16097e7122ff681016415866f837546215cfce1befd8d326fc93f"} -> {"input_sha256": "3640d3fd1b71cea70a9662fef494665706a41f89aee6cb27708e0b72dfe4ac83", "output_sha256": "3640d3fd1b71cea70a9662fef494665706a41f89aee6cb27708e0b72dfe4ac83", "outcome": "identical"}
[REBASELINE] mc2_14: {"input_sha256": "7518d12bfa2d0493f7d691e40e235c61f914d7cba501ef566387f214c817e306", "outcome": "differs", "output_sha256": "ff6e90c9822251aec15a971601321a2d51a3a805c158bd1dce0728d4ea0ab909"} -> {"input_sha256": "7518d12bfa2d0493f7d691e40e235c61f914d7cba501ef566387f214c817e306", "output_sha256": "7518d12bfa2d0493f7d691e40e235c61f914d7cba501ef566387f214c817e306", "outcome": "identical"}
[REBASELINE] mc2_15: {"input_sha256": "87fe8f51ed0a78d5663ac65117bccf5da89eae6a66f94846172553e8c7fd1a53", "outcome": "differs", "output_sha256": "cf6dcf9d46c634b2c863889867fe5efa5f39c2ee92ee70d8fac5fd6a56649d31"} -> {"input_sha256": "87fe8f51ed0a78d5663ac65117bccf5da89eae6a66f94846172553e8c7fd1a53", "output_sha256": "87fe8f51ed0a78d5663ac65117bccf5da89eae6a66f94846172553e8c7fd1a53", "outcome": "identical"}
[REBASELINE] mc2_16: {"input_sha256": "bb10f379df7f4619dc2d9a72fd8604ccfa90a5cb57fedf394b87977f4220cf42", "outcome": "differs", "output_sha256": "ab6f8b783d486159e5b04ee1c659f2f509f91fff05b1bdae080f56cde2183ab9"} -> {"input_sha256": "bb10f379df7f4619dc2d9a72fd8604ccfa90a5cb57fedf394b87977f4220cf42", "output_sha256": "bb10f379df7f4619dc2d9a72fd8604ccfa90a5cb57fedf394b87977f4220cf42", "outcome": "identical"}
[REBASELINE] mc2_17: {"input_sha256": "57931cc53ce9beadf4931d9853d2165be0d5b236017311d2f0c4b954d8ccd46b", "outcome": "differs", "output_sha256": "60d88ca6b318243f2d414de8f8b661722e094afa7fedfe2651d33b1e3c31c0f2"} -> {"input_sha256": "57931cc53ce9beadf4931d9853d2165be0d5b236017311d2f0c4b954d8ccd46b", "output_sha256": "57931cc53ce9beadf4931d9853d2165be0d5b236017311d2f0c4b954d8ccd46b", "outcome": "identical"}
[REBASELINE] mc2_18: {"input_sha256": "c87c7b91cd5c17f519d13e544e0c10e20642db8a5eeeb333a5440fab337d9dfe", "outcome": "differs", "output_sha256": "2c7a302c3936198374c221b480e7129b2e3aec2cf03a83519e14cdfd6359dbba"} -> {"input_sha256": "c87c7b91cd5c17f519d13e544e0c10e20642db8a5eeeb333a5440fab337d9dfe", "output_sha256": "c87c7b91cd5c17f519d13e544e0c10e20642db8a5eeeb333a5440fab337d9dfe", "outcome": "identical"}
[REBASELINE] mc2_19: {"input_sha256": "24c300ccb0c33da5db55852b37ac188d9426672599154ea04d3eb926661150e7", "outcome": "differs", "output_sha256": "d001cb586f3cbc7591d8c97b7a26ae274e6f6d423ed32a41feaf065cccbd8e39"} -> {"input_sha256": "24c300ccb0c33da5db55852b37ac188d9426672599154ea04d3eb926661150e7", "output_sha256": "24c300ccb0c33da5db55852b37ac188d9426672599154ea04d3eb926661150e7", "outcome": "identical"}
[REBASELINE] mc2_20: {"input_sha256": "bd1b849dca7bb080cf7ef6912f608219f292fe5e911e9b3fa8f66daca58107b4", "outcome": "differs", "output_sha256": "56d90837e89a0de3a52a76788a708af531324089804c10e47977505c2a55553e"} -> {"input_sha256": "bd1b849dca7bb080cf7ef6912f608219f292fe5e911e9b3fa8f66daca58107b4", "output_sha256": "bd1b849dca7bb080cf7ef6912f608219f292fe5e911e9b3fa8f66daca58107b4", "outcome": "identical"}
[REBASELINE] mc2_21: {"input_sha256": "0a2937d89192f5f34073ab8ead1a93a1504493bc99a503bc8d8bbd04a960e85d", "outcome": "differs", "output_sha256": "3cd65088b130453694ed40874705ed306f44c6066276b1a8f07edd28291a1102"} -> {"input_sha256": "0a2937d89192f5f34073ab8ead1a93a1504493bc99a503bc8d8bbd04a960e85d", "output_sha256": "0a2937d89192f5f34073ab8ead1a93a1504493bc99a503bc8d8bbd04a960e85d", "outcome": "identical"}
[REBASELINE] mc2_22: {"input_sha256": "a916c6164543866bb71082e698c0c8c0c839673a748a99daef89fe64dd50ed1b", "outcome": "differs", "output_sha256": "cae85974b619a88d082aa97ec27a576ffd6db1b16e11d2fcc4cd21754e5a1413"} -> {"input_sha256": "a916c6164543866bb71082e698c0c8c0c839673a748a99daef89fe64dd50ed1b", "output_sha256": "a916c6164543866bb71082e698c0c8c0c839673a748a99daef89fe64dd50ed1b", "outcome": "identical"}
[REBASELINE] mc2_23: {"input_sha256": "91df83a6103fe93d54bfb7df03c06dd5151cbee71062994da221af48d429cfbd", "outcome": "differs", "output_sha256": "1b9855d8c36e469174f4bad01174173de4f60b6d7b3b77909baa4f0767ae31be"} -> {"input_sha256": "91df83a6103fe93d54bfb7df03c06dd5151cbee71062994da221af48d429cfbd", "output_sha256": "91df83a6103fe93d54bfb7df03c06dd5151cbee71062994da221af48d429cfbd", "outcome": "identical"}
[REBASELINE] mc2_24: {"input_sha256": "d554a3948e9df993c19baec80868fbb03bff835f471096b467c58d121ed10341", "outcome": "differs", "output_sha256": "139c0868586ad5fb636f602da1fb63bb9113d49d8420ed5982e4ea35c4825c33"} -> {"input_sha256": "d554a3948e9df993c19baec80868fbb03bff835f471096b467c58d121ed10341", "output_sha256": "d554a3948e9df993c19baec80868fbb03bff835f471096b467c58d121ed10341", "outcome": "identical"}
```

### Historical E0 findings at 254751f58 (superseded by E-W)

The E0 stock rerun changed 99 fields: 48 camera fields, 4 NavMarker coordinates
and 47 objective-condition coordinates. There are no added or removed keys.
All three classes are semantic value changes; different decimal coordinates
are not identical values. Mission writers were not repaired.

| Class | Fields | Representative input → written | Writer and cause |
|---|---:|---|---|
| Cameras | 48 | mc2_01 `[Cameras]` `f NewScale = 0.444542` → `f NewScale = 0.821212` (also `ZoomMax` 1 → 2) | `editor/EditorData_save.cpp:770` calls `Camera::save`; `mclib/camera.cpp:797` and `:801` emit live scale/zoom. Initialization resets maximum zoom at `mclib/camera.cpp:350` and derives scale from altitude/pitch at `:478`. |
| NavMarker coordinates | 4 | mc2_01 `[NavMarker0]` `f yPos = -789.333313` → `f yPos = -789.333496` | `editor/EditorObjects_pilot.cpp:515` writes live appearance position. `editor/EditorObjectMgr_io.cpp:698` deliberately omits FIT NavMarker loading; the binary building record is loaded and snapped to a cell center by `editor/EditorObjectMgr.cpp:626`. |
| Objective condition coordinates | 47 | mc2_01 `[Team0Objective1Condition0]` `f PositionY = -277.333344` → `f PositionY = -277.333496` | `editor/Objective_conditions.cpp:223` resolves a structure by position; `:242` and `:243` write the resolved structure's coordinates. Unit conditions similarly write live unit coordinates at `:110` and `:111`. The original decimal value is not preserved. |

### Historical E0 catalog findings before the writer repair

`Unit::save` re-derives Part ControlDataType/ObjectProfile from the current
appearance/catalog at `editor/EditorObjects_unit.cpp:79-86`, then writes those
values at `:172-173`; `Unit::load` does not preserve the stored control/profile.
`CObjective::Save` resolves ModelID through the current catalog at
`editor/Objective.cpp:543`, derives ModelType at `:548`, and writes it at `:559`.
The stored ModelType is not retained. These are writer preservation defects
even though the stock catalog produces no control/profile or ModelType change
in this rerun. The previous overlay's vehicle→mech changes are disqualified
as stock proof. CSVFile is also taken from the current catalog at
`editor/EditorObjects_unit.cpp:175-176`; the stock rerun has no CSV change.

The stronger UnitBrain probe exposed an editor-action undo defect: its snapshot
omitted orderAuthored, leaving persistence ownership changed after undo. The
snapshot now captures/restores that flag (`editor/Action.h:209`,
`editor/Action.cpp:647-678`); the probe verifies the previous ownership flag,
order type, stance and waypoint count, then saved/reloaded ownership and values.
This action repair does not alter the catalog-dependent mission writer findings.

### Stock environment and reproduction

The private runtime is `scratch/editor-e0-install`. All eight configured FSTs
and all 7,721 resolved archive entries are SHA-256 fingerprinted. Its 7,722
loose files are stock archive bytes except the two stock-derived files below.
The manifest records each loose path, loose hash, archive container hash and
archive-entry hash (or null when no archive entry exists). Unknown loose data
or a differing shadow fails before launch unless its exact hash has a reason.
The runner clears mod selection and disables launcher JSON overrides.

- `data/objects/object2.pak`: required loose stock-derived PacketFile, absent
  from FSTs. SHA-256 `dda3fd6c8e03e21fb88dd409fba3afd355f2f58fd34c9801ea9ab17a1f0d2488`.
  All 1,188 decoded packets match `mc2srcdata-fresh/objects/object2.pak`
  (SHA-256 `eb46be8f57acb49f52d8951af25725fd50a21f55ec66273c2be6e657bebaec28`,
  also byte-identical in mc2-srcdata); only packet compression differs.
  Checked with `py -3 scratch/inspect_editor_object_pak.py`: zero changed packets.
- `data/objects/badpilots.csv`: SHA-256
  `d7d84d0f79afd52c6f73072f41be8c7203a72b53c3cd018a9548b77487ea9fd8`.
  Stock misc.fst carries 44 compressed bytes and declares 44 uncompressed bytes,
  so the generic equal-size extraction returns the zlib payload. Decoding it
  yields the original four-name, 44-byte stock pilot catalog, byte-identical
  to mc2srcdata-fresh/objects/BadPilots.csv. No gameplay value was authored.

Both hash-pinned reasons are adopted in the baseline's stock_allowlist.
`--prepare-stock-scratch` removes/replaces overlays ONLY below this worktree's
`scratch/` runtime, records changes in preparation.json, and materializes stock
entries for direct-file and directory-enumeration consumers. It cannot touch
the dev or golden installs. The original loose tree remains preserved at
`scratch/editor-e0-overlay-backup`. Runtime-generated TXM/LST caches are removed
before the next proof; they are not silently exempted by the dependency audit.

Baseline creation used the full suite with `--rebaseline --prepare-stock-scratch`
and the two explicit stock-derived allowances. Reproduce the frozen gate with:

```text
py -3 scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --keep-logs --exit-sec 3 --timeout 180
```

### Headless warning and workflow evidence

mc2_22 has 78 turrets. Its existing limit warning now honors g_cliSuppressModals
only: the default IDOK is taken and one warning fact names
`IDS_TOO_MANY_TURRETS default=IDOK`. Normal sessions still call AfxMessageBox.
The log appears twice in combined evidence because stderr and startup.log
carry the same IPC fact. mc2_22 writes a real comparison: 13 differing bytes,
four changed fields (two cameras and two objective-condition coordinates).

The workflow row invokes five production action paths: the material brush,
the SAME BuildingBrush activated by AssetBrowser, Inspector alignment,
UnitBrain waypoint placement, and mission quick-save/reload. Authored values
are checked, undo restores their preceding values, redo authors them again,
and a saved mission is reloaded after undo. Reload checks material, object ID
and position, team, order type, waypoint count and waypoint coordinates.

Objectives and campaign links remain explicitly `coverage=model-only reached=0`.
Their authored models Save/Read equal; snapshot restoration verifies model
undo. The read-only summaries do not become panel reaches by being opened.
The parser requires coverage, reach, authored, undo, saved and reloaded facts
and rejects absent, conflicting or unsuccessful facts. Inventory rows state
the same limits; none of the 37 legacy MFC dialogs is claimed instantiated.

### Artifact decisions

| Artifact | Decision and reason |
|---|---|
| `tests/smoke/editor/roundtrip-baseline.json` | ADOPT: all 24 stock input/output SHA-256 pairs and identical/differs/blocked outcomes, archive identities and two stock-derived allowances. E2's exact written-FIT floor, no masks; missing comparisons or any change fail. Only explicit --rebaseline may rewrite it, after all 24 successful comparisons, printing each change. |
| `tests/smoke/editor/old-editor-2026-10-13/.gitattributes` and `report.md` | RETAIN: orchestrator adopted them in 73b111f33 during this redo as historical before evidence. They describe the rejected non-stock environment and are not the frozen stock baseline. |
| `tests/smoke/editor/old-editor-2026-10-13/mc2_{01..21,23,24}.{written.fit,json,diff}` (69 files) | RETAIN: same historical bundle, qualified as rejected non-stock proof; no mc2_22 comparison exists in it. No file in this bundle was edited or deleted by this redo. |
| Earlier untracked report directories `2026-10-12T10-28-51`, `2026-10-12T17-58-12`, `2026-10-13T00-46-32` | Already absent at task start. This redo cannot reconstruct their individual prior deletion list. Historical lane2-state.md references remain outside the allowed paths; owner must reconcile them. |
| `scratch/editor-smoke/`, preparation/inspection helpers and original overlay backup | TRANSIENT: ignored evidence and reproducibility scratch, not adopted fixtures. Full resolved dependencies and comparison artifacts remain in the run directory named above. |

No tracked file was deleted by this redo. No engine code or mod/dev/golden
install was edited. Commits/index writes and Git Bash are forbidden here;
the orchestrator stages/commits the proposed paths and runs the full Bash board.

### Proof commands and results

- `py -3 tools/build_quiet.py --target EditRel`: final RelWithDebInfo rebuild
  PASS, exit 0, zero warnings, 42.5 seconds. A prior concurrent relink failed
  with LNK1201 writing the PDB while the editor was running; retry after the
  smoke exited succeeded. Copied only to the private scratch runtime. Build
  and scratch executable SHA-256 both equal
  `c61f21ba55d537c5f1badebcdeed15c4cff31684bfcd058d4da479d56ff908bf`.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --stock-allowlist scratch/editor-stock-allowlist.json --prepare-stock-scratch --rebaseline --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 180`:
  initial adoption PASS 40/40 in `2026-10-13T12-29-18`; printed all 24 new
  baseline pairs and archive identities. This was explicit baseline creation.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --keep-logs --exit-sec 3 --timeout 180`:
  frozen verification PASS 25/25 in `2026-10-13T12-43-22`, without rebaseline.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 180`:
  FINAL rebuilt editor PASS 40/40 in `2026-10-13T12-52-11`, no retries.
  Original rows 15/15, stock rows 24/24 exact frozen pairs, workflow row 1/1.
  All three successful runs have the SAME dependency manifest hash reported
  at the top. Final preparation recorded 3,934 derived caches/generated loose
  files removed/replaced in that run's preparation.json, before auditing.
- `py -3 tools/build_contract_harnesses.py --run --unit`: final PASS 20/20
  harnesses and 2,794 doctest cases (one expected-failure control, one skipped).
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py -q -p no:tmpdir -p no:cacheprovider`:
  final PASS 27 tests and 28 subtests. Negative controls cover stock catalog
  shadows, unknown metadata, reason/hash-pinned allowances, scratch boundary,
  stale intermediate saves, missing comparisons, input/output/outcome/archive
  baseline drift, malformed baseline JSON, model-only reach and absent facts.
- `py -3 scripts/check-editor-game-render-context-parity.py --quiet` and
  `py -3 scripts/check-editor-pass-contract-parity.py --quiet`: PASS, exit 0.
- `py -3 scripts/check-background-launch.py`: FAIL only because
  `docs/background-launch-inventory.md` is stale. Its regeneration is outside
  this task's allowed paths; no new launch-policy violation was reported.
- `git diff --check`: clean. Edited tracked files and the new source/test/JSON
  fixture retain LF. No engine source was changed.
- `bash scripts/check-contracts.sh`: NOT RUN; user forbids Git Bash in this
  sandbox. Full board proof and the stale generated inventory refresh remain
  with the orchestrator. The existing scripts_tests row collects these tests;
  no board registry was changed. Git add/commit/rm/stash were NOT run because
  git index writes are forbidden; the baseline fixture awaits staging.

Early diagnostics remain red in scratch reports: `12-15-24` lacked the
pre-archive effects bootstrap, `12-16-57`, `12-17-36` and `12-22-36` lacked
the required loose Object2 PacketFile; `12-24-41` reached workflows and a
real mc2_22 comparison but correctly failed the missing baseline; `12-27-07`
rejected unrecognized generated caches. None was counted as final proof.
