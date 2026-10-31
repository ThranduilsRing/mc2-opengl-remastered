# P3 record thread — living plan, 2026-10-31

Assignment: `A:/Games/mc2-lane8`, `claude/record-thread`, base `678380b2a179d5f4167a8361fa5ef06ae9fb62a7`.
One engine lane; local commits only. Current checkpoint includes live census instrumentation.
Continuation base: rebased cleanly onto landed integration checkpoint `678380b2a` (includes lane4 `56467c359`).
Inventory anchors and archived perf numbers below refer to the original assignment base; full-suite receipts also precede the rebase.

## Delivery and invariants

Move recording out of main-thread DrawScreen; its remaining seal/submission work must be below 0.1 ms.
Allow one extra display frame, retain one-tick order latency. Packet arenas N=3; no raw GameObject/TG pointers in packets.
Keep inline execution as the kill switch and the shipping default until the threaded path passes every gate.
Do not introduce a default-OFF feature gate. Preserve editor/viewer/headless inline behavior and mod installs.
Canonical design: maintainer memory `process/decision-fable-render-thread-and-gate-design-2026-10-29.md` (P3, ownership map, latency).
Current user assignment overrides the historical mc2-main-only restriction. Follow `A:/Games/mc2-ops/lane-rules.md`; JOBS=6.
S4 design direction: extend the existing present worker to consume the complete sealed recording packet and then present it,
rather than introduce simultaneous record and present owners of VulkanContext. One worker owns GPU recording/submission/WSI;
Main retains SDL/input, UI building, camera and extraction. Validate this against P2 handoff and inline fallback before implementation.

| Step | Status | Exit requirement / next action |
|---|---|---|
| S1 inventory | Native shape-leaf census DONE at `46a9f82a7`; external receipts still open | Farm repeats and save/load policy/reference mismatch remain open; no further census changes needed |
| S2 explicit state | DONE, proven engine `6fcde0272` | CPU view values, packet-owned light rows/caches and editor viewport callers; final receipts below. Next S3 |
| S3 rings/lifetimes | IN PROGRESS: value arenas N=3 at `3a856fc82`; typed-release graveyard at `c8022b57f`; CPU arena leases at `3b855d8f1`, all proven | RenderList/HUD values, remaining packet/GPU lifetime integration, Execute(main)/Draw(record), moved-entry owner assertions still open |
| S4 thread | NOT STARTED | Seal after sim commit, consume sealed packets, retire safely; default ON only with all gates green |

## S1 source inventory at the base

Symbols grouped only when ownership and snapshot treatment agree. `Main` includes current presentation/update work;
`Sim` is the existing overlap worker. These are migration obligations, not claims that the current packets satisfy them.
GPU `RenderCore::ViewUniforms` already exists (144-byte ABI); do not silently extend it for the legacy TGL CPU state.

| Symbol / state read (or mutated during draw) | Source anchor | Mutable by whom today | Required snapshot / ownership treatment |
|---|---|---|---|
| Environment.drawableWidth/Height, renderer, UpdateRenderers callback | GameOS/gameos/gameosmain.cpp:578; code/mechcmd2_frame.cpp:16 | Main SDL resize / engine setup | Copy dimensions and dispatch mode at seal; keep resize and window events on Main |
| g_disable_quads; ValidateConfig.shadowsOverride | GameOS/gameos/gameosmain.cpp:567 | Main renderer / validation control | Record-owned draw state; copy mutable validation options before seal |
| gosPostProcess dimensions, prevFrameHadTerrain_, scene attachments | GameOS/gameos/gameosmain.cpp:569 | Current draw and present/resource management | Record owns scene lifecycle; immutable dimensions/options per packet; synchronize resize/destruction with retirement |
| SnifferMode, mission/logistics/options mode, hasGuardBand, canMultiTextureDetail | code/mechcmd2_frame.cpp:18 | Main UI / transitions / draw | Main selects route; snapshot dispatch/config values; nonmission UI stays inline until its data is frozen |
| userInput viewport/cursor; post-ImGui cursor callback | code/mechcmd2_frame.cpp:62; code/gamecam.cpp:729 | Main input/update AND draw | Keep viewport writes/input on Main; extract cursor geometry and avoid callbacks capturing live input |
| missionActive/result, turn, scenarioTime, dt, alpha, frame | code/render_view.h:23; code/render_view.cpp:22 | Sim clock; Main extraction; worker frame increments | Copy RenderClock by value into selected packet; never reread live frameLength or counters |
| KillAmbientLight, ambient RGB temporary override, useFog | code/mission.cpp:744 | Main camera/weather and draw | Resolve override into packet lighting without writing the live eye during recording |
| eye cameraOrigin/cameraToClip/worldToCamera, projection, screenResolution, globalScaleFactor | code/gamecam.cpp:704; code/gamecam.cpp:232 | Main camera update AND render setup | Copy matrices, scale, projection/viewport constants; remove draw writes to live camera/input |
| eye ambient/directional lights, fogColor/start/full, lightDirection, night/daylight state | code/gamecam.cpp:714; code/gamecam.cpp:386 | Main camera/weather update and render | Freeze resolved scene/environment lighting and fog, preserving exact conversions/order |
| TG_Shape::s_cameraOrigin, s_cameraToClip, s_worldToClip, s_worldToCamera | mclib/tgl.h:879; mclib/tgl.cpp:96; mclib/camera_update.cpp:407 | Main camera update; SimpleCamera preview | Value matrices in explicit CPU view state; no borrowed camera matrix pointers |
| TG_Shape::viewMulX/Y, viewAddX/Y | mclib/tgl.h:886; mclib/tgl.cpp:121; code/gamecam.cpp:727 | Current draw / preview | Copy viewport values into per-view state; keep preview state separate |
| TG_Shape::fogColor, fogFull, fogStart, lighteningLevel | mclib/tgl.h:891; mclib/camera_update.cpp:409; code/weather.cpp:399 | Main camera/weather; mission reset | Freeze fog/lightning in explicit view state, including night storm changes |
| TG_Shape::s_listOfLights, s_numLights; TG_Light values | mclib/tgl.h:883; mclib/tgl.cpp:130; mclib/vk_object_lights.cpp:97 | Main appearance/light selection and draw | Copy light values, not TG_LightPtr arrays; preserve per-actor light selection |
| TG_Shape per-leaf lightData_, fogRGB, aRGBHighlight, lightsOut, alpha/classification | mclib/tgl.h:858; mclib/tgl.cpp:169 | Main presentation/appearance update; draw light slot re-submit | Extract transformed leaf/material/light values using stable resource handles, not TG_Shape pointers |
| vkLights light table/count/capacity, row generations/revision | mclib/vk_object_lights.cpp:25; mclib/vk_object_lights.cpp:476 | Main presentation/current Render owner | Packet-key dynamic rows and indices; preserve immutable baked rows with lifetime retention |
| vkLights dedup map, scene template map/frame, actor light color | mclib/vk_object_lights.cpp:38; mclib/vk_object_lights.cpp:514; mclib/vk_object_lights.cpp:593 | Current Render owner; reset each frame | Packet-key maps/reset and copied actor color; mutex alone does not freeze frame contents |
| RenderCore current EngineView, FramePackets views/lighting | code/gamecam.cpp:257; RenderCore/FramePackets.h:42 | Current GameCamera draw publisher | Publish at Main extraction, retain values by packet sequence; distinguish CPU packet retirement from GPU ticket |
| g_clock, g_list, g_hud singleton storage | code/render_view_store.cpp:8 | Main extraction and tick commit | Three slots selected explicitly by sequence; no singleton overwritten while record reads |
| RenderView::obj, watchId/generation, effects[5], position/aux/flags/mode | code/render_view.h:49; code/objmgr.cpp:1032 | Main extract; Sim object changes; Main frees at commit | Replace object/effect pointers with value draw packets and generation handles; generation checks cannot prevent concurrent mutation |
| ObjectManager terrain/movers/other ordering and draw-state switches | code/objmgr_update.cpp:933; code/objmgr.cpp:1073 | Main extraction; current draw | Preserve three ordered streams and early-stop semantics; no virtual dispatch through live GameObject |
| RenderSnapshot spans, s_arenaBuffers[2], cached static-prop rows, last snapshot | GameOS/gameos/render_snapshot.cpp:49; GameOS/gameos/render_snapshot.cpp:66 | Main extract/debug inspection | Convert CPU arenas to N=3 and retire by sequence; existing GPU N=3 does not fix this two-slot CPU arena |
| HudMover object/pilot identity, orderStatus, names, groups, health, contacts, paths | code/render_view.h:81; code/controlgui_render.cpp:469; code/forcegroupbar.cpp:33 | Main extracts Sim state; UI owns selection/roster | Copy strings/scalars/path points; replace pointer identity with ID+generation; orderStatus currently points only to immutable string literals |
| HudMover waypointMarkers shared BldgAppearance | code/render_view.h:97; code/warrior_draw.cpp:30 | Main waypoint presentation / draw | Freeze marker draw data; shared_ptr retains lifetime but does not isolate mutation |
| HudObjective object identity, text/status/points; HudSensor/blip values | code/render_view.h:102; code/controlgui_render.cpp:554 | Main extraction from Sim/objectives | Copy values and stable identity; audit InfoWindow and roster consumers transitively |
| HudVtol appearance/dust/beam raw void pointers | code/render_view.h:111; code/missiongui.cpp:454 | Main VTOL/effect update; draw | Frozen appearance and FX draw data; raw void pointers are not an acceptable packet escape hatch |
| missionInterface drag state/control GUI, FloatHelp/currentFloatHelp | code/missiongui.cpp:523; code/mission.cpp:774 | Main UI/update AND render | Main builds immutable HUD commands/text; record replays copied data in original order |
| gosFX Effect execution state, children, particle clouds, parent transforms | mclib/gosfx/effect.cpp:600; mclib/gosfx/effect.cpp:802 | Main presentation Execute; current render Draw | Execute on Main, extract frozen Draw payload including child data; copying DrawInfo alone leaves shared Effect mutable |
| gosFX execute/draw families: artillery, carnage, terrain dust, weapon bolts, VTOL | code/artlry.cpp:992; code/carnage.cpp:781; code/terrobj.cpp:985; code/weaponbolt_update.cpp:559; code/missiongui_style.cpp:68 | Main presentation; render reads effect handles retained until join | Freeze every family; preserve execution timing and effect destruction semantics |
| Oracle effect Execute immediately followed by Draw | code/gamecam.cpp:949 | Current draw, diagnostic path | Move Execute to extraction and preserve oracle-owned geometry through packet retirement |
| theClipper, MLR PerspectiveMode/GOSVertex farClipReciprocal, render submissions | code/gamecam.cpp:748; code/gamecam.cpp:866 | Current presentation/draw producer and renderer | Record owns mutable renderer state; copy queued vertices/indices and material handles; no live MLR cloud references |
| land visible terrain/water/footprints/shadows/vegetation, weather, tactical overlays | code/gamecam.cpp:811; code/gamecam.cpp:818; code/gamecam.cpp:835; code/gamecam.cpp:1083 | Main terrain geometry/weather/presentation; draw caches | Freeze visible sets/transforms/material inputs and producer commands; separate immutable level resources from per-frame mutable caches |
| compass, letterbox/fade/movie state | code/gamecam.cpp:855; code/gamecam.cpp:1108 | Main camera/UI and draw | Copy sprite/quad parameters and draw order; resolve animation/update effects on Main |
| ImGui frame, post-render callbacks, preview cameras | GameOS/gameos/gameosmain.cpp:684; code/simplecamera.cpp:250 | Main UI builder; current draw | Keep UI building/callback decisions on Main; retain copied draw lists per slot; previews require their own explicit view state |
| texture/material resources, pending frees and load/unload | code/tick_commit.cpp:22; GameOS/gameos/engine_present_vk.cpp:2291 | Main deferred resource commits; present uploads | Queue resource operations; graveyard lasts through CPU record retirement AND last GPU use, including load/resize/shutdown |
| present-thread render ownership handoff and sealed event | GameOS/gameos/present_thread.cpp:35; GameOS/gameos/engine_present_vk.cpp:2372 | Main and existing present worker, serialized handoff | Design one explicit record/present ownership chain; no two concurrent Owner::Render registrations |
| debug/capture hooks, overlay, HUD batch replay | GameOS/gameos/gameosmain.cpp:659; GameOS/gameos/gameosmain.cpp:672 | Main controls; current draw hooks | Copy controls at seal; renderer owns replay/readback; Main consumes completed diagnostics without reading an in-flight packet |

