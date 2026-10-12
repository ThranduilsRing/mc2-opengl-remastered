<!-- doc-status: current | reviewed:2026-10-12 -->
# Target architecture: MC2 as we would build it today (north star, 2026-10-12)

Read-only survey at nifty HEAD 7d1c85389. Every number below was measured with git/grep/the ratchet at that HEAD; `[unverified]` marks a claim
only a build or a run can settle. This is the WHAT and WHY the week plan (`program-plan-2026-10-12.md`) and its reviewer are judged against; it
does not schedule anything smaller than an arc.
Owner rulings this document is designed against (2026-10-12): ONE final release, shipped only when the code is migrated and the renderer is
fully Vulkan, after which the project is done — so saves may break freely until then and "within a release line" binds only the final release
to its own saves; the mission editor is in the product and gets its own modernization arc; multiplayer is compiled out behind a CMake option,
not deleted; the harness/scripts/tools tree is a subsystem to cull; a second compiler waits for the Linux arc; agents run Tracy and RenderDoc
captures autonomously (no human in the loop). Section 7 says what "done for the final release" means.

## 1. What MC2 is, and its shape today

MC2 is a real-time-tactics game over fixed content: a sim loop (`Mission::update`) steps a world of pooled game objects (mechs, vehicles,
buildings, turrets, bolts) whose warriors run ABL scripts (276 host functions in `code/ablmc2*.cpp`) over a heightfield terrain with a global
pathfinder; a renderer (GameOS `gos_*` C API over OpenGL, with 45 Vulkan island files) draws appearances the sim owns; data arrives from FST
archives and `.pak/.fit/.tgl/.fx/.abl` files; the UI is widget screens (`gui/`, `code/*screen*`, `GuiRuntime/`); the mission editor is a real
MFC application (`_AFXDLL`, 181 `CDialog` uses, 156 `CString`, 93 `DDX_Text`, 37 dialog classes, its own `UserHeap` at Editor.cpp:285; 24 files
already use ImGui) sharing the engine through `EditorBridge/` and `EditorGosRender.cpp`; tools are 618 Python files (168k lines) plus C++ cooks
(`data_tools/`, `mc2fx`, `mech_import`) and 22 contract-harness build trees (8.9k lines C++). Multiplayer is a stub (3.1k lines `code/multplyr*`,
10.3k with the mp screens) still listed directly in `CMakeLists.txt:568`, not yet behind an option.

