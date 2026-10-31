# P3 record thread — living plan, 2026-10-31

Assignment: `A:/Games/mc2-lane8`, `claude/record-thread`, base `586327c7904eabf942d12d5835c72a2d43e2248a`.
One engine lane; local commits only. Current checkpoint changes documentation only.
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
| S1 inventory | IN PROGRESS: dependency table and archived timing/count baseline below | Close transitive read inventory, exact packet item census, and proof matrix before marking complete |
| S2 explicit state | NOT STARTED | Replace TG_Shape camera/light/fog/viewport statics with value state; packet-key vkLights; prove inline parity in separate commits |
| S3 rings/lifetimes | NOT STARTED | Three CPU slots, RenderList/HUD values, retirement graveyard, Execute(main)/Draw(record), moved-entry owner assertions |
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
Exact RenderList terrain/mover/other lengths, HUD row counts, visible leaf/particle counts remain unmeasured for the live worst-view window.
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
| Audit ON mc2_01/03/10/16/24 | Pending on rebased code; lane4's vertex-declaration prerequisite is included; no local fix made |
| Farm mc2_10/24 x8 | Pending |
| SIMID 20 s / save-load | Pending |
| Day/night pixel-identical A/B | Pending |
| Validation 0 / sync-validation 20 s | Pending; unavailable validation layer is not a pass |
| Robust A/B mean/p95 deltas and CI; laptop absolute ms | Pending; use claude/perf-robust tooling after it lands; user authorizes laptop absolute timings despite older lane-rule restriction |

Next: finish S1 transitive inventory and live worst-view item census, run/record required baseline gates, then commit the completed S1 checkpoint.
Before S2, check whether perf-robust has landed; observed integration tip `678380b2a` does not contain perf-robust tip `409815680`.
The documentation commits were rebased cleanly onto that landed integration tip; rerun complete step proof on the resulting base.
Tool slowness: the baseline native unit build/run took 809 s; keep the existing build tree for continuation and request explicit mc2 targets in engine build loops.
Disk: no install or baseline-executable copy created; temporary analysis script removed from G:/mc2-tmp; 0 GB of installs freed.
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

Next: build/prove the census checkpoint using the existing tree; finish deploy before launching any game.
Capture mc2_10/24 with mission_cpu_profile.py (live bookmark windows, normal visibility), then extract plot samples
and frame-row payloads for the worst DrawScreen views and direct/nested DrawScreen child counts from Tracy unwrap.
Complete cached/native leaf counters if needed. Run audit ON 01/03/10/16/24, farm 10/24 x8, SIMID ON/OFF 20 s,
save/load, day/night pixel A/B, validation/sync-validation and robust timings; preserve actual failures as failures.
Only after these receipts close S1: start S2 explicit CPU view values, then packet-key vkLights, each with day/night A/B.
