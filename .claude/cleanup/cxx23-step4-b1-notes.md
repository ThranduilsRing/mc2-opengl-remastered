<!-- doc-status: current | reviewed:2026-10-12 -->
# C++23 step 4, batch B1 (EString -> std::string): batch notes for the Opus before-review

STATUS: notes only, NO B1 code. B1 code starts after the Opus review of this document and the three prerequisites of
`cxx23-step4-ownership-plan.md` section 11 (determinism: done; save/load guard: open, see 6; B0 harness: done).
INPUTS: `cxx23-step4-ownership-plan.md` section 5 (proof list) and the EString row of the table, `cxx23-step4-review.md` finding 10,
`program-review-2026-10-12.md` section 8, owner rulings in `ORCHESTRATOR-HANDOFF.md` (EString migrates with the gui widgets; in-repo
tools are rebuilt in the batch that changes the API; readers reject malformed content with a log line, never crash).
Every number below was taken from `git grep` at 642019a64 (counts are grep-grade, the compiler finds the rest); corrected 2026-10-12 evening after the
cleanup review (`reviews/lane1-2026-10-12-cleanup.md` F6): batch counts, the 68-vs-67 file, H11, the objective.h occurrence and includer counts.

## 1. What is being replaced, and what it really does (read from mclib/estring.{h,cpp})

`EString` is a 1990s reference-counted copy-on-write string (`m_pBuffer` -> `EBuffer{refcount,len,alloc}` + chars), implicitly convertible
to `const char*` (`operator ECHARPTR`), printf-style `Format`, MFC-CString lookalike. It differs from `std::string` in ways that a
mechanical rename gets WRONG. These were verified by reading the implementation, and H1, H2, H3, H12 and the case-insensitive ordering
were CONFIRMED by a throwaway doctest run on 2026-10-12 against the real class (empty `Data()` is null; `EString("Abc") == "abc"`, `== EString("aBC")` and
`"abc" == EString` are all true; `Compare("abc", 0)` is 0; `EString("Zeta") < EString("alpha")` is false; `MakeUpper` turns byte 0xE9 into 0xC9 under the
Windows ANSI code page; `Find` of a missing character is -1); the throwaway test was reverted, not committed. The batch B1.0 pins each with a characterization
test (see 3, B1.0) so the conversion is judged against the old behaviour, not against this prose.

