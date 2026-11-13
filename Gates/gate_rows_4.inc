P("MC2_TERRAIN_SHORELINE_EDGE_JITTER", Float, 4.0, "terrain", "", "", clampRange(0.0, 32.0) | verified),
G("MC2_TERRAIN_SHORELINE_FILE", String, "", "terrain", "", "", passthrough | verified),
P("MC2_TERRAIN_SHORELINE_FOAM_RUN", Float, 5.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SHORELINE_STRENGTH", Float, 1.0, "terrain", "", "", clampRange(0.0, 2.0) | verified),
P("MC2_TERRAIN_SHORELINE_WET_RUN", Float, 16.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN", Float, 0.78, "terrain", "", "Snow brightness dampen: <1 darkens detected-snow fragments.", verified),
L(Exact1, "MC2_TERRAIN_SOLID_WINDOW_PARITY", Bool, false, "terrain", "", "", verified),
// history: docs/gate-history.md#MC2_TERRAIN_TESS
L(Lead0Kill, "MC2_TERRAIN_TESS", Bool, true, "gameos", "", "TCS/TES terrain tessellation (gameos_graphics.cpp).", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_TESSELLATION", 3, true)),
// TERRAIN-HIRES-NEAR-1: ships ON (temporary kill-switch, =0 disables).
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_DISPLACE
L(Lead0Kill, "MC2_TERRAIN_VISUAL_DISPLACE", Bool, true, "terrain", "", "TERRAIN-HIRES-NEAR-1: draw the 4x visual height bake on VK terrain at every LOD (LOD0 at 4x density); no bake = unchanged.", passthrough | registry(Feature, "MC2_TERRAIN_VISUAL_DISPLACE", -1, true) | verified),
P("MC2_TERRAIN_VISUAL_DISPLACE_FAR", Float, 1.0, "terrain", "", "", passthrough | clampRange(0.0, 1.0) | verified),
// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 companion fade.
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE
L(Lead0Kill, "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", Bool, true, "terrain", "", "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units...", registry(Feature, "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", -1, true) | verified),
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_HEIGHT
L(Lead0, "MC2_TERRAIN_VISUAL_HEIGHT", Bool, false, "terrain", "", "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp).", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_VISUAL_HEIGHT", -1, false) | verified),
G("MC2_TERRAIN_VISUAL_HEIGHT_FILE", String, "", "terrain", "", "", passthrough | verified),
G("MC2_TEST_CRASH_FRAME", Int, 0, "gameos", "", "Diagnostic: deliberately crash at this mission frame to prove bundle collection; zero disables injection.", passthrough | verified),
G("MC2_TEST_FSR_FAIL_FRAME", Int, 0, "gameos", "", "Diagnostic: fail the FSR provider at this frame to prove the material samplers drop the FSR mip bias; zero disables.", passthrough | verified),
// history: docs/gate-history.md#MC2_TEST_SPAWN
L(Lead1, "MC2_TEST_SPAWN", Bool, false, "code", "", "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the MC2_UI_COMMAND_FILE channel.", passthrough | registry(Trace, "MC2_TRACE_TEST_SPAWN", -1, false) | verified),
L(Presence, "MC2_TEXMGR_LOAD_TRACE", Bool, false, "code", "", "", passthrough | verified),
// TRACKV quick-win: Probe-6 thin-record canary gate
L(Presence, "MC2_THIN_DEBUG", Bool, false, "terrain", "", "", passthrough),
// POST-FX-TONEMAP-V2 (SCOUR-1 #3): filmic tonemap on the composite.
L(Presence, "MC2_TRACE_EXIT", Bool, false, "gameos", "", "", verified),
// ── TREE-DIAG-REGISTER-1 (closed by GATE-PARITY-CLOSE-1) ───────────────
// Lives in kAuxEnvVars and NOT in kFeatureTable: kFeatureTable is indexed
// by RendererFeature (see the static_assert on COUNT), so appending there
// means minting an enum value for a var that gates no render path. This
// one only prints.
// history: docs/gate-history.md#MC2_TREE_DIAG_TRACE
L(Presence, "MC2_TREE_DIAG_TRACE", Bool, false, "props", "", "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.]", passthrough | registry(Trace, "MC2_TREE_DIAG_TRACE", -1, false) | verified),
// history: docs/gate-history.md#MC2_UI_COMMAND_FILE
G("MC2_UI_COMMAND_FILE", String, "", "code", "", "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file once per frame (MissionInterfaceManager::update) and executes its lines as commands.", passthrough | registry(Trace, "MC2_TRACE_UI_COMMAND_FILE", -1, false) | verified),
G("MC2_UI_DEFS_IGNORE_PACKAGE_VERSION", Bool, false, "editor", "", "", verified),
// history: docs/gate-history.md#MC2_UI_IMGUI_INFO
// history: docs/gate-history.md#MC2_UI_IMGUI_OBJ
// history: docs/gate-history.md#MC2_UI_IMGUI_PAUSE
// history: docs/gate-history.md#MC2_UI_IMGUI_TIMER
// history: docs/gate-history.md#MC2_UI_IMGUI_VIDEO
G("MC2_UNIT_DEFINITION_SOURCE", Enum, "legacy", "code", "", "", passthrough | tokens("legacy|generated") | verified),
// history: docs/gate-history.md#MC2_UNIT_PROFILE_DATA
L(Lead0, "MC2_UNIT_PROFILE_DATA", Bool, false, "unclassified", "", "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp).", passthrough | registry(Feature, "MC2_FEATURE_UNIT_PROFILE_DATA", -1, false) | verified),
G("MC2_VEGETATION_ATLAS", String, "", "gameos", "", "Allow env-var override at runtime too.", passthrough | verified),
L(DefOnLead0, "MC2_VEGETATION_CARDS", Bool, true, "gameos", "", "", passthrough | verified),
P("MC2_VEGETATION_DENSITY", Int, 25, "code", "", "", verified),
P("MC2_VEGETATION_MAX", Int, 10000000,"code", "", "", verified),
P("MC2_VEG_MAX_DIST", Float, 4096.0, "gameos", "", "", verified),
// VehicleAmbientFloor (VEHICLE-AMBIENT-FLOOR-1)
// history: docs/gate-history.md#MC2_VEHICLE_AMBIENT_FLOOR
// VehicleLitPath (VEHICLE-LIGHTING-PATH-1)
// history: docs/gate-history.md#MC2_VEHICLE_LIT_PATH
L(Lead0Kill, "MC2_VEHICLE_LIT_PATH", Bool, true, "mech", "", "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class.", passthrough | registry(Feature, "MC2_VEHICLE_LIT_PATH", 24, true) | verified),
G("MC2_VERIFY_MODE", String, "", "gameos", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_VFX_DEBUG_MODE
P("MC2_VFX_DEBUG_MODE", Int, 0, "gameos", "", "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge).", passthrough | range(0.0, 5.0) | registry(Trace, "MC2_DIAG_VFX_DEBUG_MODE", -1, false) | verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_VFX_GPU_SIM_COMPARE
L(DefOnLead0, "MC2_VFX_ORACLE_SHAPE", Bool, true, "code", "", "", verified),
// GOSFX-TUBE-RIBBON-1: gosFX Tube swept-quad ribbon oracle.
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE
L(DefOnLead0, "MC2_VFX_ORACLE_TUBE", Bool, true, "code", "", "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead...", registry(Feature, "MC2_VFX_ORACLE_TUBE", -1, true) | verified),
// history: docs/gate-history.md#MC2_VIEWMODE_FRAMEWORK
L(Lead0, "MC2_VIEWMODE_FRAMEWORK", Bool, false, "post", "", "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the...", passthrough | registry(Feature, "MC2_FEATURE_VIEWMODE_FRAMEWORK", 16, false) | verified),
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
P("MC2_VISUAL_LIVE_SETTLE_SEC", Float, 0.0, "gameos", "", "CAPTURE-LIVE-CLOCK harness option: sim-seconds the sim runs at each bookmark pose before the freeze and capture; 0 = off (sim stays frozen).", passthrough | clampRange(0.0, 60.0) | verified),
P("MC2_VISUAL_SETTLE", Int, 0, "gameos", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_VISUAL_TUNING_FILE", String, "", "gameos", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_VK_DEVICE", String, "", "vk", "", "Harness GPU routing: Vulkan device whose name contains this text; empty = first discrete.", passthrough | verified),
G("MC2_VK_FEED_DUMP", String, "", "vk", "", "", verified),
// VK-P13-SELECTOR-1 / T4: fail-soft proof knob for the engine VK context.
// history: docs/gate-history.md#MC2_VK_FORCE_FAIL
L(Lead0, "MC2_VK_FORCE_FAIL", Bool, false, "vk", "", "VK-P13-SELECTOR-1: presence-gated =1, default OFF.", passthrough | registry(Trace, "MC2_FEATURE_VK_FORCE_FAIL", -1, false) | verified),
P("MC2_VK_FRAMES_IN_FLIGHT", Int, 3, "vk", "", "Frames in flight of the Vulkan present path: 3 (default); =2 retains two frames, =1 is the kill switch back to one frame.", passthrough | verified),
G("MC2_VK_GPU_TIMESTAMPS", Bool, false, "vk", "", "GPU segment timestamps (verification/perf only): =1 arms, =0 or unset is off.", passthrough | verified),
L(Lead0Kill, "MC2_VK_LIGHTFEED_DEDUP", Bool, true, "vk", "", "", verified | passthrough),
L(Is01, "MC2_VK_MECH_FEED_NATIVE", Bool, false, "mech", "", "", verified | passthrough),
// history: docs/gate-history.md#MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT
// BUG-A pick-probe fixture (mouse->pick DPI regression fence)
// history: docs/gate-history.md#MC2_VK_PICK_PROBE
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
L(Presence, "MC2_VK_PRESENT_VERIFY", Bool, false, "gameos", "", "", passthrough | verified),
// VK-P11 TIER-1 PROP COLOR: in-place shared-GBuffer co-write (Tier-1, supersedes Tier-0).
// VK-PROP-DIRECT-CHAIN: proof slice for keeping prop color on the Vulkan-owned shared GBuffer.
L(Presence, "MC2_VK_PROP_LIGHT_PROBE", Bool, false, "vk", "", "", verified),
L(Presence, "MC2_VK_PROP_LIGHT_VERTEX_PROBE", Bool, false, "vk", "", "", verified),
L(Lead0Kill, "MC2_VK_PROP_LOD", Bool, true, "vk", "", "Distance LOD for model-override static props in the VK prop island; =0 draws LOD0 always.", verified | passthrough),
G("MC2_VK_PROP_PACKET_DUMP", String, "", "vk", "", "", verified),
L(Lead1, "MC2_VK_PROP_PERF", Bool, false, "vk", "", "", passthrough | verified),
G("MC2_VK_PROP_TEX_SLOTS", Int, 0, "vk", "", "Test knob: cap the per-object prop texture slot count (0 = device derived); exercises the more-maps-than-slots path.", verified),
G("MC2_VK_RT_SELFTEST", Bool, false, "vk", "", "Test hook (not a feature): =1 runs the one-shot ray-tracing infrastructure GPU self-test at device creation (GameOS/gameos/vk_rt_selftest.cpp).", verified),
// VK-P8a SHADOW-INTEROP: the SECOND VK geometry region + FIRST VK-produces-depth gate.
// history: docs/gate-history.md#MC2_VK_SHADOW_MATRIX_DIAG
L(Lead0, "MC2_VK_SHADOW_MATRIX_DIAG", Bool, false, "vk", "", "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp).", passthrough | registry(Trace, "MC2_TRACE_VK_SHADOW_MATRIX_DIAG", -1, false)),
// VK-P14 GATE-CONSOLIDATION-1: the ONE umbrella for the shared-GBuffer scene path.
// history: docs/gate-history.md#MC2_VK_SHARED_GBUFFER
L(Lead0Kill, "MC2_VK_SHARED_GBUFFER", Bool, true, "vk", "", "Retired: the Vulkan scene path is unconditional; =0 is ignored with a [RENDER_BACKEND] line (render_backend_select.cpp).", passthrough | registry(Feature, "MC2_FEATURE_VK_SHARED_GBUFFER", -1, true) | verified),
// VK-TIER1 SHARED SCENE DEPTH: the shared-depth substrate slice (step 2).
// history: docs/gate-history.md#MC2_VK_SHARED_SCENE_STRICT
L(Presence, "MC2_VK_TERRAIN_COPY_STATS", Bool, false, "vk", "", "", passthrough),
// VK-TERRAIN-LIGHTING: make the VK Tier-1 terrain LIT (directional relief + baked static shadow).
// history: docs/gate-history.md#MC2_VK_TERRAIN_LODFADE
// VK-P10 TERRAIN TIER-1: the terrain in-place shared-GBuffer co-write region.
// history: docs/gate-history.md#MC2_VK_VFX_TRACE
L(Presence, "MC2_VPL_CULL", Bool, false, "terrain", "", "VPL retirement: the MC2_VPL_CULL / MC2_VPL_REDUCE getenv reads are KEPT solely to gate the one-shot event=retired lifecycle lines emitted just before the...", verified),
L(Lead1, "MC2_VSYNC", Bool, false, "vk", "", "", passthrough | verified),
G("MC2_VULKAN_SPV_DIR", String, "shaders/vulkan", "vk", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_VULKAN_VALIDATION", String, "", "vk", "", "", passthrough | verified),
P("MC2_WATER_DEBUG_MODE", Int, 0, "gameos", "", "", verified),
P("MC2_WATER_EDGE_FEATHER_STRENGTH", Float, 1.0, "gameos", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
L(Lead0Kill, "MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE", Bool, true, "terrain", "", "", verified),
P("MC2_WATER_HDRI_LOD", Float, 0.0, "gameos", "", "", passthrough | verified),  // default not derivable from the tree
L(Lead0Kill, "MC2_WATER_REFLECTION", Bool, true, "gameos", "", "", verified),
L(Lead0Kill, "MC2_WATER_REFLECTION_HOLD", Bool, true, "terrain", "", "Temporary kill-switch: bounded four-frame still reflection reuse and half-res motion.", verified),
L(Lead0Kill, "MC2_WATER_REFLECTION_RT", Bool, true, "terrain", "", "Half-resolution spatial water reflection; =0 falls back to sky only. WaterReflections in options.cfg selects Off/Sky only/Half-res.", passthrough | registry(Feature, "MC2_FEATURE_WATER_REFLECTION_RT", -1, true) | verified),
// ── WATER-SUN-SPEC-2 ───────────────────────────────────────────────────
// Lives in kAuxEnvVars, not kFeatureTable: kFeatureTable is indexed by
// RendererFeature and this gate needs no enum ordinal (nothing dispatches
// on it; the only consumer is a uniform upload). Same placement rationale
// as MC2_FEATURE_WATER_SHORE_MEANDER above.
// history: docs/gate-history.md#MC2_WATER_SHINE
// -- WATER-SHORE-MODE-3 tuning knobs (GL prototype) ----------------------
P("MC2_WATER_SHORE_FEATHER", Float, 0.0, "terrain", "", "", passthrough | range(0.0, 3.4028234663852886e38) | verified),
P("MC2_WATER_SHORE_FEATHER_WU", Float, 7.0, "gameos", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
// WATER-SHORE-MASK-1 (3c864eac): shipped default-ON but was never registered.
// history: docs/gate-history.md#MC2_WATER_SHORE_MASK
L(Lead0Kill, "MC2_WATER_SHORE_MASK", Bool, true, "terrain", "", "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge.", passthrough | registry(Feature, "MC2_FEATURE_WATER_SHORE_MASK", -1, true) | verified),
// SHORELINE-GEOMETRY-1 / WATER-MEANDER-CLAMP-1: shore meander is DEFAULT OFF (0).
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER_FREQ
G("MC2_WATER_SHORE_MODE", Enum, "", "terrain", "", "", tokens("|0|1|2|3") | verified),
L(Lead0, "MC2_WATER_SKYTINT", Bool, false, "gameos", "", "", verified),
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
// history: docs/gate-history.md#MC2_XFORM_PARITY_FATAL
L(Lead1, "MC2_XFORM_PARITY_FATAL", Bool, false, "code", "", "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe (code/gamecam.cpp) to a HARD invariant.", registry(Trace, "MC2_XFORM_PARITY_FATAL", -1, false) | verified),

