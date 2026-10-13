<!-- doc-status: current | reviewed:2026-10-12 -->
# C++23 step 4, batch B1 (EString -> std::string): batch notes, revised after the Opus before-review

STATUS (2026-10-12 evening): the before-review (`cxx23-step4-b1-review.md`, dbb25604a, APPROVE-WITH-CHANGES) is applied as its changes R1-R12. DONE: B1.0 (harness: db9b1d4fa, 432b4ae03, 5e02d2347,
3a851f497, the /we4840 flag), B1.1 (runtime objectives, e5600b260), B1.2 (texture cache deleted, 5d640f3f5). NEXT: B1.3 (logistics model A), then the grouped Opus after-review (R11). The review is the
checklist; where this file and the review disagree, the review wins and this file is wrong.
INPUTS: `cxx23-step4-ownership-plan.md` sections 5 and 10, `cxx23-step4-review.md` finding 10, `program-review-2026-10-12.md` section 8, the owner rulings in
`ORCHESTRATOR-HANDOFF.md`. Owner rulings that bind B1: saves may break before the final release but every writer stays a defined format; readers reject malformed content
with a log line and never crash; the bar is "how we would do it today" (redesign, never a wrapper around a quirk); comments say WHAT and WHY, never HOW; no static-duration
owners. Owner answers (2026-10-12, defaults until he says otherwise): the 11 multiplayer front-end screens are KEPT and convert in B6 with their widgets; an objective with
no `Description` key keeps showing its Title, written explicitly in the reader.
Numbers come from `git grep` at 642019a64 (grep-grade; the compiler finds the rest, and B1.0 replaces the grep set by a compiler list, section 3).

## 1. What is being replaced, and what it really does (read from mclib/estring.{h,cpp})

`EString` is a 1990s reference-counted copy-on-write string (`m_pBuffer` -> `EBuffer{refcount,len,alloc}` + chars), implicitly convertible to `const char*`
(`operator ECHARPTR`), printf-style `Format`, MFC-CString lookalike. A mechanical rename gets the rows below WRONG. They were verified by reading the implementation and by a
throwaway doctest on 2026-10-12 (reverted); B1.0 pins each one with a committed characterization test so a conversion is judged against the old behaviour, not this prose.