Measured shape (C++ lines; `find | wc`): code 184k (390 files), mclib 223k (603 files: mlr 38.7k, gosfx 16.3k, stuff 32.3k, abl 14.5k),
GameOS 124k (323 files; VK 21.2k in 45 files; `gameos.hpp` 3,753 lines declaring 227 `gos_*` functions, 208 of them called from code/mclib/gui),
RenderCore 12.6k, Gates 4.2k, editor 56.4k (119 cpp / 109 h) + ui_editor 9.3k, gui 11.6k + GuiRuntime 5.7k, RenderWorld 1.9k, GameAdapters 1.4k,
EditorBridge 0.5k; engine+editor about 635k. Shaders 209 files / 25k. Tests: doctest 2,741 `TEST_CASE` (14.5k lines), 194 text-only contract
rows calling 137 scripts, smoke tiers 5/24/9 missions. Tooling: 1,514 tracked files under tools/scripts/tests (618 .py, 296 .json, 151 .cpp, 27 .sh
of 5.1k lines); the biggest scripts are one-time codemods and guards (`gate_codemod.py` 2,459, `gate_idioms.py` 2,217, `comment_guard.py`
1,790, `cite_guard.py` 1,553); 50 scripts (5.3k lines) are named by no other tracked file. Docs: `docs/` 384 files / 110k lines, `.claude/` 240 / 34k.
Legacy idioms (cxx17 ratchet at HEAD): raw new/delete 2,397; C casts 6,685; sentinel returns 507; `#define` constants 1,049; unscoped enums 306;
typedef 691; NULL 693; printf/sprintf 409+624; pointer+count pairs 234; 2-D index math 170. Files over 1,500 lines: 50 (gameos.hpp 3,753,
RendererFeatureRegistry.h 2,917 generated, brain_special_dispatch.cpp 2,772, engine_present_vk.cpp 2,732, warrior.cpp 2,703, file.cpp 2,685,
objective.cpp 2,657, mapdata.cpp 2,631, mpparameterscreen.cpp 2,415, gameobj.cpp 2,370, turret.cpp 2,348; editor's largest is ObjectiveDlg 1,502).
Gates: 1,140 rows in `Gates/gate_table.json` (803 Bool); 353 are registry rows (195 Feature of which 132 default-off, 156 Trace, 2 Retired); 787
rows are not classified at all; `retireBy` is set on 0 rows. Target is under 30.
Platform coupling: `windows.h` in 41 files, `HANDLE` in 30, `__try` in 3, `LINUX_BUILD` (always defined, a misnomer) in 28 files; SDL2 already
owns the window, input and audio (`gameosmain.cpp`, `gameos_sound.cpp`, `gos_render.cpp`); no `CMakePresets.json`, no `vcpkg.json`; 3rdparty is
hand-vendored (imgui, tracy, volk, vma, vk_bootstrap, stb, tinyexr, meshoptimizer, assimp, ffmpeg) beside `find_package` for SDL2/GLEW/zlib/Vulkan.
VK handles: 298 `vkCreate*` vs 284 `vkDestroy*`, 0 `unique_ptr` anywhere in GameOS.
Determinism today: the sim steps by wall clock (`timeGetTime`, mission_update.cpp:308-341); the 30 Hz fixed step and the seeded LCG exist only
behind `MC2_SMOKE_FIXED_TIMESTEP` and `MC2_DETERMINISTIC_RNG`, both default OFF (retail = CRT `rand()`); one shared RNG stream has 89 call sites;
the camera updates inside `Mission::update` before the objects; camera reads inside sim code are few (mech_update 2, gvehicl_update 2, weather
26); mc2_10 was fixed this week (wall clock in ABL, camera-dependent weather draws), mc2_24 still has a bimodal race at sim frame 3193 (one
extra draw from the shared stream), mc2_15 dies of heap corruption (0xC0000374, ~70 % of runs with Tracy on, clean under ASan, writer unknown).
Save = `PacketFile` of raw `*Data` struct dumps dispatched on `packetSize == sizeof` (now static_asserted and rejected on mismatch); RNG state,
pending path requests, sensor contacts and vehicle animation state are not saved; uninitialised bytes are serialised.
Dead weight by size: MLR 38.7k (live only as the CPU VFX clipper), Stuff containers/streams (~10k), EString/EList/ECharString 5.6k, the heaps
(28 of them, 225 class `operator new` lines), the GL twins of every VK island, the MFC half of the editor, the codemod and guard scripts.

## 2. The from-scratch design

### 2.1 Modules and dependency direction
`platform <- core <- sim <- game <- ui <- editor`, with `render` beside `sim` (depends on core+platform, reads sim snapshots through adapters)
and `tools` on core only. One rule, enforced by the include firewall script that already exists (`check-include-firewall.sh`): sim never
includes GameOS, RenderCore or SDL headers; render never includes game headers; platform is the only module that includes `<windows.h>`.
- platform: SDL2 window/input/audio/timer/files, crash bundle, a `Clock` that the sim never sees. Today: the SDL parts of GameOS (keep);
  the 41 `<windows.h>` includes outside it and `mfcplatform.hpp` (delete or move); `HeapManager`'s `VirtualAlloc` (delete with the heap).
- core: log facade (`mc2log`, keep), settings (2.7), math (glm at step 5), validating readers for FST/.pak/.fit/.csv/packet (keep the formats,
  restructure the readers around one `ByteReader` over `std::span`), `std::string`/`vector`/`expected` instead of EString/EList/Stuff.
- sim: object pools, warriors, ABL host, pathfinding, weapons, teams, weather SIMULATION, craters; input = a `TacticalOrder` queue (it already
  exists, `tacordr.cpp`, fed by `handleOrders`); output = snapshots. No wall clock, no camera, no RNG but its own streams.
- render: RenderWorld records (exists, 1.9k) + GameAdapters (exists) + RenderCore pass spine + ONE backend, Vulkan. The ARCH-VERDICT ruling
  stands: unify data, policy and kill-switches, never dispatch or shading. gosFX particle simulation moves here (render-only state, 2.6).
- game: mission flow, logistics, campaign, objectives, save/load (2.6), the screens. ui: widgets + GuiRuntime. editor: see 2.10.

### 2.2 Ownership and memory
No custom heaps: `UserHeap`, `HeapManager`, `gos_Malloc/gos_Free/HGOSHEAP`, the replaced global `operator new` and all 225 class-operator
lines go; the shipped heap is already malloc, so this deletes behaviour nobody has (except the null-return-when-not-ready guards, which become
compile errors). Value semantics by default; an owner per subsystem with a written teardown order in one place (`Game` -> `Mission` ->
{terrain, object pools, teams, warriors, ABL environment, path manager, weather} and `RenderWorld` -> {adapters, batchers, texture refs}).
Game objects stay fixed per-type POOLS owned by the manager (slot index = handle = save format, generational guards exist); everyone else holds
a handle. Shared assets (TG shapes, appearance types, class data) live in a cache owned by the Mission or the editor document, never a global;
textures through a move-only `TextureRef`. The step-4 plan (`cxx23-step4-ownership-plan.md` with the Opus fixes) IS this section per subsystem;
adopt it unchanged. Already matching: `gpu_buffer_owner`, `frame_arena`, `mc2::particles::SpecLibrary`, generational object handles.
Deleted: heaps, EString/EList/ECharString, Stuff containers and streams, `heapReady()` guards, 188 post-Malloc null checks. Restructured: pools,
caches, UI widget trees (`vector<unique_ptr<Widget>>`), file objects (scope-bound), ABL (2.5), the editor's own heaps and undo copies (2.10).

### 2.3 Sim and render separation
The sim steps at a fixed 30 Hz tick (the value `SmokeMode::fixedTimestepMs()` already uses) with an accumulator in the frame loop; the render
reads the latest snapshot (an RTS camera tolerates 33 ms without interpolation; interpolation is an addition, not a redesign). Rules: the only
sim inputs are the tick count, the `TacticalOrder` queue and mission data; wall clock reads in sim code are compile errors (the `GetLogisticsTime`
bug was exactly this); the camera is render state, so the four `eye->projectForScreenXY` sites in mech/gvehicl update move to the HUD and
`Weather` splits into sim (lightning timing, if gameplay reads it) and render (drops); every system owns an RNG stream seeded from the mission
seed (weather has one since 9782c9555; combat rolls, AI, craters, animation fall direction and bolt effects still share `gos_rand`), and
view-dependent code may never draw from a sim stream. The per-frame SIMID digest (`code/sim_identity.cpp`, movers only today) extends to
buildings, turrets, ABL state and per-stream RNG call counts and becomes the sim's primary oracle. WHY: determinism is a requirement, lockstep
multiplayer later is only "the same order queue on two machines", captures and smoke become stable, and a headless `mc2 --headless --ticks N`
(no window, no GPU; today only an offscreen window exists) lets tier2 run in minutes without the host lease.

### 2.4 Data pipeline
Source assets (`data/`, mods) -> cooked archives (FST/.pak, formats unchanged: they are mod content and the tools in `data_tools/` write them)
-> loaders that validate. Every reader returns `std::expected<T, LoadError>` or rejects with one `[LOAD v1] reject` line (ruling 5); readers
share one bounds-checked `ByteReader`. Installs carry no loose overrides (the jaegermech.ini incident); the STOCK-DATA-TRUTH gate already pins
missions, extend it to `data/tgl`. Keep: `elfHash`, LZ/zlib decoders, `.fx` byte layout, `.fit` grammar. Delete: `MemoryStream` once the
last gosFX loader reads spans. Restructure: inifile/packet/file into the reader model (the UB repairs this week were the first step); the
editor writes missions through the cooks' writers (2.10).

### 2.5 Scripting: keep ABL as a frozen VM behind a host interface
Keep ABL. WHY: 24 missions, the tutorials and every mod are `.abl` content, and the 276 host functions are the de-facto modding API; a Lua
port re-authors content for no maintainability gain, and the interpreter (14.5k lines) is small next to its content. What changes: the host
functions become one typed table (name, signature, handler) generated from a list, not 276 hand-written `exec*` bodies with manual argument
pops; the VM owns its memory (`std::vector<StackItem>` stack, one `std::pmr` resource per module, host malloc callbacks deleted); the
environment owns every module instance, warriors and the mission hold non-owning handles (today two ownership modes, review F3); no host
function may read the clock or the camera (sim rule); saved ABL state is a save section (2.6); the debugger (`abldbug*`, 1.2k) is Q5. Frozen syntax.

### 2.6 Save format: one versioned schema, written once
Because saves may break freely before the final release, there is no "same bytes" stage: the new format lands once, directly. A save is
`{header: magic, schema version, build sha, data fingerprint} + tagged sections`, each type serialised by ONE `serialize(Archive&)` used for
both directions, with a per-type round-trip doctest (random instance -> bytes -> instance -> equal) and `saveload_roundtrip.py` as the
integration oracle; it carries what the sim needs to resume exactly: per-stream RNG state, pending path requests, sensor contacts, vehicle
animation state, the tick count, ABL state. Old saves are refused with one log line (the reject path landed in 58d3ce9ce). The final release's
schema version is frozen at release and binds only that release. Keep: nothing of the packet layout is sacred (`PacketFile` stays only if a
tool reads it [unverified]). Delete: raw `memcpy` of `*Data`, uninitialised-byte serialisation, the `packetSize == sizeof` dispatch.

### 2.7 Configuration: settings, not gates
Three kinds, each with one home: user settings (`options.cfg`: resolution, audio, quality); developer settings (a `dev.cfg` read by the same
typed registry: capture, trace categories, fixture hooks); CMake options (what is compiled: Tracy, ASan, editor, multiplayer OFF). A runtime
GATE is a temporary switch with a mandatory `retireBy` and a losing path that is deleted when it retires; the amnesty rule is that every one of
the 1,140 rows becomes a setting, a log category (the 156 Trace rows), a CMake option, or is deleted with its losing path. Keep: the typed
registry (`gate::get<>`), `gate_gen.py`. Delete: the 787 unclassified rows' env semantics, `RendererFeatureRegistry.h` as a second inventory,
every GL/backend gate once GL is gone. Restructure: trace gates into `mc2log` categories.

### 2.8 Testing and proof
Four tiers, each with one oracle, all runnable by an agent with no human: doctest under ASan (unit; lifetimes, readers, serialisers, math
corpus); headless sim digests (tier2 = all 24 missions, run-to-run identical, in minutes, every merge); captures the AGENT views (goldens,
day and night; MAD is not a pass) with a maintained frame debugger for the pass-level diff; Tracy deltas for every perf claim; page-heap
tier2 once per arc. Ratchets freeze outside the current step (meta-review B9). A `verified.json` ledger replaces prose.
Maintained tool over home-grown (ruling): a home-grown tool survives only if it proves something the maintained one cannot, and that is
worth owning. The judgments:
| need | adopt (maintained) | home-grown today | verdict: what ours proves that the tool does not |
|---|---|---|---|
| frame capture + pass diff, GL and VK | RenderDoc python API (`renderdoc` module) | `renderdoc_capture.py` (drives renderdoccmd), `tools/renderdoc/` | keep only the 100-line launcher; pass diffs move to the API; delete the convert/diff scripts |
| VK regression and parity oracle | GFXReconstruct: `gfxrecon-capture` at the golden frame per mission, `gfxrecon-convert` command-stream diff + offscreen replay image | VK present-dump PPM path, `vk_feed_dump.h`, hand VK capture oracle (R1 step 1) | nothing: the stream diff names the changed draw, which no pixel compare does; R1 step 1 becomes "wire GFXReconstruct" |
| pixel goldens at a deterministic sim frame | none does bookmark-paired goldens with a sim-digest sidecar | `visual_verify/{capture,ab,verify,bless,montage_compact}.py` | keep as the thin golden layer (~1.5k lines); delete `regions`, `triptych`, `capture_ab` duplicates |
| VK correctness in smoke | Vulkan validation layers + vkconfig profile; zero errors = pass row in tier1/tier2 | layer requested at `vulkan_context.cpp:177`, not graded | nothing; grade it |
| GPU perf and GPU crash | AMD RGP + RGD via Radeon Developer Panel on the 7900 XTX (agent-run; CLI automation [unverified]) | `vk_gpu_timestamps.h` Tracy GPU zones | keep the Tracy GPU zones (continuous); RGP/RGD for deep dives and for the mc2_15 driver-writer hypothesis |
| CPU perf | Tracy + `tracy-csvexport` | `tracy_capture_compare.py` (capture + per-zone table) | keep the 200-line capture driver; the compare becomes csvexport + diff |
| idiom rewrites | clang-tidy `modernize-*` (use-nullptr, use-using, use-std-print/format, macro-to-enum, use-override), `readability-*` | `tools/tidy` 35 files: printf rewriters, `rewrite_defines/enums`, `objhash` proof | keep `objhash` (object-identity proof, no equivalent) and `rewrite_enums` (no modernize check for enum class); printf rewriting moves to `modernize-use-std-print/format` under the same corpus proof; delete the rest when steps 2/3 close |
| static analysis | MSVC `/analyze` now (C26815/C26816 already named by the step-4 plan); clang analyzer + cppcheck in the Linux arc | none | nothing; add the board row |
| memory errors | ASan (MSVC now), UBSan at the Linux arc, Application Verifier page heap for the real-heap writer | `heap_shim`, lane-3 `quarantine_patch.py` page-guard allocator | delete the quarantine patch; `heap_shim` stays only as the unit-tier allocator |
| duplicate code | jscpd (or PMD CPD) with a tightening threshold | `dup_lines_ratchet.py` + baseline JSON | nothing worth owning; delete the ratchet |
| shader reflection and ABI lockstep | SPIRV-Reflect / SPIRV-Cross (reflection already runs on them [which: unverified]) | `tools/shader_reflect`, `gpu_abi` generator, lockstep checks | keep only the C++-vs-GLSL lockstep generator (reflection tools do not know our C++ structs) |
| sim identity, save round-trip, smoke lease, gates, lane merge | nothing maintained fits | `sim_determinism.py`, `saveload_roundtrip.py`, `run_smoke.py`, `gate_gen.py`, `lane_merge.py` | ours by nature: keep, and keep them small |
Keep: doctest tier, `build_all_targets.py`, `objhash`/`split_verify` until the text-rewrite steps close. Delete: harness build trees whose
cases fit doctest (22 configures), contract rows that guard idioms a compiler or clang-tidy now guards (audit the 194; keep format/ABI rows).

### 2.9 Build and portability
`CMakePresets.json` (msvc-ninja-dev, msvc-asan, msvc-release; linux-gcc added in the Linux arc), a `vcpkg.json` manifest for SDL2/mixer/ttf,
zlib, glm, assimp, Vulkan via the SDK; `3rdparty/` keeps only pinned source deps (imgui, tracy, volk, vma, vk_bootstrap, stb, tinyexr,
meshoptimizer). No second compiler until the Linux arc (ruling 5); until then portability is a shape rule: `<windows.h>` only in platform, no
new HANDLE/COM/SEH, the 3 `__try` sites replaced by the existing vectored crash handler. `LINUX_BUILD` is deleted (always defined, so its `#else`
arms are dead; the dead-asm method). Vulkan only: one backend with RAII owner types over volk/VMA; GL deleted after R1. SDL and the PCH stay.

### 2.10 The mission editor
Target: one SDL+ImGui application (`EditRel` today) that is the game's module stack plus an `editor` module: an editor DOCUMENT (the mission
being edited: terrain, objects, objectives, teams, ABL assignments, campaign links) with explicit ownership and a command-pattern undo stack
(today `EList<EditorObject*>` copies sharing refcounted appearInfo, Action.h:103-140), ImGui panels over that document (the 37 MFC dialog
classes become panels; `EditorInterface_imgui.cpp` and the ImGui inspector are the seed), a viewport that renders through `EditorBridge` on the
SAME Vulkan backend as the game (no GL in the editor either), and writers shared with the cooks so a loaded-and-saved stock mission is
byte-identical. Deleted: `_AFXDLL`, `mfcplatform.hpp`, `EditorMFC.cpp`, `CDialog/CString/DDX_*` (430 uses), the editor's private `UserHeap`/
`tglHeap`, the Win32 message loop. Kept: `EditorObjectMgr`, `EditorData*` (the document model, restructured to owned values), the 15-case
editor smoke as the floor (`scripts/run_editor_smoke.py`), mission `.fit`/`.pak` formats. WHY: the editor is in the product; MFC blocks every
portability and ownership rule above; dialogs on ImGui are the cheapest full-coverage UI an agent can smoke-test without a human.

