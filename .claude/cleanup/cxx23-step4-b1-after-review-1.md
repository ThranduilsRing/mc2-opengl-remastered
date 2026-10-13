<!-- doc-status: current | reviewed:2026-10-13 -->
# C++23 step 4 B1: adversarial after-review RA1 of B1.0-B1.3 (`afa99eaf6..fb1eea32c`)

VERDICT: **ACCEPT-WITH-FIX.** The batches follow the before-review (`cxx23-step4-b1-review.md`, dbb25604a) where it matters. Every identity compare
on a data name is `mc2::str::iequals`, and the one fold site that fires on stock data (`PMWChopper`/`pmwchopper`) is converted. Accessors return
`const std::string&`, readers return optionals, and writers always write their key. No wrapper and no new static-duration owner were added. The
remapped call-site count (381) equals the fresh deprecation census (381). Nothing needs a revert. One proof is weaker than its text says: the save
noise set is defined by record CLASS, and at byte level the cross-build MechWarriorData difference is NOT inside the run-to-run noise (F1). The
logistics writers and readers changed by B1.3 have no executed proof yet (F2). Read: the range's code and docs, notes sections 1-10 and 8b,
lane1-state, both Sonnet reviews, `mc2str.h`, `objective_text.*`, `objective_flags.*`, the B1.3a/b diffs, the gui bridge, `team.cpp`, the
tests, `analyze_dangling.py`, `estring_callsites.py`. Measured (read-only, no build or launch): byte offsets over the lane's own saves in
`%TEMP%/b1save/{old_*,f_*,g_*,h_*}/save1.ims` through `tools/save_inspect.load`.

Numbering: the lane swapped my B1.1 and B1.2 (lane B1.1 = objectives, B1.2 = texture cache) and split B1.3 by header (3a variant+mech, 3b pilot).
This review uses the lane's numbers.

## 1. R1-R12
| R | status | evidence / judgement |
|---|---|---|
| R1 H1 | applied | Notes H1 row corrected. B1.1 resolved `m_modelName` (the writer always writes it). B1.3 had no null-test sites; the `Length()` override tests became `empty()`. The `getCurrent*` null-tests are deferred to B1.4 with a design (section 7) |
| R2 census | applied | 5e02d2347 deprecation build -> `cxx23-step4-b1-callsites.txt` (the file name differs from mine; fine). The `estring_callsites --check` board row (F7) and `--remap` are in. The fresh exact census at fb1eea32c (381/56) equals the remapped count after 3b (381), so the remap drops nothing it should keep. Keep the exact census at the end of every batch: `--check` cannot see a NEW EString use |
| R3 H13-H15 | applied | `test_estring.cpp` pins H1-H14 on the old class. `test_objective_text.cpp` pins the new reader/writer rules. Sonnet F11's caveats stand (the H12 pin only runs when `GetACP()==1252`; the corpus is file names only) |
| R4 | applied | The sentence is deleted. Notes 6.2 states that objectives and the deployed force feed mission logic |
| R5 | applied | The order changed and B1.3 was split. The cap counts edited files: 21 for 3a, 13 for 3b. The mp screens stay KEPT for B6 (owner) |
| R6 save proof | **partial** | The method is old exe vs new exe. The noise set is a list of packet classes, and the lane explained offsets only for MechData/GroundVehicleData (the vptrs at 2952/3000). At byte level the cross-build MechWarriorData diff is NOT a subset of the within-exe noise: F1 |
| R7 | not due | The editor proof plan is in notes 6.4. It applies at B1.6/B1.7 |
| R8 | applied | There is no printf-template formatter. `std::format` is used in `objective_flags.cpp` |
| R9 bridge | applied, improved | `aText`/`aTextListItem` get `const char*` + `const std::string&`, `aEdit::entry()` returns `std::string` through `from_c`, and `setEntry` gets the pair. No `string_view` overload, no `EString::view()`, and no `EString::str()` either: `entry()` makes it unneeded, which is better than R9 asked. `test_gui_bridge.cpp` covers literal, `const char*`, `std::string`, EString and `0L` |
| R10 lifetime gate | applied, deviation accepted | See section 2 |
| R11 cadence | applied | This is RA1 |
| R12 probe | **partial** | It ran on tier2 x4, five mechbay-harness screens (auto-win of campaign mission 1), the mechlab boot and a quickload. It did NOT run a campaign start from the main menu, a campaign save/load, or the editor `-mission` load. The editor leg is due before B1.6 (condition G6). See section 3 |