| # | EString behaviour | std::string | Consequence for conversion |
|---|---|---|---|
| H1 | `Data()` returns null only for a string that was NEVER ASSIGNED or was `Empty()`'d (`EBuffer::Data()` returns 0 when `m_Alloc_Length == 0`, which is `s_Empty_Buffer` alone, also reached by `Assign(nullptr)`). `es = ""` and `Format("")` allocate, so they give a non-null `""`. A `Data()` null-test therefore means "never assigned or emptied", not "length 0" | `c_str()` is always a valid `""` | every `Data()` / implicit-conversion site is classified null-test, pass-through or use. A null-test decides absent-vs-empty AT THE SITE: `std::optional<std::string>` where "absent" is a real state (`bigVideoName`: an operation without a `PreVideo` key), `empty()` otherwise. A pass-through goes to `c_str()` only after reading what the callee does with `""` versus null. Seen: `objective.cpp:2549` (`m_modelName.Data()`, assigned-but-empty changes meaning), `logistics.cpp:346` (`if (videoName) playFullScreenVideo(videoName)`: after a naive conversion it plays `""`), `missionselectionscreen.cpp:252,444-447`, `editor/ObjectivesDlg.cpp:152,160`, `tgainfo.cpp:223,332` (deleted with the cache, B1.2). `saveload.cpp:97,113` would `strlen(nullptr)` today on an empty campaign name: it becomes a defined one-byte packet (an improvement, listed in B1.4) |
| H2 | equality is CASE-INSENSITIVE in practice: `Compare(s, true)` does `memcmp` over the ARGUMENT length and, when bytes differ, FALLS THROUGH to `lstrcmpi`; `Compare(x)` / `Compare(x, 0)` is case-insensitive by design (35 call sites: variant, pilot, mech, purchase-file names); `operator<` falls to the same call. `lstrcmpi` is `CompareString(LOCALE_USER_DEFAULT, NORM_IGNORECASE)`: a word sort, locale-dependent (a tr-TR user does not match `FILE`/`file`) and Windows-only | byte-exact, case-sensitive | the identity is kept case-insensitive (names come from data; making them case-exact is a content change) but as an **ASCII fold**, locale-free: `mc2::str::iequals`; orders use a total order `mc2::str::ci_less` (ASCII-folded lexicographic, byte compare as tiebreak). Sites that depend on the fold: the 35 `Compare(x, 0)` lookups, `missionselectionscreen.cpp:325`, `logisticsmissioninfo_query.cpp:121,163`, `logisticsdialog.cpp:831-1778` (save-file names vs list text), objective flag IDs (`CBooleanArray`, gameplay), `getAllPurchaseFiles`. Sort sites (visible UI order, no save or sim effect): `logisticspilotlistbox.cpp:330`, `mechlistbox.cpp:597-610`, `mechlopedia.cpp:1329`. The comment at `logisticsdata_save.cpp:577` ("Compare is case-sensitive") is false and is fixed. The probe (section 3, B1.0d) says which sites actually fold on stock data |
| H3 | `operator==(const EChar*, const EString&)` calls `Compare(p)` WITHOUT the case flag, `EString == const char*` WITH it: same answer only because of H2. `operator<=(const EChar*, const EString&)` returns strict `<` | n/a | vanishes once every comparison is a named helper; the compiler-driven site list (section 3) finds out whether anything calls the odd `<=` |
| H4 | COW: copies share the buffer. By-value accessors (`Title`, `Description`, `ActivateFlagID`, `ResetStatusFlagID`, `ModelName`, objective.h:713-755; `MissionName/Author/Blurb/VideoFilename/DownloadURL`, EditorData.h:209-272) keep a caller's `const char*` valid TODAY only because the returned copy shares the member's buffer. Holders: `editor/ObjectivesDlg.cpp:152,160` (`const char* p = (*it)->ActivateFlagID().Data();`) and `editor/BeautySidecarPreview.cpp:46` (`return ...MissionName().Data();` from a `const char*` function, in a file with NO EString token) | a copy owns new storage | accessors that return a member return `const std::string&` (C6): the whole class disappears by construction. Computed text returns `std::string` by value. The lifetime gate (section 5) is the second net |
| H5 | non-const `operator[]` unshares; const `operator[]` asserts `Index < len` | UB past size | benign |
| H6 | `SubString(start, END)` and `Remove(start, END)` take an INCLUSIVE end, `Left(n)`/`Right(n)` are counts, `Find` returns `INVALID_INDEX` (-1), `Replace(start, s)` OVERWRITES in place | `substr(pos, COUNT)`, `npos`, `replace(pos, n, s)` | `find() == -1` happens to work (`-1` converts to `npos`) but `int pos = s.find(...)` truncates. About 30 sites, each converted by hand after a doctest of the old call's exact result |
| H7 | `Length()` is `lstrlen` (stops at an embedded NUL), `Size()` is `m_Data_Length` | `size()` counts NULs | one pinned test; matters only if a record field with embedded NULs reached an EString |
| H8 | `Compare` with the case flag reads `Length` bytes of the ARGUMENT from the receiver before the length check (an over-read for a short receiver) | n/a | disappears with the conversion; a latent ASan finding the characterization test may show (noted in the commit) |
| H9 | `Format` is printf-style, unannotated (the compiler never checks arguments). **Every `EString::Format` in scope has a LITERAL template (grep of 123 sites)**: `editor/forest.cpp:48` and `EditorObjectMgr_io.cpp:114` are MFC `CString::Format` and out of scope | `std::format` | a literal template converts to `std::format` by hand, compile-time checked. There is NO runtime-template site, so **no printf-template formatter is built** (it was in the earlier draft and is dropped) |
| H10 | `operator ECHARPTR` lets an EString reach a `...` call; the `(char*)(const char*)purchaseFileName` casts in `logisticsdata_availability.cpp:60` exist because of it | `.c_str()`; a `std::string` passed through `...` COMPILES and prints garbage (C4840) | **C4840 as a hard error (`/we4840`) in the targets of the batch**, landed in B1.0; every printf-style call in a touched file is read, not only compiled |
| H11 | `CreateMBCS`/`CreateUNICODE` allocate for the caller; the only call outside estring.cpp is `tests/unit/alloc_probe.cpp:89` (`d.CreateMBCS()`) | n/a | deleted with the class (B6) |
| H12 | `MakeUpper`/`MakeLower` call `CharUpper`/`CharLower` (ANSI code page, locale-aware; about 28 hits in code/ and editor/, 9 in `mechlopedia.cpp`). The game-side `MakeLower` sites act on FILE NAMES (`logisticsdata_availability.cpp:32`, `_inventory.cpp:477`, `logisticsvariant.cpp:49`, `pilotreadyscreen.cpp:801`): ASCII fold, the same function as H2. The `MakeUpper` sites act on DISPLAY text from the string table (mechlopedia x9, `logisticspilotlistbox.cpp:89`, `pilotreadyscreen.cpp:842`). The editor `MakeLower` sites are MFC `CString` and out of scope | `std::toupper` per byte is C-locale ASCII | `mc2::str::ascii_lower/ascii_upper`: bytes >= 0x80 pass through unchanged (defined and deterministic; stock English text is ASCII, a localized string resource is the one non-ASCII source), pinned by a test next to the old class's code-page behaviour |
| H13 | a null `const char*` into `std::string` / `std::string_view` is UB, where `EString(nullptr)` and `es = nullptr` were defined to give empty. Null sources: `aListItem::getText()` over an empty EString member (the gui is converted in B6), the `getCurrent*` getters of H1, `File::getFilename()`, the readers | UB at construction | every legacy `const char*` entering a converted string goes through `mc2::str::from_c(p)` (null gives empty); never `std::string(p)` on a pointer not proven non-null. Tests: `EString(nullptr)` and `es = nullptr` are empty (old behaviour pinned), `from_c(nullptr)` is empty, `from_c("")` is empty |
| H14 | COW kept buffer identity across copies: a `const char*` taken from one copy stayed valid after the original died. `std::string` (SSO included) ties `c_str()` to the object, so a STORED pointer whose owner is copied, moved or destroyed now dangles | owner-bound | every stored `const char*` from a converted string (members, arrays such as `getAllPurchaseFiles(const char**)`, list items) is listed in the batch commit with its owner-lifetime argument; prefer storing the string or an index (C8). Tests: the old class's pointer outlives its source (pinned as the behaviour being given up), and the seeded lifetime negatives (section 5) |
| H15 | writer and reader quirks. `sWriteIdString(nullptr)` writes nothing (objective.cpp:283-284) while a direct `writeIdString(name, nullptr)` would crash (`strncpy` from null, inifile.cpp:919), and `Objective` writers omit a key when `Data()` is null: objectives are written into saves (saveload.cpp:181) and mission FIT files. `sReadIdString` leaves its out-param untouched on a missing key and callers reuse one temporary, so a missing `Description` shows the Title text and a missing `ObjectiveModel` takes the previous field (objective.cpp:269-281, 1698-1752; the same in `editor/Objective_internal.h:87`, `editor/Objective.cpp:342-375`). Consistent with the ledgered `ObjectiveModel` re-save difference | n/a | readers return `std::optional<std::string>` (a missing key is an empty optional, never a stale temporary); the Description-falls-back-to-Title behaviour is kept deliberately and WRITTEN in the reader (owner answer); writers ALWAYS write the key (empty is legal) and never call `writeIdString` with null; a key too long for the reader's buffer is malformed and gets a log line, a missing key does not. Tests: the old reader's reuse and `sWriteIdString(null)`-omits-the-key pinned, then the new reader/writer doctests (B1.1) |