## 3. Gap table

| today's subsystem | target home | action | blocker | payoff | effort | what proves it |
|---|---|---|---|---|---|---|
| Mission::update wall-clock step, shared `gos_rand` | sim kernel (2.3) | restructure | mc2_24 bimodal race root cause | high | med | tier2 24/24 run-to-run identical digests |
| camera inside the sim step, `eye->` in update code, Weather | render + sim split | restructure | none | high | low | camera-park pair identical on every tier1 mission |
| `TacticalOrder` -> `handleOrders` direct apply | sim command queue | restructure | Q2 (one-tick order latency) | high (MP road) | med | replay of a recorded order log reproduces the digest |
| no headless mode (`MC2_WINDOW_OFFSCREEN` only) | platform + sim | rewrite (new) | sim/render firewall | high | med | `mc2 --headless` runs tier2 with no GPU |
| UserHeap/HeapManager/class operator new, 28 heaps | deleted | delete | B0 harness | high | high | LG03 at the named floor; page-heap tier2 green |
| EString/EList/ECharString, Stuff containers/streams | std | delete | widgets batch (EString) | med | med | build_all_targets, boot-to-screen captures |
| object pools + generational handles | sim (owned by manager) | keep/restructure | none (saves may break) | med | med | objmgr harness with slot reuse |
| `*Data` struct dumps, PacketFile | save schema (2.6) | rewrite | S1 (what state the sim has) | high | med | per-type round-trip doctest, roundtrip identical post-load |
| ABL VM + host callbacks + 276 exec bodies | sim/script host | restructure | none | med | med | mc2 digest unchanged; `.abl` corpus unchanged |
| ABL instance double ownership | environment owns | restructure | save schema | med | low | saveload_roundtrip under ASan |
| MLR clipper/lights/light map (38.7k) | deleted | delete | GPU-particle parity oracle | med | high | VFX A/B captures viewed, VK; -38k lines |
| gosFX CPU particle sim (16k) | render-only | keep/restructure | Q6 | low | low | digest excludes it; captures unchanged |
| GL backend + GL twin shaders + GLEW | deleted | delete | VK flip checklist items 1-13 | high | high | goldens re-blessed under VK, viewed; no GL symbol links |
| VK islands, raw handles | render backend with RAII | restructure | none | med | med | vkCreate = owner count; ASan unit tier |
| gameos.hpp 3,753 lines / 227 gos_* | platform.h + gfx API split | restructure | include firewall rule | med | med | firewall row; compile-time delta |
| `<windows.h>` in 41 files, HANDLE 30, `__try` 3 | platform only | restructure | heaps gone first | med | low | firewall row; Linux arc compiles |
| `LINUX_BUILD` arms (28 files) | deleted | delete | none | low | low | `cl /EP` identical (dead-asm method) |
| 1,140 gate rows | settings / log categories / CMake | delete most | per-gate losing-path proof | high | med | registry < 30 runtime gates |
| 156 Trace gates | mc2log categories | restructure | logging facade final API | med | low | log_identity on tier1 |
| editor: MFC app, own heaps, pointer undo, GL viewport | ImGui editor on the VK stack (2.10) | rewrite | S2 heaps, R1 VK viewport | high (product) | high | editor smoke; stock missions round-trip byte-identical |
| multiplayer 10.3k (listed in CMake directly) | `MC2_MULTIPLAYER=OFF` option, redesign later | compile out now | none | low now | low | default build has no multplyr objects |
| tools/scripts/tests: 618 py / 168k, 22 harness trees, 194 rows, 27 sh | maintained tools per the 2.8 table (RenderDoc API, GFXReconstruct, validation layers, RGP/RGD, Tracy csvexport, clang-tidy modernize, /analyze, ASan/UBSan, AppVerifier, jscpd, SPIRV tools) + one inventory naming the caller of every survivor | adopt + delete most | audit | high (agent speed) | med | inventory check row; Python lines and board time halve; each deleted tool's proof named by its replacement |
| docs/ 110k lines, 36 stale cleanup docs | archive on E: | delete | none | low | low | doc_links green |
| `Stuff::` math (6,053 uses) -> glm | core math | restructure (step 5) | bit-identity corpus | med | high | corpus bit-identical or sim stays Stuff |

