// GpuStaticPropBatcher, slice 1 of 9: gates and eager caches, counters, the SPFLUSH cost split and the pool/billboard knobs.
// The rest is gos_static_prop_batcher_{state,program,mapload,finalize,submit,flush,shadow,seam}.cpp, contiguous slices of the old file
// in file order (keep them in that order in the build); what they share is gos_static_prop_batcher_internal.h.


#include "gos_static_prop_batcher_internal.h"



// Global runtime toggle for the GPU static-prop renderer. Defined here
// (in the gameos lib) so every consumer — mc2.exe, aseconv, other data
// tools that link mclib — resolves the symbol.
bool g_useGpuStaticProps = false;
// Default-on as of 2026-05-04. Flipped without Stage 2.E visual-diff gate
// passing — Stage 2.D parity check (29 missions, 0 mismatches, +0 destroys)
// establishes numerical correctness; visual confirmation is via prior
// power-user testing with MC2_GPU_OBJECTS=1. Set MC2_GPU_OBJECTS=0 in
// env to opt out.
bool g_useGpuObjects = true;

extern const bool s_alphaTrace = mc2::gate::get<"MC2_ALPHA_TEST_TRACE">();
// NOTE: raw gos_terrain_indirect_getDispatchMvp16() is phase-private to terrain/
// water passes — object draw uses gos_GetObjectDrawMVP() (the accessor above).
// Enforced by scripts/check-object-mvp-currency.py.

// MC2_PROP_FIXB_MVP — default ON (=0 reverts, killswitch). Consume the
// dispatch-MVP snapshot when the solid pass is armed so object draws agree with
// terrain depth during camera motion (the un-armed arm returns the live pointer
// anyway, so this is byte-identical when the solid pass is not armed). Sampled
// once at process start.
static bool s_propFixBMvpEnabled() {
    static const bool on = mc2::gate::get<"MC2_PROP_FIXB_MVP">();
    return on;
}
// Returns the MVP the object draw should upload (GL_FALSE, axisSwap*worldToClip).
// Gate OFF -> live terrain MVP. Gate ON + solid pass armed -> dispatch snapshot,
// with live-MVP nullptr-safety identical to the terrain chunk fallback.
// RENDER-VIEW-CURRENCY-1: snapshot only when its view epoch matches the current
// view; otherwise the live MVP. Closes the BUG1/3/5 staleness (objects projected
// through a stale / mid-frame-superseded camera). Killswitch preserved via the arg.
const float* gos_GetObjectFixBMVPMat4() {
    return gos_GetObjectDrawMVP(s_propFixBMvpEnabled());
}

// MC2_TREE_DIAG_TRACE=1 — diagnostic 2026-05-05 for the "black billboard square" bug.
// Prints up to 8 lines from each of submit() (dynamic register/replay path) and
// submitCachedInstance() (static recipe replay path), with all GpuStaticPropInstance
// fields formatted identically so a side-by-side grep diff pinpoints which baked
// field is stale on the static replay. Counters are function-local statics so they
// reset per process lifetime; format matches both call sites verbatim.
extern const bool s_treeDiagTrace = mc2::gate::get<"MC2_TREE_DIAG_TRACE">();

// MC2_SPOTLIGHT_REAL_TRACE=1 — (E) SpotLight_ → real illumination baseline counter.
// Stage 0 instrumentation (T0.1, plan 2026-05-20). Counts per-frame static-prop
// submitMultiShape events where the child has isSpotlight==true (i.e. the per-instance
// shape whose source typeShape carried the "SpotLight_*" node-name prefix and that
// the CPU path tags via tgl.cpp:259/475). First-hit is always-on (one-line stderr
// confirms a SpotLight_ child was observed at least once even without the env);
// per-frame and 600-frame summary streams are env-gated to keep the log quiet by
// default. Pattern mirrors the [INSTR v1] schema family + the 600-frame window
// used by [OBJBATCHER v1] / [TGL_POOL v1].
extern const bool s_spotlightRealTrace = mc2::gate::get<"MC2_SPOTLIGHT_REAL_TRACE">();