Remaining source audit: enumerate transitive terrain/appearance/MLR/HUD consumers before declaring this table exhaustive.
Existing render-thread design must not be implemented by wrapping draw_screen in std::thread: the rows above identify concrete races.
Existing tick_commit clears the singleton list and releases waypoint appearances before frees; its join-only lifetime is insufficient for P3.
Lexical census: 55 explicitly qualified references to the 14 targeted TG_Shape static fields across owned C++ headers/sources,
including declarations/definitions (not 55 runtime reads). Consumers also include `mclib/cone_glow.cpp`, `mclib/preview_draw.cpp`,
`mclib/quad_draw.cpp`, `mclib/msl.h`, `GameOS/gameos/gos_static_prop_batcher_internal.h` and `gos_static_prop_batcher_submit.cpp`.
Unqualified references inside TG_Shape methods still need separate review. gosFX Effect::Draw recursively reads m_children,
m_specification->m_state and m_localToParent (effect.cpp:817), so freezing only the root transform or retaining Effect lifetime cannot isolate Draw.

## S1 archived counts and timing receipt

Requested quiet-measure document is absent from this checkout and mc2-main. Read it without importing code using
`git show claude/perf-robust:docs/perf/2026-10-30-quiet-measure.md` (branch observed at `409815680`).
Raw exact-base receipt: `E:/mc2-archive-2026-10/proofs/quiet-measure2/reserved-core-rerun2/pair1-main/20261031T002037.582057Z/profile.json`.
Executable SHA256 `84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023`; capture sidecar reports clean base `586327c7904e`.
mc2_24 worst DrawScreen bookmark is `low_horizon_0`, pose (-1216,-5737.334,78.554), yaw 0, pitch 10, altitude 150; 1920x1080.
Live frame range [720,1086), 366 recorded DrawScreen events. Values below are inclusive elapsed times; nested scopes MUST NOT be summed.
One capture only, provisional under host noise: reserved external CPU p95 13.45%, whole-machine external p95 34.275%.
No paired CI or new performance gain claimed. User accepts noise; report these numbers rather than suppressing them.

| Zone | Calls over window | Calls/frame | Inclusive ms/frame | Event p95 ms |
|---|---:|---:|---:|---:|
| Frame | 366 | 1 | 4.077871 | 4.889435 |
| DrawScreen | 366 | 1 | 1.961896 | 2.384493 |
| GameCamera::render activeScene | 366 | 1 | 1.794249 | 2.194703 |
| GameCamera::render textureManagerRenderLists | 366 | 1 | 1.076722 | 1.288273 |
| GameCamera::render objects | 366 | 1 | 0.614985 | 0.778071 |
| ExtractRenderSnapshot | 366 | 1 | 0.166971 | 0.199130 |
| ExtractRenderList | 366 | 1 | 0.137890 | 0.159310 |
| GameCamera::render shadows | 366 | 1 | 0.091435 | 0.135380 |
| GameCamera::render waterFastPath | 366 | 1 | 0.061868 | 0.081150 |
| GameCamera::render vegetationFlush | 366 | 1 | 0.044592 | 0.055610 |
| GameCamera::render particlesFlush | 366 | 1 | 0.033518 | 0.049980 |
| addLightDataStructure scan | 5490 | 15 | 0.010971 | 0.001000 |
| gosFX.ParticleCloud.Execute | 2910 | 7.9508 | 0.008565 | 0.002400 |
| gosFX.Effect.Execute | 3276 | 8.9508 | 0.001743 | 0.001140 |
| DrawText | 3660 | 10 | 0.006801 | 0.001780 |
| DrawTris | 9516 | 26 | 0.003632 | 0.000350 |

Calls are zone invocations, not triangle/particle counts. Particle and Effect Execute scopes can overlap; do not add them.
Full-capture world receipt reports 70 movers/46 mechs, not 70 rendered movers in this view.
Live RenderList/HUD lengths and submitted payload counts are measured in the continuation below; exact native visible-leaf counts remain open.
Do not substitute final-frame log counters or frozen screenshot sidecars for that live-window census.
Tracy zone floor confirmed from CLAUDE.md:88: 100 ns minimum, no per-element hot-loop zones.
Some existing scopes measure below that floor; add no new scopes there. Use aggregate counts at phase boundaries for the missing census.
Extraction is already 0.166971 ms inclusive in this view, so adding packet copies to Main can exceed the DrawScreen budget even if work is renamed.
Report extraction/seal costs separately and track whole Frame to expose that risk.

## Receipts and continuation