| # | EString behaviour | std::string | Consequence for conversion |
|---|---|---|---|
| H1 | `Data()` of an EMPTY string returns `nullptr` (`EBuffer::Data()` returns 0 when `m_Alloc_Length == 0`, and `s_Empty_Buffer` has none), so `const char* p = es;` is null for a default-constructed string | `c_str()` is always valid `""` | callers that test `Data()` as "has text" change meaning. Seen: `code/objective.cpp:2549` `if (m_modelName.Data())`, `mclib/tgainfo.cpp:223,332` `if (g_textureCache_...->Data() && strcmp(...))`. Callers that pass the null on (`seekBlock(es.Data())`, `Title(x.Data())`) now pass `""`: reads of a null become reads of an empty name. Rule: each `Data()` site is classified null-test / pass-through / use, and null-tests become `!s.empty()` |
| H2 | equality is CASE-INSENSITIVE in practice: `Compare(s, true)` does `memcmp` over the ARGUMENT length and, when it differs, FALLS THROUGH to `lstrcmpi` (a locale collation compare), so `"Abc" == EString("abc")` is true; `Compare(x)` / `Compare(x, 0)` is case-insensitive by design (35 call sites, mech/variant/pilot names); `operator<` falls to the same `lstrcmpi` | byte-exact, case-sensitive | EVERY `==`, `!=`, `<`, `Compare` on an EString keeps its case-insensitive meaning: one shared helper (`mc2::str::iequals` / `icompare`, locale-independent ASCII fold, tested against `lstrcmpi` on the corpus of names in `data/`) and no bare `==` on a converted string unless the site is shown case-exact. Sorting by `operator<` (listboxes, map keys) changes order between "Zeta" and "alpha" otherwise |
| H3 | `operator==(const EChar*, const EString&)` calls `Compare(p)` WITHOUT the case flag, `EString == const char*` calls it WITH: same answer today only because of H2 | n/a | do not "simplify" the asymmetry before H2 is pinned |
| H4 | COW: copies share the buffer; `Title()`/`Description()` return copies (`objective.h:713-720`) and the returned EString's buffer is the member's buffer | a copy owns new storage | `const char* t = obj.Title();` is valid today while `obj` lives and DANGLES after (finding 10). Same for `getCurrentMission()` style `const EString&` returns once they become by-value |
| H5 | `operator[]` non-const unshares (`ChecEBufferDoRealloc`); const `operator[]` asserts `Index < len` | UB past size | benign |
| H6 | index conventions: `SubString(start, END)` and `Remove(start, END)` take an INCLUSIVE end index, `Left(n)`/`Right(n)` are counts, `Find` returns `INVALID_INDEX` (-1), `Replace(start, s)` OVERWRITES in place | `substr(pos, COUNT)`, `npos` (size_t max), `replace(pos, n, s)` | off-by-one and `== -1` vs `== npos` traps; every Find/SubString/Remove/Replace site (about 30) converted by hand with a unit test of the exact old call, never by pattern |
| H7 | `Length()` is `lstrlen` (stops at an embedded NUL), `Size()` is `m_Data_Length` | `size()` counts NULs | only matters if a buffer with embedded NULs reached an EString (record fields read from files): pin with one test |
| H8 | `Compare` with a case-sensitive flag reads `Length` bytes of the ARGUMENT from the receiver even when the receiver is shorter (`memcmp(Data(), p, Length)` before the length check) | n/a | an existing over-read for short receivers; disappears with the conversion (note it in the commit, it is a latent ASan finding the doctest tier may show on the characterization test) |
| H9 | `Format` is printf-style (`%d %ld %s %c`, 103 call sites in the non-gui set, two with a RUNTIME template: `editor/forest.cpp:48`, and any `cLoadString` template) and unannotated, so the compiler never checks its arguments (C4477 does not apply) | `std::format` / `mc2::fmt::to_buf` use `{}` | a literal template converts to `mc2::fmt` by hand; a runtime template (string-table text, mod content) CANNOT be rewritten to `{}`: keep a printf-template formatter (`mc2::fmt::printf_to_string`, size-probing vsnprintf like the current implementation) for those and list them |
| H10 | `operator ECHARPTR` lets an EString pass wherever `const char*` is wanted, including into `...` (the casts `(char*)(const char*)purchaseFileName` in `logisticsdata_availability.cpp:60` exist because of it) | needs `.c_str()`; a std::string passed through `...` COMPILES (MSVC warns C4840, not an error today) and prints garbage | add C4840 to the `/we` set of the batch's targets before B1.1; every `Format`/printf call is read, not only compiled |
| H12 | `MakeUpper`/`MakeLower` call `CharUpper`/`CharLower` (ANSI code page, locale-aware; about 28 grep hits in code/ and editor/, 9 in `mechlopedia.cpp`; some of the editor ones act on MFC `CString`) | `std::toupper` per byte is C-locale ASCII | bytes above 0x7F (accented mech or pilot names in mod data) upper-case differently; use the same helper family as H2 so both fold the same way |
| H11 | `CreateMBCS`/`CreateUNICODE` allocate for the caller; the only call site outside estring.cpp is `tests/unit/alloc_probe.cpp:89` (`d.CreateMBCS()`, migrated in B1.8) | n/a | delete with the class |

Raw-owner facts (own_inventory, `--report --type EString`): one `new EString` (`mclib/tgainfo.cpp:219`, the lazily created
`g_textureCache_FilenameOfLastLoadedTexture`, comment says gos static-init order forbids a global object) and its one `delete`
(`code/mechcmd2_frame.cpp:302`). That is the whole B1 ownership content: it becomes a function-local `static std::string` (or a member of
the texture-cache owner) and the extern in `code/mechcmd2_internal.h:208` goes away. `tests/unit/alloc_probe.cpp` (12) and
`tests/unit/test_estring.cpp` (5) use the class and migrate in the last batch.