## 2. The lifetime gate is clang-cl only: acceptable, and what it misses
Accepted. The seeded file proves that clang-cl catches all four shapes on this host and that the clean twin is silent. `cl /analyze` caught none
of them, and a gate that catches nothing protects nothing. The leg that mattered in R10 was "trust only proven shapes", and the script enforces
it (an exact tag set, exit 2 on a TU it could not parse). C6 removes the main hazard class (by-value accessors over COW) by construction.
**What the gate cannot see** (each one needs review or a run):
(a) A pointer into a LIVE member that the owner's later mutation invalidates (`p = v.getName().c_str(); v.setName(x); use(p)`). There is no
temporary, so nothing fires.
(b) Pointers that escape through a return or an out-array: `getBestPilot` returns `const char*` into the pilot list, which three callers
`(char*)`-cast; `getPlayerVariantNames` fills an array. These are safe today only because `pilots`/`variants` are linked lists that never
relocate.
(c) A pointer into the SSO buffer of an object that is later copied or moved. `PILOT_LIST` is `EList<LogisticsPilot, const LogisticsPilot&>`
by value, so a pointer taken from a source pilot before `Append` refers to the source and not to the stored copy.
(d) TUs that are not passed to `--check` (only edited TUs are), and code under inactive `#if` branches.
B1.4 must list (b)-type sites in its commit (C8). It must also turn `getBestPilot` into a `const std::string&` return, because writing through the
`(char*)` cast would now be UB on a `std::string`.

## 3. The probe's "exactly one fold-only site on stock data"
The probe's coverage is ENOUGH for the EQUALITY sites of B1.0-B1.3. On all-ASCII input, `iequals` equals the old `lstrcmpi` equality by
construction, and the corpus test confirms it on 7046 names and their case variants. An unreached equality site therefore cannot change behaviour
on stock data. The remaining differences are non-ASCII names (a cp1252 letter folded under `lstrcmpi` and no longer does) and `CompareString`
ignorable code points. Stock data has neither. A user-typed mechlab variant name can (`Atlás`/`ATLÁS` are now two names). That is acceptable under
the ASCII-fold ruling but unlisted: see F3.
The coverage is NOT enough for the ORDER sites, which is where a hidden visible change can sit:
- **Pilot list** (`logisticspilotlistbox.cpp:330`, `ci_total_less` for pilots equal in rank and gunnery). The probe never ran a sort, and the
  "launch" capture shows only the starting roster. The old order is a word sort (hyphen and apostrophe weighted apart), and stock pilot display
  names come from the string table, where `D'Arcy`-type punctuation is plausible. The variant names got a stock probe (146 names, 0 pairs move);
  the pilot names did not: F2.
- **Mechlopedia**: B1.3 left its two `S_stricmp` sorts alone. `S_stricmp` folds to LOWER case, the same fold as `icompare`, so those sites and
  `ci_total_less` already agree for every name that differs by more than case. The EString sort at `mechlopedia.cpp:1328` (`Compare(x, 1) < 0`)
  is B1.5. It needs the same stock-name probe and the `encyclopedia*` captures, plus a fix for the `encyclopedia_vehicles` STOP first, or that tab
  stays unproven.
- **Mechlist** (`mechlistbox.cpp:612`): proven on the stock variants. A user-created variant may move (punctuation); list it in F3.
The stale-reader half of the probe never saw a campaign load, which is where B1.4's readers sit. The same `readIdString`-into-a-shared-`tmp`
pattern exists there: `LogisticsMissionInfo::init` (a missing `Video` takes the `ABLScript` text) and `loadMech` (a missing `Pilot` keeps the
`Variant` text). See A3.