| Gate | Current checkpoint |
|---|---|
| Assignment preflight | Clean, correct branch/root/base; parser returns overall false only because canonical deploy/smoke command extraction is ambiguous; commands read directly |
| Source investigation helper | Read-only scout produced no result before interruption; no helper source changes |
| Build | PASS baseline Ninja RelWithDebInfo, -j6, exit 0; `E:/mc2-archive-2026-10/proofs/record-thread/s1-build.log` |
| Post-rebase build | PASS: `py -3 scripts/ninja_build.py mc2 -j6`, exit 0; `s1-rebase-build.log`; no engine modifications in this lane |
| Native unit suite | PASS: 3585 cases passed, 2 skipped; doctest SUCCESS, harness native stage 809 s including cold build; `s1-unit.log` under the proof directory above. Nine expected-failure assertions are included in doctest's successful verdict |
| Python harnesses / contracts | PASS: `py -3 tools/build_contract_harnesses.py --run --unit -j6` exit 0; `bash scripts/check-contracts.sh -j6` exit 0; `s1-unit.log` and `s1-contracts.log` under the proof directory above |
| Five ratchets | PASS: file_size, cxx17 (--jobs 1), dup_lines, axis_swizzle, split_map; exit 0 each; `s1-ratchet-{size,cxx,dup,axis,split}.log`; no baselines modified |
| Post-rebase ratchets | PASS: same five checks, cxx17 --jobs 6; exit 0 each; `s1-rebase-ratchet-{size,cxx,dup,axis,split}.log`; no baselines modified |
| Audit ON mc2_01/03/10/16/24 | PASS at `91a88988a`, 15 s each, all five effective configs show audit=1, no violations; `s1-audit-head.log`, full logs in `s1-audit-artifacts/` |
| Farm mc2_10/24 x8 | UNPROVEN: job remained queued behind another lane for the 1200 s bound; cancelled only our queued job and removed its stage/work tree. `s1-farm-{submit,wait,cancel}.log`; no game run or pass claimed |
| SIMID 20 s / save-load | SIMID PASS: overlap OFF/ON, audit=1, 857 identical sim frames; committed baseline matches first 800. Save/load baseline check FAIL: comparison policy differs, decoded FIT identical; `s1-{simid,saveload}.log` |
| Day/night pixel-identical A/B | PASS `3b0a6a393` vs `91a88988a`: RGBA identical day/night, both duplicated poses (4 PNGs); `s1-pixel-parity.json`, `s1-ab-before/`, `s1-ab-after/` |
| Validation 0 / sync-validation 20 s | PASS mc2_10, 20 s, errors=0 warnings=0; Khronos layer and sync messenger explicitly engaged. `s1-sync-validation.log`, full game log in `s1-sync-artifacts/` |
| Robust A/B mean/p95 deltas and CI; laptop absolute ms | Pending; use claude/perf-robust tooling after it lands; user authorizes laptop absolute timings despite older lane-rule restriction |

Next: finish S1 transitive inventory and live worst-view item census, run/record required baseline gates, then commit the completed S1 checkpoint.
Before S2, check whether perf-robust has landed; observed integration tip `678380b2a` does not contain perf-robust tip `409815680`.
The documentation commits were rebased cleanly onto that landed integration tip; rerun complete step proof on the resulting base.
Tool slowness: the baseline native unit build/run took 809 s; keep the existing build tree for continuation and request explicit mc2 targets in engine build loops.
Prior checkpoint disk: no install or baseline copy; this continuation's install is recorded below. No extra install created.
For each S2/S3/S4 commit, append its exact tip, commands, proof paths and failures here. S4 stays inline if any required gate is red.

## Continuation state, 2026-10-31 (live census)

Done: verified clean assignment at `3b0a6a393`; rebuilt mc2 plus mc2_asset_viewer/ui_editor at that HEAD.
Receipts: `s1-head-build.log` and `s1-companion-build.log` in the existing record-thread proof directory.
The earlier post-rebase executable embedded `46b16ce8d`, so deploy correctly refused it; no provenance waiver used.
Baseline exe retained as `s1-baseline.exe` in that proof directory (not another install).
One lean install created: `E:/mc2-installs/claude-record-thread`; initial deploy cooks missing sidecars and copies kits.
Initial five-mission attempt did not run: the deploy folder remained leased beyond its 120-second wait.
Use `run_smoke.py --exe <install>/mc2.exe`, not an absolute directory as `--deploy`.

Archive worst DrawScreen windows (same exact-base profile.json cited above): mc2_24 `low_horizon_0` [720,1086),
mc2_10 `sun_facing_low` [9351,9826), p95 2.136588 ms. The latter differs from the worst whole-Frame view (`wolfman`).
Use the DrawScreen criterion for this assignment's item census; retain the whole-Frame measurements separately.
Added RenderList/HudSnapshot row lengths as per-extraction Tracy plots and armed-only draw payload counts to the
existing benchmark CPU rows. Counts distinguish CPU fallback leaves, dynamic prop leaf submissions, mech bone
payloads, particle vertex records and particle-equivalent sixths (six authored card vertices charge one particle).
These are submissions before GPU culling; mech bones must not be reported as visible leaves. Cached static props
and exact visible mech leaves still require their native consumer counters before the complete leaf census can close.
No per-element zones, new flags, standalone harnesses or test infrastructure added.

Transitive ownership audit: terrain draw consumes active terrain blocks/vertices and land's mutable visible caches;
appearance dispatch retains mutable hierarchy matrices/materials and the CPU fallback reads eye, Camera::HazeFactor,
useFog, turn and TG light pointers (preview_draw.cpp:197+). GPU mech submission reads the live shape/bone palette and
per-actor texture handles (gos_mech_batcher_flush.cpp:13+). Dynamic prop submission reads live shape materials and
appends mutable buckets (gos_static_prop_batcher_submit.cpp:8+). MLR/gosFX recursively combines specification state
and parent transforms and reads children (effect.cpp:802+); retaining the root Effect cannot freeze these values.
HUD row vectors contain nested strings/paths/marker appearances and VTOL pointers, beyond the outer vector lengths.
All remain extraction/ownership obligations for S3, not safe data to move to a thread unchanged.

Green code checkpoint: `91a88988a` (`perf(render): count live frame payloads`). Targeted mc2/viewer/ui_editor build,
three covering contract rows (mission_cpu_profile, static_prop_family, particles_no_cpu_projection), and selected
native vfx_frozen tests PASS: `s1-census-{build,contracts,unit}.log`. All-targets PASS, all three stages, in
`s1-census-alltargets.log`; root 366 s, standalone tools 43 s, doctest build 91 s. No new tree was requested by hand.
Deploy PASS: `s1-census-deploy.log` (kits already verified by initial deploy; unchanged kit copies skipped).

Live proof: `s1-live-census/20261031T062935.159332Z/{profile.json,s1-census.json}` under the existing proof root.
All 15 recorded items have exactly one sample per live frame (407 night, 294 day); not final-frame counters.
The noisy run's worst DrawScreen views are night sun_facing_low [8480,8887), day low_horizon_180 [1589,1883).
Day low_horizon_0 remains a reference view [691,987); its separate reduction is `s1-reference-view.json`.
Reference-view reduction PASS, 296 samples/item: terrain/movers/other 2677/70/0; HUD matches the day column below;
CPU leaves 0..1, dynamic prop leaves 929, mech bones 0, particle records/sixths 24..174 (4..29 particle-equivalents).

| Per-frame item (min..max; submitted work before GPU occlusion) | mc2_10 sun_facing_low | mc2_24 low_horizon_180 |
|---|---|---|
| RenderList terrain / movers / other | 2398 / 48 / 13..16 | 2677 / 70 / 0..8 |
| HUD movers / roster / sensors / coverage | 48 / 20 / 0 / 5 | 70 / 13 / 0 / 11 |
| HUD objectives / blips / VTOLs | 6 / 17..18 / 0 | 6 / 13 / 0 |
| CPU fallback leaves / dynamic prop leaves | 0..6 / 108 | 0..1 / 939 |
| Mech bone records | 119..128 | 0 |
| Particle vertex records / budget sixths | 234..798 / 234..798 | 150..504 / 150..504 |

Six budget sixths equal one particle-equivalent: night 39..133, day 25..84; these are not active effect counts.
Direct DrawScreen children, exactly once per frame: Camera.UpdateRenderers 1.709557/2.307296 ms per frame,
Render.PostProcess 0.090703/0.100562 ms (night/day). Full nested child invocation census is in s1-census.json.
Archived exact-base direct children are separately in `s1-archived-census.json` (475 night / 366 day frames).
Nested inclusive scopes overlap; never sum them. Fresh DrawScreen mean/p95: night 1.809/2.659 ms,
day low_horizon_180 2.440/3.580 ms; low_horizon_0 2.411/3.527 ms.
Profiler exit 2 = NOISY, no capture errors: reserved-core external CPU p95 37.9/40.0%, whole-machine 82.1375/83.01875%.
No speedup or confidence interval claimed. `409815680` is still not in main at `47631b0b4`; robust tooling remains unlanded.

Save/load failure diagnosis: current tools/save_noise_mask.json differs from the stored baseline's frozen comparison
in MechWarriorData, WeaponBoltData, TerrainObjectData and MechData. No baseline or mask changed; no rerun can fix
that metadata mismatch without an explicit policy/baseline repair. The harness restored the pre-test save; five backups retained.
Farm job `8fb477e253a3464f86b95f766c1c670e` never started. Its queue JSON was atomically renamed to .cancelled
before deleting only that job's checked stage/work directories; another lane's running job was left alone.