## 4. The highest-payoff attacks (judged, in attack order)

Three file-disjoint lanes: S (sim and data: code/, mclib/ non-render), R (render: GameOS/, RenderCore/, shaders/) and E (editor/). T (tooling)
is a short solo arc first because every other arc's agents pay for the bloat. Effort is in Fable-agent-days of 4-10 h arcs including proofs.

**T1. Tooling cull** (~4 days, first, solo). Rule: a script, harness or board row survives only if a tracked caller names it (CMake,
`check-contracts.sh`, `run_smoke.py`, `tidy_batch.py`, `lane_merge.py`, `build_all_targets.py`) and that caller is itself live; one-time
codemods are deleted in the commit after they land (`gate_codemod.py`, `comment_guard.py`, `cite_guard.py`, the `phase/proofs`, the tidy
rewriters once steps 2/3 close); the 22 harness build trees become doctest cases or die; the 194 rows are audited to the ones that guard a
format, an ABI or a launch rule; `terrain_beautify` (11.1k), `blender_pipeline` (5.1k), `unit_converter` (5.0k) are kept only if the final
release's data pipeline calls them; every maintained adoption in the 2.8 table lands here with the home-grown tool it retires. Output:
`tools/inventory.json` (tool -> caller -> proof it replaces) with a board row that fails on an unlisted script. WHY first: 168k lines of
Python is three quarters of mclib; agents read it, wait for it and maintain it. Proof: Python lines and board wall time halve; tier1 and
doctest unchanged; validation layers graded in tier1 from this arc on.

