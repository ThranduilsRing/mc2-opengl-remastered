P("MC2_TERRAIN_SHORELINE_EDGE_JITTER", Float, 4.0, "terrain", "", "", clampRange(0.0, 32.0) | verified),
G("MC2_TERRAIN_SHORELINE_FILE", String, "", "terrain", "", "", passthrough | verified),
P("MC2_TERRAIN_SHORELINE_FOAM", Float, 1.0, "terrain", "", "", clampRange(0.0, 2.0) | verified),
P("MC2_TERRAIN_SHORELINE_FOAM_HEIGHT", Float, 5.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SHORELINE_FOAM_RUN", Float, 5.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SHORELINE_STRENGTH", Float, 1.0, "terrain", "", "", clampRange(0.0, 2.0) | verified),
P("MC2_TERRAIN_SHORELINE_WET_HEIGHT", Float, 16.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SHORELINE_WET_RUN", Float, 16.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
L(Lead0, "MC2_TERRAIN_SLOPE_BIAS", Bool, false, "terrain", "", "", verified),
P("MC2_TERRAIN_SLOPE_BIAS_STRENGTH", Float, 1.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN", Float, 0.78, "terrain", "", "Snow brightness dampen: <1 darkens detected-snow fragments.", verified),
L(Lead0Kill, "MC2_TERRAIN_SOLID_NARROW", Bool, true, "terrain", "", "", verified),
L(Exact1, "MC2_TERRAIN_SOLID_WINDOW_PARITY", Bool, false, "terrain", "", "", verified),
L(Lead0, "MC2_TERRAIN_SURFACE", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_TERRAIN_SURFACE_TRACE", Bool, false, "terrain", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_TERRAIN_TESS
L(Lead0Kill, "MC2_TERRAIN_TESS", Bool, true, "gameos", "", "TCS/TES terrain tessellation (gameos_graphics.cpp).", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_TESSELLATION", 3, true)),
L(Lead0, "MC2_TERRAIN_TILE_CENSUS", Bool, false, "terrain", "", "", verified),
L(Lead0, "MC2_TERRAIN_VISUAL_DISPLACE", Bool, false, "terrain", "", "", passthrough | verified),
P("MC2_TERRAIN_VISUAL_DISPLACE_FAR", Float, 1.0, "terrain", "", "", passthrough | clampRange(0.0, 1.0) | verified),
// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 companion fade.
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE
L(Lead0Kill, "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", Bool, true, "terrain", "", "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units...", registry(Feature, "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", -1, true) | verified),
P("MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE_RADIUS", Float, 256.0, "code", "", "World-unit radius of the mover object-fade of the visual terrain displacement; clamps to 32..1024.", clampRange(32.0, 1024.0) | verified),
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_HEIGHT
L(Lead0, "MC2_TERRAIN_VISUAL_HEIGHT", Bool, false, "terrain", "", "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp).", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_VISUAL_HEIGHT", -1, false) | verified),
G("MC2_TERRAIN_VISUAL_HEIGHT_FILE", String, "", "terrain", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_TEST_SPAWN
L(Lead1, "MC2_TEST_SPAWN", Bool, false, "code", "", "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the MC2_UI_COMMAND_FILE channel.", passthrough | registry(Trace, "MC2_TRACE_TEST_SPAWN", -1, false) | verified),
P("MC2_TEXMGR_KTX_PRIMARY", Int, 1, "mech", "", "", passthrough | verified),
L(Presence, "MC2_TEXMGR_LOAD_TRACE", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_TEX_LIFECYCLE_TRACE", Bool, false, "props", "", "MC2_TEX_LIFECYCLE_TRACE=1 - same env flag used by mclib/txmmgr.cpp, mclib/msl.cpp, GameOS/gameos/gos_static_prop_batcher.cpp so all [TEX_LIFECYCLE v1]...", verified),
L(Presence, "MC2_TEX_LIFECYCLE_TRACE_VERBOSE", Bool, false, "code", "", "", verified),
L(Presence, "MC2_TGL_POOL_TRACE", Bool, false, "gameos", "", "", verified),
// TRACKV quick-win: Probe-6 thin-record canary gate
// history: docs/gate-history.md#MC2_THIN_CANARY
L(Lead1, "MC2_THIN_CANARY", Bool, false, "terrain", "", "TRACKV: gates Probe-6 thin-record canary readback (per-frame ~80KB glGetBufferSubData x2 + CPU recipeIdx/flags compare, diagnostic for the corrupt-recipeIdx...", registry(Trace, "MC2_TRACE_THIN_CANARY", -1, false)),
L(Presence, "MC2_THIN_DEBUG", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_TOBJ_COST_SPLIT", Bool, false, "unclassified", "", "[TOBJSPLIT v1] env-gated RDTSC cost split for the recalcBounds slice.", passthrough | verified),
L(Presence, "MC2_TOBJ_PARITY", Bool, false, "terrain", "", "[TOBJPARITY v1] env-gated STANDALONE readback-vs-coarse superset-parity instrument.", passthrough | verified),
// POST-FX-TONEMAP-V2 (SCOUR-1 #3): filmic tonemap on the composite.
L(Presence, "MC2_TRACE_EXIT", Bool, false, "gameos", "", "", verified),
// ── TREE-DIAG-REGISTER-1 (closed by GATE-PARITY-CLOSE-1) ───────────────
// Lives in kAuxEnvVars and NOT in kFeatureTable: kFeatureTable is indexed
// by RendererFeature (see the static_assert on COUNT), so appending there
// means minting an enum value for a var that gates no render path. This
// one only prints.
// history: docs/gate-history.md#MC2_TREE_DIAG_TRACE
L(Presence, "MC2_TREE_DIAG_TRACE", Bool, false, "props", "", "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.]", passthrough | registry(Trace, "MC2_TREE_DIAG_TRACE", -1, false) | verified),
L(Presence, "MC2_TREE_REG_TRACE", Bool, false, "props", "", "", verified),
L(Presence, "MC2_TUBE_PROFILE_LOG", Bool, false, "code", "", "", verified),
L(Lead1, "MC2_TXMMGR_BOUNDS_TRACE", Bool, false, "code", "", "[TXMMGR_BOUNDS v1] - TXMMGR-BOUNDS-HARDEN-1 exhaustion counters.", verified),
L(Presence, "MC2_TYPE_TABLE_CAND_LOG", Bool, false, "props", "", "", passthrough | verified),
L(Presence, "MC2_TYPE_TABLE_CAND_VERBOSE", Bool, false, "props", "", "", passthrough | verified),
L(Lead0Kill, "MC2_UI_ASPECT_ANCHOR", Bool, true, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_UI_COMMAND_FILE
G("MC2_UI_COMMAND_FILE", String, "", "code", "", "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file once per frame (MissionInterfaceManager::update) and executes its lines as commands.", passthrough | registry(Trace, "MC2_TRACE_UI_COMMAND_FILE", -1, false) | verified),
G("MC2_UI_DEFS_IGNORE_PACKAGE_VERSION", Bool, false, "editor", "", "", verified),
L(Presence, "MC2_UI_DEFS_LOADSCREEN", Bool, false, "editor", "", "", verified),
L(Presence, "MC2_UI_DEFS_RESULTS", Bool, false, "editor", "", "", verified),
L(Lead0, "MC2_UI_DEFS_TRACE", Bool, false, "unclassified", "", "", verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_INFO
L(Lead0, "MC2_UI_IMGUI_INFO", Bool, false, "code", "", "INFO-IMGUI-SLICE-1: =1 renders the in-mission unit-Info panel as an ImGui window (unit name + health bar + loadout list with scrollbar) laid out inside...", registry(Trace, "MC2_TRACE_UI_IMGUI_INFO", -1, false) | verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_OBJ
L(Lead0, "MC2_UI_IMGUI_OBJ", Bool, false, "code", "", "OBJ-IMGUI-SLICE-1: =1 renders the in-mission OBJECTIVES overlay as ImGui (baked left-edge frame from tools/ui_art_reconstruct.py + procedural...", registry(Trace, "MC2_TRACE_UI_IMGUI_OBJ", -1, false) | verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_PAUSE
L(Lead0, "MC2_UI_IMGUI_PAUSE", Bool, false, "code", "", "PAUSE-IMGUI-SLICE-1: =1 renders the in-mission pause menu as an ImGui window (GAME PAUSED + objectives/save/load/options/abort/exit/return) laid out inside...", registry(Trace, "MC2_TRACE_UI_IMGUI_PAUSE", -1, false) | verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_TIMER
L(Lead0, "MC2_UI_IMGUI_TIMER", Bool, false, "code", "", "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission mission-timer display as ImGui (right-aligned MM:SS via HudFont + the TimerStatic atlas tile) laid out...", registry(Trace, "MC2_TRACE_UI_IMGUI_TIMER", -1, false) | verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_VIDEO
L(Lead0, "MC2_UI_IMGUI_VIDEO", Bool, false, "code", "", "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission comm-video panel as ImGui - VideoStatic chrome tiles + the decoder's frame texture at the same...", registry(Trace, "MC2_TRACE_UI_IMGUI_VIDEO", -1, false) | verified),
L(Presence, "MC2_UI_PASS_MEASURE", Bool, false, "gameos", "", "UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only telemetry inside the existing PassIdentity::UI RAII scope.", passthrough | verified),
G("MC2_UNIT_DEFINITION_SOURCE", Enum, "legacy", "code", "", "", passthrough | tokens("legacy|generated") | verified),
// history: docs/gate-history.md#MC2_UNIT_PROFILE_DATA
L(Lead0, "MC2_UNIT_PROFILE_DATA", Bool, false, "unclassified", "", "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp).", passthrough | registry(Feature, "MC2_FEATURE_UNIT_PROFILE_DATA", -1, false) | verified),
L(Presence, "MC2_VANISH_PROBE", Bool, false, "code", "", "", verified),
G("MC2_VEGETATION_ATLAS", String, "", "gameos", "", "Allow env-var override at runtime too.", passthrough | verified),
L(DefOnLead0, "MC2_VEGETATION_CARDS", Bool, true, "gameos", "", "", passthrough | verified),
P("MC2_VEGETATION_DENSITY", Int, 25, "code", "", "", verified),
P("MC2_VEGETATION_MAX", Int, 5000000, "code", "", "", verified),
L(Lead0, "MC2_VEG_DEBUG_FORCE_VISIBLE", Bool, false, "gameos", "", "", verified),
P("MC2_VEG_MAX_DIST", Float, 4096.0, "gameos", "", "", verified),
// VehicleAmbientFloor (VEHICLE-AMBIENT-FLOOR-1)
// history: docs/gate-history.md#MC2_VEHICLE_AMBIENT_FLOOR
L(Lead0Kill, "MC2_VEHICLE_AMBIENT_FLOOR", Bool, true, "code", "", "VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON.", passthrough | registry(Feature, "MC2_VEHICLE_AMBIENT_FLOOR", 28, true)),
// VehicleLitPath (VEHICLE-LIGHTING-PATH-1)
// history: docs/gate-history.md#MC2_VEHICLE_LIT_PATH
L(Lead0Kill, "MC2_VEHICLE_LIT_PATH", Bool, true, "mech", "", "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class.", passthrough | registry(Feature, "MC2_VEHICLE_LIT_PATH", 26, true) | verified),
G("MC2_VERIFY_MODE", String, "", "gameos", "", "", passthrough | verified),
L(Presence, "MC2_VERTEX_PROJECT_FAST", Bool, false, "gameos", "", "", passthrough | verified),
L(Presence, "MC2_VERTEX_PROJECT_PARITY", Bool, false, "gameos", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_VFX_AGE_SAMPLE
L(Lead1, "MC2_VFX_AGE_SAMPLE", Bool, false, "code", "", "VFX-AGE-SAMPLE-1: sample GPU-particle spec curves (color/alpha/size/UV) at the routed effect's real CPU-advanced normalized age (gosFX Effect::m_age...", registry(Feature, "MC2_FEATURE_VFX_AGE_SAMPLE", 14, false) | verified),
L(Lead1, "MC2_VFX_BLACKBODY", Bool, false, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_VFX_DEBUG_MODE
P("MC2_VFX_DEBUG_MODE", Int, 0, "gameos", "", "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge).", passthrough | range(0.0, 5.0) | registry(Trace, "MC2_DIAG_VFX_DEBUG_MODE", -1, false) | verified),  // default not derivable from the tree
L(Lead1, "MC2_VFX_DISTORTION", Bool, false, "gameos", "", "", verified),
P("MC2_VFX_DISTORT_AMP", Float, 0.04, "gameos", "", "Screen-UV wobble amplitude of the distortion pass; outside (0, 0.5) reads the default.", rangeAbove(0.0, 0.4999999701976776) | verified),
L(Lead1, "MC2_VFX_DISTORT_FIXTURE", Bool, false, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_VFX_GPU_SIM_CARDCLOUD
L(Lead1, "MC2_VFX_GPU_SIM_CARDCLOUD", Bool, false, "code", "", "VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2 of the originals-restoration arc; docs/vfx-gpu-sim-spec.md).", registry(Feature, "MC2_FEATURE_VFX_GPU_SIM_CARDCLOUD", 16, false)),
// history: docs/gate-history.md#MC2_VFX_GPU_SIM_COMPARE
// history: docs/gate-history.md#MC2_VFX_ORACLE_RENDER
L(Lead1, "MC2_VFX_ORACLE_RENDER", Bool, false, "code", "", "VFX-ORIGINAL-RECORD-ABI-1 (Phase 1 of the originals-restoration arc; see docs/vfx-originals-restoration-design.md).", passthrough | registry(Feature, "MC2_FEATURE_VFX_ORACLE_RENDER", 15, false) | verified),
L(DefOnLead0, "MC2_VFX_ORACLE_SHAPE", Bool, true, "code", "", "", verified),
L(Lead1, "MC2_VFX_ORACLE_SHAPE_LOG", Bool, false, "unclassified", "", "", verified),
// GOSFX-TUBE-RIBBON-1: gosFX Tube swept-quad ribbon oracle.
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE
L(DefOnLead0, "MC2_VFX_ORACLE_TUBE", Bool, true, "code", "", "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead...", registry(Feature, "MC2_VFX_ORACLE_TUBE", -1, true) | verified),
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE_COVERAGE
L(Lead1, "MC2_VFX_ORACLE_TUBE_COVERAGE", Bool, false, "gameos", "", "GOSFX-TUBE-RIBBON-1: wrap the deferred ribbon draw in a GL_SAMPLES_PASSED occlusion query; logs [VFX_ORACLE_TUBE coverage] ribbons/samples/fbo/sceneDrawBufs...", registry(Trace, "MC2_VFX_ORACLE_TUBE_COVERAGE", -1, false)),
L(Lead1, "MC2_VFX_SCENECOLOR_GRAB", Bool, false, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_VIEWMODE_FRAMEWORK
L(Lead0, "MC2_VIEWMODE_FRAMEWORK", Bool, false, "post", "", "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the...", passthrough | registry(Feature, "MC2_FEATURE_VIEWMODE_FRAMEWORK", 17, false) | verified),
// TRACKV-TACTICAL-PRESENTATION: ViewMode seed + LowLight tunables + tactical
// overlay. All default-OFF/inert; Visual path byte-identical when unset.
// (MC2_VIEWMODE_FRAMEWORK itself is the RendererFeature::ViewmodeFramework
// enum entry, registered in kFeatureTable above.)
// history: docs/gate-history.md#MC2_VIEW_MODE
G("MC2_VIEW_MODE", String, "", "post", "", "VIEWMODE-POSTPROCESS-PRESENTATION-1: seeds the startup presentation mode (matches RenderCore::ViewMode).", passthrough | registry(Trace, "MC2_TUNE_VIEW_MODE", -1, false) | verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_VIEW_UNIFORMS
L(Lead0Kill, "MC2_VIEW_UNIFORMS", Bool, true, "props", "", "ViewUniforms UBO at binding=3.", passthrough | registry(Feature, "MC2_FEATURE_VIEW_UNIFORMS", 12, true) | verified),
G("MC2_VISUAL_BOOKMARK_CAPTURE", String, "", "vk", "", "Path of the bookmarks.json the capture replay drives; any set value (even empty) arms the replay.", passthrough | verified),
L(Lead0, "MC2_VISUAL_CAPTURE_CLEAN", Bool, false, "gameos", "", "", verified),
G("MC2_VISUAL_CAPTURE_DIR", String, "", "gameos", "", "Directory the visual capture primitive and the bookmark replay write into; set-but-empty is kept as empty (replay falls back to .).", passthrough | verified),
L(Presence, "MC2_VISUAL_CAPTURE_EXIT_AFTER", Bool, false, "gameos", "", "VK-NOINTEROP-STRICT-REVIEW: opt-in clean shutdown after the sweep so the VK island destroy lines (forbidden_* counters) + the final [VK_PRESENT] logHealth...", verified),
P("MC2_VISUAL_CAPTURE_FRAME", Int, -1, "vk", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_VISUAL_CAPTURE_ON_FIRE
L(Presence, "MC2_VISUAL_CAPTURE_ON_FIRE", Bool, false, "unclassified", "", "VK-FIELD-VFX-8: GL twin of MC2_VK_PRESENT_DUMP_ON_FIRE.", registry(Trace, "MC2_TRACE_VISUAL_CAPTURE_ON_FIRE", -1, false) | verified),
L(Lead0, "MC2_VISUAL_DIFF_CAPTURE", Bool, false, "gameos", "", "", verified),
P("MC2_VISUAL_DIFF_FRAME_N", Int, 90, "gameos", "", "", verified),
P("MC2_VISUAL_DIFF_MAX_FRAMES", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
G("MC2_VISUAL_DIFF_MISSION", String, "", "gameos", "", "", verified),  // default not derivable from the tree
G("MC2_VISUAL_DIFF_OUT", String, "", "gameos", "", "", verified),
P("MC2_VISUAL_SETTLE", Int, 0, "gameos", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_VISUAL_TUNING_FILE", String, "", "gameos", "", "", passthrough | verified),  // default not derivable from the tree
L(Lead0Kill, "MC2_VIS_SEMANTICS", Bool, true, "code", "", "ONE gate.", verified | passthrough),
L(Lead0, "MC2_VIS_SEMANTICS_TRACE", Bool, false, "code", "", "", verified | passthrough),
P("MC2_VIS_SEMANTICS_TRACE_EVERY", Int, 120, "code", "", "", passthrough | verified),
G("MC2_VK_FEED_DUMP", String, "", "vk", "", "", verified),
// VK-P13-SELECTOR-1 / T4: fail-soft proof knob for the engine VK context.
// history: docs/gate-history.md#MC2_VK_FORCE_FAIL
L(Lead0, "MC2_VK_FORCE_FAIL", Bool, false, "vk", "", "VK-P13-SELECTOR-1: presence-gated =1, default OFF.", passthrough | registry(Trace, "MC2_FEATURE_VK_FORCE_FAIL", -1, false) | verified),
P("MC2_VK_FRAMES_IN_FLIGHT", Int, 2, "vk", "", "Frames in flight of the Vulkan present path: 2 (default); =1 is the kill switch back to one frame.", passthrough | verified),
L(Presence, "MC2_VK_GPU_TIMESTAMPS", Bool, false, "vk", "", "", passthrough | verified),
L(Presence, "MC2_VK_HUD_DRAW_DUMP", Bool, false, "gameos", "", "Env-token-only memo (no disarm interaction): the env block is process-static, so one getenv per process is exact.", verified),
L(Lead0Kill, "MC2_VK_LIGHTFEED_DEDUP", Bool, true, "vk", "", "", verified | passthrough),
L(Is01, "MC2_VK_MECH_FEED_NATIVE", Bool, false, "mech", "", "", verified | passthrough),
L(Lead0Kill, "MC2_VK_MECH_FENCE_DEFER", Bool, true, "vk", "", "", verified | passthrough),
L(Lead1, "MC2_VK_MECH_PERF", Bool, false, "vk", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT
L(Lead1, "MC2_VK_OBJECTID_DIRECT_CHAIN", Bool, false, "vk", "", "", passthrough | verified),
// BUG-A pick-probe fixture (mouse->pick DPI regression fence)
// history: docs/gate-history.md#MC2_VK_PICK_PROBE
L(Presence, "MC2_VK_PICK_PROBE", Bool, false, "code", "", "BUG-A pick regression fence: default-OFF.", passthrough | registry(Trace, "MC2_VK_PICK_PROBE", -1, false) | verified),
L(Lead0Kill, "MC2_VK_POST_SCREENFX", Bool, true, "vk", "", "VK native screen-shadow, cloud and shoreline passes, on under the VK umbrella; =0 turns the VK passes off.", verified | passthrough),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_CLOUD", Bool, true, "vk", "", "", verified | passthrough),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_SHADOW", Bool, true, "vk", "", "", verified | passthrough),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_SHORELINE", Bool, true, "vk", "", "", verified | passthrough),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_FRAME
P("MC2_VK_PRESENT_DUMP_FRAME", Int, 0, "vk", "", "VK-FIELD-VFX-3: frame pin for MC2_VK_PRESENT_DUMP_PPM.", registry(Trace, "MC2_TRACE_VK_PRESENT_DUMP_FRAME", -1, false) | verified),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_ON_FIRE
L(Presence, "MC2_VK_PRESENT_DUMP_ON_FIRE", Bool, false, "vk", "", "VK-FIELD-VFX-5: dynamic fire-event pin for MC2_VK_PRESENT_DUMP_PPM.", registry(Trace, "MC2_TRACE_VK_PRESENT_DUMP_ON_FIRE", -1, false) | verified),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_PPM
G("MC2_VK_PRESENT_DUMP_PPM", String, "", "vk", "", "VK-FIELD-ORACLE-1: headless present-pixel oracle.", registry(Trace, "MC2_TRACE_VK_PRESENT_DUMP_PPM", -1, false) | verified),
// history: docs/gate-history.md#MC2_VK_PRESENT_NATIVE_STRICT
L(Lead1, "MC2_VK_PRESENT_NATIVE_STRICT", Bool, false, "vk", "", "VK present-tail deletion-readiness canary.", passthrough | registry(Trace, "MC2_TRACE_VK_PRESENT_NATIVE_STRICT", -1, false) | verified),
L(Lead1, "MC2_VK_PRESENT_SKIP_GL_HANDOFF", Bool, false, "vk", "", "", passthrough | verified),
L(Presence, "MC2_VK_PRESENT_VERIFY", Bool, false, "gameos", "", "", passthrough | verified),
// VK-P11 TIER-1 PROP COLOR: in-place shared-GBuffer co-write (Tier-1, supersedes Tier-0).
// VK-PROP-DIRECT-CHAIN: proof slice for keeping prop color on the Vulkan-owned shared GBuffer.
L(Lead0Kill, "MC2_VK_PROP_FENCE_DEFER", Bool, true, "vk", "", "", verified | passthrough),
L(Presence, "MC2_VK_PROP_LIGHT_PROBE", Bool, false, "vk", "", "", verified),
L(Presence, "MC2_VK_PROP_LIGHT_VERTEX_PROBE", Bool, false, "vk", "", "", verified),
G("MC2_VK_PROP_PACKET_DUMP", String, "", "vk", "", "", verified),
L(Lead1, "MC2_VK_PROP_PERF", Bool, false, "vk", "", "", passthrough | verified),
L(Lead1, "MC2_VK_SELECT_TEST", Bool, false, "code", "", "", verified),
// VK-P8a SHADOW-INTEROP: the SECOND VK geometry region + FIRST VK-produces-depth gate.
// history: docs/gate-history.md#MC2_VK_SHADOW_MATRIX_DIAG
L(Lead0, "MC2_VK_SHADOW_MATRIX_DIAG", Bool, false, "vk", "", "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp).", passthrough | registry(Trace, "MC2_TRACE_VK_SHADOW_MATRIX_DIAG", -1, false)),
// VK-P14 GATE-CONSOLIDATION-1: the ONE umbrella for the shared-GBuffer scene path.
// history: docs/gate-history.md#MC2_VK_SHARED_GBUFFER
L(Lead0Kill, "MC2_VK_SHARED_GBUFFER", Bool, true, "vk", "", "Retired: the Vulkan scene path is unconditional; =0 is ignored with a [RENDER_BACKEND] line (render_backend_select.cpp).", passthrough | registry(Feature, "MC2_FEATURE_VK_SHARED_GBUFFER", -1, true) | verified),
// VK-TIER1 SHARED SCENE DEPTH: the shared-depth substrate slice (step 2).
// history: docs/gate-history.md#MC2_VK_SHARED_SCENE_STRICT
L(Presence, "MC2_VK_TERRAIN_COPY_STATS", Bool, false, "vk", "", "", passthrough | verified),
L(Lead0Kill, "MC2_VK_TERRAIN_FENCE_DEFER", Bool, true, "vk", "", "", verified | passthrough),
// VK-TERRAIN-LIGHTING: make the VK Tier-1 terrain LIT (directional relief + baked static shadow).
// history: docs/gate-history.md#MC2_VK_TERRAIN_LODFADE
L(Exact1, "MC2_VK_TERRAIN_LODFADE", Bool, false, "terrain", "", "VK-TERRAIN-LODFADE: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DETAIL).", passthrough | registry(Feature, "MC2_FEATURE_VK_TERRAIN_LODFADE", -1, false) | verified),
L(Lead1, "MC2_VK_TERRAIN_PERF", Bool, false, "vk", "", "", passthrough | verified),
// VK-P10 TERRAIN TIER-1: the terrain in-place shared-GBuffer co-write region.
L(Exact1, "MC2_VK_TERRAIN_VISUALDISPLACE_FAR", Bool, false, "vk", "", "", verified | passthrough),
// history: docs/gate-history.md#MC2_VK_VFX_TRACE
L(Lead0, "MC2_VK_VFX_TRACE", Bool, false, "vk", "", "VK-FIELD-VFX-2: per-bolt weapon-fire pipeline trace.", registry(Trace, "MC2_TRACE_VK_VFX_TRACE", -1, false) | verified),
L(Lead0Kill, "MC2_VK_WATER_FENCE_DEFER", Bool, true, "vk", "", "", verified | passthrough),
L(Lead1, "MC2_VK_WATER_PERF", Bool, false, "vk", "", "", passthrough | verified),
L(Presence, "MC2_VPL_CULL", Bool, false, "terrain", "", "VPL retirement: the MC2_VPL_CULL / MC2_VPL_REDUCE getenv reads are KEPT solely to gate the one-shot event=retired lifecycle lines emitted just before the...", verified),
L(Presence, "MC2_VPL_PICK", Bool, false, "code", "", "VPL-retirement Step 3 (3a): recursion-free tile selection.", verified),
L(Presence, "MC2_VPL_REDUCE", Bool, false, "terrain", "", "", verified),
L(Lead1, "MC2_VSYNC", Bool, false, "vk", "", "", passthrough | verified),
G("MC2_VULKAN_SPV_DIR", String, "shaders/vulkan", "vk", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_VULKAN_VALIDATION", String, "", "vk", "", "", passthrough | verified),
L(Presence, "MC2_WATCHID_GENERATION", Bool, false, "code", "", "MF3-GENERATIONAL-HANDLE-1: generational watch-id validation killswitch.", passthrough | verified),
L(Presence, "MC2_WATER_ASPECT_DIAG", Bool, false, "code", "", "[WATER_ASPECT_DIAG v1] env-gated one-shot (MC2_WATER_ASPECT_DIAG=1), silent by default.", verified),
L(Lead0, "MC2_WATER_BIAS_LEGACY", Bool, false, "gameos", "", "", verified),
L(Presence, "MC2_WATER_DEBUG", Bool, false, "terrain", "", "MC2_WATER_DEBUG=1: post-warmup population recon for the renderWater slice.", passthrough | verified),
P("MC2_WATER_DEBUG_MODE", Int, 0, "gameos", "", "", verified),
L(Lead0Kill, "MC2_WATER_EDGE_FEATHER", Bool, true, "gameos", "", "", verified),
P("MC2_WATER_EDGE_FEATHER_STRENGTH", Float, 1.0, "gameos", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
L(Presence, "MC2_WATER_GATE_DIAG", Bool, false, "terrain", "", "S2.15 gate diag: editor fails to arm FAST water path - print once per (mission/state-change) what gate fails.", passthrough | verified),
L(Lead0Kill, "MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE", Bool, true, "terrain", "", "", verified),
P("MC2_WATER_HDRI_LOD", Float, 0.0, "gameos", "", "", passthrough | verified),  // default not derivable from the tree
L(Lead0, "MC2_WATER_HDRI_REFL_FULL", Bool, false, "gameos", "", "", passthrough | verified),
L(Presence, "MC2_WATER_MATERIAL_PROBE", Bool, false, "terrain", "", "", verified),
L(Lead0Kill, "MC2_WATER_REFLECTION", Bool, true, "gameos", "", "", verified),
L(Lead0, "MC2_WATER_REFLECTION_RT", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_WATER_S6_COST", Bool, false, "terrain", "", "S6 coarse cost A/B instrument: ONE QPC pair around the WHOLE per-frame setupTextures loop (NOT per-quad - the per-quad std::chrono COST_SPLIT scopes are...", passthrough | verified),
L(Presence, "MC2_WATER_S6_TRACE", Bool, false, "terrain", "", "", verified),
// ── WATER-SUN-SPEC-2 ───────────────────────────────────────────────────
// Lives in kAuxEnvVars, not kFeatureTable: kFeatureTable is indexed by
// RendererFeature and this gate needs no enum ordinal (nothing dispatches
// on it; the only consumer is a uniform upload). Same placement rationale
// as MC2_FEATURE_WATER_SHORE_MEANDER above.
// history: docs/gate-history.md#MC2_WATER_SHINE
G("MC2_WATER_SHINE", String, "", "gameos", "", "[DEFAULT-OFF OPT-IN.", passthrough | registry(Feature, "MC2_FEATURE_WATER_SHINE", -1, false) | verified),
// -- WATER-SHORE-MODE-3 tuning knobs (GL prototype) ----------------------
P("MC2_WATER_SHORE_FEATHER", Float, 0.0, "terrain", "", "", passthrough | range(0.0, 3.4028234663852886e38) | verified),
P("MC2_WATER_SHORE_FEATHER_WU", Float, 7.0, "gameos", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
// WATER-SHORE-MASK-1 (3c864eac): shipped default-ON but was never registered.
// history: docs/gate-history.md#MC2_WATER_SHORE_MASK
L(Lead0Kill, "MC2_WATER_SHORE_MASK", Bool, true, "terrain", "", "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge.", passthrough | registry(Feature, "MC2_FEATURE_WATER_SHORE_MASK", -1, true) | verified),
// SHORELINE-GEOMETRY-1 / WATER-MEANDER-CLAMP-1: shore meander is DEFAULT OFF (0).
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER
P("MC2_WATER_SHORE_MEANDER", Float, 0.0, "terrain", "", "SHORELINE-GEOMETRY-1: fBm meander amplitude in world units applied to the shore contour in gos_terrain_water_stream.cpp ShoreMeanderWu(); breaks the...", passthrough | registry(Feature, "MC2_FEATURE_WATER_SHORE_MEANDER", -1, false) | range(0.0, 3.4028234663852886e38) | verified),
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER_FREQ
P("MC2_WATER_SHORE_MEANDER_FREQ", Float, 1.0 / 384.0, "terrain", "", "SHORELINE-GEOMETRY-1 companion tuning knob for MC2_WATER_SHORE_MEANDER: spatial frequency of the shore-contour fBm, in cycles per world unit...", passthrough | registry(Feature, "MC2_TUNE_WATER_SHORE_MEANDER_FREQ", -1, false) | rangeAbove(0.0, 3.4028234663852886e38) | verified),
G("MC2_WATER_SHORE_MODE", Enum, "", "terrain", "", "", tokens("|0|1|2|3") | verified),
L(Lead0, "MC2_WATER_SKYTINT", Bool, false, "gameos", "", "", verified),
L(Presence, "MC2_WATER_STREAM_DEBUG", Bool, false, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_WATER_UPLOAD_NARROW", Bool, true, "terrain", "", ""),
P("MC2_WIDTH", Int, 0, "gameos", "", "(c) #49 windowed-resolution oracle.", passthrough | verified),
L(Lead0, "MC2_WINDOWED", Bool, false, "gameos", "", "", passthrough | verified),
// ── NOFOCUS-HARDEN-1 ───────────────────────────────────────────────────
// KIND NOTE, stated so a later audit does not have to guess: this is
// classified Trace to sit beside MC2_TRACE_NO_CURSOR_GRAB above, which is
// its functional sibling (both are harness/window hands-off gates read in
// GameOS/gameos/gos_render.cpp). It is NOT a Feature: it gates no render
// code path and changes no rendered pixel -- the GL drawable keeps its full
// size, so glReadPixels/FBO capture is bit-identical with it on or off.
// What it changes is the OS window's POSITION, plus a refusal of fullscreen.
// `defaultOn` is false and that is what the reader implements
// (gos_render.cpp window_offscreen_requested(): `e && e[0] && e[0] != '0'`,
// i.e. unset => OFF). Do not "tidy" it to true: an unset engine launch is
// exactly-legacy, and the harness turns it on explicitly.
// history: docs/gate-history.md#MC2_WINDOW_OFFSCREEN
L(Lead0, "MC2_WINDOW_OFFSCREEN", Bool, false, "unclassified", "", "NOFOCUS-HARDEN-1: create the SDL window HIDDEN at (-32000,-32000) and keep it there, so an agent-launched engine window never occupies visible desktop area...", passthrough | registry(Trace, "MC2_TRACE_WINDOW_OFFSCREEN", -1, false) | verified),
L(Lead0Kill, "MC2_WINDOW_RESIZABLE", Bool, true, "gameos", "", "", verified),
G("MC2_WORLD_ORDER_TRACE", Bool, false, "code", "", "W2: emit ordered world views, terrain activity, lifecycle events and update calls per mission tick. Diagnostic only.", passthrough | registry(Trace, "MC2_WORLD_ORDER_TRACE", -1, false) | verified),
// history: docs/gate-history.md#MC2_XFORM_PARITY_FATAL
L(Lead1, "MC2_XFORM_PARITY_FATAL", Bool, false, "code", "", "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe (code/gamecam.cpp) to a HARD invariant.", registry(Trace, "MC2_XFORM_PARITY_FATAL", -1, false) | verified),