// SP-BATCHER-ALPHASCAN-GATE-1: see header. Starts at 1 so a fresh scan (cached
// gen 0) always runs its first pass regardless of any early destruction events.
std::atomic<uint64_t> g_staticPropAlphaGeneration{1};

extern const bool s_spflushCostSplitEnabled = mc2::gate::get<"MC2_STATIC_PROP_FLUSH_COST_SPLIT">();

namespace gos_static_prop_batcher_detail {
unsigned long long s_spflush_submit_map_lookup_cyc = 0;
unsigned long long s_spflush_color_zero_fill_cyc   = 0;
}  // namespace gos_static_prop_batcher_detail

namespace spflush_cost_split {
void AddSubmitMapLookupCycles(unsigned long long c) {
    if (s_spflushCostSplitEnabled) s_spflush_submit_map_lookup_cyc += c;
}
void AddColorZeroFillCycles(unsigned long long c) {
    if (s_spflushCostSplitEnabled) s_spflush_color_zero_fill_cyc += c;
}
unsigned long long ConsumeSubmitMapLookupCycles() {
    const unsigned long long v = s_spflush_submit_map_lookup_cyc;
    s_spflush_submit_map_lookup_cyc = 0;
    return v;
}
unsigned long long ConsumeColorZeroFillCycles() {
    const unsigned long long v = s_spflush_color_zero_fill_cyc;
    s_spflush_color_zero_fill_cyc = 0;
    return v;
}
}  // namespace spflush_cost_split

// MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1 — forces the legacy per-type-cap
// coalesce layout (keeps existing path; new global-pool path is disabled).
// Env-read once at process start; survives until soak completes.
extern const bool s_globalPoolLegacy = mc2::gate::get<"MC2_STATIC_PROP_GLOBAL_POOL_LEGACY">();

// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03) — when set, forces the LEGACY
// per-instance frame-pool path (_PositionsOnly + non-null listOfVertices gate)
// for GPU-registered static-prop types. Default (unset) is the new GPU-only
// path: registered instances run the zero-pool hierarchy walk in *Appearance::
// update and submit straight from rec.shapeToWorld, so the TGL frame pools stay
// flat regardless of override mesh size × instance count. This flag is the A/B
// escape hatch. Env-read once at process start.
extern const bool s_legacyInstancePools = mc2::gate::get<"MC2_LEGACY_INSTANCE_POOLS">();
bool gos_StaticPropLegacyInstancePools() { return s_legacyInstancePools; }

// DrawPacket v5: per-draw-call substitutive dispatch.
// Gate: MC2_DRAW_PACKET_COALESCE_V5=1
// Extension: ARB_base_instance (GL 4.2 core, available on all tier1 GPUs).
extern const bool s_v5Enabled = mc2::gate::get<"MC2_DRAW_PACKET_COALESCE_V5">();
extern const bool s_v5TraceEnabled = mc2::gate::get<"MC2_DRAW_PACKET_COALESCE_V5_TRACE">();
// GLboolean: GL_TRUE on any GL 4.2+ context. Evaluated lazily via GLEW at first use.
// Safe: GLEW is initialized before any batcher code runs (gos_Init() order).
bool s_baseInstanceSupported = false;  // set on first gate-ON flush
bool s_v5Armed      = false;  // true once gate-arm checks have run
bool s_v5Disarmed   = false;  // true if gate-arm check failed for session

// DrawPacket v7: canonical packet+meta array dispatch — DEFAULT ON.
// Kill-switch: MC2_STATIC_PROP_LEGACY_DISPATCH=1 reverts to legacy multidraw.
// Requires: ARB_base_instance (checked independently of v5 arm block).
extern const bool s_v6Enabled = !mc2::gate::get<"MC2_STATIC_PROP_LEGACY_DISPATCH">();
extern const bool s_v6TraceEnabled = mc2::gate::get<"MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE">();
bool s_v6Armed    = false;
bool s_v6Disarmed = false;

// v3 snapshot build: separate dispatch arrays for snapshot-built path.
// Reused each flush via resize() — no heap alloc after first frame.
std::vector<RenderCore::DrawPacket> s_snapV6Packets;
std::vector<StaticPropDispatchMeta> s_snapV6Meta;

