L(Presence, "MC2_LOG_PREVIEW", Bool, false, "mech", "", "", verified),
// history: docs/gate-history.md#MC2_MATERIAL_GPU
L(Lead0Kill, "MC2_MATERIAL_GPU", Bool, true, "unclassified", "", "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26).", passthrough | registry(Feature, "MC2_FEATURE_MATERIAL_GPU", 9, true) | verified),
// history: docs/gate-history.md#MC2_MATERIAL_GPU_SAMPLE
L(Lead0Kill, "MC2_MATERIAL_GPU_SAMPLE", Bool, true, "props", "", "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26).", passthrough | registry(Feature, "MC2_FEATURE_MATERIAL_GPU_SAMPLE", 10, true) | verified),
// history: docs/gate-history.md#MC2_MATERIAL_KTX
L(Presence, "MC2_MECH_BATCHER_STATS", Bool, false, "mech", "", "", verified),
P("MC2_MECH_FILL", Int, 2, "mech", "", "Mech lighting strength, ships ON at 2. Dev A/B override: 0 = original look, 1 = shared swapped sun + sun-sized fill, 2 = plus wrapped N.L and fresnel rim.", clampRange(0, 2) | verified),
L(Lead1, "MC2_MECH_IMPORT_ANIMATE", Bool, false, "code", "", "Opt-in, default OFF (static FORCE_CLIP bake stays the safe path).", verified),
G("MC2_MECH_IMPORT_FORCE_BONE", String, "j_LThigh", "mech", "", "", verified),
G("MC2_MECH_IMPORT_FORCE_CLIP", String, "", "code", "", "BT2018-SKEL-ENGINE-1B: by default bake the REST (bind) pose.", verified),
P("MC2_MECH_IMPORT_FORCE_FRAME", Int, 0, "code", "", "", verified),
P("MC2_MECH_IMPORT_FORCE_POSE", Int, 0, "mech", "", "", verified),
L(Lead1, "MC2_MECH_IMPORT_GPU", Bool, false, "code", "", "", verified),
P("MC2_MECH_IMPORT_GPU_LIFT", Float, 0.0, "code", "", "", verified),
G("MC2_MECH_SKEL_BONE_DUMP", String, "", "code", "", "MECH-BONE-PARITY-GATE-1: dump the engine's REST joint-global bone matrices (the same mc2skel output the bake consumes) as JSON when...", verified),
P("MC2_MECH_SKEL_HEIGHT", Float, 50.0, "code", "", "", verified),
P("MC2_MECH_SPECULAR_STRENGTH", Float, 0.05, "mech", "", "", clampRange(0.0, 4.0) | verified),
L(Lead0Kill, "MC2_MECH_SPECULAR_V1", Bool, true, "mech", "", "", verified),
G("MC2_MECH_SURFACE_MATERIAL", Enum, "", "mech", "", "Gate: MC2_MECH_SURFACE_MATERIAL controls which profiles to load.", tokens("|0|metal061b|none|painted_subtle") | verified),
L(Lead0Kill, "MC2_MECH_VIEWUNIFORMS", Bool, true, "mech", "", "", verified),
L(Lead0, "MC2_MENU_CANARY_SKIP_INTRO", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_MISSION_CYCLE_TEST", Bool, false, "code", "", "", passthrough | verified),
// MissionIntroLegacyRender (TRACKV-CI-GATE-RESTORE-1)
L(Presence, "MC2_MISSION_SPLIT", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_MODOVERRIDE_TRACE", Bool, false, "props", "", "", verified),
G("MC2_MOD_DEPS", String, "", "code", "", "Stock campaign: no active mod.", passthrough | verified),
L(Lead1, "MC2_MOD_HEALTH_DIAG", Bool, false, "code", "", "MC2_MOD_HEALTH_DIAG=1 - emit [MOD_HEALTH v1] AI-liveness milestone lines (tools/mod_smoke.py sets it).", passthrough | verified),
// META-FIX-BATCH-3 (recurrence-proofing, designs A + B — 2026-07-16)
// history: docs/gate-history.md#MC2_MOVER_COHERENCE_AUDIT
L(Lead0, "MC2_MOVER_COHERENCE_AUDIT", Bool, false, "code", "", "META-FIX design A runtime tier (code/mission.cpp auditMoverCoherence, ~1 Hz): read-only audit that the five per-mover registries (isOnGUI render gate...", passthrough | registry(Trace, "MC2_TRACE_MOVER_COHERENCE_AUDIT", -1, false) | verified),
P("MC2_MOVER_CONTACT_SHADOW", Float, 0.0, "terrain", "", "", passthrough | range(0.0, 1.0) | verified),
P("MC2_MOVER_FIXTURE", Int, 0, "sim", "", "M6 dev selector: =N tops the mission up to N movers on two teams with move/attack orders (code/mover_budget_fixture.h, tools/mover_budget.py). 0 = off.", passthrough | range(0, 1000) | verified),
// history: docs/gate-history.md#MC2_MOVE_RECON
// history: docs/gate-history.md#MC2_NIGHT_AMBIENT_FLOOR
P("MC2_NIGHT_AMBIENT_FLOOR", Int, 0, "code", "", "NIGHT-LIGHT-POOLS-1 knob: minimum per-channel output (0..64, clamped) of Camera::getLightRed/Green/Blue -- the family feeding the colormap night burn-in...", clampRange(0, 64) | registry(Trace, "MC2_TUNE_NIGHT_AMBIENT_FLOOR", -1, false) | verified),
// history: docs/gate-history.md#MC2_NO_ACTIVATE
L(Lead0, "MC2_NO_ACTIVATE", Bool, false, "editor", "", "Mission Editor (editor/EditorMFC.cpp, MainFrm.cpp): come up minimized and non-activating, never taking the foreground or keyboard focus.", registry(Trace, "MC2_TRACE_NO_ACTIVATE", -1, false) | verified),
// history: docs/gate-history.md#MC2_NO_CURSOR_GRAB
L(Lead0Def, "MC2_NO_CURSOR_GRAB", Bool, false, "gameos", "", "SMOKE-NO-CURSOR-STEAL-1: suppress ALL OS-cursor manipulation -- window mouse-grab, Win32 ClipCursor clamp, and gos_SetMousePosition warps -- plus window...", registry(Trace, "MC2_TRACE_NO_CURSOR_GRAB", -1, false) | verified),
L(Presence, "MC2_NO_LAUNCHER", Bool, false, "gameos", "", "", passthrough | verified),
G("MC2_OBJECT_ADMISSION_PREDICATE", Enum, "", "code", "", "", passthrough | tokens("|legacy|modern") | verified),
// history: docs/gate-history.md#MC2_OBJECT_ID_BUFFER
L(DefOnLead0, "MC2_OBJECT_ID_BUFFER", Bool, true, "code", "", "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5).", passthrough | registry(Feature, "MC2_FEATURE_OBJECT_ID_BUFFER", 2, true) | verified),
P("MC2_OBJECT_POLY_OFFSET_FACTOR", Float, -1.0, "props", "", "", passthrough),
L(Lead0, "MC2_OBJECT_WALK_FACTS", Bool, false, "code", "", "", verified),
G("MC2_OOB_FOG_COLOR", String, "", "post", "", "", verified),
G("MC2_OPTIONS_OVERRIDE", String, "", "code", "", "Test channel: Key=Value,... options.cfg keys applied over the file at startup (smoke matrices, capture drivers); never saved.", passthrough | verified),
L(Presence, "MC2_OS_CURSOR", Bool, false, "unclassified", "", "Hide the OS cursor.", verified),
// RUNWAY-CLOSE-1: overlay-art flat-light exemption A/B arm (enum 63, LAST).
// OverlayCementAdopt (OVERLAY-CEMENT-ADOPT-1): now options.cfg StockOverlayConcrete
// history: docs/gate-history.md#MC2_OVERLAY_CEMENT_ADOPT
// CEMENT-PATCHY-CAUSE-1: the concrete patchwork is a TWO-SHADER SEAM, not
// an atlas-tone problem. Attribution receipt in .claude/CEMENT-PATCHY-CAUSE-1-STATUS.md.
// OVERLAY-TILE-HIRES-1: high-resolution terrain overlay tiles.
// history: docs/gate-history.md#MC2_OVERLAY_TILE_HIRES
G("MC2_OVERLAY_TILE_HIRES", String, "256", "terrain", "", "OVERLAY-TILE-HIRES-1: honor the overlay tile TGA's actual dimensions instead of the legacy hard-assumed 64px (initOverlay header resync mirrors...", passthrough | registry(Feature, "MC2_OVERLAY_TILE_HIRES", -1, true) | verified),
G("MC2_PLAYTEST_EXE", String, "", "editor", "", "", verified),
G("MC2_PLAY_MISSION", Bool, false, "code", "", "Dev-only scripted mission commander and objective verdict reporting.", passthrough | verified),
G("MC2_PLAY_MISSION_GOTO", String, "", "code", "", "Dev-only: order the home lance to world position x,y during a mission test.", passthrough | verified),
G("MC2_PLAY_MISSION_INVULNERABLE", Bool, false, "code", "", "Dev-only home-team damage immunity for mission tests.", passthrough | verified),
G("MC2_PLAY_MISSION_SECONDS", Int, 300, "code", "", "Dev-only mission test simulation timeout in seconds.", passthrough | verified),
G("MC2_PLAY_MISSION_SKIP_CINEMATICS", Bool, false, "code", "", "Dev-only mission test cinematic skip; direct mission harness skips briefing.", passthrough | verified),
L(Lead0P, "MC2_POSTPROCESS_COMPUTE_BLUR", Bool, false, "post", "", ""),
// POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening (GL composite).
G("MC2_PRESENTATION_DISABLED", Bool, false, "code", "", "Replay proof: suppress presentation extraction updates while simulation continues.", passthrough | registry(Trace, "MC2_PRESENTATION_DISABLED", -1, false) | verified),
P("MC2_PREVIEW_FBO_SCALE", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
// PPC / direct-fire projectile knobs (gameplay; launcher-toggleable)
// history: docs/gate-history.md#MC2_PROJECTILE_SPEED_MULT
P("MC2_PROJECTILE_SPEED_MULT", Float, 1.0, "code", "", "Direct-fire projectile speed multiplier (PPC/autocannon/gauss = non-arc bolts).", passthrough | rangeAbove(0.0, 10.0) | registry(Feature, "MC2_PROJECTILE_SPEED_MULT", -1, false) | verified),
G("MC2_PROJECTZ_BYPASS_MODE", Enum, "", "code", "", "", tokens("|Compare|Off|Bypass") | verified),
L(Presence, "MC2_PROJECTZ_TRACE", Bool, false, "code", "", "", passthrough | verified),
// PropBaseLightDir (PROP-BASE-LIGHT-DIR-1)
G("MC2_PROP_DEBUG_VIEW", Enum, "0", "render", "", "Prop material view: 0 off, 1 albedo, 2 world normal, 3 raw normal sample (BC5 RG), 4 roughness, 5 metallic, 6 AO, 7 material id hash.", passthrough | registry(Trace, "MC2_DIAG_PROP_DEBUG_VIEW", -1, false) | tokens("0|1|2|3|4|5|6|7") | verified),
L(DefOnAtoiNZ, "MC2_PROP_FIXB_MVP", Bool, true, "props", "", "", verified),  // default not derivable from the tree
// PropFoliageAlphaFlag (FOLIAGE-ALPHAFLAG-1)
// FOGLIGHT-LATCH-1: diagnostic probe for the one-shot per-actor light latch.
L(Presence, "MC2_PURCHASE_ALL", Bool, false, "code", "", "MC2_PURCHASE_ALL (default OFF; launcher Engine Option): unlock every COMPONENT/weapon so modded gear (e.g.", verified),
L(Lead0Kill, "MC2_QUADSETUP_ARMED_SKIP", Bool, true, "terrain", "", "", passthrough | verified),
P("MC2_RDC_CAPTURE_FRAME", Int, 0, "gameos", "", "", verified),
G("MC2_RDC_CAPTURE_PATH", String, "", "gameos", "", "Expand and VALIDATE the path template here, at arm time -- deliberately NOT down in resolveApiOnce().", verified),
L(Presence, "MC2_RDC_EXIT_AFTER", Bool, false, "gameos", "", "", verified),
P("MC2_RDC_MAX_WAIT", Int, 600, "gameos", "", "RenderDoc capture: frames to wait past the armed frame before giving up.", passthrough | verified),
G("MC2_RDC_MISSION", String, "", "gameos", "", "", verified),
L(Presence, "MC2_REBUILD_MOD_CACHE", Bool, false, "code", "", "", reread | verified),
L(Presence, "MC2_REGFLUSH_GUARD2", Bool, false, "props", "", "STATIC-REGISTRY-CURRENTNESS-GUARD-2: generalize the R2b stale_after_drawn counter beyond the single tree case.", passthrough | verified),
// VK-P13-SELECTOR-1 / T1: the top-level GL-vs-Vulkan backend selector.
// history: docs/gate-history.md#MC2_RENDER_BACKEND
G("MC2_RENDER_BACKEND", String, "", "unclassified", "", "Legacy selector, read only to report an ignored value: the engine renders on Vulkan only (the gl opt-out was removed).", passthrough | registry(Feature, "MC2_FEATURE_RENDER_BACKEND", -1, false) | verified),
L(Lead0, "MC2_RENDER_CONTRACT_ASSERT", Bool, false, "code", "", "", passthrough),
L(Lead1, "MC2_RENDER_PASS_CONTRACT_ASSERT", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_RENDER_PASS_ORDER
L(Lead0, "MC2_RENDER_PASS_ORDER", Bool, false, "code", "", "CONTRACT-3: per-frame resource-ordering audit (mclib/render_contract.cpp) - checks each pass's reads[] are satisfied by a prior write before beginPass.", registry(Trace, "MC2_FEATURE_RENDER_PASS_ORDER", -1, false) | verified),
L(Exact1, "MC2_RENDER_PASS_TIME", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_RENDER_PASS_TIME_EVERY", Int, 0, "gameos", "", "", passthrough | verified),
L(Presence, "MC2_RENDER_PATH", Bool, false, "props", "", "G5 RENDER-PATH OBSERVABILITY (descriptive; env-gated; ZERO behavior change): one line per registered static-prop type recording which render path it takes.", verified),
P("MC2_RENDER_SCALE", Float, 1.0, "post", "", "Scene resolution relative to output; spatial cubic upscale, native HUD. Restart to apply.", clampRange(0.5, 1.0) | passthrough | verified),
L(Presence, "MC2_RENDER_WATER_FASTPATH", Bool, false, "terrain", "", "", passthrough | verified),
P("MC2_RENDER_WATER_FASTPATH_DEBUG", Int, 0, "gameos", "", ""),
// history: docs/gate-history.md#MC2_RESOLVE_TRACE_FILE
G("MC2_REPLAY_CONTENT", String, "", "code", "", "Mission/mod content digest supplied and checked by tools/command_replay.py.", passthrough | registry(Trace, "MC2_REPLAY_CONTENT", -1, false) | verified),
G("MC2_RESOLVE_TRACE_FILE", String, "", "code", "", "META-FIX design E tier 3 (mclib/file.cpp): output path for the MC2_LOG_FILE_RESOLVE=2 JSONL resolve trace, opened for append at InitModSearchPaths (relative...", passthrough | registry(Trace, "MC2_TRACE_RESOLVE_TRACE_FILE", -1, false) | verified),
// META-FIX-BATCH-2 (recurrence-proofing, designs D + F — 2026-07-16)
// history: docs/gate-history.md#MC2_RES_ASSERT
G("MC2_RES_ASSERT", String, "", "code", "", "META-FIX design D runtime tier (RenderCore/ResolutionContract.h): audit mode, unset/0 = off, fatal = abort, anything else = log.", passthrough | registry(Trace, "MC2_TRACE_RES_ASSERT", -1, false) | verified),
L(Presence, "MC2_RES_DIAG", Bool, false, "post", "", "[RES_DIAG v1] One-shot dump of the resolution split: scene FBO size (this object's width_/height_) vs the HUD canvas (Environment.screenWidth) vs the native...", passthrough | verified),
L(Presence, "MC2_RING_TRACE", Bool, false, "terrain", "", "?? Probe 8: compare draw-time MVP fingerprint vs compute-time fingerprint.", passthrough),
G("MC2_RNG_SEED", String, "", "mech", "", "DETERMINISTIC-RNG-1: seed text (decimal, 0x hex, leading-0 octal; strtoul base 0 at the reader); empty = the fixed or mission-hash seed.", passthrough | verified),
// RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6): feather solid cement pad/runway edges
// that abut bare terrain on the indirect terrain path (gos_terrain.frag).
G("MC2_RUN_ID", String, "", "harness", "", "Harness-owned box wrapper metadata; ignored by the engine.", passthrough),
G("MC2_SAVE_DIR", String, "", "code", "", "Player-path harness only: directory used instead of ~/.mechcommander2/savegame for saves and quick saves; scripts/run_player_path.py always sets it.", passthrough | verified),
L(Lead0Kill, "MC2_SCRATCH", Bool, true, "code", "2026-12-31", "Mod VFS layer 0: index mods/<active>/.scratch/data/ above the active mod; 0 = kill-switch.", verified),
P("MC2_SCREENSHOT_AT_FRAME", Int, -1, "gameos", "", "", passthrough | verified),
G("MC2_SCREENSHOT_PATH", String, "", "gameos", "", "Oracle: deterministic backbuffer screenshot at a target frame.", passthrough | verified),
G("MC2_SCREENXY_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy|Modern") | verified),
G("MC2_SELECTION_PICKING_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy|Modern") | verified),
// SetupTexturesLegacyForce (TRACKV-CI-GATE-RESTORE-1)
// history: docs/gate-history.md#MC2_SETUPTEXTURES_LEGACY_FORCE