Next: complete native visible-leaf census (cached props and native mechs) without treating bones/dynamic submits as visible leaves;
the current census explicitly does not close that gap. Repair the save/load policy/reference mismatch in its owning lane,
resubmit 10/24 x8 when the farm queue can serve it, and obtain robust paired CI/laptop timings after tooling lands.
Then close S1 and start S2 explicit CPU view values followed by packet-key vkLights, pixel-identical day/night per commit.
Reuse deployed `91a88988a` with an explicit reference commit after documentation advances HEAD; rebuild on the next engine change.
Tool slowness: full shared-header checkpoint took ~8 min and repeated Tracy decoding took minutes; reuse these receipts.
Disk: 0 GB local installs freed; leave only `E:/mc2-installs/claude-record-thread`; no extra local install.
Own remote farm stage/work removed. Baseline exe and proof-only reductions live in the proof root. No own child left running.

## Continuation checkpoint, 2026-10-31 (native shape leaves)

Assignment unchanged: lane8 / claude/record-thread / base `678380b2a`.
S2 has not started. Its explicit-state change and independent proof cycle will not fit this run's remaining call allocation.

First code checkpoint `f8f764128`: armed native color draw counters include cached props and native mech/vehicle draws,
exclude duplicate prop fringe and mech depth passes, and run before GPU HZB/alpha/hidden-bone rejection.
Source review found that texture groups split one shape into several packets, so this checkpoint measures native
**material-packet instances**, not unique leaves. Its `native_*_leaves` field names must not be used to claim shape counts.
Receipt `s1-native-live/20261031T075736.356841Z/s1-census.json`: all 17 counters have one sample per live frame.
Night wolfman: 334 frames, 2072 prop packet instances / 35 mech packet instances; day low_horizon_0: 185 frames,
5007 / 20. Day has zero freshly submitted mech bones but still draws the retained native feed: bone submits cannot
substitute for leaf counts. DrawScreen mean/p95: night 2.119/3.162 ms, day 3.768/5.722 ms, provisional under load.
Direct children each once/frame: Camera.UpdateRenderers 2.006834/3.582726 ms and Render.PostProcess 0.101735/0.134190 ms.
Reserved external CPU p95 45.6/50.275%, whole-machine external p95 82.1625/86.58125%; profiler exit 2 = NOISY.

All receipts below are under `E:/mc2-archive-2026-10/proofs/record-thread/`.
`f8f764128` proof PASS: `s1-native-{head-build,companions,alltargets,unit,contracts}.log`, five
`s1-native-ratchet-*.log` (no baseline modifications), `s1-native-audit.log` (five missions, audit ON),
`s1-native-sync.log` (20 s, 0 validation errors/warnings), `s1-native-simid.log` (20 s; 1607 identical frames,
first 800 baseline frames unchanged). `s1-native-pixel-parity.json`: four PNGs byte-identical to `91a88988a`;
day/night viewed. Companion executables were rebuilt after deploy correctly rejected stale viewer provenance.
Two invalid harness invocations were corrected before any pass claim: bookmark arguments require JSON paths;
SIMID `check` implies baseline checking and already injects overlap audit, so omit `--baseline` and a duplicate audit `--env`.

Correction `46a9f82a7`: prop consumer counts each type-bucket instance once across its material packets;
mech extraction counts actor/node pairs once using the existing contiguous nodeLocalIndex packet order
(`gos_mech_batcher_geometry.cpp:190,369`). The mech count is retained with the existing native feed across idle frames,
reset with its snapshots, and charged only when native color runs record. Counts are CPU-view submitted shape leaves,
including the mech island's legacy vehicles, before GPU rejection; they are not post-occlusion visible-pixel counts.
Green offline receipts: `s1-leaf-{build,alltargets,unit,contracts,deploy}.log` and five `s1-leaf-ratchet-*.log`.
Final live receipt: `s1-leaf-live/20261031T081627.583420Z/{profile.json,s1-census.json}`.
All 17 counters have exactly one sample per live frame: 426 night / 305 day. Native census gap is closed.

| Per-frame native item | mc2_10 sun_facing_low | mc2_24 low_horizon_180 |
|---|---|---|
| Prop shape leaves, including cached registry replay | 1177 | 5109 |
| Mech/vehicle actor/node leaves | 32..34 | 20 |
| Freshly submitted mech bone records (not leaves) | 119..128 | 0 |
| DrawScreen mean / p95 ms | 1.669808 / 2.430974 | 2.310070 / 3.489325 |
| Camera.UpdateRenderers inclusive ms/frame | 1.578097 | 2.186819 |
| Render.PostProcess inclusive ms/frame | 0.083509 | 0.092145 |

CPU-view submitted leaves precede GPU rejection; do not reinterpret them as post-occlusion survivors.
The zero-bone day window still reports 20 native actor/node leaves, proving the retained-feed census is live.
Timing is provisional/NOISY (profiler exit 2), not a speedup claim: reserved external CPU p95 28.825/24.275%,
whole-machine external p95 66.1/55.28125%. Direct-child scopes each occur once per frame; nested scopes overlap.
Final proof PASS: `s1-leaf-audit.log` (all five 15 s missions, audit=1 in all five game logs, zero VIOLATION lines),
`s1-leaf-simid.log` (20 s, 1654 identical common sim frames, first 800 match baseline in every digest),
`s1-leaf-sync.log` (20 s, errors=0/warnings=0; Khronos sync validation layer and messenger engaged).
`s1-leaf-pixel-parity.json`: four PNGs byte-identical to `f8f764128` (transitively identical to `91a88988a`);
both final day/night PNGs viewed. `s1-leaf-ab/` holds captures; `s1-leaf-{audit,sync}-artifacts/` holds full logs.
No further build or game verification is needed for this unchanged engine checkpoint.

Open external receipts: farm queue read failed SSH authentication (configured default key absent); no repeat job submitted.
The inherited save/load comparison-policy/reference mismatch remains unmodified; owning-lane repair is still required.
Main now contains the quiet-measure document, but its tools/perf tree has no paired_cpu.py; robust paired tooling/CI and
laptop timings remain unavailable. Do not import an unlanded performance branch or claim a speedup from noisy captures.
Next after S1 receipts: CPU TG view values passed by value (separate from the 144-byte GPU RenderCore::ViewUniforms ABI),
including teardown null semantics, preview camera separation, viewport/fog/lightning and copied light values; remove global
read dependencies rather than hide the old statics behind another singleton. Then packet-key vkLights in a separate commit.
Build mc2 + mc2_asset_viewer + ui_editor together before deploy. After docs advance HEAD, use explicit `--ref-commit 46a9f82a7`
for run_smoke/SIMID against the retained deployed engine; rebuild on the next engine change. No provenance waiver.
Tool slowness: the read-only scout timed out at 420 s; use a bounded question/timeout next time.
Disk: no extra install created or removed (0 GB freed); keep only `E:/mc2-installs/claude-record-thread`.
All own build, profiler, game and reduction children have exited. Finish with a docs-only commit after `git diff --check`.

## S2 CPU view continuation, 2026-10-31

S1 cheap receipt audit: configured farm key `~/.ssh/mc2farm_ed25519` is absent; repeats remain open.
Offline recheck of retained saves with the committed baseline's mask gets beyond the policy error but fails
packet 2 text shape (34238 vs 39012 bytes): `s1-policy-recheck.{py,log}` in the proof root. No baseline re-pin.
Scout bounded at 60 seconds produced no output; direct symbol/call-site reads used instead.

CPU view change: remove TG_Shape camera matrix aliases, viewport/fog/lightning statics. Each Camera owns
`mc2::tgl::ViewUniforms` (separate GPU ABI); consumers receive values, including preview camera separation,
camera-valid state, perspective/night/haze and copies of the ambient/infinite light values consumed by CPU draw.
Deleted terrain's unused camera-position load. CPU drawing no longer installs/restores global light lists.
GPU light gathering still uses the existing light-list statics; that belongs to the following S2 commit.
Green static receipt: `s2-view-contracts.log`, ten covering rows including all five ratchets, no baselines raised.
CRLF-aware whitespace check: `git -c core.whitespace=cr-at-eol diff --cached --check` PASS.
Next: shared-header build_all_targets once; selected native fallback/light tests; commit-first lane deploy;
pixel-identical mc2_01/mc2_10 captures against s1-leaf-ab, viewed; SIMID 20 s, overlap audit ON, sync validation 0.
Only then start the separate packet-key vkLights change. Keep one install `E:/mc2-installs/claude-record-thread`.

## S2 CPU view proof closed, 2026-10-31 continuation