// v3 env gate. File-local — not exported in header. Cached at process start.
// DEFAULT ON (STATIC-PROP-V3-FLIP, 2026-05-27). Kill-switch: MC2_SNAPSHOT_STATIC_PROP_BUILD=0.
// Unset or any value other than literal '0' = ON. Live builder + compare authority retained as fallback.
extern const bool s_snapshotBuildEnabled = mc2::gate::get<"MC2_SNAPSHOT_STATIC_PROP_BUILD">();
// STATIC-PROP-SNAPSHOT-FINISH (v8): snapshot is the sole draw-packet owner.
// DEBUG/LEGACY kill-switch — default OFF (retired). =1 restores the v3-flip dual
// build (live + snapshot_packet_build) + compare path for regression bisect / A-B.
extern const bool s_keepLiveBuilder = mc2::gate::get<"MC2_STATIC_PROP_LIVE_BUILDER">();
// One-shot arm log latch (the arm log itself is emitted in a LATER task, when retirement is real).
bool s_v8ArmLogged = false;

// v3 per-flush counters. Reset each flush by the runV6 block.
// Read by batcher_getSnapshotBuildStats() for render_snapshot.cpp ok gate.
uint32_t s_spBuildAttempted      = 0u;
uint32_t s_spBuildCountMismatch  = 0u;
uint32_t s_spBuildPacketMismatch = 0u;
uint32_t s_spBuildMetaMismatch   = 0u;
uint32_t s_spBuildFallback       = 0u;
uint32_t s_spBuildRetired        = 0u;  // v8: 1 when live builder + compare retired this flush
// Latched on first fallback; never reset. Guards the first-occurrence log line.
bool s_spBuildFirstFallbackLogged = false;
// Latched on first snap-cull collision; never reset. Guards the one-shot collision log line.
bool s_spBuildCollisionLoggedOnce = false;

// Returns 0 (opaque) or 1 (alpha-test). Returns 0xFFFFFFFFu for unknown pipelineId.
// Used by snapshot builder to derive group from the snapshot row's stored pipelineId.
uint32_t pipelineId_to_group(uint32_t pid) {
    using P = RenderCore::PipelineId;
    if (pid == static_cast<uint32_t>(P::StaticPropOpaque))    return 0u;
    if (pid == static_cast<uint32_t>(P::StaticPropAlphaTest)) return 1u;
    return 0xFFFFFFFFu;
}

// v6 per-frame counters (reset each v6-active flush).
uint32_t s_v6FrameDrawsIssued        = 0u;
uint32_t s_v6FrameZeroInstSkips      = 0u;
uint32_t s_v6FrameSortedOob          = 0u;
uint32_t s_v6FramePacketOob          = 0u;
uint32_t s_v6FrameTypeOob            = 0u;
uint32_t s_v6FrameLockstepViolations = 0u;

// v2.3 snap-cull: per-frame counters. Reset at top of flush(); read via batcher_getSnapCullStats().
uint32_t s_snapCullSkipped      = 0u;
uint32_t s_snapCullActive       = 0u;
uint32_t s_snapCullSlotMismatch = 0u;
// Env-var gate: strictly MC2_SNAP_CULL=1. Unset and MC2_SNAP_CULL=0 both leave snap-cull OFF.
extern const bool s_snapCullEnabled = mc2::gate::get<"MC2_SNAP_CULL">();
uint32_t s_v6FrameGlErrors           = 0u;
uint32_t s_v6TotalFrameCount         = 0u;

// v5 per-frame counters (reset at start of each v5-active flush).
uint32_t s_v5FrameDrawsIssued     = 0u;
uint32_t s_v5FrameZeroInstSkips   = 0u;
uint32_t s_v5FrameSortedOob       = 0u;
uint32_t s_v5FramePacketOob       = 0u;
uint32_t s_v5FrameTypeOob         = 0u;
uint32_t s_v5FrameBaseInstMissing = 0u;
uint32_t s_v5FrameGlErrors        = 0u;
uint32_t s_v5TotalFrameCount      = 0u;