## 2. The caller set (verified; the plan said code 46, editor 16, gui 7, mclib 3)

`git grep -l EString`: code 48, editor 18, gui 7, mclib 3 (estring.cpp, estring.h, tgainfo.cpp), tests 2. The plan's 46/16 undercounted
by two files each (code: mpstats.cpp and saveload.cpp; editor: EditorData_internal.h and Objective_internal.h are the two headers, plus the
dialogs). Outside the gui widgets that is 48 + 18 + 1 (tgainfo) = 67 files (the list, file -> batch, is `cxx23-step4-b1-files.json`, checked against `git grep -lw EString`; three
Python files under `tools/` - `own_inventory.py`, `tidy/printf_buffers.py` and its test - mention the word in strings and are not consumers), and the true touched set is larger because a header member
that changes type breaks every includer that uses it. Header fan-out of the headers that carry EString in a signature or member:
`code/logisticsdata.h` 33 includers, `editor/EditorData.h` 22, `editor/Objective.h` 14, `code/logisticspilot.h` 11, `code/logisticsmech.h` 10,
`code/logisticsdialog.h` 10, `code/missionbriefingscreen.h` 7, `code/logisticsvariant.h` 7, `code/objective.h` 6, the rest 2-5. Each batch
below lists the includers it will also touch; the compiler enumerates them and the count is reported in the commit.

COUPLING TO THE GUI WIDGETS (not in B1, but B1 must not break it): the widget API takes EString today: `aText::setText(const EString&)`
(`gui/asystem.h:315`), `aEdit::getEntry(EString&)` / `setEntry(const EString&, BYTE)` (`gui/aedit.h:46`), the member `text`/`hiddenText`
of `aListItem`/`aText`/`aEdit` (`gui/alistbox.h:91,134`, `gui/asystem.h:318`, `gui/aedit.h:130`), plus 3 local uses (`gui/aanim.cpp`,
`gui/aedit.cpp`, `gui/logisticsscreen.cpp`). The widget batch (B6) converts those. Until then a converted `code/` caller needs a bridge:
**B1.0 adds `std::string_view` overloads next to each EString one** (`setText(std::string_view)`, `setEntry(std::string_view, BYTE)`,
`getEntry(std::string&)`) that forward to the EString member, so the 14 screen files that call the widgets convert first and B6 later
deletes the EString overloads and flips the members. No behaviour change at the bridge; it adds 3 gui headers and 2 .cpp to B1.0.

## 3. Batches (<= 25 files each; one subsystem; Opus review of EACH batch before the next, per the plan)

Order is leaf-first by header fan-out so a later batch never re-edits a header an earlier one finished. File counts include the headers; the
"+includers" figure is from the fan-out above and is reported, not budgeted.