Game/preview proof for engine commits `5b24b0e3f`, `c4b9e24af`, `c823bf486` is green. Retained receipts:
`s2-view-{build-fixed,final-build,alltargets,unit,final-contracts}.log`; selected native light/forceZ/cone
tests: 34 cases / 4570 assertions PASS; ten covering rows including all five ratchets PASS, no baseline changes.
The corrected root build and companion builds supersede the initial Camera compile failure.
`s2-view-deploy-fixed.log` deploys clean `c823bf486` into the sole lane install.
Day/night `s2-view-ab/` captures were viewed in the preceding run and matched S1; the retained parity script
was rerun here: all four PNGs and RGBA payloads are identical to `s1-leaf-ab/` (`s2-view-pixel-parity.json`).
Audit is effectively ON in both capture logs and the sync run, with no VIOLATION lines.
`s2-view-sync.log` and its full artifacts: 20 s mc2_10 PASS, Khronos sync validation engaged, errors=0/warnings=0.
Requested fresh pinned fixture: `py -3 tools/sim_determinism.py check mc2_10 --duration 20 --install
E:/mc2-installs/claude-record-thread --ref-commit c823bf486 --out
E:/mc2-archive-2026-10/proofs/record-thread/s2-view-simid-final` PASS: 1221 identical common sim frames,
first 800 match the historical baseline in every digest. Audit is injected by the harness.
The earlier mc2_01 pair has no pinned baseline and is excluded. No baseline was re-pinned.
All receipts use the existing `E:/mc2-archive-2026-10/proofs/record-thread/` root.
Next: packet-key light rows, count, generation/revision and three caches; carry the retained version through
both native feeds and shared upload checks, preserving the baked prefix and same-frame high-water rule.

## S2 packet-key light checkpoint (implementation; final proof below)

Light rows/count, row generations/revision and dedup/template/actor-slot maps now belong to an owned CPU Packet.
Each beginFrame reset assigns a monotonically increasing packet key, independently of simulationFrame.
Native mech/prop feeds retain a const packet version and its exact metadata; writes detach if a feed retains
the prior version. Inline draws normally reuse the allocation. Baked rows survive resets, while re-bakes
cannot mutate an older feed; dynamic count still advances above a newly baked same-frame prefix.
SharedLightFeed reads the retained feed's generations/revision and checks its packet key before same-frame reuse.
No feature gate, GPU ABI change, new harness or three-slot ring introduced; retirement slots remain S3.
Green pre-build receipt: `s2-lights-contracts.log`, ten covering rows/all five ratchets PASS, whitespace PASS.
Next: commit-first build_all_targets once (-j6), selected native lighting tests, explicit lane deploy, then
day/night pixel parity, mc2_10 SIMID 20 s/audit ON and sync validation 0. Stop at that proven S2 checkpoint.

Shared-header checkpoint correction: `s2-lights-alltargets.log` exposed two retained `TG_Shape::SetViewport`
calls in `editor/EditorCamera.h` from the prior CPU-view step. The previous all-target receipt was not a
successful complete root build. Both calls now update the editor camera's own `tglView`.
Standalone tools and doctest corpus built PASS (19/5/5/28 s); root failed after 748 s at the editor call sites.
The dependent proof shell correctly stopped before tests/deploy/games. Next: finish only the failed root
stage, then resume the proof stages. Reuse the existing all-target Ninja payload via explicit
`--build-dir A:/Games/mc2-lane8/build-alltargets --build64-root A:/Games/mc2-lane8/build-alltargets`;
this keeps deploy's provenance, companion and object-freshness checks active on the tree just built.
Editor follow-up's covering row plus all five ratchets PASS in `s2-editor-contracts.log`.

## S2 final proven checkpoint, 2026-10-31

Tested engine `6fcde027223c0e4795819b81538b12ed255a0d4f`, including packet-light commit `ed9d6207f`
and the editor CPU viewport correction. Source/branch stayed clean during proof. Local commits only.
All receipt names below are relative to `E:/mc2-archive-2026-10/proofs/record-thread/`.

| Required proof | Final receipt |
|---|---|
| Shared-header all targets | Standalone tools and doctest corpus PASS in `s2-lights-alltargets.log`; the initially failed editor root is corrected by `py -3 tools/build_all_targets.py --root-only -j6`, PASS in `s2-lights-root-fixed.log` (100 s). No repeat of unchanged standalone stages |
| Native lighting | `py -3 tools/build_contract_harnesses.py --run --unit --unit-only --compile-cache --only object_light_row_harness -j6`: 30 cases / 844 assertions PASS, `s2-lights-unit.log` |
| Covering contracts / five ratchets | Ten rows PASS in `s2-lights-contracts.log`; editor follow-up's six rows PASS in `s2-editor-contracts.log`; no baselines raised |
| Verified lane deploy | `s2-lights-deploy.log`: explicit all-target Ninja build dir/root, source manifest `6fcde027223c`, payload verified; one install only |
| Day/night parity | `s2-lights-pixel-parity.json`: all four PNGs byte-identical to `s1-leaf-ab/`; both new day/night images viewed. Captures in `s2-lights-ab/`, both audit=1, no VIOLATION lines |
| Pinned 20 s SIMID | `s2-lights-simid-detail.log`: 1630 identical common frames, first 800 baseline frames match every digest; both game logs have audit=1 and no violations |
| Sync validation 0 | `s2-lights-sync.log`: mc2_10 20 s PASS, errors=0/warnings=0, content errors=0; full `s2-lights-sync-artifacts/` log confirms Khronos sync layer/messenger and audit=1, no violations; lease wait 14 s |

The first light pair is retained as a FAIL in `s2-lights-simid.log`: HZB stderr split its `tested=` record
around run b's SIMID frame 312 (`s2-lights-simid/b/mc2_10.log:2962-2963`), leaving `animrng=27=`.
All preceding digest/RNG fields matched. No parser/filter/mask/baseline was changed to accept this line.
The harness-prescribed diagnostic rerun used the same `check mc2_10 --duration 20 --install
E:/mc2-installs/claude-record-thread --ref-commit 6fcde0272` plus
`--env MC2_SIM_IDENTITY_TRACE_DETAIL=1 --out E:/mc2-archive-2026-10/proofs/record-thread/s2-lights-simid-detail`.
It passed the full comparison and historical baseline. Avoidable cost: a ~6-minute repeat due to interleaved
diagnostic records; atomic log writes belong in the logging owner follow-up, not a relaxed comparison.

Next S3: three CPU slots selected by render sequence; retain each light Feed together with its native
instance light indices. Convert RenderList/HUD and the two-slot render snapshot arena to retained value
packets, then retirement/graveyard through CPU-record completion and last GPU use. Split FX Execute(Main)
from frozen Draw data and add moved-entry owner assertions. S4 worker integration remains after S3.
S1 external farm-key/save-policy/paired-performance gaps remain as recorded; no baseline re-pin or new farm job.
The deployed binary stays at `6fcde0272` after this docs-only closure: use that explicit ref for retained
proof runs; rebuild on the next engine change. No additional verification is needed for unchanged code.
Disk: 0 GB freed; leave only `E:/mc2-installs/claude-record-thread`. All own build/game/proof children exited.

## S3 first proven checkpoint, 2026-10-31

Expanded the existing value-only FramePackets and RenderSnapshot arenas to three CPU slots, indexed by the
full 64-bit extraction/render sequence before narrowing. Updated native tests cover preservation through two
subsequent frames, recycling on the third, skipped publish, repeated same-frame allocations and large sequences.
The synchronous-copy contract remains explicit: three slots alone do not grant asynchronous reader retention.
No raw GameObject/TG pointer was added to a packet and no gos_/vk recording entry moved threads in this step.
Native Vulkan island logging now builds the complete tagged line and emits one fwrite, including HZB's
512-byte message path, to prevent CRT formatting fragments from splitting concurrent SIMID records.

Tested engine `3a856fc8206ace2faeb204259cfa59bbad7c9bc9`. Proof names use `s3-arena-*` in
`E:/mc2-archive-2026-10/proofs/record-thread/`. Offline work completed before runtime proof;
the harnesses queued behind mc2-main merge checks and another lane's profiler under bounded leases.
Offline receipts: `s3-arena-contracts.log` PASS all five ratchets (32 s); `s3-arena-unit.log` PASS
22 selected native cases / 241 assertions. No ratchet baseline or SIMID parsing policy changed.
The read-only scout did not return useful output; stopped only its own PID 54828 and descendants.
Avoidable cost: stalled scout; always pass `--timeout 60` on the next bounded question.
PowerShell profile loading was denied by the host execution policy; skipped profile loading, all work commands ran without it.

| Required proof | Final receipt |
|---|---|
| Shared-header all targets | `s3-arena-alltargets.log`: every stage PASS; root 136 s, standalone tools 7/4/7 s, doctest corpus 4 s. Reused existing trees |
| Verified deploy | `s3-arena-deploy.log`: manifest `3a856fc8206a`, 3855 payload rows, deploy COMPLETE verified; sole lane install |
| Day/night pixels, viewed | `s3-arena-pixel-parity.json`: four PNGs/RGBA payloads identical to `s2-lights-ab/`; viewed new day/night PNGs. `s3-arena-ab/` capture logs show audit=1 and no violations |
| Detailed 20 s mc2_10 SIMID | `s3-arena-simid-detail.log`: 903 identical common frames; first 800 match pinned baseline in every digest. Both game logs confirm detail=1/audit=1, no violations or rejected/interleaved SIMID records |
| Sync validation zero | `s3-arena-sync.log`: mc2_10 20 s PASS, errors=0/warnings=0/content errors=0. Full `s3-arena-sync-artifacts/` log confirms Khronos sync layer/messenger, audit=1, no violations |

