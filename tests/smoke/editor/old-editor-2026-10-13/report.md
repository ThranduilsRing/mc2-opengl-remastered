# Editor smoke 2026-10-13T11-19-02  result=FAIL (38/40)

Round-trip findings: 23. PASS counts performed comparisons, including recorded differences.

| Case | Result | Bucket | rc | sec | foliage_count |
|------|--------|--------|----|----|---------------|
| gen_map_basic | PASS | - | 0 | 65.1 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| foliage_present | PASS | - | 0 | 25.2 | 73 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| foliage_missing | PASS | - | 0 | 24.2 | 0 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| foliage_garbage | PASS | - | 0 | 24.0 | 0 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| foliage_menu_commands | PASS | - | 0 | 21.9 | 73 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| outliner | PASS | - | 0 | 24.0 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| inspector | PASS | - | 0 | 23.8 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| validate | PASS | - | 0 | 23.4 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| inspector_edit | PASS | - | 0 | 24.2 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| asset_browser | PASS | - | 0 | 24.3 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| gameplay_debugger | PASS | - | 0 | 24.7 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| undo_history | PASS | - | 0 | 25.3 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| place_oob | PASS | - | 0 | 24.7 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| gen_save_load:gen+save | PASS | - | 0 | 30.7 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| gen_save_load:load | PASS | - | 0 | 18.0 | -1 |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_01 | FINDING | - | 0 | 22.6 | -1 |
|  | | FINDING: changed 41 fields in Cameras, NavMarker0, Part10, Part11, Part14, Part15, Part16, Part17, Part18, Part19, Part20, Part21, Part22, Part25, Part26, Part27, Part29, Team0Objective1Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_02 | FINDING | - | 0 | 22.7 | -1 |
|  | | FINDING: changed 53 fields in Cameras, Part10, Part12, Part13, Part14, Part15, Part20, Part21, Part22, Part23, Part30, Part34, Part35, Part36, Part41, Part42, Part43, Part44, Part45, Part46, Part8, Part9, Team0Objective4Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_03 | FINDING | - | 0 | 21.3 | -1 |
|  | | FINDING: changed 18 fields in Cameras, Part14, Part16, Part27, Part28, Part34, Part35. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_04 | FINDING | - | 0 | 22.3 | -1 |
|  | | FINDING: changed 66 fields in Cameras, NavMarker0, Part14, Part20, Part23, Part25, Part29, Part32, Part33, Part34, Part38, Part39, Part40, Part57, Part58, Part59, Part60, Part61, Part62, Part63, Part64, Part65, Part66, Part67, Part68, Part69, Part71, Part72, Part73, Team0Objective1. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_05 | FINDING | - | 0 | 22.8 | -1 |
|  | | FINDING: changed 48 fields in Cameras, Part10, Part11, Part12, Part13, Part18, Part20, Part21, Part22, Part24, Part25, Part27, Part28, Part29, Part30, Part8, Part9, Team0Objective0Condition0, Team0Objective2Condition0, Team0Objective3Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_06 | FINDING | - | 0 | 50.2 | -1 |
|  | | FINDING: changed 50 fields in Cameras, Part11, Part13, Part14, Part15, Part16, Part17, Part22, Part23, Part24, Part25, Part37, Part38, Part39, Part40, Part47, Part49, Part8, Team0Objective3Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_07 | FINDING | - | 0 | 71.4 | -1 |
|  | | FINDING: changed 76 fields in Cameras, Part10, Part11, Part12, Part13, Part14, Part15, Part16, Part18, Part23, Part24, Part33, Part34, Part35, Part36, Part37, Part38, Part39, Part4, Part40, Part41, Part42, Part43, Part44, Part45, Part46, Part47, Part48, Part5, Part50, Part51, Part6, Part7, Part8, Part9. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_08 | FINDING | - | 0 | 41.6 | -1 |
|  | | FINDING: changed 14 fields in Cameras, Part53, Part54, Part55, Part56, Part57, Team0Objective0Condition0, Team0Objective2Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_09 | FINDING | - | 0 | 31.3 | -1 |
|  | | FINDING: changed 42 fields in Cameras, Part10, Part11, Part15, Part16, Part21, Part22, Part23, Part32, Part33, Part37, Part48, Part60, Team0Objective1Condition0, Team0Objective1Condition1, Team0Objective1Condition3, Team0Objective2Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_10 | FINDING | - | 0 | 23.5 | -1 |
|  | | FINDING: changed 38 fields in Cameras, Part10, Part34, Part35, Part36, Part37, Part38, Part39, Part40, Part41, Part42, Part43, Part44, Part45, Part46, Part47, Part48, Part9. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_11 | FINDING | - | 0 | 24.8 | -1 |
|  | | FINDING: changed 10 fields in Cameras, Part38, Team0Objective0Condition4, Team0Objective3Condition0, Team0Objective4Condition0, Team0Objective6Condition0, Team0Objective6Condition1. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_12 | FINDING | - | 0 | 22.5 | -1 |
|  | | FINDING: changed 62 fields in Cameras, Part26, Part27, Part28, Part38, Part39, Part40, Part41, Part49, Part50, Part51, Part52, Part53, Part54, Part60, Part61, Part62, Part63, Part64, Part73, Part74, Part75, Part77, Part78, Part79, Part80, Part81, Part82, Part83, Part84, Part85. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_13 | FINDING | - | 0 | 21.9 | -1 |
|  | | FINDING: changed 60 fields in Cameras, Part10, Part11, Part12, Part13, Part14, Part15, Part16, Part17, Part18, Part19, Part35, Part36, Part37, Part38, Part39, Part40, Part41, Part42, Part43, Part44, Part45, Part46, Part47, Part48, Part49, Part7, Part8, Team0Objective0Condition1, Team0Objective1Condition0, Team0Objective2Condition0, Team0Objective3Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_14 | FINDING | - | 0 | 24.1 | -1 |
|  | | FINDING: changed 11 fields in Cameras, Part14, Part33, Team0Objective2Condition0, Team0Objective6Condition0, Team0Objective7Condition0, Team0Objective8Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_15 | FINDING | - | 0 | 22.7 | -1 |
|  | | FINDING: changed 42 fields in Cameras, Part23, Part24, Part27, Part28, Part30, Part31, Part38, Part39, Part41, Part42, Part59, Part60, Part61, Part62, Team0Objective3Condition0, Team0Objective4Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_16 | FINDING | - | 0 | 29.3 | -1 |
|  | | FINDING: changed 35 fields in Cameras, Part15, Part18, Part20, Part23, Part26, Part27, Part30, Part31, Part35, Part45, Part46, Part48, Part49, Team0Objective0Condition0, Team0Objective5Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_17 | FINDING | - | 0 | 24.1 | -1 |
|  | | FINDING: changed 23 fields in Cameras, NavMarker0, Part43, Part44, Part45, Part46, Part47, Part48, Part49, Part50. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_18 | FINDING | - | 0 | 23.4 | -1 |
|  | | FINDING: changed 37 fields in Cameras, Part21, Part22, Part23, Part24, Part37, Part38, Part39, Part44, Part49, Part56, Part57, Part58, Team0Objective3Condition0, Team0Objective4Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_19 | FINDING | - | 0 | 22.3 | -1 |
|  | | FINDING: changed 41 fields in Cameras, Part30, Part31, Part41, Part42, Part43, Part48, Part49, Part50, Part52, Part53, Part85, Part86, Team0Objective4Condition0, Team0Objective6Condition0, Team0Objective7Condition0, Team0Objective7Condition2, Team0Objective7Condition4, Team0Objective8Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_20 | FINDING | - | 0 | 22.3 | -1 |
|  | | FINDING: changed 50 fields in Cameras, NavMarker0, Part30, Part31, Part32, Part33, Part44, Part45, Part52, Part58, Part59, Part60, Part71, Part72, Part73, Part74, Part75, Part76, Part77, Part78, Part79. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_21 | FINDING | - | 0 | 23.3 | -1 |
|  | | FINDING: changed 9 fields in Cameras, Part18, Team0Objective1Condition0, Team0Objective1Condition1, Team0Objective2Condition0, Team0Objective2Condition2, Team0Objective4Condition0. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_22 | FAIL | timeout | 1 | 180.1 | - |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_23 | FINDING | - | 0 | 30.9 | -1 |
|  | | FINDING: changed 69 fields in Cameras, Part10, Part11, Part12, Part14, Part19, Part22, Part33, Part34, Part35, Part46, Part47, Part48, Part49, Part50, Part51, Part52, Part53, Part54, Part8, Part83, Part84, Part85, Part9, Team0Objective2Condition0, Team0Objective3. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| stock_roundtrip:mc2_24 | FINDING | - | 0 | 37.3 | -1 |
|  | | FINDING: changed 39 fields in Cameras, Part13, Part14, Part36, Part37, Part38, Part39, Part40, Part41, Part42, Part43, Part58, Part59, Part60, Part61, Part62, Part69, Part70, Team0Objective0Condition1. Full keys and text diff in evidence. | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
| workflows | FAIL | workflow_probe_failed | 0 | 32.6 | -1 |
|  | | terrain_paint/PaintMaterial: missing or unsuccessful probe | | | |
|  | | `[CONFIG v1] summary total=980 set=8 nondefault=8 unregistered=0 badvalue=0 premain_stale=0 badvalue_unverified=0` | | | |
