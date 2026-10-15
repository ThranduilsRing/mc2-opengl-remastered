<!-- doc-status: current | reviewed:2026-10-12 -->
# C++23 step 4 B1 (EString -> std::string): adversarial before-review of `cxx23-step4-b1-notes.md` (f4ab40f30)

VERDICT: **APPROVE-WITH-CHANGES.** The H-table is the right method (pin the old behaviour with characterization tests, then judge
each site), and lane 1 found the four semantics that a rename gets wrong. Five things are missing or wrong, and each one could
regress the game without anything noticing: (1) H1 is misstated. `Data()` is null only for a string that was never assigned or was `Empty()`'d; `= ""` gives a non-null `""`.
(2) Constructing a `std::string` from a null `const char*` is UB, where `EString(nullptr)` was defined. The notes have no row for this.
(3) The objective writers OMIT a key when `Data()` is null (`sWriteIdString`, objective.cpp:283). Objectives are written into savegames
(saveload.cpp:181), so B1.3 is a save-path batch as well.
(4) The claim that "EString does not feed the simulation" is false: objective boolean flags are looked up case-insensitively (`CBooleanArray::elementPos`, objective.h:98-112), and the deployed force comes from logistics name lookups.
(5) Dangling sites exist in files with no `EString` token (`editor/BeautySidecarPreview.cpp:46`), so a census of the 67 grep-set files cannot be the net.

## 1. Judgement of lane 1's findings (read from the class and the callers)
- **H1, corrected.** `EBuffer::Data()` returns 0 when `m_Alloc_Length == 0`. That is true only of `s_Empty_Buffer`, which a default-constructed, `Empty()`'d, or
  `Assign(nullptr)`'d string points to. `Assign("")` allocates (estring.cpp:327-352), so `es = ""` and `Format("")` give a non-null `""`. A
  `Data()` null-test therefore means "never assigned", not "length 0". Converting it to `!empty()` changes the assigned-but-empty case (`if (m_modelName.Data())`,
  objective.cpp:2549). Callers that relied on null: `logistics.cpp:346` (`if (videoName) playFullScreenVideo(videoName)` fed by
  `getCurrentBigVideo()` = `bigVideoName`, unassigned when an operation has no `PreVideo` key, logisticsmissioninfo.cpp:155): after conversion it plays `""`;
  `getCurrentVideo/Description/FriendlyName` return members through `const char*` and callers test null (missionselectionscreen.cpp:252, 444-447).
  `editor/ObjectivesDlg.cpp:152,160` tests `0 == p`. `saveload.cpp:97,113` would `strlen(nullptr)` today on an empty campaign name (it becomes a defined
  1-byte packet: an improvement, list it).
- **H2: case-insensitive is right, lstrcmpi is wrong.** `Compare(p, true)` runs `memcmp` over the argument length and then falls back to `lstrcmpi`. `lstrcmpi` is
  `CompareString(LOCALE_USER_DEFAULT, NORM_IGNORECASE)`: word sort (hyphen and apostrophe weighted apart), locale-dependent (a tr-TR user does not match
  `FILE`/`file`), and Windows-only. It is a host-dependent behaviour, not a contract to preserve. Ruling: **ASCII case fold** for identity, and for order a
  **total order** (ASCII-folded lexicographic, then a byte compare as tiebreak). This applies the SortList explicit-total-order ruling to string keys. Sites that depend on folding:
  the 35 `Compare(x, 0)` lookups (variant, pilot, mech and purchase file names, logisticsdata_*.cpp), `missionselectionscreen.cpp:325`,
  `logisticsmissioninfo_query.cpp:121,163` (mission file names), `logisticsdialog.cpp:831-1778` (save-file names vs list text), objective flag IDs
  (`CBooleanArray`, gameplay), and `getAllPurchaseFiles` (already `_stricmp`). Sort sites: `logisticspilotlistbox.cpp:330`, `mechlistbox.cpp:597-610`,
  `mechlopedia.cpp:1329` (listbox insertion order: visible UI, no save or sim effect). The comment at `logisticsdata_save.cpp:577` ("Compare is case-sensitive") is false: fix it.
- **H3** (asymmetric `==`) needs no special handling once every comparison is a named helper. `operator<=(const EChar*, const EString&)` actually returns strict `<`
  (estring.h:444-447): the census (change R2) finds out whether anything calls it.