| Batch | Subsystem | Files | Content | Why this position |
|---|---|---|---|---|
| B1.0 | bridge and pins (tests + tiny library change, no caller moves) | 6 + tests | `EString::str()` / `view()` (returns `std::string` / `std::string_view`, for the empty buffer the empty text), the three gui overload bridges (3 headers, 2 .cpp), `mc2::str::iequals/icompare`, the printf-template formatter, the characterization tests of H1/H2/H3/H6/H7/H8/H9 in `tests/unit/test_estring.cpp` (the old class stays the oracle: the same cases run against the helper), C4840 into the `/we` list, the `/analyze` lifetime gate recipe (see 5) with its seeded negative test | everything after depends on it; it is the only batch that adds behaviour, so its review is the strictest |
| B1.1 | texture-cache global and its extern | 3 touched, 2 of them in the 67 (`mclib/tgainfo.cpp`, `code/mechcmd2_internal.h`); `code/mechcmd2_frame.cpp` only holds the `delete` of the pointer and has no EString token | the one `new EString`/`delete` pair becomes a function-local static `std::string`; the H1 null test becomes `!empty()` | the only raw-owner site, tiny, proves the H1 rule once |
| B1.2 | logistics data model | 19 (`logisticsdata.{h,cpp}` + `_availability,_init,_inventory,_modify,_save`, `logisticsvariant.h` + `_stats.cpp`, `logisticsmech.h`, `logisticsmechdisplay.cpp`, `logisticspilot.h`, `logisticspilotlistbox.cpp`, `logisticsmissioninfo.{h,cpp}` + `_progress,_query`, `logisticsdialog.{h,cpp}`) +33 includers of logisticsdata.h | member and accessor types (`getCurrentMission()` returns `const EString&` into a member: becomes `const std::string&`, still a reference, so no H4 case); the 35 `Compare(name, 0)` name lookups -> `iequals` (H2); variant/pilot file names are save-file fields (`logisticsdata_save.cpp`) | largest fan-out header first; data model has no UI; save/load is touched (3 `Format`/name fields in `_save.cpp`, `saveload.cpp` in B1.4): the save path is the highest-risk file of the batch |
| B1.3 | mission objectives (runtime) | 2 + 5 includers (`code/objective.{h,cpp}`; the sixth `#include "objective.h"` hit is objective.cpp itself; includers `controlgui_internal.h`, `gametacmap.cpp`, `mission.h`, `missionbriefingscreen.cpp`, `team.h`) | 175 occurrences of the word (179 substring hits: 4 are `CEStringList`, an MFC type): the polymorphic `virtual EString Description()` / `InstanceDescription()` family (31 + 15 overrides in the header), `Title()`/`Description()` by-value accessors (H4), fixed-buffer reads `tmpECStr.Data()` into `Title(...)` | the H4 hazard lives here; `/analyze` is the gate, and every `const char* x = ...Description()`/`Title()` is listed by grep and read |
| B1.4 | front-end screens (calls into the widget bridge) | 15 (`chatwindow, controlgui_chat, infowindow, mechbayscreen_mechs, mechlabscreen.{h,cpp}, mechlistbox, mechlopedia, missionbriefingscreen.{h,cpp}, missionselectionscreen, pilotreadyscreen, salvagemecharea, saveload, simplecomponentlistbox` .cpp) | 12 `MakeUpper`/`Find`/`Replace` sites in `mechlopedia.cpp` (H6, H12), 2 `Format` with `%ld/%ld  %s` (infowindow), `saveload.cpp` campaign name fields | uses the B1.0 gui bridge; `saveload.cpp` is the second save-path file |
| B1.5 | multiplayer front-end screens | 11 (`mpconnectiontype, mpgamebrowser, mphostgame, mploadmap.{h,cpp}, mpparameterscreen.{h,cpp}, mpprefs.{h,cpp}, mpsetuparea, mpstats`) | UI screens, compiled in the default build although `MC2_MULTIPLAYER` is OFF (only `multplyr*.cpp` is gated) | named exception (ruling 2026-10-11): converted last and mechanically, no new behaviour; if a file here cannot be proven without a session it is listed as such in its commit; `code/multplyr*` is not touched |
| B1.6 | editor objectives | 10 (`editor/Objective.{h,cpp}, Objective_actions.cpp, Objective_conditions.cpp, Objective_internal.h, ObjectiveDlg.cpp, ObjectivesDlg.cpp, ObjectivesSummaryPanel.cpp, PointerSelectObjectDlg.cpp, TargetAreaDlg.cpp`) +14 includers | the editor's own objective model (`editor/Objective.h` 68, `Objective_conditions.cpp` 59, `Objective_actions.cpp` 30), mixed with MFC `CString`/`_T` at the dialog boundary; objectives are WRITTEN to mission FIT files, so the byte output of a saved mission is the proof | editor is a priority arc of its own; this is the biggest editor file group |
| B1.7 | editor data | 8 (`editor/CampaignData.cpp, ChooseBuildingDlg.cpp, ChooseUnitDlg.cpp, EditorData.h, EditorData_internal.h, EditorData_tacmap.cpp, EditorData_terrain.cpp, EditorObjectMgr_io.cpp`) +22 includers of EditorData.h | `EditorData` string members (mission name, author, blurb, video, URL): file-format fields; `EditorData_terrain.cpp:246-387` reads fixed buffers into them | after B1.6 so the Objective header is final |
| B1.8 | end state | tests (2) + `mclib/estring.{h,cpp}` | `alloc_probe.cpp` and `test_estring.cpp` leave the class; the class is NOT deleted until the gui widgets (B6) drop their overloads and members | deletion is a B6 commit; the owner ruling keeps the class alive for the widgets |