Raw-owner facts (own_inventory, `--report --type EString`): one `new EString` (`mclib/tgainfo.cpp:219`, the lazily created `g_textureCache_FilenameOfLastLoadedTexture`) and its one
`delete` (`code/mechcmd2_frame.cpp:302`). A function-local `static std::string` would be a static-duration owner, which ownership plan section 12 forbids. The cache it serves
(`tgainfo.cpp:208-355`: re-use the pixels of a <= 64x64 TGA after the second consecutive load of the same name) saves one small decode and returns stale pixels if the file changes.
**It is deleted, not converted** (B1.2): 7 statics, the extern at `mechcmd2_internal.h:208`, the delete at `mechcmd2_frame.cpp:300-304`. EString new/delete goes 1/1 to 0/0 by removal.
`tests/unit/alloc_probe.cpp` (12) and `tests/unit/test_estring.cpp` (5) use the class and stay as the oracle until B6.

## 2. The caller set

`git grep -l EString`: code 48, editor 18, gui 7, mclib 3 (estring.cpp, estring.h, tgainfo.cpp), tests 2. Outside the gui widgets: 67 files (`cxx23-step4-b1-files.json`, file -> batch,
asserted equal to `git grep -lw EString 642019a64`). **56 of them convert in B1.1-B1.7; the 11 multiplayer screens (`mp*.cpp/h`) are KEPT (owner) and convert in B6** with their widgets.
Until then they get only the edits a B1 header forces. The true touched set is larger than the grep set (a header member that changes type breaks every includer, and a file
with no EString token can hold a dangling pointer: `editor/BeautySidecarPreview.cpp`, `code/mechcmd2_frame.cpp`), so **the site list is the compiler's, not grep's** (section 3, B1.0b).
Header fan-out: `code/logisticsdata.h` 33 includers, `editor/EditorData.h` 22, `editor/Objective.h` 14, `code/logisticspilot.h` 11, `code/logisticsmech.h` 10, `code/logisticsdialog.h` 10,
`code/missionbriefingscreen.h` 7, `code/logisticsvariant.h` 7, `code/objective.h` 6, the rest 2-5.

COUPLING TO THE GUI WIDGETS (not B1, but B1 must not break it): `aText::setText(const EString&)` (`gui/asystem.h:315`), `aEdit::getEntry(EString&)` / `setEntry(const EString&, BYTE)`
(`gui/aedit.h:46`), the members `text`/`hiddenText` of `aListItem`/`aText`/`aEdit` (`gui/alistbox.h:91,134`, `gui/asystem.h:318`, `gui/aedit.h:130`), 3 local uses (`gui/aanim.cpp`,
`gui/aedit.cpp`, `gui/logisticsscreen.cpp`). **B1.0 adds the bridge as `setText(const char*)` plus `setText(const std::string&)` forwarding to the EString member** (on `aText` and `aTextListItem`; the matching
`aEdit::setEntry` overloads, and `aEdit::entry()` returning `std::string`, the final API; `tests/unit/test_gui_bridge.cpp` is the compile-time overload-resolution test): there is NO `setText(std::string_view)` (it would make every `setText("literal")` ambiguous
between two user-defined conversions) and NO `EString::view()` (a view of a temporary EString dangles). `EString::str()` exists and is used only on text coming FROM a widget.
Each overload carries a WHAT+WHY comment naming B6 as its deletion point; **B6 folds the three into one `setText(std::string_view)` with zero caller edits**, and deletes `str()` and the class.
A compile test calls `setText("x")`, `setText(std::string{})`, `setText(es)` and `setText(0L)`.