## 4. The save noise set: can B1.4-B1.7 proceed with it?
Measured over the lane's saves (`save1.ims`; pairs f_a/f_c, g_a/g_b and h_a/h_b give the within-exe noise; f vs g vs h is cross-build):
- **Text packet**: byte-identical across f, g and h. **MechWarriorData offsets 0-2167** (name, callsign, video, audio, brain strings, skills)
  are byte-identical in every pair. This positively closes the B1.3b question "did a pilot text field change?", which the class-level argument
  only assumed.
- **MechData/GroundVehicleData 2953-2955 and 3001-3003**: the vptrs of `MoverControl`/`MoverDynamics`. They are inert on load: `mover_misc.cpp:552-553`
  copy-assigns, and copy assignment does not copy a vptr. They are harmless, but they depend on the build and on the boot (Windows image ASLR is
  per boot), so they are never equal between builds.
- **MechWarriorData, NOT in the within-exe noise union**: one record (index 9 of 120) has 24 four-byte words at 2192-2451, 3188-3447 and
  3520-3779. That is three blocks 996 and 332 bytes apart, which matches `tacOrder[3]` / `lastTacOrder` / `curTacOrder` (sizeof `TacticalOrder`
  = 332 by layout). The values relocate with the image (between g and h the low 16 bits are kept, exactly like the vptrs). These are stack or heap
  leftovers that include code addresses, in fields the order code never initialises. It is the same defect class as the Team roster tail (fixed)
  and the objective model fields (fixed). The lane's text puts them in "the noise set (they differ between two runs of one exe)". That is true of
  the CLASS and false of these BYTES.
- **CommanderData** varies so widely run to run (whole 50-byte spans) that three pairs do not bound it. It is not a usable proof surface. B1 does
  not write commanders.