Totals outside the gui: the table has nine rows; the 67 EString-token files sit in the seven caller batches B1.1-B1.7 (2 + 19 + 2 + 15 + 11 + 10 + 8; the
largest is 19), B1.0 and B1.8 move no callers. The number of batches is a consequence of the 25-file cap and the
header fan-out; B1.2 could split in two (data model 11 + dialog/pilot/mech display 8) if the review wants a tighter cap.

## 4. Conversion rule (per site; the compiler drives, a reader decides)

1. Type: owned text `EString` -> `std::string`; borrowed read-only parameter -> `std::string_view` (or `const std::string&` when it is passed on
   to a `const char*` API that needs NUL termination: `seekBlock(const char*)`, `AddString`, `playDigitalStream`; a `string_view` is NOT NUL
   terminated, so a `string_view` parameter that reaches a C API is a bug; the rule is "`const std::string&` or `const char*` at C boundaries").
2. `Data()` -> `c_str()` at a C boundary; test-for-null `if (s.Data())` -> `if (!s.empty())` (H1); a pass-through of `Data()` stays `c_str()` and the callee
   already handled null (check, per site, that it did; most do not).
3. Operators: `==`/`!=`/`<`/`Compare` -> `iequals`/`icompare` (H2). A site is allowed byte-exact only when the comparison is shown (by test or data) never to see
   differing case; that is a reviewed exception, listed in the commit.
4. `Format("...%d...")` -> `mc2::fmt::to_buf`/`std::format` with `{}` for LITERAL templates (compile-time checked); a runtime template -> the printf-template
   formatter (H9). Variadic string arguments are `.c_str()` (H10).
5. Index API (Find, SubString, Remove, Replace, Left, Right, Insert) -> hand conversion with the old call's expected result written as a test first (H6).
6. By-value returns that callers keep as pointers (H4): `Title()`, `Description()`, `InstanceDescription()`, `LocalizedTitle()`, `DescriptionOf...Species()`:
   return type becomes `std::string`; every caller that stores `.c_str()` of the temporary is rewritten to hold the string. The gate is `/analyze`
   C26815/C26816 ("the pointer is dangling because it points at a temporary instance which was destroyed") over the batch's TUs. Accessors whose
   result is a reference to a member (`getCurrentMission`) stay `const std::string&`.
7. `operator ECHARPTR` disappears: each implicit conversion becomes an explicit `.c_str()`; the compiler lists them all.
8. Headers: change the member/signature type in the header in the same commit as every caller the compiler reports; never leave both forms.

HOW /analyze CATCHES THE H4 CASE. `const char* p = obj.Title().c_str();` (or `const char* p = obj.Title();` once `Title()` returns std::string) is a pointer
into a destroyed temporary: the C++ Core Check lifetime rules report C26815 (dangling pointer from a temporary) and C26816 (dangling after the owner is
destroyed / reassigned). There is NO analyze wiring in the repo today (`build-analysis/` is a stale /W3-only tree; the plan only names the codes), so B1.0
delivers it: a script (`tools/analyze_lifetime.py`) that compiles a batch's TUs from `compile_commands.json` with `cl /analyze:only /analyze:plugin EspXEngine.dll`
and the lifetime ruleset, treats C26815/C26816 as failures, and is accepted only after a SEEDED NEGATIVE TEST (a 5-line TU holding the dangling pattern must
fire both codes; a clean twin must not). Until that test passes the gate is not trusted. The grep list of `.Title()/.Description()/.InstanceDescription()`
assignments to `const char*` is the second, manual net; /analyze does not see a pointer that crosses a function boundary through a `const char*` parameter
that the callee stores. The objective setters (`Title(EString)`, `Description(EString)`, `ModelName(...)`) take the string BY VALUE and assign it into the
member, so they copy (COW share today, a real copy after); the review should confirm that no `const char*` setter in the batch keeps its argument.

