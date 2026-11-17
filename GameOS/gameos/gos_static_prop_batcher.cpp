// GpuStaticPropBatcher, slice 1 of 9: gates and eager caches, counters, the SPFLUSH cost split and the pool/billboard knobs.
// The rest is gos_static_prop_batcher_{state,program,mapload,finalize,submit,flush,shadow,seam}.cpp, contiguous slices of the old file
// in file order (keep them in that order in the build); what they share is gos_static_prop_batcher_internal.h.


#include "gos_static_prop_batcher_internal.h"




extern const bool s_alphaTrace = false;
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

// MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1 — forces the legacy per-type-cap
// coalesce layout (keeps existing path; new global-pool path is disabled).
// Env-read once at process start; survives until soak completes.
extern const bool s_globalPoolLegacy = mc2::gate::get<"MC2_STATIC_PROP_GLOBAL_POOL_LEGACY">();

// v3 snapshot build: separate dispatch arrays for snapshot-built path.
// Reused each flush via resize() — no heap alloc after first frame.
std::vector<RenderCore::DrawPacket> s_snapV6Packets;
std::vector<StaticPropDispatchMeta> s_snapV6Meta;

// STATIC-PROP-SNAPSHOT-FINISH (v8): the snapshot is the sole draw-packet owner; the live builder only runs as the
// structural fallback for a frame whose snapshot is unusable.

// v3 per-flush counters. Reset each flush by the runV6 block.
// Read by batcher_getSnapshotBuildStats() for render_snapshot.cpp ok gate.
uint32_t s_spBuildAttempted      = 0u;
uint32_t s_spBuildCountMismatch  = 0u;
uint32_t s_spBuildPacketMismatch = 0u;
uint32_t s_spBuildMetaMismatch   = 0u;
uint32_t s_spBuildFallback       = 0u;

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
// This 0.5 is the pre-mission fallback only (shell and mech-bay frames, or a
// missing data/visual_tuning.json). data/visual_tuning.json overwrites it on
// every mission load (visual_tuning_profile.cpp "staticPropIblStrength"), so
// the per-mission strength is tuned there.
float g_iblShStrength = 0.5f;

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
// Startup value 0.2; external linkage so GuiRuntime.cpp can drive a slider.
//
// static_prop.vert sums base_light + the object-light ambient + the SH-IBL probe before the sun term, so an
// away-facing normal never falls near black. The fill is therefore an ADDITIVE post-texture term weighted by
// (1 - litMax) (see the c.rgb block in static_prop.frag): arithmetically live on every fragment the clamp has
// not already saturated.
float g_terrainDecalFill = 0.2f;

// TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-color match for the cliff mesh
// decal. Blends the flagged decal's albedo toward the terrain COLORMAP sampled
// at the decal's world-XY (same atlas + world->UV transform the terrain chunk
// shader uses). Applied ONLY to kFlagDecalFill (bit 3) fragments in the shader.
// Startup value 0.35 (subtle base-color pull toward terrain). External linkage
// so GuiRuntime.cpp can drive the "Color blend" slider.
float g_terrainDecalColorBlend = 0.35f;

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel specular slider value
// (interactive; EditorInspector.cpp drives it, range 0..3). Full-strength
// specular was too blunt on flat-roofed legacy assets without material
// specular masking, so the startup value is 0.5.
float g_pbrV1Strength = 0.5f;

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