// [GPUPROPS v1] setup-path attribution for registerType calls.
// 0=unknown 1=mission_init_firstload 2=ims_objmgr_load 3=sp_logistics_postinit
// Declared extern in gos_static_prop_batcher.h; set by mission.cpp / saveload.cpp / logistics.cpp.
int g_lightProbeSetupPath = 0;

// V-IBL-STATIC-1: ImGui slider value (extern decl in ibl_sh_runtime.h).
// Default 1.0 -> nominal projector output strength when env-gate is on.
// Defined at file scope (NOT in anonymous namespace) for external linkage
// so GuiRuntime/EditorInspector.cpp can drive the slider.
//
// V-IBL-STATIC-1-SOAK: optional MC2_STATIC_PROP_IBL_SH_STRENGTH env override
// for the default. ImGui slider remains the interactive tuning surface; the
// env exists so headless capture harnesses can drive strength deterministically
// from a parent process. Parsed once at process start, clamped 0.0..3.0.
// Env unset/empty -> 0.5f.
//
// ★ 0.5f IS A PRE-MISSION FALLBACK ONLY. NO SHIPPED MISSION EVER RENDERS AT
// IT. (Correction landed by INCAPABLE-BATCH-1, 2026-07-31; the text here used
// to read "default 0.5f (V-IBL-STATIC-1-TUNE: user eyeball pass at e061e44c
// selected 0.5 as 'looks good')", which is a live falsehood in every sense
// that matters.) data/visual_tuning.json OVERWRITES this global on EVERY
// mission load -- visual_tuning_profile.cpp applyKey("staticPropIblStrength")
// -> visualTuning_applyProfile(), called unconditionally from
// code/mission.cpp:3874, and the file ships (scripts/deploy_payload.py:118,
// verified present in the 2026-07-30 install). The values that actually
// render are:
//     defaults           0.55
//     mc2_01/10/24       0.60
//     mc2_03             0.55 (explicit)
//     gaea_peaks_01      0.65
// The ONLY way 0.5f survives into a frame is (a) frames drawn before the
// first mission load (shell/mechbay -- gos_mech_batcher.cpp shares this same
// global), or (b) data/visual_tuning.json missing/unreadable.
//
// CONSEQUENCES, both of which have already cost real work:
//   - Any eyeball tuning pass recorded "at 0.5" was graded on a value the
//     game does not use. If you tune this feature, edit the JSON.
//   - Setting MC2_STATIC_PROP_IBL_SH_STRENGTH does not merely override this
//     line, it DISABLES the whole per-mission JSON override
//     (visual_tuning_profile.cpp guards the apply with envIsSet()), so an
//     env-driven A/B is measuring a different mission profile than a default
//     launch. docs/tier1_env_vars.md carries this warning too.
// Left at 0.5f on purpose: changing it would move shell/mechbay frames (a
// user-visible default-launch delta) while changing nothing in-mission.
// If you change it, change data/visual_tuning.json "defaults" in the same
// commit so the two cannot disagree again.
// KNOWN STILL-DIVERGENT (not fixed here -- the file is held by another lane):
// tools/class_light_band_harness/class_light_band_harness.cpp hardcodes
// kIblStrengthDefault = 0.5 and asserts it, so every band the harness models
// is computed at a strength no mission uses.
float g_iblShStrength = mc2::gate::get<"MC2_STATIC_PROP_IBL_SH_STRENGTH">();