## 5. Proof list (every batch; the plan's section 5 mapped to commands)

1. `py -3 scripts/ninja_build.py` (game) then `py -3 tools/build_all_targets.py` (game, editor, mc2fx, asset_viewer, mech_import, tg_import_dump, launcher,
   data_tools, tests): an API change must rebuild every in-repo consumer in the same batch. 0 errors, no new warnings in the touched dirs (C4840 now an error).
2. `py -3 tools/build_contract_harnesses.py --asan --run --unit` (doctest under ASan, 56 s): every batch, with the characterization tests of H1-H10 growing
   with the batch. Plus `alloc_probe` create/copy/move/destroy cases for the one raw owner (B1.1).
3. `py -3 tools/own_inventory.py --check` and `--report --type EString`: new/delete rows for EString go 1/1 -> 0/0 in B1.1; the baseline is rewritten in
   the same commit with the reason.
4. `/analyze` lifetime gate on the batch's TUs (C26815/C26816 = 0), batches B1.3 (mandatory), B1.2, B1.4, B1.6, B1.7 (touch by-value returns).
5. Determinism: `py -3 tools/sim_determinism.py check mc2_03` / `mc2_10` / `mc2_24` (and `mc2_16` with `MC2_FIXTURE_ORDER_LIVENESS=1`) against the committed
   baselines; every batch (EString does not feed the simulation, so a digest change would be an alarm, not a finding). Runs are 20-45 s each; mc2_03 needs
   `--duration 45` when the host is loaded (a 20 s run reached 377 of the 1000 baseline frames on 2026-10-12).
6. Save/load: `py -3 tools/saveload_roundtrip.py resave --mission mc2_01` (byte identity of a re-save, old vs new exe) for B1.2 and B1.4 (the two batches that
   touch `logisticsdata_save.cpp` / `saveload.cpp`). The recorded-baseline form (`--baseline` / `--check`, `tests/saveload/baselines/`) does NOT exist yet:
   no baseline is committed because MechWarriorData/CommanderData/FIT fields differ run to run (`saveload-fidelity-2026-10-11.md`). B1.2/B1.4 therefore
   depend on the resave byte-identity run (same frame, old exe vs new exe) rather than on a baseline; the owner ruling (saves load within a release line,
   across the ownership step only once the round-trip test exists) still applies to a campaign-name or file-name field that changes length handling.
7. `py -3 scripts/run_smoke.py --tier tier1 --deploy 0.4 --continue --wait-timeout 3600` after commit + build + deploy (5/5), plus smoke-lite while iterating.
8. `py -3 scripts/run_editor_smoke.py` for B1.6 and B1.7 (and B1.0/B1.8 if `editor/` is touched); for B1.6 also: open a mission with objectives, save, re-open,
   compare the saved `.fit` bytes with the pre-batch save (objective text, condition and action names are written through these strings).
9. Front-end screens (B1.4): the listbox, mechlopedia and mission-briefing screens are exercised by `tests/smoke` front-end scripts only if they exist; otherwise
   a viewed menu capture (`tools/visual_verify`, GL) before/after: sorted lists (H2 ordering) are compared by eye, MAD is not a signal.
10. `bash scripts/check-contracts.sh` (cxx17 ratchet moves down only; log tag census unchanged unless a `Format` line was a tag; comment-guard; file-size).
11. Opus review of the batch diff before the next batch starts (plan section 6); the review gets the characterization-test list and the H-table above.

## 6. Risks and open points for the reviewer

R1. **Case-insensitivity (H2)** is the highest-probability silent regression: a bare `==` after a rename keeps compiling and breaks lookups that depend on
    case folding (mech and pilot file names, variant names, mission names, `Compare(name, 0)`). Mitigation: no bare `==`; the helper; the corpus test.
    Question: ASCII fold or `lstrcmpi`/locale fold? The data is ASCII (stock) but mods can carry accented names; `lstrcmpi` is the status quo.