## 3. B1.0, the harness batch (adds behaviour; everything after depends on it)

a. **Characterization doctests** in `tests/unit/test_estring.cpp`, run against the old class: H1 (never assigned null, `Empty()` null, `= ""` non-null, `Format("")` non-null),
   H2/H3 (`==`, `<`, `Compare` fold through `lstrcmpi`, the odd `<=`), H6 (inclusive ends, `Find` -1, overwrite `Replace`), H7, H8, H12 (`MakeUpper` of byte 0xE9 under the code page),
   H13 (`EString(nullptr)`, `es = nullptr`), H14 (a copy's pointer outlives the source). Then the new helpers (`mc2::str::from_c`, `iequals`, `ci_less`, `ascii_lower/upper`) are
   tested against the same cases, with a fixture of stock identifiers (extracted once, committed) showing `iequals` agrees with `lstrcmpi` on every equal pair, order disagreements listed.
b. **The site list is a compiler list, not grep.** In an uncommitted local build the old API is marked `[[deprecated]]` (`operator ECHARPTR`, `Data`, every comparison operator,
   `Compare`, `Make{Upper,Lower}`, `Find/ReverseFind/SubString/Left/Right/Remove/Replace/Insert`, `Format`). The warning list (file:line:kind, every TU, editor included) is committed as
   `.claude/cleanup/cxx23-step4-b1-callsites.txt` with counts per file. It also catches `std::string == const char*`, which compiles silently, and files without the token. The
   deprecation itself is NEVER committed. Each batch commit removes its rows; the count only goes down.
c. **The lifetime gate** (section 5): `tests/analyze/analyze_dangling.py` with a seeded negative test (a board row: it runs in 10 s). `/we4840` (a class object through `...`) is its own
   commit, B1.0f (C4840 is off by default, so the earlier builds showed none; with the flag on the trees had one site in the editor and one in gameos): the flag is applied to mc2, mclib, gui and EditRel, not to gameos (lane 2).
d. **A one-off probe on the OLD exe** (a diagnostic patch applied by script, NEVER committed): log every `Compare`/`==`/`<` where the fold matched and the bytes did not, by call site; log every
   `sReadIdString` miss whose caller then used the stale value. Run on stock data: tier2 missions mc2_01, mc2_03, mc2_10, mc2_24 (these boot into missions, so the logistics and campaign
   screens are NOT covered by them: `MC2_BOOT_TO_MECHLAB`, `MC2_BOOT_TO_BAY` and a campaign start are separate runs when the harness allows). The result (sites and counts on stock data)
   is recorded in section 8 of this file. It tells which fold sites matter on stock content and which only for mods.
e. **The gui bridge** (section 2).

## 4. Batches (cap: 25 EDITED files = token files plus the includers the compiler forces; soft, reported in each commit)

A header flip whose extra includers need only mechanical `.c_str()` / `from_c` edits may go to 40 and the commit says so. Order: smallest blast radius first, so the rules (H1, H2, H4,
H15, the save path) are proven on objectives, then leaf-first by header fan-out.

| Batch | Subsystem | Token files | Content | Proof (on top of section 6's base list) |
|---|---|---|---|---|
| B1.0 | harness | tests + 5 gui/mclib files | section 3 | the seeded negatives fire on each claimed shape and the clean twin is silent; corpus test; the bridge compile test |
| B1.1 | runtime objectives: `code/objective.{h,cpp}` + includers (`controlgui_internal.h`, `gametacmap.cpp`, `mission.h`, `missionbriefingscreen.cpp`, `team.h`) | 2 | 175 word occurrences (179 substring hits: 4 are `CEStringList`, an MFC type). The `virtual EString Description()` / `InstanceDescription()` family (31 + 15 overrides) becomes `std::string_view` of a literal (C6), `Title()`/`Description()` return `const std::string&`, `CBooleanArray` on `iequals`, the readers/writers per C9, the Title fallback explicit | lifetime gate plus clang-cl on the TUs; a `CBooleanArray` doctest with mixed-case keys; a reader doctest (missing Description gives the Title by rule, missing ObjectiveModel gives empty); **save diff (section 6.3)**; `sim_determinism check` mc2_03/10/24 and mc2_16 with liveness; tier1 |
| B1.2 | texture-cache static string DELETED: `mclib/tgainfo.cpp`, `code/mechcmd2_internal.h`, `code/mechcmd2_frame.cpp` | 2 (+1 with no token) | the cache (7 statics, the extern, the delete) goes; no replacement owner | `own_inventory` EString 1/1 to 0/0 with the baseline rewritten and the reason; tier1 5/5; one day capture of mc2_01 viewed (icons and UI textures load) |
| B1.3 | logistics model A: `logisticsvariant.h`, `_stats.cpp`, `logisticsmech.h`, `logisticsmechdisplay.cpp`, `logisticspilot.h`, `logisticspilotlistbox.cpp` + includers (10 + 11 + 7 of the headers) | 6 | name lookups to `iequals` (H2), sort sites to `ci_less`, `MakeLower` file names (H12) | lifetime gate; doctests of the converted lookups on a case-variant fixture; `MC2_BOOT_TO_MECHLAB` and `_BAY` captures viewed (lists and order); determinism check |
| B1.4 | logistics model B: `logisticsmissioninfo{,_progress,_query}`, `logisticsdata{,_availability,_init,_inventory,_modify,_save}`, `saveload.cpp`, `logisticsdialog.{h,cpp}` + includers (33 of `logisticsdata.h`) | 14 | member and accessor types (`getCurrent*` per C7: `std::optional` for the video names), the 35 `Compare(name, 0)` lookups, the save-file name fields, `saveload.cpp` campaign names (H1 improvement). If the edited count passes 40 the batch splits at `logisticsdialog` | **save diff** on mc2_01 and mc2_16; a campaign-start boot whose log shows no `""` playback; lifetime gate; determinism; tier1 |
| B1.5 | front-end screens (remaining locals: `chatwindow`, `controlgui_chat`, `infowindow`, `mechbayscreen_mechs`, `mechlabscreen.{h,cpp}`, `mechlistbox`, `mechlopedia`, `missionbriefingscreen.{h,cpp}`, `missionselectionscreen`, `pilotreadyscreen`, `salvagemecharea`, `simplecomponentlistbox`) | 14 | mechlopedia H6/H12, two `Format` with `%ld/%ld  %s` in infowindow, briefing and selection screens; uses the bridge | `MC2_BOOT_TO_SCREEN=encyclopedia_*` captures viewed (upper case and substring sites); briefing capture; tier1 |
| B1.6 | editor model: `editor/Objective*`, the dialogs get compile edits only | 10 | the editor's own objective model (`Objective.h` 68 occurrences, `_conditions` 59, `_actions` 30), mixed with MFC `CString`/`_T` at the dialog boundary; the same helpers and reader/writer rules as B1.1 (C13: the two models are parallel copies, 3,459 and 2,727 lines; unifying them is the editor arc's job) | editor batch proof (section 6.4) |
| B1.7 | editor data: `CampaignData`, `ChooseBuildingDlg`, `ChooseUnitDlg`, `EditorData{,_internal}.h`, `EditorData_tacmap`, `EditorData_terrain`, `EditorObjectMgr_io` (+ `BeautySidecarPreview.cpp`, no token) | 8 | `EditorData` string members are file-format fields; `EditorData_terrain.cpp:246-387` reads fixed buffers into them | editor batch proof (section 6.4) |
| B1.8 | end state | tests | census = 0 outside `gui/` and the mp screens; `test_estring.cpp` and the `alloc_probe` EString cases STAY until B6 (the oracle while the class lives) | — |
| B6 | the 11 mp screens (`mpconnectiontype, mpgamebrowser, mphostgame, mploadmap, mpparameterscreen, mpprefs, mpsetuparea, mpstats`) with the gui widgets | 11 | mechanical, no new behaviour; compile + `build_all_targets` + unit only (no session layer); `code/multplyr*` is not touched | owned by the B6 plan |

Reviews (R11): the owner's cadence replaces "an Opus review of each batch". ONE grouped Opus after-review after B1.0-B1.3 (the semantic batches) and one after the editor batches B1.6/B1.7; every
session ends with a Sonnet read-only cleanup review of the diff, using `cxx23-step4-b1-review.md` as its checklist.

## 5. Lifetime gate (`/analyze`, corrected) and the dangling shapes

There is no analyze wiring in the repo today (`build-analysis/` is a stale /W3-only tree). The correction to the earlier draft: MSVC **C26815** is "the pointer is dangling because it points at a
temporary instance which was destroyed"; **C26816** is "the pointer points to memory allocated on the stack". The lifetime checks are INTRA-procedural. The earlier fallback
(`bugprone-dangling-handle`) only covers `string_view`; the fallback is **clang-cl `-Wdangling -Wdangling-gsl -Wreturn-stack-address`** over `compile_commands.json` (clang-cl is on the host,
`docs/host-tools.md`), which sees the `c_str()` of a temporary.
`tests/analyze/analyze_dangling.py` compiles a seeded negative TU holding three shapes: (a) a local `const char* p = f().c_str(); use(p);`, (b) `return f().c_str();` from a `const char*`
function, (c) a member that stores the pointer. It runs `cl /analyze` with C26815/C26816 enabled and FAILS if any shape is missed, then tries clang-cl for the missed ones; a clean twin
must be silent. **The gate is trusted only for the shapes the seeded test proves it catches**; the other shapes are covered by the call-site list (3b) plus the accessor rule C6 (accessors return
`const std::string&`). The script lives under `tests/` (lane 1's area; `tools/` is lane 2's per program-review section 8). A board row only if it runs under a minute, otherwise a documented manual step.

## 6. Proof list (every code batch; the plan's section 5 mapped to commands)

1. `py -3 scripts/ninja_build.py` (game) then `py -3 tools/build_all_targets.py` (game, editor, mc2fx, asset_viewer, mech_import, tg_import_dump, launcher, data_tools, tests): an API
   change rebuilds every in-repo consumer in the same batch. 0 errors, no new warnings in the touched dirs (C4840 an error).
2. `py -3 tools/build_contract_harnesses.py --run --unit --asan` (doctest under ASan), `py -3 tools/own_inventory.py --check` with the baseline diff committed, `bash scripts/check-contracts.sh`
   (cxx17 ratchet moves down only; comment guard; file size; log tag census). Determinism: `py -3 tools/sim_determinism.py check mc2_03 / mc2_10 / mc2_24` (and `mc2_16` with
   `MC2_FIXTURE_ORDER_LIVENESS=1`). Objective state and the deployed force feed mission logic (flags via `CBooleanArray`, logistics name lookups), and SIMID sees the force only on runs that pass
   through logistics and objective state only indirectly, so a digest change is an alarm, not the only signal.
3. **Save proof (batches that touch a save path: B1.1, B1.3, B1.4).** `saveload_roundtrip resave` compares save/quickload/save within ONE exe and is NOT identical (1159 of 20000 packets, log
   2026-10-11 19:55). The proof is old exe vs new exe at a fixed frame: **two saves from the OLD exe** give the noise set; one from the NEW exe; `py -3 tools/save_inspect.py diff` of
   old-vs-new must show only noise-set packets, and the string packets (0 version, 1 campaign, the mission-FIT text packet with objectives and flags) must be byte-identical except for listed expected
   lines (the writer now always writes the objective keys). Every differing packet is named in the commit.
4. **Editor batch proof (B1.6, B1.7).** `scripts/run_editor_smoke.py` runs 15 cases on `-gen-map` and contains no objectives, so it is not enough: run `-mission=<stock mission with objectives, flags
   and a blurb> -smoke-save=<out>` with the pre-batch and the post-batch editor and byte-diff the saved `.fit`; the expected differences are listed (C9). The MFC dialogs are not reachable
   headless, so `ObjectiveDlg`/`ObjectivesDlg` edits are compile-only plus a reader's sign-off, and the commit says so.
5. `py -3 scripts/run_smoke.py --tier tier1 --deploy 0.4 --continue --wait-timeout 3600` after commit + build + deploy (5/5). tier1 boots straight into missions and does NOT exercise logistics,
   campaign flow or dialogs: front-end batches add viewed captures (`MC2_BOOT_TO_MECHLAB`, `_BAY`, `MC2_BOOT_TO_SCREEN=...`; sorted lists compared by eye, MAD is not a signal).

## 7. Conversion rules (paste into the lane brief)

- C1 Types: owned text `std::string`; a read-only parameter `std::string_view`, except where the text reaches a C API (`seekBlock`, `writeIdString`, `AddString`, `playDigitalStream`, printf), where it is
  `const std::string&` or `const char*`. A `string_view` is never a member, never a return value (except a string literal), never a local bound to a call result.
- C2 Every legacy `const char*` entering a converted string goes through `mc2::str::from_c(p)` (null gives empty). Never `std::string(p)` on a pointer not proven non-null.
- C3 Identity comparisons of names that come from data use `mc2::str::iequals` (ASCII fold, locale-free); containers keyed by such names use the transparent `mc2::str::ci_less` / `ci_equal` (equivalence =
  `iequals`). There is no case-insensitive string TYPE. A bare `==` is allowed only when both operands are produced by code, and the commit names the site.
- C4 A sorted list uses `mc2::str::ci_total_less` (folded lexicographic, then the raw bytes, so case variants have a fixed order; `ci_less` is NOT used for sorting because it leaves case variants
  equivalent). Each sort site states its order in a comment; expected UI order changes (punctuation such as `-`, `'`, `_`, and case variants that compared equal) are listed
  in the commit with the stock pairs that move.
- C5 `MakeLower`/`MakeUpper` become `mc2::str::ascii_lower/ascii_upper`; bytes >= 0x80 pass through, a test pins that.
- C6 Accessors that return a member return `const std::string&`; computed text returns `std::string`; constant species names become `std::string_view` of a literal; setters take `std::string` by value and move it.
- C7 `Data()` null-test: decide absent vs empty per site (`std::optional<std::string>` where absent is meaningful, else `empty()`); a pass-through goes to `c_str()` only after checking what the callee does with `""`.
- C8 A stored `const char*` from a converted string is listed with its owner's lifetime; prefer storing the string or an index.
- C9 Text readers return `std::optional<std::string>`; text writers ALWAYS write the key and never call `writeIdString` with null; a missing key is not malformed (no log line), an over-long key is.
- C10 `Format` with a literal template becomes `std::format`; a possibly-null `const char*` argument goes through `mc2::fmt::safe`; never pass a class object to `...`.
- C11 Index API: hand-convert each site with a doctest of the old call's exact result written first (inclusive end, `-1` vs `npos`, overwrite `Replace`).
- C12 A header changes type in the same commit as every includer the compiler reports. Never a dual form, and no `.str()` outside widget-boundary code.
- C13 The code and editor objective models use the SAME helpers and rules.
- C14 Comments are WHAT+WHY; wrong ones (`logisticsdata_save.cpp:577`) are deleted.

## 8. Results recorded by B1.0 (2026-10-12)

**Call sites (3b, `cxx23-step4-b1-callsites.txt`).** 598 sites in 76 files from the deprecation build of the old API: operator ECHARPTR 212, Data 187, Format 103, Compare 35, operator== 13,
Find 11, MakeUpper 11, Remove 7, MakeLower 4, Left/Right/ReverseFind/operator!= 3 each, SubString 2, Insert 1. Per batch (files that name the type): B1.1 90, B1.2 4, B1.3 15, B1.4 99, B1.5 82,
B1.6 135, B1.7 26, B6 (mp screens) 50; 97 more sites in 21 files that do not name the type (includers the compiler forces, and the gui widgets). The files outside the grep set that a batch must
therefore also edit: `code/missionbegin.cpp` 9, `logistics_begin.cpp` 5, `mainmenu.cpp` 4, `controlgui_objectives.cpp` 3, `pilotreviewarea.cpp` 3, `logisticsvariant.cpp` 6, `logisticsmech.cpp` 3,
`logisticspilot.cpp` 3, `mission_movers.cpp` 2, `controlgui_update.cpp`, `mission_update.cpp`, `missionresults.cpp`, `pausewindow.cpp` (1 each) in code/; `editor/EditorData_save.cpp` 9,
`EditorInterface_terrain.cpp` 6, `BeautySidecarPreview.cpp` 1 in editor/. (The Compare count of 35 matches the review's 35 `Compare(x, 0)` lookups.)

**Probe on the old exe (3d; a diagnostic patch applied by script and reverted, never committed).** Run on the install `mc2-win64-v0.4` with the exe built from 5e02d2347 plus the patch:
tier2 missions mc2_01, mc2_03, mc2_10, mc2_24 (28 s each), the logistics screens `purchase`, `bay`, `loadout`, `briefing`, `launch` through `scripts/mechbay_preview_harness.py` (this boots the
first campaign mission with auto-win and then parks on the screen), `bay` with `MC2_BOOT_TO_MECHLAB=1`, and one quicksave / quickload round trip of mc2_01 (`saveload_roundtrip.py resave`).
- **Fold-only matches (the bytes differ, the old compare said equal): ONE site on stock data**, hit in every mission boot and every logistics screen and again on quickload:
  `LogisticsData::updateAvailability` (`code/logisticsdata_availability.cpp:245`, `(*pIter).getFileName().Compare(pilotName, 0)`, reached from `ensureMechBayDataLoaded`, `LogisticsData::init`
  and `LogisticsData::load`). The pairs are the lower-case pilot file name against the mixed-case name in the availability list: `pmwchopper`/`PMWChopper`, `pmwcreep`/`PMWCreep`,
  `pmwhacksaw`/`PMWHacksaw`, `pmwpsycho`/`PMWPsycho`, 15 hits per run. **A bare `==` there leaves those pilots unavailable in the stock campaign**: this is the one C3 site that matters on stock
  data, and `iequals` is the conversion. Every other `Compare(x, 0)` / `==` site either never saw a case difference in these runs or was not reached.
- **Order disagreements with a plain ASCII fold**: none fired in these runs (no sort site that differs from `lstrcmpi` was reached: mechlopedia, the pilot list and mech list sorts need screens
  that the harness does not open). The corpus test lists the order differences over the 7046 stock file names: 20 adjacent pairs, all `digit`/`.` after `_`/`-` (lstrcmpi is a word sort, the new
  order is plain byte order of the folded text); equality agrees on every name and its case variants.
- **Stale-reader hits (`sReadIdString` found no key and the caller reused the previous text): none while loading a mission from its FIT; two on quickload of mc2_01**, both
  `key=ObjectiveModel` with the stale text empty (`""`), plus one benign `BooleanString0` (the loop's terminating read of an empty flag block). The cause on quickload: the objective writer
  OMITS `ObjectiveModel` when the name was never assigned (H15), the reader then keeps whatever the previous read left in its shared temporary (here the empty `ResetStatusFlagID`), and
  `ModelName("")` is assigned where the original was null. No stock objective was seen taking the Title text for a missing `Description` (the stock FITs carry the key); the explicit
  "Description falls back to the Title" rule in the reader is for authored and mod content.
- Noise set of the save (old exe, two runs, mc2_01 at frame 1000): 241 of 20000 packets differ run to run (TerrainObjectData 191, MechWarriorData 41, MechData 6, CommanderData 2, and the mission FIT
  text packet 2). The text-packet noise is the objective MODEL fields written uninitialised: `ModelType`, `BaseColor`, `HighlightColor`, `HighlightColor2` carry stack garbage
  (`-1052649298` against `0`) in objectives that have no model. B1.1 initialises them so the writer is a defined format.

**Lifetime gate (3c, `tests/analyze/analyze_dangling.py`).** On this host (MSVC 14.44, clang 23.1): `cl /analyze` with the CppCoreCheckLifetimeRules set reports NONE of the c_str() shapes
(C26815/C26816 do not fire; it flagged only a returned local array, as C26847). clang-cl catches all four seeded shapes: A (local holds `make().c_str()`) `-Wdangling-gsl`, B (`return make().c_str()`)
and B2 (`return local.c_str()`) `-Wreturn-stack-address`, C (member stores a pointer into a temporary) `-Wdangling-assignment-gsl`; the clean twin is silent in both. The gate is therefore clang-cl
and covers exactly these shapes. It runs in about 10 s and is a board row (`analyze_dangling`).

**B1.1 (runtime objectives, e5600b260).** `code/objective.{h,cpp}` have no EString left; two new leaf TUs hold the rules and are unit-tested (`code/objective_text.{h,cpp}`: `read` returns an optional,
`write` always writes the key, `readFields` carries the missing-key rule for Title / Description / ActivateFlagID / ResetStatusFlagID / ObjectiveModel; `code/objective_flags.{h,cpp}`: the flag table on
`iequals`). 46 dead virtuals (28 `Description()`, 13 `InstanceDescription()` overrides, the pure declarations, `DescriptionOf*Species`) were deleted after a repository-wide grep found no caller.
The save proof on mc2_01 (old exe twice, new exe three times, all at render frame 1000): the objective text packet differs from the old exe by exactly two added `st ObjectiveModel = ""` lines (the
two nav-marker objectives, whose model name the old writer omitted) and four model fields that were stack garbage and are now 0; it is identical from run to run in the new exe. The MoverData
image in MechData (134) and GroundVehicleData (151) differs at bytes 2953-2955 and 3001-3003 of every record: those are the saved vftable pointers of the embedded `MoverControl` (offset 2952) and
`MoverDynamics` (offset 3000), which change with every link (identical across the two old runs, different between any two builds). The rest (MechWarriorData, CommanderData, TeamData,
TerrainObjectData) lies in classes that already differ run to run in the old exe. Gates: build_all_targets rc 0, doctest 2781 under ASan, own_inventory 0 rose, sim_determinism mc2_03 / mc2_10 /
mc2_24 / mc2_16 (liveness) PASS against the committed baselines, tier1 5/5 (2026-10-12T20-29-56).
**B1.2 (texture cache, 5d640f3f5).** EString new/delete 1/1 -> 0/0; the mc2_01 day capture and the mechbay screen viewed, unchanged.
**/we4840** (B1.0f): the root CMakeLists makes C4840 an error for mc2, mclib, gui and EditRel; a probe TU passing a std::string to printf fails to compile with it. Found on the way: an MFC CString passed to
`sprintf` at `editor/EditorData_terrain.cpp:100` (cast to LPCTSTR, the MFC idiom) and a REAL latent bug in lane 2's file, `GameOS/gameos/vulkan_context.cpp:109`, where `ctxlog("... '%s' ...", env)` passes a
`std::string` and prints garbage on an unrecognised validation preset name; gameos joins the flag list when lane 2 fixes it (`env.c_str()`).
**Findings for the editor and ABL arcs, not fixed here.** (1) `FitIniFile::readIdString` joins the first two lines of a multi-line quoted value without a break, so a multi-line Title or Description
loses its first line break on every round trip (old and new code alike). (2) `CPlayBIK` / `CPlayWAV` read a path as `"./" + value` and save the prefixed value, so the prefix grows with every
save and load. (3) Records are saved as raw memory images including vftable pointers (the finding above), which makes a byte diff of two builds noisy by construction; the save-format guard
work should mask or zero them. (4) `FitIniFile::readIdString` returns BUFFER_TOO_SMALL for a value of exactly the buffer length and leaves it unterminated; `objective_text::read` supplies the terminator.

## 9. What the gates cannot see, and what a wrong conversion looks like

SIMID digests movers, buildings, turrets, ABL instances and RNG counts. It cannot see UI text, list order, file names opened (a failed open falls back silently), save string packets, editor output,
logistics state (inventory, pilots, campaign progress) or the objective flag table.
- A bare `==` on a data name: a mech, pilot or variant is missing from the mechbay/purchase lists, a quickload drops a deployed mech, or an objective flag is never found and the objective never
  completes. Caught by the probe (which sites matter), the doctests, the BAY/MECHLAB captures and the save diff.
- null to `""`: `playFullScreenVideo("")` after an operation without `PreVideo`; `.fit` files gain `= ""` lines. Caught by the campaign-start boot log, the save diff and the editor `.fit` diff.
- A dangling `c_str()`: garbage text, or a heap-use-after-free under ASan only if a test runs it. Caught by C6 (by construction), the lifetime gate and clang-cl.
- `std::string` from null (H13): an access violation at construction, in a tier1 crash bucket on mission paths and only when the screen is used elsewhere. Caught by C2 and the call-site review.
- Index off-by-one (H6): a truncated or extra character in mechlopedia and briefing text. Caught by doctests first, then viewed captures.

## 10. Risks that remain

- Header fan-out: `logisticsdata.h` is included by 33 files; a member change there recompiles most of `code/` (about 2-3 min incremental in `build64-ninja`, 15 min for `build_all_targets` cold).
- The mp screens compile in the default build although `MC2_MULTIPLAYER` is OFF; until B6 they are touched only where a B1 header forces it.
- Gui bridge overloads add to three gui headers; lane 2's claim is GameOS/, RenderCore/, shaders/, Gates/, `test_gate.cpp`, so the file sets are disjoint (confirmed at the start of B1.0).
- Zero-vector ABL host-interface question and the saveload baselines are outside B1.