**S1. Deterministic sim kernel + headless runner** (~10 days). Fixed 30 Hz tick default ON (retail path deleted, not gated), per-system RNG
streams, wall clock and camera banned from sim by the firewall, `TacticalOrder` queue as the sole input, `--headless` mode, SIMID extended
(buildings, turrets, ABL, RNG counts) and written into every smoke artifact. WHY: every later proof (ownership, save, ABL, step 5 math) is
"digest unchanged", which is worthless until the digest is trustworthy and cheap; headless tier2 removes the host lease and the 20-minute
waits that shaped this week. Unblocks: S2-S4, lockstep later, tier2/3 in cadence. Blockers to clear first: the mc2_24 race at frame 3193 (lane
1 has it pinned to one extra shared-stream draw: per-system streams are the structural fix, but the root cause must be named before the pair
is declared green) and the mc2_15 writer (page heap on the lane-3 install, owner action; until then tier2 is 23/24 at best). Proof: tier2
24/24 identical over two runs and two camera poses, headless digest equals windowed digest, mc2_10/mc2_24 camera-park pairs.

**S2. Ownership redesign, step 4 B0-B8** (~16 days). The plan as written with the review fixes, minus the save-compatibility stages: order
B0, B1 EString, B2 EList/containers, B4 raw-buffer heaps, B5 caches+textures, B6 widgets, B7 pools, B8 gosFX+ABL; the editor's heaps are
E1's. WHY: it deletes the heap class of bugs (use-after-free through "reclaimed with the heap", the mc2_15 shape), removes `VirtualAlloc` and
most Win32 from mclib, and is the prerequisite for the save and ABL redesigns. Blockers: S1 (digest), the B0 harness (alloc_probe, ASan unit
tier, own_inventory, pointer-ordering scan), tier3 mods installed before B3. Proof: LG03 at the named floor, heaps and class operators gone,
mission-reload delta, page-heap tier2 green, captures viewed for B5/B6.