R2. **Null-as-empty (H1)** changes what C APIs see: `Data()` null becomes `""`. Some callees might treat null as "not given" and `""` as a name to
    look up (`seekBlock`, `playDigitalStream`, `AddString`). Each pass-through site is classified; unknown callee behaviour is tested, not assumed.
R3. **Dangling by-value returns (H4)** are invisible to the compiler and to ASan unless the freed block is reused; `/analyze` gate plus the manual
    grep net; the 10 setters' copy behaviour confirmed. Question: should `Title()`/`Description()` return `const std::string&` to the member instead of by value
    (keeps every existing `const char*` holder valid, removes the whole class of finding) at the price of exposing the member? Preferred here: yes for
    accessors that return a member, by value only for the computed ones (`LocalizedTitle`, `DescriptionOf...Species`).
R4. **printf templates from data (H9)**: string-table entries used as `Format` templates cannot move to `{}`; the printf formatter stays. A mod that
    ships a `%`-template keeps working; a `{}`-template in mod text must not be interpreted. Question: is a permanent printf-template helper acceptable, or
    should string-table templates move to a `{}` dialect with a one-time data pass? (Mods are content; a dialect change breaks them: recommended no.)
R5. **Save and mission-file strings**: `logisticsdata_save.cpp`, `saveload.cpp` (campaign names), editor objective writers. The strings are written
    through `fprintf`-style/`FitIni` writers: length handling (H7) and null-as-empty (H1) are the two ways bytes could change. Proof is the re-save byte
    identity run and the editor re-save compare; both exist or are defined above, the recorded-baseline form does not (6).
R6. **Gui bridge** adds overloads to three gui headers while lane 2 may be editing neighbouring gui/GameOS files: the bridge commit is small, and the
    file sets are disjoint from lane 2's claim (GameOS/, RenderCore/, shaders/, Gates/, test_gate.cpp); confirm at the start of B1.0.
R7. **Multiplayer screens (B1.5)** compile but cannot be exercised (no session layer): proof there is compile + build_all_targets + unit tests only; the commit
    says so. Ruling needed only if a mp* file turns out to depend on a `multplyr*` type that is OFF.
R8. **`/analyze` is unproven here** (no wiring, no seeded test): if the lifetime ruleset cannot be made to fire on the seeded negative within one hour of B1.0,
    the fallback is clang-tidy `bugprone-dangling-handle` + `bugprone-use-after-move` + `clang-analyzer-cplusplus.*` on the same TUs (clang-tidy is installed,
    `docs/host-tools.md`) and the manual grep net, and the review is told so.
R9. Header fan-out: `logisticsdata.h` is included by 33 files; a member change there recompiles most of `code/`: expected build cost about 2-3 min incremental in
    `build64-ninja`, 15 min for `build_all_targets` cold.
R10. The batch cap counts files in the grep set; the real touched set per batch is reported in each commit message (grep set + compiler-found includers) so
    the reviewer sees the number the cap is meant to bound.

## 7. Questions for the Opus reviewer (answer before B1.0 starts)

1. Is the bridge-first order (B1.0 bridge + pins, then callers leaf-first, class kept alive for the widgets) right, versus converting the widgets first
   (B6 before B1) so the class dies with its last user? The owner ruling says EString migrates with the widgets; the notes keep non-gui callers separate so the
   front-end and data-model batches (B1.2-B1.7) can proceed while B6 is still unscheduled.
2. H2: preserve case-insensitive equality everywhere (recommended) or take the opportunity to make names case-exact where the data allows (a content
   change; not recommended in a "same behaviour" step).
3. R3 accessor shape (reference to member versus by value).
4. R4 printf-template formatter kept permanently.
5. Is the batch order acceptable given that B1.2 and B1.4 touch the save path before the saveload baseline exists (6), or should the saveload baseline be a
   prerequisite of B1.2? (The notes use resave byte identity as the interim proof.)
6. B1.2 at 19 files: split in two?