// LIGHT-BILLBOARD-GPU-1 (master-FAIL #8): beacon/runway light fixtures
// (TG_Shape::isLightBillboard, node-name substring "beaconlight") pop in and
// out with the camera angle across the whole mc2_10 airbase.
//
// CORRECTED DIAGNOSIS. The predecessor lane (EDGE-LIGHT-BILLBOARD-CULL-1,
// 9f8c8086) described these as flat quads that get backface-culled, and made
// the legacy CPU TG_Shape::Render loop two-sided. Both halves are wrong:
// (a) the props render through THIS batcher, not that loop — hence the
// measured near-null efficacy; and (b) reading data/tgl/runwaylight.ase, they
// are not quads at all but 6-vertex / 7-face truncated triangular prisms
// ~2.85 units tall, whose only emissive element is the small HORIZONTAL
// top-cap triangle.
//
// The real mechanism, reproduced stock from a fixed camera POSITION with only
// the pitch varied (bookmarks 204753 vs 204907): the cap's projected area
// falls off with view angle, and the whole fixture already subtends only 1-2
// pixels at RTS ranges, so at grazing angles the cap drops below one pixel and
// is never rasterised — every runway light vanishes at once. This is sub-pixel
// geometry, not backface culling, so the deferred "two-sided draw bucket"
// would NOT have fixed it either.
//
// The cure is in static_prop.vert (per-instance flag bit 4): a RIGID
// camera-facing billboard (local coords re-emitted in an eye-facing basis, so
// the cap always faces the viewer and can never be back-culled) plus a
// MINIMUM SCREEN SIZE clamp so the fixture never shrinks below a pixel. No
// two-sided draw bucket and no glDisable(GL_CULL_FACE) bracket is needed in
// the hot MDI-coalesced per-packet draw loop.
//
// Gate MC2_LIGHT_BILLBOARD_FACING — killswitch, default **OFF**. "0" (and
// unset) => the flag bit is never emitted => shader block never runs =>
// byte-identical stock. "1" => the modern marker-light treatment. "2" =>
// verify-by-exaggeration (bit 5 as well: fixture scaled 4x in the shader).
// Companion CPU-path gate MC2_LIGHT_BILLBOARD_TWOSIDED (tgl.cpp) is unchanged.
//
// RUNWAY-MARKER-LIGHTS-1: this shipped default-ON in f412423e and the user
// verdict on the result was "atrociously hideous". The shader's min-screen-
// size clamp was uncapped, so every runway/taxiway fixture held a constant
// ~4-7px hard-edged bright triangle at ALL camera distances; at map-overview
// altitude the whole regularly-spaced population read as white dashed slashes
// scattered over the dark valley. The clamp is now capped (static_prop.vert,
// kLbMaxScale) so fixtures shrink normally with distance again, AND the gate
// is default-OFF so stock behaviour is what ships unless a user opts in.
// Setting MC2_LIGHT_BILLBOARD_FACING=0, or simply leaving it unset, disables
// the modern marker-light treatment entirely.
int gos_lightBillboardFacingMode() {
    static const int s_mode = []() -> int {
        const std::string v = mc2::gate::str<"MC2_LIGHT_BILLBOARD_FACING">();   // "" when unset or empty
        if (v.empty()) return 0;          // default OFF (stock)
        if (v[0] == '0') return 0;        // killswitch
        if (v[0] == '2') return 2;        // exaggerate (verification)
        return 1;                         // opt-in modern treatment
    }();
    return s_mode;
}