**Ruling: B1.4-B1.7 MAY proceed** with a known noise set, and the save-schema arc does NOT have to fix these first. B1 changes no record struct.
B1.4's real proof surface is the campaign text and the mission text packet, which carry no binary noise, and the defects are inert on load. But
the noise set must be an **offset mask, not a class list** (F1). Once a mask exists, a B1 regression landing in MechWarriorData's text fields,
which are the fields logistics feeds, is visible instead of absorbed. Before release, the save-format guard work should zero these fields at
write time (initialise `TacticalOrder`'s unset fields; write records through a zeroed staging copy so a vptr never reaches the file). That is
notes finding (3), and it stays with that arc.

## 5. Sorting with `ci_total_less`
Correct as the explicit total order.
- `icompare` folds to lower case, then `a < b` on `string_view`. The standard specifies `char_traits<char>::lt` as an unsigned compare, so the
  tie-break is a byte order and the relation is a strict total order. Equivalence is byte equality.
- Mechlist: old `items[i] > new` -> insert before. New `ci_total_less(new, items[i])` gives the same direction. An equal (byte-identical) name
  goes AFTER the existing one, which is stable insertion.
- Pilot list: the duplicate test (`iequals`) runs BEFORE the order test, so case variants are rejected as duplicates and the byte tie-break is
  unreachable there. The comment says so. It is consistent, not a contradiction.
- `ci_less` (equivalence = `iequals`) has no sorting caller. That is correct, and the header says why.

## 6. What the gates do not see in these batches, and the one proof that closes it
SIMID, tier1, the captures and doctest do not exercise:
- the logistics WRITERS changed by B1.3 (`LogisticsVariant::save`, `LogisticsMech::save`, `LogisticsPilot::save`, `.c_str()` of the same text);
- the logistics READERS (`LogisticsData::load`/`loadMech`/`loadVariant` lookups, now `iequals` over a `from_c(tmp)`);
- mechlab variant create, replace and delete with user-typed names;
- salvage with a destroyed mech (the `s_deployedMechSnapshot` path);
- every non-ASCII name.
The class-level unit test is impossible because the classes do not link in the unit tier.
**The one proof: a campaign save round trip across exes.**
1. Write a campaign save at the same point (the harness auto-win of mission 1, then the logistics save) with the pre-B1.3 exe 1266aa1a2, with
   45f39522a, and with the B1.4 exe.
2. Text-diff the three saves. Expect identical output or listed lines.
3. LOAD the 1266aa1a2 save with the newest exe and re-save it. The re-save must equal the original. This executes every reader lookup above.
4. Run `analyze_dangling --check` and the B1.4 diff alongside.
This closes B1.3's writer gap and is B1.4's own proof, so it is condition G2 and not a B1.3 rework. An optional extra: one mechbay-harness run on
an ASan build turns any dangling `c_str()` on the logistics screens into a hard stop. Do it once at the end of B1.4 if the ASan game target builds.

## 7. B1.4 design (notes 8b): APPROVED WITH AMENDMENTS
- A1 **By-value `std::string`, empty = none: approved**, over my C7 `std::optional` suggestion. At every caller listed, absent and empty both
  mean "no video / no text", so an optional would carry no information. Record this as the C7 decision with the caller list in the commit. The
  `LogisticsData` pass-throughs return `std::string` by value too, and NONE of them may `return x().c_str()` (shape B, which the gate catches).
  `getCurrentABLScript()` feeds `missionbegin.cpp:325` (`if (brainfile)`): keep the `"NONE"` sentinel, test `empty()`, and list that a present
  empty `ABLScript` now skips the brain where it used to try `".abl"`.
- A2 **Correct a sentence in 8b**: `bigVideoName` is NOT written to the campaign save (only `BigMoviePlayed`, `logisticsmissioninfo.cpp:477`).
  The expected campaign-text changes are those of `CampaignFile`, `CampaignName`, `PlayerName`, `MissionName` and `MissionFileName`. If a name
  was never assigned, the old `writeIdString(null)` crashed (H15), so no stock save can hold that case. Say so, and expect zero changed lines on
  stock data.
- A3 **The reader rule (C9) applies to B1.4's FIT reads**: `LogisticsMissionInfo::init` (`Video` reuses the stale `tmp`, `PreVideo` is guarded),
  `loadMech` (`Pilot` reuses the `Variant` text), and `LogisticsVehicle::init` (`AppearanceName` into an uninitialised `tmp`). Promote
  `objective_text::read` to ONE shared optional-returning FIT text reader that both models use (C13 spirit; no second copy). Check the stock
  campaign FITs offline for groups without `Video`/`PreVideo`/`ABLScript` before deciding what absence means at each key.
- A4 **Campaign-start boot check needs a positive control.** The `""` playback sites are `logistics.cpp:346` (after a WON mission:
  `mechbay_preview_harness` auto-win reaches it, `MC2_BOOT_TO_BAY` may not) and `missionbegin.cpp:808` -> `logistics.cpp:395` (already filters
  `""`). Add one census-registered `MC2_LOG` at each decision (`pre-video: <name>|none`). The check passes only when the log shows the site was
  REACHED and logged `none` or a real name. A log that is silent because the path was skipped proves nothing.
- A5 B1.4b touches `logisticsdata_save.cpp`: `s_deployedMechSnapshot` (a pre-existing static-duration vector, now of `std::string`) becomes a
  `LogisticsData` member (plan section 12). Hoist `from_c(tmp)` out of the pilot loops in `load`/`loadMech`, or pass the `char` array as a
  `string_view`: no allocation per iteration.
- A6 The split (4a mission info + forced includers, 4b dialog + the rest of `logisticsdata*`) is approved. The 40-file soft cap and its report
  per commit stand.

## 8. Baseline changes
- **LG06 +2 (ea995215d, `readFlagID(file, idKey, indexKey)`)** and the merge rewrite 8ac9bb84e (+1 `from_c`, +1 `objective_text` keys):
  **justified.** They are `const char*` parameters at the FitIniFile C boundary, which C1 explicitly sanctions, and the commits name them. This is
  not a loosening. B1.3a then took LG06 -2. The same mis-count will recur through B1.7 and B6, so I recommend a reviewed per-line marker for
  sanctioned C-boundary parameters in `cxx17_ratchet`. With it, LG06 only ever moves down and an upward rewrite becomes a red flag again. This is
  lane 2's tool, so raise it at the next merge; it is not a B1 condition.
- own_inventory (EString 1/1 -> 0/0 by deletion; `objective.cpp` 10 -> 8, the two deleted `delete`s), LG03/LG05/LG09 decreases, the log-tag
  census (two new `MC2_LOG(Io, Warn)`), and the `callers.json` regeneration all match the code. None is a loosening.
- Process note: B1.1's tier1 ran on a dirty-tree exe (the e5600b260 message says so; this breaks COMMIT -> BUILD -> DEPLOY). Later tier1 runs on
  committed exes that contain B1.1 (06ea51611, 45f39522a) supersede it, so it needs no action. Do not repeat it.

## 9. Fixes (concrete edits)
- **F1 (medium, before B1.4's save diff)**: replace the class-level noise sentence (notes section 8 "B1.3a/b" proof and lane1-state "B1.3
  save-diff explanation") with an offset mask. Build it as a JSON under `tests/` or `.claude/cleanup/`: per record class, the byte ranges that
  differ between two runs of one exe, plus the vptr ranges of MechData/GroundVehicleData. The diff then reports "bytes outside the mask". Name the
  MechWarriorData record-9 words (2192-3779, three `TacticalOrder` blocks, image-relative stale words) as a pre-existing writer defect for the
  save-format arc, and state that MechWarriorData 0-2167 is byte-identical across builds. If the mask needs a `--mask` option in
  `tools/save_inspect.py`, `check_conflict` first (`tools/` is lane 2's).
- **F2 (low-medium, before B1.4b ends)**: an offline pilot-name order probe, the same as the variant probe. Take the stock pilot display names
  from the string table and compare the sign of `CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE)` (via ctypes) against `icompare` for every
  pair. Record the result in notes section 8 and list any pairs that move. If B1.5's mechlopedia sort is ready, run it over the encyclopedia names
  in the same pass.
- **F3 (low)**: add the unlisted behaviour changes to notes section 8:
  - `CObjective::Save` and the condition/action `Save`s now write `"\r\n"` as `"\n"` (`objective_text::write`; the old code called
    `writeIdString` raw), so the save bytes of multi-line objective text change;
  - user-typed non-ASCII variant names no longer fold (cp1252);
  - user-created variant names may reorder in the mechlist;
  - `getVariant(csv, n)` with a null name now matches like `""` instead of reaching `strstr(null)`.
- **F4 (low, with the next `objective_text` touch)**: `CBooleanArray::load` ends at the first over-long `BooleanString{i}` and silently drops
  every later flag. Only the bad entry is malformed. Let the reader tell absent from malformed (for example `std::expected<std::string, ReadError>`)
  and skip the malformed entry with its log line.
- **F5 (nit)**: `logisticsvariant.cpp:178-180` `uiIconOverride[i] = up.icon.c_str()` -> `= up.icon`.

## 10. B1.4: GO, with conditions
- G1 F1's mask exists before the first B1.4 mission-save diff.
- G2 The campaign-save round trip of section 6 (three exes plus a cross-load). If it needs a harness hook, add it in B1.4a's first commit as a
  harness argument, not a new Feature gate (gate amnesty).
- G3 Amendments A1-A6 go into notes 8b before code starts.
- G4 F2 runs before B1.4b closes. It does not block B1.4a.
- G5 For every batch: `analyze_dangling --check` over every edited TU, and the exact deprecation census at the end with remapped count = exact
  count.
- G6 The editor `-mission` leg of the R12 probe runs on the old editor before B1.6 starts.

Owner questions: none. Every point above is decidable under the existing rulings (ASCII fold, saves may break before release, no new gates).