S3 remains incomplete. Next: freeze RenderList/HUD draw payloads (existing RenderView/HUD still contain live pointers),
retained light feeds with matching instance indices, retirement-bound graveyard through CPU completion and
GPU last use, and all FX families' Execute(Main)/frozen Draw split. Do not rotate legacy pointer lists and call
them immutable packets; no S4 worker integration until these ownership obligations are complete.
Start with `code/render_view.h`, `code/render_view_store.cpp`, `code/objmgr.cpp:1032` and the transitive producer
inventory above. Preserve the three ordered object streams and HUD draw order; freezing only identity or retaining
mutable Effect/Appearance objects does not meet the packet contract. Add assertOwner on each entry actually moved.
The full remaining freeze spans many engine families and will not fit this run's remaining 150-call allowance;
stop at this independently proven preparatory checkpoint rather than commit a partial pointer-based ring.
All own build/game/proof children have exited; the stalled scout subtree was terminated. No push.
Disk: 0 GB freed, no extra install created. Leave only `E:/mc2-installs/claude-record-thread` at engine `3a856fc82`.
After this docs-only closure, pass that explicit reference for retained binary proof; do not rebuild unchanged code.

## S3 CPU retirement implementation, 2026-10-31 (final proof below)

Implementing the named retirement graveyard as one bounded S3 checkpoint. Three sequence-keyed CPU leases
hold typed effect/light releases and object destruction until all older readers retire, including out-of-order
completion. Main admits/retires packets and drains destructors; existing GPU ticket destruction stays in place.
The inline path brackets extraction and draw with a lease. This does not freeze mutable RenderList/HUD values,
retain waypoint appearance vectors across commit, or authorize S4 dispatch. Generic Release queue commands
(paint, FX creation, appearance RNG) keep their original commit timing; only typed destruction gets held.
New native tests cover commit while retained, out-of-order retirement, three-slot reuse and reentrant destruction.
Next: final five ratchets plus buffer_lifetime, all-target checkpoint once, selected tick tests, commit-first deploy,
day/night pixel parity viewed, mc2_10 detailed SIMID 20 s and audit/sync validation zero. Record receipts below.
Scout failed to return useful output twice (second bounded at 60 s); stopped only the first scout's PID 9552 subtree.
Used targeted source ranges after its timeout. Budget remains below 150 calls; sole install unchanged.

## S3 typed-release retirement checkpoint proven, 2026-10-31

Tested engine `c8022b57f5acbc10f3961661b7844364356591b8`; local commit only. Main brackets the current
inline extraction/draw with sequence-keyed admission/retirement. The typed effect/light/object helpers hold
destruction for every older outstanding CPU lease; generic presentation commands retain their commit timing.
This is a CPU lifetime checkpoint, not completion of the full S3 graveyard or immutable packet migration.
Existing Vulkan image retirement remains ticket-bound (`vk_texture_residency.cpp:277,401`); no GPU queue changed.
All receipts below are under `E:/mc2-archive-2026-10/proofs/record-thread/`.

| Required proof | Receipt |
|---|---|
| Shared-header all targets | `s3-retirement-alltargets.log`: all stages PASS, root 63 s, standalone tools 13/5/5 s, doctest build 11 s; reused existing trees, -j6 |
| Native retirement tests | `s3-retirement-unit.log`: six cases / 54 assertions PASS; ordered tick commit, retained frees, out-of-order completion, all slots reused, reentrant destruction, light world removal/free |
| Five ratchets / covering row | `s3-retirement-head-contracts.log`: five ratchets plus buffer_lifetime PASS at tested HEAD, no baselines raised; earlier intermediate receipts retained |
| Verified explicit lane deploy | `s3-retirement-deploy.log`: COMPLETE, verified 3855-row manifest at `c8022b57f5ac`, explicit all-target build dir/root; existing cook-byproduct hygiene advisory only |
| Day/night pixel parity, viewed | `s3-retirement-pixel-parity.json`: four PNGs byte-identical to `s3-arena-ab/`; both new day/night images viewed. Captures `s3-retirement-ab/`, audit effectively 1, no violations |
| Detailed mc2_10 SIMID, 20 s | `s3-retirement-simid-detail.log`: 1817 identical common frames; first 800 match the pinned baseline in every digest. Both logs detail=1/audit=1, no violations |
| Sync validation zero, 20 s | `s3-retirement-sync.log` and `s3-retirement-sync-artifacts/`: PASS; Khronos sync validation engaged, errors=0/warnings=0/content errors=0; audit=1, no violations |

Exact next work: freeze the three ordered RenderList streams into owned draw values, including native geometry,
material/light indices and their retained light feeds; eliminate virtual dispatch through `RenderView::obj`.
Freeze HUD marker/VTOL/FX draw payloads and copied UI commands, then attach their storage and retained resources
to the same packet sequence. Legacy `commitTickQueues` still clears the singleton lists/waypoint holders; it
must not run concurrently with readers of those lists. The three value arenas still recycle synchronously:
wire actual record completion back to Main retirement before permitting async readers or slot reuse.
Split every gosFX family's Execute(Main) from frozen recursive Draw data, and assertOwner at each gos_/vk entry
actually moved. None moved in this checkpoint. Keep S4 inline until all these ownership obligations pass.
The full remaining producer migration exceeds a bounded follow-up within this run; stop at this proven checkpoint.
No farm fallback needed, no startup timeout or lease wait observed in this proof. Game processes total about 2 min.
All own build/game/scout children exited. PowerShell profile loading was denied and skipped; commands ran without it.
Avoidable cost: stalled scout calls; use one bounded query, then its permitted targeted-range fallback after timeout.
Disk: 0 GB freed, no extra install created; leave only `E:/mc2-installs/claude-record-thread` at `c8022b57f`.
After this docs-only closure use explicit `--ref-commit c8022b57f` for retained binary proof; do not rebuild unchanged code.

## S3 arena lease integration, 2026-10-31 (proof pending)

This continuation adds commits only during W92; no branch recreation, rebase or history rewrite.
FrameSlotArenas now holds explicit CPU admission leases and refuses occupied-slot reuse or stale retirement.
The game loop uses the typed-release packet sequence for both the RenderSnapshot arena and FramePackets
publisher. Display-frame metadata remains separate. Main releases both value arenas after inline DrawScreen,
before draining typed destruction; S4 must replace that point with actual worker completion.
Inline editor/viewer/tool callers keep synchronous frame-keyed behavior. GPU ticket retirement is unchanged.
Native tests cover occupied-slot rejection, stale/duplicate retirement, out-of-order completion, large sequences,
repeated display frames, and skipped publication when a leased slot recycles. Owner assertions guard snapshot
extraction/retirement. The bounded scout timed out after 25 s; used its permitted direct-range fallback.
Next: five ratchets + buffer_lifetime, one shared-header all-target build, selected FramePackets native tests,
commit-first explicit lane deploy, day/night byte parity viewed, mc2_10 detailed SIMID 20 s, audit/sync validation 0.
Remaining S3: immutable RenderList/HUD native draw values with retained light feeds; recursive FX Execute/Main
and frozen Draw split. Legacy object/appearance/effect pointers still block S4. One existing lane install only.

## S3 CPU arena lease checkpoint proven, 2026-10-31

Tested engine `3b855d8f1d2b773be2526002957fea2e010ee39f`; local additive commit during W92.
Both existing value arenas now retain the same CPU sequence as the typed-release graveyard. Occupied-slot
reuse aborts in release builds; stale/duplicate retirement is rejected. FramePackets preserves display-frame
metadata separately, including repeated display frames. Snapshot admission/retirement assert Main ownership.
No GPU ticket changed, no legacy pointer lists rotated, and no gos_/vk recording entry moved threads.
All receipt paths below are relative to `E:/mc2-archive-2026-10/proofs/record-thread/`.