- **H4 / finding 10.** By-value accessors that return a member (`Title`, `Description`, `ActivateFlagID`, `ResetStatusFlagID`, `ModelName`, objective.h:713-755;
  `MissionName/Author/Blurb/VideoFilename/DownloadURL`, EditorData.h:209-272) keep their callers' pointers valid TODAY only because the copy shares the COW buffer.
  The sites that hold one: `editor/ObjectivesDlg.cpp:152,160` (`const char* p = (*it)->ActivateFlagID().Data();`), `editor/BeautySidecarPreview.cpp:46`
  (`return ...MissionName().Data();` out of a `const char*` function, in a file with no EString token), plus the `.Data()` uses that stay inside one expression (OK).
  Returning `const std::string&` from member accessors removes the whole class. That is the fix; /analyze is the second net.
- **H6.** `find() == -1` happens to work (`-1` converts to `npos`), but `int pos = s.find(...)` truncates. The real trap is the inclusive end of `SubString`/`Remove`. Hand conversion
  with a test, as the notes say.
- **H9 is wrong in its example.** `editor/forest.cpp:48` and `EditorObjectMgr_io.cpp:114` are MFC `CString::Format`. Every `EString::Format` in scope has a literal template
  (grep of 123 sites). No printf-template formatter is needed in B1.
- **H12.** The game-side `MakeLower` sites act on file names (logisticsdata_availability.cpp:32, _inventory.cpp:477, logisticsvariant.cpp:49, pilotreadyscreen.cpp:801):
  ASCII fold, the same function as H2. `MakeUpper` sites act on DISPLAY text from the string table (mechlopedia x9, logisticspilotlistbox.cpp:89,
  pilotreadyscreen.cpp:842). Stock English text is ASCII, but a localized strings resource is the one non-ASCII source. ASCII upper leaves bytes >= 0x80
  unchanged, which is defined and deterministic, so accept it and pin it with a test. The editor `MakeLower` sites are all `CString`: out of scope.
- **New H13: null `const char*` into `std::string` / `std::string_view` is UB.** `EString(nullptr)` and `es = nullptr` were defined to give empty.
  Sources of null: `aListItem::getText()` over an empty EString member (the gui is not converted until B6), the `getCurrent*` getters above, `File::getFilename()`, readers.
  Every legacy `const char*` that enters a converted string goes through `mc2::str::from_c(const char*)` (null gives empty).
- **New H14: COW kept buffer identity across copies.** A `const char*` taken from one EString copy stayed valid after the original died. `std::string` (SSO included)
  ties `c_str()` to the object. A stored pointer whose owner is copied or relocated now dangles. Every STORED `const char*` from a converted string
  (members, arrays like `getAllPurchaseFiles(const char**)`, list items) is listed with its owner-lifetime argument.
- **New H15: writer and reader quirks.** `sWriteIdString(nullptr)` writes nothing (objective.cpp:283-284), while a direct `writeIdString(name, nullptr)` would
  crash (`strncpy` from null, inifile.cpp:919). `sReadIdString` leaves its out-param untouched on a missing key and callers reuse one temporary, so a missing `Description`
  takes the Title text and a missing `ObjectiveModel` takes the previous field (objective.cpp:269-281 and 1698-1752; the same in editor/Objective_internal.h:87 and editor/Objective.cpp:342-375). That is
  consistent with the ledgered `ObjectiveModel` re-save difference. The B1.3/B1.6 conversion fixes it rather than wrapping it (rule C9).
- **Raw owner (B1.1).** A function-local `static std::string` would be a static-duration owner, which plan section 12 forbids. The cache it serves (tgainfo.cpp:208-355: re-use the
  pixels of a <= 64x64 TGA after the second consecutive load of the same name) saves one small decode, and returns stale pixels if the file changes. **Delete
  the cache** (7 statics, the extern at mechcmd2_internal.h:208, the delete at mechcmd2_frame.cpp:300-304). EString new/delete goes 1/1 to 0/0 by removal.
- **/analyze.** The plan's gate stands, with one correction: C26816 is "pointer points to memory allocated on the stack", not "dangling after the owner is reassigned".
  The lifetime checks are intra-procedural. The R8 fallback named in the notes (`bugprone-dangling-handle`) only covers `string_view`. Use clang-cl
  `-Wdangling -Wdangling-gsl -Wreturn-stack-address` from `compile_commands.json` instead (clang-cl is on the host, docs/host-tools.md). It sees `c_str()` of a temporary.

## 2. Required changes to the notes (concrete edits)
- **R1. Rewrite H1** as in section 1 ("null = never assigned or `Empty()`'d; `""` is non-null"). Classify every `Data()` / implicit-conversion site as null-test, pass-through
  or use, and for null-tests write the target semantics explicitly (`std::optional<std::string>` where "absent" is a real state, e.g. `bigVideoName`; else `empty()`).