**S3. Save schema** (~5 days, after S1, before or during B7). One versioned schema, written once (2.6). WHY: the final release must resume
the sim exactly (RNG, path requests, contacts), and a replay file is the determinism hunt's best tool. Proof: per-type round-trip doctests,
roundtrip identical post-load for the full run, a v0.4 save refused with one line.

**S4. ABL host interface** (~4 days, inside/after B8). Generated host table, VM-owned memory, environment-owned instances, clock/camera-free
host functions. WHY: 276 hand-written exec bodies are the largest unreviewed API surface in the sim and the last place a wall clock leaked.
Proof: digest unchanged on tier2, `.abl` corpus byte-unchanged, saveload under ASan.

**R1. Vulkan only** (~18 days, parallel to S1-S3). Order: VK-aware capture oracle (checklist item 1; goldens read the GL FBO so VK was never
covered), RAII handles island by island, the parity gaps (dynamic buildings, mech shadows, night light pools, letterbox, FORCE_43), the editor
viewport through `EditorBridge` on VK, flip (`git revert abffc9ab7`), goldens re-blessed, then delete the GL backend, GLEW and the twin shaders
and their gates. WHY: the final release is fully Vulkan by ruling; two backends double every render change and every capture. Blockers: the
capture oracle, the shadow stride fix, Q3 (GL fallback). Proof: the 15-step checklist, tier1 under VK, day+night A/B viewed by the agent,
RenderDoc pass diff on any differing capture, Tracy delta recorded, no GL symbol in the link.