// TERRAIN-DECAL-FILL-1: ambient/fill term for the cliff-wall mesh-decal's
// shadow side. Applied ONLY to fragments whose instance carries the
// kFlagDecalFill bit (bit 3), set on the MarbleCliff decal instances at
// registration (bdactor.cpp). All other static props (mechs/buildings/trees)
// never carry that bit, so they are byte-identical regardless of this value.
// MC2_TERRAIN_DECAL_FILL overrides (clamped 0..1). External linkage so
// GuiRuntime.cpp can drive a slider.
//
// ★ TWO CORRECTIONS (INCAPABLE-BATCH-1, 2026-07-31). The text removed from
// here claimed "static_prop lighting is max(N·L,0) with no ambient floor, so a
// face pointing away from the sun goes to ~0 (black void)". THAT PREMISE IS
// FALSE AT HEAD, and it is why the v1 implementation of this feature shipped
// dead:
//   1. There IS an ambient floor, and it is large. static_prop.vert sums
//      base_light + object-light ambientTerm + the DEFAULT-ON SH-IBL probe
//      before the sun term. The probe alone returns irradiance 0.232..4.896
//      over the sphere and is multiplied by staticPropIblStrength (0.55-0.65
//      from data/visual_tuning.json), so an away-facing normal lands around
//      0.13 at the absolute worst and typically far higher -- never a black
//      void. Because litRgb therefore essentially never fell below 0.20, the
//      v1 form `max(litRgb, u_terrainDecalFill)` returned litRgb every time
//      and was a visual no-op (LIGHTING-HEADROOM-AUDIT.md section 3 #4).
//      7de56d52 fixed it by moving to a post-texture ADDITIVE weighted by
//      (1 - litMax); that form is arithmetically live because litMax < 1 on
//      any fragment the clamp has not already saturated. See the note at
//      shaders/static_prop.frag next to the c.rgb block.
//   2. "Default 0.20" reads as default-ON and is NOT. Bit 3 is only ever set
//      inside `if (useCliffWallFrame)` (mclib/bdactor.cpp), which requires
//      s_terrainDecal == true -- and that gate is an opt-in:
//      MC2_TERRAIN_DECAL is default OFF (reader mclib/bdactor.cpp:4827).
//      On a default launch ZERO fragments carry the flag, so this 0.20
//      reaches nothing; it is the fill strength for the opted-in case.
//      (Phrasing note, 2026-07-31: this sentence used to end "...not a
//      shipped default-on value", which check-gate-default-agreement.py
//      read as an ASSERTION that the gate defaults ON and reported as a
//      contradiction -- the opposite of what the line said. The checker's
//      negation blind spot is fixed there (_NEG_RE); the line is also
//      restated positively here so the claim is machine-CHECKED and agrees,
//      rather than merely skipped.)
float g_terrainDecalFill = mc2::gate::get<"MC2_TERRAIN_DECAL_FILL">();

// TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-color match for the cliff mesh
// decal. Blends the flagged decal's albedo toward the terrain COLORMAP sampled
// at the decal's world-XY (same atlas + world->UV transform the terrain chunk
// shader uses). Applied ONLY to kFlagDecalFill (bit 3) fragments in the shader;
// 0.0 (default when MC2_TERRAIN_DECAL_COLORBLEND unset) OR flag-unset OR
// blend==0 -> byte-identical for every other static prop AND the decal itself.
// Default 0.35 (subtle base-color pull toward terrain). Env override clamped
// 0..1. External linkage so GuiRuntime.cpp can drive the "Color blend" slider.
float g_terrainDecalColorBlend = mc2::gate::get<"MC2_TERRAIN_DECAL_COLORBLEND">();

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel specular slider value
// (interactive). Default 1.0f. Only contributes when the env-gate
// s_pbrV1Enabled is ON (and MC2_VIEW_UNIFORMS is not disabled). Optional env
// override MC2_STATIC_PROP_PBR_V1_STRENGTH wins over the default; clamped
// 0..3 to match the slider range. External linkage so EditorInspector.cpp
// can drive the slider (same pattern as g_iblShStrength).
// 2026-05-28: default dialled back from 1.0 → 0.5 per user direction.
// Full-strength Schlick specular was too blunt on flat-roofed legacy
// assets without material specular masking; 0.5 is the new "looks
// sensible without bespoke per-material tuning" baseline. Env override
// still wins (clamped 0..3).
float g_pbrV1Strength = mc2::gate::get<"MC2_STATIC_PROP_PBR_V1_STRENGTH">();

// V-MATERIAL-PBR-2-TUNE-UI: ImGui-driven runtime roughness override layer
// on top of the static_prop.vert 0.6 literal baseline. Default DISABLED;
// per-frame upload sends -1.0 sentinel which the shader treats as "use
// literal" -> byte-identical to V-MATERIAL-PBR-2-TUNE. When the inspector
// flips _Enabled, the slider value (0.05..1.0) is uploaded and overrides
// the literal. Debug/tuning surface only -- no MaterialGpu read, no
// batcher default change, no schema change.
// 2026-05-28: defaults flipped to enabled=true / value=0.95 per user
// direction. The 0.6 literal in static_prop.vert produced too-glossy
// highlights on legacy assets; the runtime override at 0.95 makes
// surfaces visibly rougher (closer to the look the assets were authored
// for) without touching the shader. Override stays env-disable-able via
// the Static Prop Tuning ImGui checkbox.
bool  g_pbrV1RoughnessOverrideEnabled = true;
float g_pbrV1RoughnessOverrideValue   = 0.95f;