- **R2. Add a census by deprecation to B1.0 and make it the site list, not grep.** In an uncommitted local build, mark `[[deprecated]]` on `operator ECHARPTR`, `Data`,
  every comparison operator, `Compare`, `Make{Upper,Lower}`, `Find/ReverseFind/SubString/Left/Right/Remove/Replace/Insert`, and `Format`. The C4996 list
  (file:line:kind, every TU, editor included) is committed as `.claude/cleanup/b1-estring-census.txt`. Each batch commit removes its rows; the board
  checks that the count only goes down. This catches `std::string == const char*`, which compiles silently, and the files with no EString token.
- **R3. Add H13, H14, H15** to the table, each with its rule (section 3: C2, C8, C9) and characterization tests: `EString(nullptr)`, `es = ""` gives non-null, `sReadIdString`
  reuse, and `sWriteIdString(null)` omits the key.
- **R4. Delete the "EString does not feed the simulation" sentence** (notes 5.5). Replace it with: objective flags (`CBooleanArray`) and logistics name lookups feed mission
  logic and the deployed force. SIMID sees the force only on runs that pass through logistics, and sees objective state only indirectly (section 5).
- **R5. Batch order and caps** (section 4). Objectives move ahead of the logistics model, B1.2 splits by header, B1.5 (multiplayer screens) moves to B6,
  B1.1 deletes the cache instead of converting it, and the cap counts EDITED files (grep set plus includers that needed edits) with a soft cap of 25. A header flip whose
  extra includers need only mechanical `.c_str()`/`from_c` edits may go to 40, and the commit says so.
- **R6. Fix the save proof** (5.6). `resave` compares save/quickload/save within ONE exe and is not identical (1159 of 20000 packets, log 2026-10-11 19:55).
  The batch proof is old exe vs new exe: two pre-batch saves at the same frame give the noise set; `tools/save_inspect.py diff` pre vs post must show only noise-set
  packets, and the string packets (0 version, 1 campaign, the mission-FIT text packet with objectives and flags) are byte-identical apart from listed expected lines.
  B1.3 (objectives, saveload.cpp:181) joins B1.2/B1.4 as a save-path batch.
- **R7. Fix the editor proof** (5.8). The 15 editor-smoke cases run on `-gen-map` and contain no objectives. B1.6/B1.7 run `-mission=<stock mission with objectives,
  flags and a blurb> -smoke-save=<out>` with the pre- and post-batch editor, then diff the `.fit` bytes. Expected diffs are listed (rule C9). The MFC dialogs are not reachable headless, so
  `ObjectivesDlg`/`ObjectiveDlg` edits are compile-only plus a reader's sign-off, and the commit says so.
- **R8. Correct H9/R4**: there are no runtime-template EString `Format` sites, so drop the printf-template formatter from B1.0.
- **R9. Gui bridge** (2, B1.0): a `setText(std::string_view)` next to `setText(const EString&)` makes every `setText("literal")` AMBIGUOUS (two user-defined
  conversions). The bridge is `setText(const char*)` plus `setText(const std::string&)` forwarding to the EString member, so B6 collapses all three into one
  `setText(std::string_view)` with zero caller edits. `aEdit` gets `std::string entry() const` (the final API), not `getEntry(std::string&)`. Drop
  `EString::view()` (a view of a temporary EString dangles) and keep `str()`, used only on text coming FROM a widget. A compile test in B1.0 calls
  `setText("x")`, `setText(std::string{})`, `setText(es)` and `setText(0L)`. Each overload carries a WHAT+WHY comment naming B6 as its deletion point.
- **R10. Fix the /analyze section**: the C26816 text, a seeded negative test with three shapes ((a) local `const char* p = f().c_str(); use(p);`, (b) `return f().c_str();` from a
  `const char*` function, (c) a member that stores the pointer), and the clang-cl fallback. The gate is trusted only for the shapes the seeded test proves it
  catches; the other shapes are covered by R2 plus the accessor rule. Put the script under lane 1's claim (`tests/analyze/`), or confirm `tools/` with `check_conflict`
  (program-review section 8 gives `tools/` to lane 2).
- **R11. Review cadence**: the owner's 2026-10-12 15:58 ruling ("Opus only now and then", a Sonnet cleanup review per session) replaces "Opus review of EACH batch". There is one
  after-review (RA1) for B1.0-B1.3, the semantic batches, and one after the editor batch. The other batches get the Sonnet session-end review with this document as its checklist.