**E1. Editor rework** (~18 days, lane E; UI shell first, heaps after S2 B4, viewport after R1's editor step). Order: SDL+ImGui host replaces
the MFC message loop and `EditorMFC.cpp`; document model with owned values and a command undo stack; the 37 dialogs become panels in order of
the editor smoke's coverage (terrain, objects, objectives, teams, campaign); shared writers; delete MFC. WHY: product-critical by ruling, and
the only module that blocks the no-Win32 and no-heap rules outright. Blockers: S2 B4 (its heaps), R1 (its viewport). Proof: editor smoke
(expanded to every panel), every stock mission loads and re-saves byte-identical, editor under ASan, agent-viewed captures of each panel.

**R2. MLR retirement via GPU particles** (~8 days, after R1's capture oracle). Bring `MC2_GPU_PARTICLES` to parity on effects-heavy captures,
flip, delete the MLR clipper/lights/light map; gosFX stays as the particle simulation (render-only). WHY: -38k lines of the oldest code, one
VFX path. Blockers: a VFX parity oracle (none exists), Q6. Proof: VFX A/B viewed on missile/PPC scenes, mc2fx dump identical.

**X1. Platform module, then Linux** (~10 days, after S2 and R1). gameos.hpp split, `<windows.h>` confined to platform, `LINUX_BUILD` arms
deleted, presets + vcpkg manifest, then the second compiler and the Linux build (ruling 5). Gate amnesty is not an arc: each arc deletes its
gates with its losing paths, and one 2-day sweep moves Trace rows to log categories.

Not arcs: the mc2_15 writer (a blocker inside S1/S2, page heap not redesign); GameOS giants (ride X1); step 5 math (after S1's digest and the
bit-identity corpus); multiplayer compile-out (a 1-day item inside T1).

## 5. Stop doing, or delete outright
- Delete after an audit, tag first: MLR (R2), the GL backend, GLEW and twin shaders (R1), `UserHeap`/`HeapManager`/class operators (S2),
  EString/EList/ECharString and the remaining Stuff containers/streams (S2), `LINUX_BUILD` `#else` arms (X1), the MFC half of the editor (E1),
  `MemoryStream` after the last span reader, the 787 unclassified gate rows' env semantics and `RendererFeatureRegistry.h` as a second inventory
  (2.7), the codemods and guards named in T1, harness build trees whose cases fit doctest, contract rows a compiler now enforces.
- Stop: gating new behaviour behind env vars (a gate needs `retireBy`); "same bytes" proofs for codegen changes (digest and ASan instead);
  prose verification bullets; preserving save compatibility before the final release; editing the compiled-out multiplayer; loose data
  overrides in installs; windowed tier2 once headless exists; waiting for a human to view a capture (the agent views and records it);
  writing a new proof tool before checking the 2.8 table; Opus per batch (one Fable review per arc against this document).
- Archive to `E:/mc2-archive-2026-10/docs`: the 36 stale cleanup docs, the `docs/` corpus no checker reads (`doc_links` first), old handoff text.

## 6. Questions for the owner (only where the answer changes the design)
1. Sim tick: fixed 30 Hz (today's smoke value, closest to the original feel) is the design; confirm, since it fixes gameplay speed, the save
   tick unit and the replay format for good.
2. May single-player orders go through the same `TacticalOrder` queue as a future lockstep peer, i.e. applied at the next tick (up to 33 ms
   later) instead of immediately? Yes makes multiplayer a transport problem; no keeps a second apply path forever.
3. GL after the VK flip: the design deletes it entirely (one backend, GLEW gone). Confirm there is no GPU without Vulkan 1.2 you must support.
4. Editor scope for the final release: which workflows are must-have (terrain paint, object placement, objectives, teams/ABL assignment,
   campaign links, mission export) and which existing dialogs may be dropped rather than rebuilt? E1's panel list is this answer.
5. ABL: confirm the language and bytecode are frozen (no new syntax, host table is the modding API) and whether the ABL debugger stays as a
   modder tool or is deleted.
6. Are visual effects render-only, i.e. may gosFX move out of the sim digest and later be simulated on the GPU with no gameplay reads?
   If any gameplay reads effect state (hit timing, smoke blocking sensors), say which and it stays in the sim.
7. Tooling cull: may T1 delete `terrain_beautify`, `blender_pipeline`, `unit_converter` and the BT2018/mco/mc2x import tools if the final
   release's data pipeline does not call them (they are kept in git history and the E: archive)?
8. The page-heap IFEO key for `mc2-ph.exe` on the lane-3 install: still pending on your side; it is the first blocker of S1.

## 7. "Done for the final release" checklist
1. Renderer: Vulkan is the only backend; no GL/GLEW symbol in any target; goldens blessed under VK on all 24 missions, day and night, viewed.
2. Sim: fixed 30 Hz tick, per-system RNG, no wall clock or camera in sim (firewall row), tier2 24/24 run-to-run and cross-camera identical,
   headless digest equals windowed; mc2_15 and mc2_24 closed with named root causes.
3. Memory: no custom heaps, no class `operator new`, LG03 at the named floor, doctest tier and `saveload_roundtrip` green under ASan, page-heap
   tier2 green, mission-reload delta recorded.
4. Save: one versioned schema, per-type round-trip doctests, full-run roundtrip identical post-load; the shipped version frozen and documented.
5. Data: every reader rejects malformed content with one log line (fuzz doctest per reader); tier3 mod missions pass; installs carry no loose
   overrides; cooks regenerate the shipped data from source.
6. Editor: SDL+ImGui, no MFC, no private heaps, VK viewport, every stock mission round-trips byte-identical, editor smoke covers every panel.
7. Configuration: under 30 runtime gates, each with `retireBy`; settings in `options.cfg`/`dev.cfg`; Trace rows are log categories.
8. Code shape: no file over 1,000 lines, `gameos.hpp` split, `<windows.h>` only in platform, `LINUX_BUILD` gone, multiplayer behind
   `MC2_MULTIPLAYER=OFF`, MLR and the Stuff containers deleted, C++23 steps 1-5 closed (step 5 by the bit-identity ruling).
9. Proof and tooling: `tools/inventory.json` names every surviving script and its caller; `verified.json` records the release sha's build,
   doctest, board, tier1/2/3, captures and Tracy baseline; the whole proof runs with no human in the loop.
10. Portability: Linux build green on the second compiler with tier2 headless identical to Windows digests (macOS is after this list).