| Required proof | Receipt |
|---|---|
| Shared-header all targets | `py -3 tools/build_all_targets.py -j6`: every stage PASS; root 203 s, standalone tools 13/8/11 s, doctest build 15 s; `s3-leases-alltargets.log` |
| Selected native tests | `py -3 tools/build_contract_harnesses.py --run --unit --unit-only --compile-cache --only 'retained CPU arenas*' --only 'CPU sequence retains*' --only '*published*' --only '*slot arenas*' -j6`: seven cases / 85 assertions PASS; `s3-leases-unit.log` |
| Five ratchets / covering row | `bash scripts/check-contracts.sh -j6 --rows '^(file_size_ratchet|cxx17_ratchet|dup_lines_ratchet|axis_swizzle_ratchet|split_map|buffer_lifetime)$'`: six PASS, 74.92 s; `s3-leases-contracts.log`; whitespace PASS, no baselines raised |
| Explicit lane deploy | `s3-leases-deploy.log`: COMPLETE verified, 3855-row manifest at `3b855d8f1d2b`; same explicit all-target build dir/root as previous checkpoint; existing cook-byproduct hygiene advisory only |
| Day/night pixels, viewed | `s3-leases-pixel-parity.json`: all four PNG and RGBA payloads identical to `s3-retirement-ab/`; both new scenes viewed. Captures `s3-leases-ab/{day,night}/`, saved_mechday/saved_mechnight bookmarks, audit=1 and no violations |
| mc2_10 detailed SIMID, 20 s | `s3-leases-simid-detail.log`: 1730 identical common frames; all first 800 pinned baseline frames match every digest. Same check command as prior checkpoint with `--ref-commit 3b855d8f1`, `--env MC2_SIM_IDENTITY_TRACE_DETAIL=1`, output `s3-leases-simid-detail/`; both game logs audit=1, no violations |
| Sync validation zero, 20 s | `s3-leases-sync.log` / `s3-leases-sync-artifacts/`: PASS, errors=0/warnings=0/content errors=0. Full game log confirms Khronos sync layer/messenger engaged, audit=1 and no violations |

Next S3: freeze the three ordered RenderList streams into owned native geometry/material/light-index values
and retained light feeds, eliminating `RenderView::obj` dispatch (`code/objmgr.cpp:1032`, `code/render_view.h`).
Freeze HUD copied commands plus marker/VTOL payloads; attach storage/resources to the CPU sequence rather
than rotating the legacy pointer-bearing vectors. Split every FX family's Execute(Main) from recursive frozen
Draw data, with assertOwner on each gos_/vk entry actually moved. CPU completion must cover the full record
worker before Main releases leases; resource destruction must also respect the existing GPU last-use tickets.
This producer migration spans multiple engine families and exceeds the remaining bounded proof cycle;
stop at this independent lifetime checkpoint. S4 remains inline until those obligations are complete.
Game processes ran about 3 minutes; lease waits about 1 minute (day capture and sync run reported contention).
All own build/game/scout/proof children exited. Initial PowerShell profile loading was denied and skipped;
subsequent commands used no profile. Scout timed out at 25 s and exited; targeted-range fallback used.
Avoidable token cost: broad historical plan reads; next continuation should read this tail and named source ranges.
Disk: 0 GB freed, no extra install created; leave only `E:/mc2-installs/claude-record-thread` at `3b855d8f1`.
After this docs-only closure use explicit `--ref-commit 3b855d8f1` for retained binary proof; do not rebuild unchanged code.

## Continuation: first frozen draw producer proven, 2026-10-31

Additive checkpoint on claude/record-thread after 65eaf8828; S4 stays inline.
WeaponBolt beam/laser geometry, UVs, type colors/alpha, beam mode and texture handles
now extract into values in RenderView instead of reusable WeaponBolt rv* members.
After extraction Main copies visible bolt payloads into the current RenderSnapshot
arena; the existing CPU sequence retains those bytes through inline recording.
Inline tools can consume staging values synchronously. FX pointers, live object
dispatch, camera projection, RenderList/HUD containers and light feeds remain.
Native test retains a packet across staging destruction and another extraction,
checks copied geometry/material/texture values, and exercises occupied-slot reuse.
Offline green: s3-bolt-contracts.log (five ratchets + buffer_lifetime, 6 PASS),
s3-bolt-alltargets.log plus s3-bolt-root-resume.log (all targets built; root resumed
after fixing legacy mutable projection inputs), s3-bolt-unit.log (3 cases / 41
assertions PASS). Logs under E:/mc2-archive-2026-10/proofs/record-thread/.
Tested engine: eaede08edc60 (local additive commit). Provenance refreshed for both
mc2 and its shipped companion viewer; deploy refused the stale viewer until its
matching build completed. No provenance checks or warnings were waived.

| Required proof | Receipt under the same proof directory |
|---|---|
| Five ratchets / covering row at engine HEAD | s3-bolt-head-contracts.log: six PASS, 28 s, no baselines raised |
| Explicit existing lane deploy | s3-bolt-deploy.log: COMPLETE, verified 3855-row manifest at eaede08edc60 |
| Day/night parity, viewed | s3-bolt-pixel-parity.json: all four PNGs byte-identical to s3-leases-ab; new day/night images viewed; s3-bolt-ab/{day,night} |
| mc2_10 detailed SIMID, 20 s | s3-bolt-simid-detail.log: 1870 identical common frames, first 800 match pinned baseline in every digest; both logs detail=1/audit=1 |
| Sync validation, 20 s | s3-bolt-sync.log and s3-bolt-sync-artifacts/: PASS; Khronos sync layer/messenger engaged, errors=0, warnings=0, content errors=0 |

All five game logs confirm audit=1; none contains an overlap violation. No shadow-bake
crash occurred. SIMID's first invocation stopped before launch because an explicit
audit argument duplicated the harness's own keyword; the successful run uses its
default audit=1. Initial PowerShell profile loading was denied and skipped.

Exact next step: continue native TerrainObject/Building/mover draw values (geometry,
material/light indices and retained light feeds), replacing RenderView::obj dispatch.
Then continue remaining native RenderList/HUD/recursive FX freezing, Execute/Draw
split and moved-entry owner assertions. No worker dispatch in this checkpoint.
HUD marker/VTOL and copied UI draw commands still require packet ownership; gosFX
must freeze recursive Draw inputs before Execute can overlap recording. Bolt world
positions and material values are frozen, but projection, FX objects and texture
resource ownership still use existing synchronous paths. Keep S4 inline until every
producer and CPU/GPU resource lifetime obligation is complete. That remaining
multi-family migration exceeds this bounded proof cycle; resume from this tail.
All own children exited. Game runs about 2.5 min, lease waits 0 observed. No farm
fallback or startup timeout. Disk freed 0 GB; sole lane install left at
E:/mc2-installs/claude-record-thread, engine eaede08edc60. After this docs-only
closure use --ref-commit eaede08ed for retained binary proof; do not rebuild unchanged
engine code. Avoidable cost: broad reconnaissance; start the next producer at its
named render/extract ranges and inspect projection signatures before editing.

## S3 retained RenderList/HUD containers, 2026-10-31 (proof pending)

Additive continuation from 0178b2750 during W93. FrameDraws owns copies of the
three ordered RenderList vectors, render clock, HUD strings/paths/arrays and
shared waypoint marker holders. RenderSnapshot retains the immutable copy for
its CPU sequence. DrawFrameScope selects it only during inline DrawScreen;
const accessors prevent draws from mutating staging through the read API.
Extraction and commit use separate Main-asserted staging accessors. Main drops
the copied containers before retiring the arenas and draining typed frees.
Native unit coverage destroys/reuses staging while retaining the frame, checks
ordering and deep HUD values, marker ownership, nested scope restoration and
the synchronous inline-tool fallback. Six covering rows pass (46.87 s), and
git diff --check is clean. Remaining proof: all-target shared-header build,
selected FramePackets cases, commit-first lane deploy, day/night pixel parity
viewed, detailed mc2_10 SIMID 20 s and audit/sync validation zero.
This freezes containers only: RenderView::obj, appearance/FX and VTOL pointers
are still borrowed, and the marker shapes remain mutable. No worker dispatch.
Next native producers remain TerrainObject::extractRenderView/renderView
(code/terrobj.cpp:1020-1105), Building equivalents (code/bldng.cpp:1079-1157),
BldgAppearance::render (mclib/bdactor_render.cpp:42-434) and native submissions;
resolve geometry/material/light indices plus retained feeds before eliminating
object dispatch. Then freeze marker/VTOL/copied UI and recursive FX inputs,
split Execute(Main)/Draw(record), add assertions at actually moved entries,
and connect retirement to actual worker completion. Keep S4 inline meanwhile.

Offline checkpoint: engine commit f7942b628. All-target root (all/ui_editor/order
seam) and three companion builds passed; root took 428 s. The initial doctest
compile failed only on the new test's unqualified clock() versus the CRT clock;
qualified calls corrected it. Selected frame/bolt/CPU-lifetime tests now pass,
4 cases / 69 assertions, 17 s (s3-containers-unit.log). No repeated all-target
build is needed for that test-only correction. Remaining: refresh shipped binary
provenance after the correction commit, final selected contracts, explicit lane
deploy, day/night parity, detailed SIMID and audit/sync proof. Build slowness:
the shared RenderSnapshot/RenderView headers fan out to 254 engine/editor jobs.

## S3 retained container proof closed, 2026-10-31

Proven shipped engine/viewer: a9a8adc00ba0, additive commits after 0178b2750.
S4 remains inline. This checkpoint owns container bytes and waypoint holders;
it does not freeze native appearance/FX/VTOL pointees or eliminate obj dispatch.
All receipts below are under E:/mc2-archive-2026-10/proofs/record-thread/.