- **R12. One-off fold-hit probe** (B1.0, uncommitted patch on the OLD exe). Log every `Compare`/`==` where the fold matched and bytes did not, by call site, during
  tier1, `MC2_BOOT_TO_MECHLAB`, `MC2_BOOT_TO_BAY`, a campaign start and an editor `-mission` load. Log every `sReadIdString` miss whose caller then used the stale
  value. The result (sites and counts on stock data) goes into the notes. It tells which C3 sites matter on stock data and which only for mods.

## 3. Conversion rules (paste into the lane brief)
- C1 Types: owned text is `std::string`; a read-only parameter is `std::string_view`, except where the text reaches a C API (`seekBlock`, `writeIdString`, `AddString`,
  `playDigitalStream`, printf): there it is `const std::string&` or `const char*`. A `string_view` is never a member, never a return value (except a string literal),
  and never a local bound to a call result.
- C2 Every legacy `const char*` entering a converted string goes through `mc2::str::from_c(p)` (null gives empty). Never `std::string(p)` on a pointer that is not proven non-null.
- C3 Identity comparisons of names that come from data (file names, variant/pilot/mech/mission names, flag IDs, list-item text) use `mc2::str::iequals`
  (ASCII fold, locale-free). Containers keyed by such names use a transparent `mc2::str::ci_less`. There is no case-insensitive string TYPE.
  A bare `==` is allowed only when both operands are produced by code, and the commit names that site.
- C4 Orders: `ci_less` is the total order (ASCII-folded lexicographic, then a byte compare as tiebreak). Each sort site states the order in a comment. Expected UI order
  changes (punctuation such as `-`, `'` and `_`, and case variants that compared equal before) are listed in the commit with the stock pairs that move.
- C5 `MakeLower`/`MakeUpper` become `mc2::str::ascii_lower/ascii_upper` (`std::ranges::transform` over `unsigned char`). Bytes >= 0x80 pass through
  unchanged, and a test pins that.
- C6 Accessors that return a member return `const std::string&`. Computed text returns `std::string` by value. Constant species names (`Description()` of the
  condition and action classes, objective.h:166-634) become `std::string_view` returning a literal. Setters take `std::string` by value and move it.
- C7 `Data()` null-test: decide absent vs empty per site (`std::optional<std::string>` when absent is meaningful, e.g. `bigVideoName`, otherwise `empty()`).
  Pass-through goes to `c_str()` only after checking what the callee does with `""` versus null.
- C8 A stored `const char*` from a converted string is listed with its owner's lifetime. Prefer storing the `std::string` (or an index) to storing the pointer.
- C9 Text readers return `std::optional<std::string>` (a missing key is an empty optional, never a stale temporary). Text writers ALWAYS write the key (an empty value is legal) and
  never call `writeIdString` with null. A missing key is not malformed, so there is no log line; a key too long for the reader's buffer is malformed and gets a log line.
- C10 `Format` with a literal template becomes `std::format`. A possibly-null `const char*` argument goes through `mc2::fmt::safe`, and the `mc2::fmt::as_*` helpers apply where printf
  conversion semantics differ. Never pass a class object to `...` (C4840 is a hard error in the batch's targets).
- C11 Index API: hand-convert each site with a doctest of the old call's exact result written first (inclusive end, `-1` vs `npos`, overwrite `Replace`).
- C12 A header changes type in the same commit as every includer that the compiler reports. There is never a dual form, and no `.str()` outside widget-boundary code.
- C13 The code and editor objective models are converted with the SAME helpers and the same reader/writer rules (they are parallel copies: code 3,459 lines and
  editor 2,727 lines). Unifying them is the editor arc's job, not B1's.
- C14 Comments are WHAT+WHY; delete wrong ones (logisticsdata_save.cpp:577).

## 4. Batches and the proof bar per batch
| batch | content | proof (on top of: build + `build_all_targets.py`, doctest under ASan, `check-contracts.sh`, census rows only go down) |
|---|---|---|
| B1.0 | helpers (`from_c`, `iequals`, `ci_less`, `ascii_*`), bridge (R9), `EString::str()`, characterization tests H1-H8 and H13-H15, C4840 `/we`, lifetime gate (R10), census (R2), probe (R12) | the seeded negative test fires on each claimed shape and the clean twin is silent; a corpus test (a committed fixture of the stock identifiers, extracted once) shows `iequals` agrees with `lstrcmpi` on 100 % of equal pairs, with order disagreements listed; the bridge compile test |
| B1.1 | delete the TGA last-load cache (3 files) | `own_inventory` EString 1/1 to 0/0; tier1 5/5; one day capture of mc2_01 viewed (icons and UI textures load) |
| B1.2 | runtime objectives `code/objective.{h,cpp}` + includers (C6, C9, `CBooleanArray` on `iequals`) | lifetime gate plus clang-cl on the TUs; a doctest of `CBooleanArray` with mixed-case keys; a reader doctest (missing Description/ObjectiveModel gives empty); save diff (R6); `sim_determinism check` on mc2_03/10/16/24; tier1 |
| B1.3 | logistics model A: `logisticsvariant`, `logisticsmech`, `logisticspilot` headers + .cpp | lifetime gate; doctests of the converted lookups on a case-variant fixture; `MC2_BOOT_TO_MECHLAB` and `_BAY` captures viewed (lists and order); determinism check |
| B1.4 | logistics model B: `logisticsmissioninfo*`, `logisticsdata*` incl. `_save`, `saveload.cpp`, `logisticsdialog` | save diff (R6) on mc2_01 and mc2_16; the C7 decisions for the video getters; a campaign-start boot (operation video path) whose log shows no `""` playback; determinism check; tier1 |
| B1.5 | front-end screens (remaining locals: mechlopedia H6/H12, infowindow, briefing, selection, pilotready, salvage, chat) | `MC2_BOOT_TO_SCREEN=encyclopedia_*` captures viewed (upper case and substring sites); briefing capture; tier1 |
| B1.6 | editor model: `editor/Objective*`, `EditorData*`, `CampaignData`, `EditorObjectMgr_io`; the dialogs get compile edits only | `run_editor_smoke.py` 15/15 plus R7 (`-mission` + `-smoke-save` `.fit` byte diff, old vs new editor, expected lines listed) |
| B1.7 | tests and end state | census = 0 outside `gui/` and the mp screens; `test_estring.cpp` and the `alloc_probe` EString cases STAY until B6 (they are the oracle while the class lives) |
Not a B1 batch: the 11 mp screens (`mp*.cpp/h`) convert in B6 with their widgets. Until then they get only edits that the B1 headers force.

## 5. What the gates cannot see, and what a wrong conversion looks like
SIMID digests movers, buildings, turrets, ABL instances and RNG counts. It cannot see UI text, list order, file names opened (a failed open falls back
silently), save string packets, editor output, logistics state (inventory, pilots, campaign progress) or the objective flag table. tier1 boots straight into
missions and does not exercise logistics, campaign flow or dialogs. The failure modes, and what catches each:
- A bare `==` on a data name: a mech, pilot or variant is missing from the mechbay/purchase lists, a quickload drops a deployed mech, or an objective flag is never found and
  the objective never completes. Caught by the R12 probe (which sites matter), the doctests, the BAY/MECHLAB captures and the save diff. SIMID catches it only when the force or the ABL changes.
- null to `""`: `playFullScreenVideo("")` after an operation without `PreVideo`; `.fit` files gain `= ""` lines. Caught by the campaign-start boot log, the save diff and the editor `.fit` diff.
- A dangling `c_str()` (BeautySidecarPreview, ObjectivesDlg): garbage text, or a heap-use-after-free under ASan only if a test runs it. Caught by C6 (by construction), the lifetime gate and clang-cl.
- `std::string` from null (H13): an access violation at construction. tier1 crash bucket on mission paths; elsewhere it shows only when the screen is used. Caught by C2 and census review.
- Index off-by-one (H6): a truncated or extra character in mechlopedia and briefing text. Caught by doctests first, then viewed captures (MAD is not a signal).

## 6. Answers to the lane's six questions
1. Bridge first: YES. B6 is a widget-ownership redesign and is not scheduled; the bridge is the B6 API landing early (R9), not a wrapper around the quirks. It is
   deleted at B6, together with `EString::str()` and the class.
2. Keep case-insensitive identity where names come from data (C3), as ASCII fold, not `lstrcmpi`. Do not make names case-exact in B1: that would be a content change.
3. Member accessors return `const std::string&`; computed text returns by value; constant names return `std::string_view` of a literal (C6).
4. No printf-template formatter: the scope contains no runtime template (R8).
5. A recorded baseline is not a prerequisite (saves may break before the final release), but the proof is old-vs-new save diff over the noise set (R6), not
   `resave`. The writers must stay a defined format (C9), and every byte change is listed in the commit.
6. Split B1.2, by header (B1.3 and B1.4 in section 4), and put objectives ahead of it: two files plus six includers carry every hazard (H1, H2, H4, H15, save
   path), so the rules get proven on the smallest blast radius.

## 7. Owner questions (answered 2026-10-12)
- Q1. The 11 multiplayer front-end screens are KEPT and convert in B6 with their widgets.
- Q2. An objective without a `Description` key shows its Title, written explicitly in the reader.