| Proof | Receipt |
|---|---|
| Shared-header all targets | s3-containers-alltargets.log: root all/ui_editor/order seam and three standalone tools built; 428/6/5/5 s. Initial doctest compile failed on the test-only CRT clock ambiguity, corrected by a9a8adc00; s3-containers-unit.log rebuilds that corpus successfully. No second engine build required |
| Selected native lifetime cases | s3-containers-unit.log: frame/HUD staging reuse, nested scopes, marker lifetime, bolt and CPU leases; 4 cases / 69 assertions PASS, 17 s |
| Final five ratchets + buffer_lifetime | s3-containers-head-contracts.log: six PASS, 29.38 s, no baseline raised; whitespace clean |
| Shipped provenance | s3-containers-provenance.log: only identity TU and mc2/viewer relinks, clean a9a8adc00ba0 |
| Existing lane deploy | s3-containers-deploy.log: COMPLETE, verified 3855-row manifest; unchanged cook-byproduct hygiene advisory |
| Day/night pixels, viewed | s3-containers-pixel-parity.json: all four PNGs byte-identical to s3-bolt-ab; both new scenes viewed; s3-containers-ab/{day,night}, each capture exited in ~21 s |
| Detailed SIMID, mc2_10 20 s | s3-containers-simid-detail.log: 1106 identical common frames, first 800 match pinned baseline in every digest; both game logs confirm TRACE_DETAIL=1 |
| Sync validation, mc2_10 20 s | s3-containers-sync.log and s3-containers-sync-artifacts/: PASS, errors=0/warnings=0/content errors=0; full log confirms Khronos layer and messenger engaged with preset=sync |

All five game logs confirm effective overlap audit=1; none contains VIOLATION.
The first receipt scan incorrectly expected a literal audit=1 in capture logs;
those logs report CONFIG name=MC2_SIM_OVERLAP_AUDIT value=1 instead. No rerun,
proof gate waiver or game change was needed. No startup timeout/farm fallback.
Game processes ~3 min, observed lease wait 0 min. All own children exited.
Initial PowerShell profile loading was policy-denied and skipped; no required
proof was skipped. Disk freed 0 GB; no extra install created, sole lane install
left at E:/mc2-installs/claude-record-thread with engine a9a8adc00ba0.
After this docs-only commit use --ref-commit a9a8adc00 for retained binary proof.

Exact next producer ranges: code/terrobj.cpp:1020-1105 and code/bldng.cpp:1079-1157
feed mclib/bdactor_render.cpp:42-434; that draw still mutates TG shape highlight,
queries static eligibility and submits live shapes. Freeze native geometry,
material/light indices with retained light feeds, then replace RenderView::obj
dispatch. Marker/VTOL/copied UI and recursive FX Draw values remain next, followed
by Execute(Main)/Draw(record), moved-entry owner assertions and actual record
completion before arena/resource retirement. The scoped container copies alone
do not permit any of those live pointees to overlap an async record worker.
That multi-family producer migration needs its own bounded implementation/proof
cycle; resume here without rebuilding this unchanged engine.
Largest avoidable cost: broad source reading/scouting before the concrete edit;
bound the next scout with --timeout 25 and keep reads at the producer ranges above.

## S3 rigid FX native shape packets, 2026-10-31 (proof pending)

Additive continuation from 77dda168b, assignment base 0178b2750. Rigid native
Shape/ShapeCloud/DebrisCloud leaves now have FreezeNativeShape(Main capture)
and DrawFrozenNativeShape(value replay). Cold-cache positions, UVs and fan
indices plus ordered submaterial, blend, matrix, scale and color values live
in the retained snapshot arena. DrawFrameScope binds that arena and restores
it across nested scopes; standalone tools preserve synchronous submission.
The existing native mesh test now destroys the source shape, overwrites its
draw inputs and fills another retained slot before cold/hot packet replay.

This is a leaf boundary only: recursive Effect::Draw still reads live child
and spec data, and live callers capture/replay immediately inside DrawScreen.
Texture pool indices still need resource retention/resolution before moving
replay off Main. Building/TerrainObject appearances, VTOL and copied UI remain
open at the exact ranges above; no obj dispatch was removed. S4 stays inline.
Next: finish covering rows, shared-header all-target build and selected
frame/native-FX tests; commit-first lane deploy and the existing day/night,
detailed SIMID and audit/sync proofs. Record receipts and commit here, then
move the native appearance and recursive FX producers to Main extraction.

Offline receipts: s3-native-shape-contracts.log six rows PASS (32.20 s);
s3-native-shape-unit.log selected frame/bolt/CPU lease and fresh-process FX
driver PASS, 4 top-level cases / 74 assertions (49 s), rigid-mesh child
1 case / 37 assertions, existing particle arms all green. The first harness
invocation rejected a suite name used as a case filter before building;
corrected to the existing retained CPU case, no test skipped. CRLF-aware
whitespace check (`git -c core.whitespace=cr-at-eol diff --check`) is clean.

## S3 rigid native FX leaf proof closed, 2026-10-31

Engine commits e47992cf7 and 4bfa5434f; shipped game/viewer 4bfa5434f4c0,
clean provenance. This bounded checkpoint freezes rigid Shape/ShapeCloud/
DebrisCloud leaf submissions only, still captured/replayed inline. S4 remains
inline. The larger native appearance/VTOL/recursive FX migration is unfinished.
All receipts below are under E:/mc2-archive-2026-10/proofs/record-thread/.

| Proof | Receipt |
|---|---|
| Shared-header all targets | s3-native-shape-alltargets.log: game/editor/viewer/root tools, three standalone projects and doctest corpus PASS; root 800 s, companion stages 4-11 s |
| Final implementation-only correction | s3-native-shape-final-build.log: mc2 + mc2_asset_viewer PASS, 7 s, unchanged shared headers; reused build-alltargets Ninja tree and deployed its output directly |
| Native lifetime/FX tests | s3-native-shape-final-unit.log: 4 top-level cases / 74 assertions PASS, 16 s; fresh-process rigid mesh 37 assertions and particle/RNG arms PASS |
| Committed-tree covering rows | s3-native-shape-final-contracts.log: six PASS, 36.60 s, no baseline raised |
| Existing lane deploy | s3-native-shape-deploy.log: COMPLETE, 3855-row manifest at 4bfa5434f4c0; 203 s; pre-existing cook-byproduct hygiene advisory |
| Day/night pixels, viewed | s3-native-shape-pixel-parity.json: all four PNGs byte-identical to s3-containers-ab; new day/night images viewed; s3-native-shape-ab/{day,night} |
| Detailed mc2_10 SIMID, 20 s | s3-native-shape-simid-detail.log: 1260 identical common frames; first 800 match pinned baseline in every digest; both game logs confirm TRACE_DETAIL=1 |
| Sync validation, mc2_10 20 s | s3-native-shape-sync.log + s3-native-shape-sync-artifacts/: PASS, errors=0/warnings=0/content errors=0; full log confirms Khronos layer and messenger engaged with preset=sync |

All five game logs confirm overlap audit=1 and contain no VIOLATION. Observed
lease wait 0 min; game harness wall time about 3.5 min. No startup timeout or
farm fallback. All own build/game/proof children exited. No extra install was
created: disk freed 0 GB, leave E:/mc2-installs/claude-record-thread only.
Use --ref-commit 4bfa5434f after this docs-only commit; do not rebuild unchanged
engine code just to stamp documentation provenance.

Corrections: the first static scan preceded the engine commit, so the ratchet
still scanned the old committed tree. Its subsequent head scan caught .first
on our non-pair aggregate and a pointer/count helper. 4bfa5434f uses aggregate
initialization/structured binding and std::span; headers/layout unchanged.
Initial SIMID invocation redundantly supplied the harness-owned audit setting
and failed before launch; corrected to use its built-in mc2_10 audit. Initial
PowerShell profile loading was policy-denied and skipped; no required proof
was skipped. One receipt scan had a PowerShell array-construction error; the
corrected scan checked all five full logs without rerunning games.

Exact next: native appearances still start at code/terrobj.cpp:1020-1105,
code/bldng.cpp:1079-1157 and mclib/bdactor_render.cpp:42-434. Resolve/copy native
geometry, materials and light rows with retained light feeds before removing
RenderView::obj dispatch. For gosFX, Effect::Draw at mclib/gosfx/effect.cpp:802
still traverses live children/spec/local transforms; attach rigid leaf packets
to the retained recursive frame root and freeze the remaining particle/tube
leaves after Main Execute. Then migrate marker/VTOL/copied UI payloads, add
assertions at actually moved entries, and bind arena/resource retirement to
actual record completion. Do not enable S4 from this leaf-only checkpoint.
That multi-family change needs a separate bounded implementation/proof cycle.

Tool slowness: shared-header root build 800 s and deploy 203 s. Largest
avoidable token cost was broad reconnaissance before choosing the concrete
leaf edit; resume at the exact producer ranges and APIs above. Run committed-
tree ratchets after committing so they actually inspect the new code.
