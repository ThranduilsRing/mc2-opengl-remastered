// GpuStaticPropBatcher: the shared GPU buffer, ring, program-location, coalesce and static-draw state, the IBL helpers and the material and base-instance bookkeeping.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"


namespace gos_static_prop_batcher_detail {

// Slice 1 (global instance pool). Default global cap and base-instance SSBO binding.
constexpr uint32_t STATIC_PROP_GLOBAL_CAP_DEFAULT = 131072u;

// Immutable per-map geometry.
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
// VK-P8a SHADOW-INTEROP: byte sizes of the shared immutable VBO/IBO, captured at
// finalize BEFORE the CPU staging vectors are freed (line ~2532). The Vulkan shadow
// island reads these + the GL handles to copy the caster geometry ONCE per map
// (the CPU staging is gone by then). 0 until finalized / after unmap.
uint64_t s_sharedVboBytes = 0;
uint64_t s_sharedIboBytes = 0;
GLuint s_sharedVao = 0;

// Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO.
// Holds {hotPinkRGB, hotYellowRGB, hotGreenRGB} per registered TG_TypeShape,
// indexed by inst.typeID at draw time. Built once at finalizeGeometry from
// TG_TypeShape::hotPinkRGB/hotYellowRGB/hotGreenRGB (mclib/tgl.h:575-577) and
// uploaded as immutable storage. Consumed by static_prop.vert via std430
// binding 2 to feed get_base_light()'s per-type magic-color parameters.
// 48 bytes per type × ~50 types ≈ 2.4 KB total.
GLuint s_perTypeSsbo = 0;
// SHADOW-STATIC-BUILDINGS-2: one-shot all-buildings static shadow instance SSBO
// (declared here so onMapUnload can free it; built in drawStaticBuildingShadows).
GLuint s_staticBldgShadowSsbo = 0;
// SHADOW-DYNAMIC-PROP-CASTERS-1: one-shot all-non-building-props dynamic shadow
// instance SSBO (declared here so onMapUnload can free it; built per-frame in
// drawDynamicPropShadows). Separate from the static building SSBO so the two
// passes never alias.
// SCENE-SSBO-OWNER-SWEEP-1: this is the ONE live default-path buffer in COMPLETENESS-
// SWEEP gap B#5 -- the dynamic prop-shadow caster path is gated MC2_SHADOW_DYNAMIC_PROP_
// CASTERS (DEFAULT ON) inside the always-on dynamic shadow pass, so it is created on the
// stock run. Narrowed behind a GpuBufferOwner identity record; every gen/bind/bufferData/
// bindbase/delete site reaches the raw handle ONLY via s_dynamicPropShadowSsbo (the
// #define below aliases the bare name to a READ-ONLY GLuint view of owner.backendHandle
// -> zero site churn; writes go through the owner field via a GLuint local, L6.1). Same
// GL args/slot/flags/order. Mission lifetime: rebuilt per frame but freed on onMapUnload;
// registered at create (bufferData), invalidated on destroy.
// The sibling buffers in gap B#5 are DEFERRED to the exclusion ledger (created only under
// default-OFF gates): s_staticBldgShadowSsbo (MC2_STATIC_PROP_BUILDING_SHADOW, default
// OFF); s_blockVisSsbo (gos_vegetation.cpp, MC2_VEGETATION_CARDS, default OFF); particle
// s_ssbo (gos_particle_bridge.cpp, MC2_GPU_PARTICLES, default OFF -- CPU/MLR is the stock
// path) + s_tubePos/Col/UvSsbo (MC2_VFX_ORACLE_TUBE, default OFF/parked).
RenderCore::GpuBufferOwner s_dynamicPropShadowOwner{
    RenderCore::RenderResourceId::DynamicPropShadowSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "DynamicPropShadowSsbo",
    0u};
// typeIDs drawn into the static building shadow map. When the static building
// shadow is active, flushShadow(skipStaticBuildingTypes=true) skips these in the
// DYNAMIC pass so buildings don't cast a redundant (fuzzy) second shadow.
std::unordered_set<uint32_t> s_buildingShadowTypeIDs;

GLuint   s_instanceSsbo = 0;
GLuint   s_colorSsbo    = 0;
// VK-SHADOW-RING-OWNER-1: parallel GpuBufferOwner identity records for the two ring
// SSBOs. The raw GLuint globals above are UNCHANGED so every gen/bind/map/delete site
// keeps its exact GL args/order; the owner's backendHandle is written from the freshly
// gen'd handle (pattern (b), mirrors view_uniforms). Registered at create, invalidated
// on destroy (grow-realloc). Lifetime Persistent: persistent-mapped ring, realloc-on-grow.
RenderCore::GpuBufferOwner s_instanceSsboOwner{
    RenderCore::RenderResourceId::StaticPropInstanceRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "StaticPropInstanceRingSsbo",
    0u};
RenderCore::GpuBufferOwner s_colorSsboOwner{
    RenderCore::RenderResourceId::StaticPropColorRingSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "StaticPropColorRingSsbo",
    0u};
void*    s_instanceMap  = nullptr;
void*    s_colorMap     = nullptr;
GLsync   s_fence[RING_FRAMES] = {0};
uint32_t s_frameSlot = 0;
size_t   s_instanceCapacity = 0;
size_t   s_colorCapacity    = 0;

// Per-slot byte usage of the parity SSBO. Tracks how much of slot
// kParitySlotBytes was actually written in the most recent visit to that
// slot, so the next-visit readback at the top of flush() can scope its
// glGetBufferSubData to exactly the live range. Indexed by s_frameSlot.
size_t   s_parityBytesUsedThisFrame = 0;
size_t   s_parityBytesUsedPerSlot[RING_FRAMES] = {0};
// Discard buffer for the readback. Sized once on first parity-on visit;
// std::vector keeps it alive for the lifetime of the process so we don't
// reallocate per frame. Bytes are read into this and DISCARDED — Stage
// 2.D.1 does no compare; 2.D.2/2.D.3 will use these bytes against a CPU
// recompute for the byte-wise compare.
std::vector<uint8_t> s_parityReadbackScratch;
std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// STATICPROP-COLORS-FILL-DEBUGONLY-1: the per-instance Colors SSBO (binding 1) is
// read by NO production shader — static_prop.vert declares colors_ but never uses
// it, firstColorOffset is unused, and the coalesce multidraw never binds binding 1
// (only the legacy non-coalesce draw does, gos_static_prop_batcher.cpp ~:6256).
// So the per-static-instance colors zero-fill (~80ns/leaf at wolfman) is pure
// waste. DEFAULT-SKIP for static-registry instances; MC2_STATIC_PROP_COLORS_FILL=1
// restores the legacy fill (kill-switch). Dynamic submit()/submitMultiShape path
// is untouched.
extern const bool s_staticPropColorsFill = mc2::gate::get<"MC2_STATIC_PROP_COLORS_FILL">();

extern const bool s_bucketOrderTrace = mc2::gate::get<"MC2_BUCKET_ORDER_TRACE">();

// 2b Stage 2 (Mechanism B-reinject): persistent per-type static instance store,
// rebuilt only when the registry generation changes. Each frame the registry
// flush BULK-injects these blocks into s_bucketsByType (one memcpy per type)
// instead of re-walking the registry per leaf — killing the per-leaf walk/push/
// map overhead. Downstream (uploadAllBucketsIfNeeded + coalesce pool build) read
// s_bucketsByType unchanged. Static blocks carry 0 colors (Stage 1).
std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
uint64_t s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // sentinel = never built

// Stage 3.C: per-submitMultiShape batch accumulator. Cleared at the start
// of each submitMultiShape(); populated by submit() per leaf. After
// submitMultiShape() returns true, getLastBuiltBatch() returns this vector
// for snapshot registration in GpuStaticPropRegistry.
std::vector<GpuStaticPropInstance> s_lastBuiltBatch;

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;
std::vector<RenderCore::StaticPropTypeDesc>        s_typeDescTable; // v0: cross-seam mirror

// [GPUPROPS v1] setup-path instrumentation (internal; path tag lives at file scope).
bool s_gpuPropsTrace = mc2::gate::get<"MC2_GPUPROPS_TRACE">();
// g_regCall counts EVERY registerType invocation after the null guard and
// BEFORE the idempotent s_typeIndex.count early-return, so 0 for a path means
// registerType was genuinely never invoked on it.
unsigned g_regCall[4]     = {0,0,0,0};
unsigned g_regLateDrop[4] = {0,0,0,0};

// Peak instance count per typeID since mission load; resized lazily in the
// per-frame flush loop. Slice-1: tracked but not consumed (foundation for
// slice-2 GPU-emit, where peak[t] drives slot allocation).
std::vector<uint32_t> s_perTypePeak;

// Coalesce ring frame slot — separate from legacy s_frameSlot (line ~:95).
// Advanced inside batcher_prepareBaseInstanceTable() each frame under
// non-legacy mode; mirrors s_frameSlot under legacy mode. Reset per-mission
// in onMapLoad() for hygiene.
uint32_t s_coalesceFrameSlot = 0;


std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Main static-prop program (Task 9). Lazy-loaded on first flush()/flushShadow().
// We keep a glsl_program* around for any future uniform-introspection needs,
// but most call sites will read the raw GL handle via s_staticPropProgram.
glsl_program* s_staticPropProgramObj = nullptr;
GLuint        s_staticPropProgram    = 0;

// Stage 2.D.1.1 (Item 2): parity uniform locations cached at link time.
// Querying glGetUniformLocation every flush was a per-flush driver round-trip;
// cache once here. The shader is not hot-reloadable (s_programLoadTried latches
// after first attempt, no in-process relink path), so the cache is valid for
// the lifetime of the program handle. -1 signals "not found in shader" and is
// safe to pass to glUniform*i (the spec says glUniform* is a no-op for loc=-1).
GLint s_loc_u_parityWrite        = -1;
GLint s_loc_u_parityVertsPerType = -1;
GLint s_loc_u_parityBaseVertex   = -1;

// Latched once a compile/link attempt has failed. We never retry inside a
// session because shader source can only change between runs. With this
// latched true, submit() returns false (so callers CPU-fallback), and
// flush()/flushShadow() short-circuit immediately. The user can keep the
// killswitch ON or OFF with no behavioral difference until the next build.
bool s_programLoadTried  = false;
bool s_programLoadFailed = false;

GLuint s_coalesceInstanceSsbo      = 0;  // ring-buffered, persistent-mapped
GLuint s_perDrawSsbo               = 0;  // PerDrawEntry per type, sorted (binding 4)

// MaterialGpu-2 sidecar — active by default (v5: default-ON).
// No shader consumer until MaterialGpu-3.
// s_packetMaterialIdx[i] maps draw slot i (= s_sortedPacketOrder[i] position)
// to its entry in s_materialGpuTable.
// Size invariant: s_packetMaterialIdx.size() == s_sortedPacketOrder.size().

// MC2_MATERIAL_GPU — defaults ON.
// Set MC2_MATERIAL_GPU=0 to disable table upload, SSBO bind, and compare.
// Upload gate: active by default; powers compare invariant (albedoTex == texArrayLayer).
extern const bool s_materialGpuEnabled = mc2::gate::get<"MC2_MATERIAL_GPU">();

// MC2_MATERIAL_GPU_SAMPLE — defaults ON (shader sampling active by default).
// Set MC2_MATERIAL_GPU_SAMPLE=0 to disable shader sampling and fall back to texArrayLayer.
// sampleOn requires s_materialGpuEnabled (MC2_MATERIAL_GPU) also active — see sampleOn gate.
// Invariant: materials[materialIdx].albedoTex == texArrayLayer must hold while both paths live.
extern const bool s_materialGpuSampleEnabled = mc2::gate::get<"MC2_MATERIAL_GPU_SAMPLE">();

// V-AMBIENT-STATIC-1: hemisphere ambient fill. Default-OFF; only ON when
// MC2_STATIC_PROP_AMBIENT_V1 is set to a non-"0" value. When OFF (default),
// the uploaded strength is 0.0 so static_prop.vert's hemisphere term is a
// bitwise no-op and the output is byte-identical to pre-slice af314d22.
// Read at first use, after main() has applied any launcher_env.json: a namespace-scope initializer would
// resolve the gate during static initialization, before that apply, and keep the stale value.
bool staticPropAmbientV1Enabled() {
    static const bool on = mc2::gate::get<"MC2_STATIC_PROP_AMBIENT_V1">();
    return on;
}

// PROP-BASE-LIGHT-DIR-1: directional (hemispherical-wrap) modulation of the
// baked per-vertex base_light seed. See the long rationale block in
// shaders/static_prop.vert next to `vec3 baseLightDir`.
//
// The user's defect -- "this building face should be in shadow as the light is
// from behind" -- is NOT a shadow bug. base_light is orientation-free, so a
// wall facing away from the sun renders as bright as one facing it. Its sun
// term is already ndl==0, so no shadow term can darken it (measured: <=0.05%
// of pixels day, 0.000% night for MC2_PROP_SHADOW_RECEIVE). This gate gives
// that baked seed the missing orientation term.
//
// VALUE = float strength 0..1, NOT a bool. This is deliberately one knob
// rather than an enable-bool plus a separate strength var: the strength IS the
// enable (0.0 == off), which makes an ENV_DROP failure impossible to confuse
// with a weak setting, and lets the default be tuned without a code change.
//   unset / "0" -> 0.0 -> shader's `> 0.0` guard skips the whole block ->
//                  seed == base_light -> BYTE-IDENTICAL to pre-slice output.
//   "0.45"      -> back-faces scaled to 55% of their baked seed; faces
//                  pointing at the sun are multiplied by exactly 1.0.
// Clamped to [0,1] because >1 would drive the back-face factor negative and
// produce black-clamped walls -- the exact failure mode of the blunt IBL
// killswitch the user rejected.
extern const float s_propBaseLightDirStrength = mc2::gate::get<"MC2_PROP_BASE_LIGHT_DIR">();

// FAR-BACKDROP-LIT: gate + tunables for the night-shade substitution applied to
// far/impostor static-prop LODs (see static_prop.vert). Default-OFF; "=1" / any
// non-"0", non-empty value enables. When OFF the per-frame upload pushes
// u_farBackdropLit=0 so the shader's `if (u_farBackdropLit != 0)` branch is
// skipped entirely -> seed==base_light -> byte-identical to the pre-change
// render (stronger than a multiply-by-zero: the substitution code never runs).
// Distance band defaults bracket the LOD impostor distance (MC2_IMPOSTOR_DIST,
// default 800 world units; bdactor.cpp selectActiveLOD) so the fade completes as
// the impostor takes over, hiding the LOD pop. All tunable at runtime via env.
// EDGE-LIGHT-EMISSION-1 (master-FAIL #8, re-framed). User verdict that opened
// this lane: "edge light is just doing the wrong thing entirely. the actual
// lights are not on at ALL." Two predecessor lanes chased whether the light
// FIXTURES are visible (backface culling, then camera-facing billboards + a
// screen-size clamp). That framing is superseded: visibility was never the
// problem. The fixtures were being drawn -- they were drawn UNLIT.
//
// Stock MC2 lights them through authored hot-colour vertex tags. All four
// fixture meshes (data/tgl/{runway,red,green,blue}beaconlight.ase) paint their
// post base pure green = 0xff00ff00 = "Hot Green -- Building Base Lights --
// ONLY at NIGHT", and each fixture's .ini declares the colour to emit in a
// `//Night Lights` block (runwaylight + redbeaconlight 0x00ff0000 red,
// greenbeaconlight 0x0000ff00, bluebeaconlight 0x000000ff -- so per-fixture
// colour is stock data and needs no new authoring). The legacy CPU path reads
// eye->getIsNight()/getNightFactor() (mclib/tgl.cpp:1901) and honours them.
// THIS path passed `false, 0.0` hardcoded into get_base_light(), so all three
// night-only branches (lit windows, outside building lights, base lights) took
// their daytime arm and emitted nothing.
//
// Gate MC2_STATIC_PROP_NIGHT_TAGS: 0 = OFF (isNight/factor uploaded as 0/0 ->
// byte-identical to the pre-fix render), 1 = ON (stock emission restored),
// 2 = verify-by-exaggeration (emission delta scaled by
// MC2_STATIC_PROP_NIGHT_TAGS_SCALE, default 4x under mode 2).
//
// BLAST RADIUS, stated plainly: this is not scoped to runway lights. It
// restores the night tags for EVERY static prop that carries one, so building
// windows and building base lights relight at night too. That is equally
// stock-correct and equally currently-wrong, but it is a broad visual change
// and is the reason this ships default-OFF pending user click-through.
//
// NOTE ON DISTANCE BEHAVIOUR: the emission rides real fixture geometry, so it
// shrinks and fades with distance on its own. It deliberately does NOT
// resurrect any screen-size clamp -- the unbounded clamp is what produced the
// "atrociously hideous" overview streaks (RUNWAY-MARKER-LIGHTS-1).
extern const int s_nightTagMode = mc2::gate::get<"MC2_STATIC_PROP_NIGHT_TAGS">();
extern const float s_nightTagScale = []() {
    if (mc2::gate::isNonEmpty<"MC2_STATIC_PROP_NIGHT_TAGS_SCALE">())
        return mc2::gate::get<"MC2_STATIC_PROP_NIGHT_TAGS_SCALE">();   // row clamps to [0, 16]
    return (s_nightTagMode == 2) ? 4.0f : 1.0f;
}();

// BEACON-FIXTURE-ID-1: fixture-scoped night emission.
//
// MC2_STATIC_PROP_NIGHT_TAGS (above) restores the stock night vertex tags for
// EVERY tagged prop, which is why it ships default-OFF: 738 of 2947 stock .ase
// carry the Hot Green tag and only 8 of them are runway/beacon fixtures -- the
// other ~730 are buildings. The tag's stock name is literally "Building Base
// Lights" (shaders/include/lighting.hglsl:118); the fixtures BORROW it. So
// vertex colour cannot separate fixtures from buildings, and scoping by colour
// would relight 730 buildings to buy 8 fixtures -- straight into the user's
// separate over-lit-buildings complaint.
//
// This gate scopes by FIXTURE IDENTITY instead, via a discriminator on the
// per-shape .ini emission colours (see isEmissiveFixtureType below). It relights
// ONLY the runway/taxiway/beacon fixtures and leaves every building byte-
// identical, so it is safe to default ON.
//
// Unset/"1" -> stock fixture emission (default ON). "0" -> off, byte-identical
// to the pre-fix render. Numeric > 1 -> verify-by-exaggeration. Resolved once at
// process start.
extern const float s_fixtureLightsScale = mc2::gate::get<"MC2_STATIC_PROP_FIXTURE_LIGHTS">();

// FIXTURE-GLOW-1: strength of the bounded, distance-attenuated ADDITIVE glow
// applied to the fixture types isEmissiveFixtureType() classifies.
//
// This is the successor mechanism to MC2_STATIC_PROP_FIXTURE_LIGHTS above, and
// it exists because that gate provably CANNOT fix the defect: get_base_light()
// clamps at 1.0 (scales 1/4/8 render byte-identical frames), and whatever
// survives the clamp is then multiplied by the fixture's dark lens albedo in
// static_prop.frag. A light source is not expressible as a multiplier on a dark
// texture. This gate adds a separate term after both, in the fragment shader.
//
// The two properties that keep it out of the RUNWAY-MARKER-LIGHTS-1 failure
// mode (constant-screen-size dashes at overview altitude) are enforced in the
// shader, not here: no geometry/screen-size term of any kind, and an attenuation
// that is arithmetically zero beyond a compile-time far plane well inside
// overview altitude. The near/far window is deliberately NOT env-tunable so the
// bound cannot be configured away.
//
// DEFAULT-OFF on measured evidence. The mechanism is verified and bounded
// (see .claude/FIXTURE-GLOW-1-STATUS.md), but at strength 1.0 the visible
// result on the only fixtures that could be observed on screen -- the green
// beacons of mc2_10 -- is a modest pale-ing of an already-green cone, not the
// "a light came on" the user asked for. Two facts drive the OFF default:
//   * the fixture lens already renders at channel max, so the ONLY headroom
//     left in an LDR target is the OTHER channels -- i.e. any additional glow
//     necessarily desaturates toward white. That is physically what a hot light
//     core does, but it is an aesthetic call, and this user has already
//     rejected one fixture-lighting attempt as "atrociously hideous";
//   * the red and blue fixture types were never observed glowing on screen (see
//     the status doc) -- only green was, for the fifth lane running.
// Shipping this ON would repeat the predecessor's outcome: a safe default-ON
// change that does not close the complaint. It needs a strength picked by eye.
//
// "0"/unset -> off, byte-identical. Numeric -> strength; 1.0 is the calibrated
// baseline, 3-4 is the "clearly a light" range. Resolved once at process start.
extern const float s_fixtureGlowStrength = mc2::gate::get<"MC2_STATIC_PROP_FIXTURE_GLOW">();

// BEACON-FIXTURE-ID-1: the fixture discriminator, validated against all 773
// stock data/tgl .ini.
//
// A light fixture declares the SAME saturated primary for all three Hot*RGB
// slots -- it has one emission colour and does not care which magic tag the
// artist painted the post with. Buildings declare desaturated greys/olives
// (0x00707070, 0x00557238, 0x006AA6A6, ...) and declare them INDEPENDENTLY per
// slot, so their three values are essentially never equal.
//
// Both halves of the test are load-bearing. "Saturated primary" ALONE is leaky
// because mclib/bdactor.cpp:813-819 defaults an undeclared slot to the magic tag
// itself (HotGreenRGB -> 0xff00ff00, a saturated primary): 503 of 773 .ini trip
// the naive test. Requiring all three EQUAL rejects every one of those, because
// the three defaults differ from each other.
//
// Measured on the stock data set: 4 of 773 .ini match -- runwaylight,
// redbeaconlight, greenbeaconlight, bluebeaconlight -- backing the ~24 fixture
// .ase (base/x/dam/damx/l1 variants). Zero buildings match. This is data-driven
// rather than a hardcoded name list, so it also classifies modded fixtures that
// follow the same authoring convention.
bool isEmissiveFixtureType(DWORD pink, DWORD yellow, DWORD green)
{
    const DWORD rgbP = pink & 0x00FFFFFFu;
    const DWORD rgbY = yellow & 0x00FFFFFFu;
    const DWORD rgbG = green & 0x00FFFFFFu;
    if (rgbP != rgbY || rgbY != rgbG) return false;   // rejects the default-mix case
    const int r = (int)((rgbP >> 16) & 0xFFu);
    const int g = (int)((rgbP >>  8) & 0xFFu);
    const int b = (int)((rgbP >>  0) & 0xFFu);
    const int mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    const int mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
    return mx >= 200 && mn <= 40 && (mx - mn) >= 160;  // saturated primary
}

// BEACON-FIXTURE-ID-1: numeric receipt -- how many registered types the
// discriminator classified as fixtures on the loaded map.
unsigned s_fixtureTypeCount = 0;

extern const bool s_farBackdropLitEnabled = mc2::gate::get<"MC2_FAR_BACKDROP_LIT">();
extern const float s_farBackdropNear = mc2::gate::get<"MC2_FAR_BACKDROP_LIT_NEAR">();
extern const float s_farBackdropFar = mc2::gate::get<"MC2_FAR_BACKDROP_LIT_FAR">();
extern const float s_farBackdropStrength = mc2::gate::get<"MC2_FAR_BACKDROP_LIT_STRENGTH">();
// Optional debug/exaggeration additive tint, "r,g,b" in [0..N] (default 0,0,0).
// Used for verify-by-exaggeration (e.g. "1,0,0" paints substituted far geometry
// red to prove the path fires on the ridge / impostor pixels).
float s_farBackdropTint[3] = {0.0f, 0.0f, 0.0f};
static const bool s_farBackdropTintParsed = []() {
    const std::string v = mc2::gate::str<"MC2_FAR_BACKDROP_LIT_TINT">();   // "" when unset or empty
    if (!v.empty()) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (sscanf(v.c_str(), "%f,%f,%f", &r, &g, &b) >= 1) {
            s_farBackdropTint[0] = r; s_farBackdropTint[1] = g; s_farBackdropTint[2] = b;
        }
    }
    return true;
}();

// V-IBL-STATIC-1: SH-L2 image-based ambient lane.
//   - env var MC2_STATIC_PROP_IBL_SH is the AUTHORITATIVE GATE
//     (resolved once at process start; matches V-AMBIENT-STATIC-1 pattern).
//   - runtime global g_iblShStrength (below) is the ImGui slider value;
//     it ONLY contributes when the env-gate is on.
//   - per-frame upload: iblShStrengthNightScaled() -- was
//     `s_iblShEnabled ? g_iblShStrength : 0.0f` until IBL-NIGHT-SCALE-1 added
//     the night curve on top (see that block below). Still 0.0f when the
//     feature is off, and still exactly g_iblShStrength in daylight.
//     => env unset/0 -> upload 0.0 -> shader `if (u_iblShStrength > 0.0)`
//        short-circuits to byte-identical pre-slice output (proof of
//        default-OFF kill-switch parity, stronger than multiply-by-zero).
//     => env=1 + slider=0.0 -> upload 0.0 -> visually disabled but the
//        feature flag IS active in the registry (env-only gate).
//     => env=1 + slider=N -> upload N (range 0.0..3.0 enforced by slider UI).
// V-IBL-DEFAULT-FLIP (2026-05-27): default-ON; explicit "=0" is kill-switch.
// env unset -> true; env="0" -> false; env="1"/anything else -> true.
// Read at first use (after main() applied launcher_env.json), see staticPropAmbientV1Enabled().
bool iblShEnabled() {
    static const bool on = mc2::gate::get<"MC2_STATIC_PROP_IBL_SH">();
    return on;
}

// IBL-NIGHT-SCALE-1: night-scale the SH-IBL ambient probe.
//
// THE DEFECT. The SH set applied to every static prop is projected from a DAY
// SKY (RenderCore/IblShCoeffs.h: "Source HDRI: data/hdr/DaySkyHDRI063B_4K.exr"),
// there is exactly ONE set for every mission and every time of day
// (kMissionShMap[] is empty), and NOTHING scaled it by night. Since the flip to
// default-ON (2026-05-27) that means a day sky has been lighting every night
// mission at full strength. Measured on mc2_10 night (full nightFactor): static
// props run +44%..+83% brighter than with the probe off and one building face
// hard-clips to 255 on 2.49% of its pixels. Stock MC2 had no IBL at all.
//
// WHY NOT THE EXISTING KILLSWITCH. MC2_STATIC_PROP_IBL_SH=0 removes the term
// outright and drives the perimeter wall, turrets and bunker domes 29%..46%
// darker -- the near-black props the user has separately complained about. The
// probe's DIRECTIONAL STRUCTURE (sky above, ground below) is still correct at
// night; only its INTENSITY was wrong. So this scales toward a floor rather
// than to zero.
//
// THE CURVE: scale = 1 - (1 - floor) * nightFactor, i.e. mix(1.0, floor,
// nightFactor). Chosen by measured sweep on mc2_10 night (see
// .claude/IBL-NIGHT-SCALE-1-STATUS.md), emulating each candidate floor via the
// pre-existing MC2_STATIC_PROP_IBL_SH_STRENGTH env on ONE binary:
//
//   floor  bldg clip%   wall_a   dome_a  turret_c  turret_a
//    1.00     2.49       0.0%     0.0%     0.0%     0.0%   (today's default)
//    0.35     0.57      -9.4%   -11.8%   -11.2%    -7.7%
//    0.25     0.13     -14.1%   -18.4%   -18.3%   -13.0%   <-- chosen (the knee)
//    0.00     0.00     -36.9%   -45.6%   -38.0%   -29.0%   (== blunt killswitch)
//
// 0.25 removes 95% of the clipping for a 13-18% dim, where 0.0 costs 29-46%.
//
// DAY IS BYTE-IDENTICAL BY ARITHMETIC, not by measurement: at day nightFactor
// is exactly 0.0f, so the multiplier is exactly 1.0f and `strength * 1.0f`
// returns the identical float. No epsilon, no rounding.
//
// Value semantics: float 0..1 = the night floor. "1.0" -> multiplier is exactly
// 1.0 at every nightFactor -> byte-identical to the pre-slice build (this is
// the killswitch). "0.0" -> hard (1 - nightFactor). Unset -> 0.25.
static const float s_iblNightFloor = mc2::gate::get<"MC2_STATIC_PROP_IBL_NIGHT_SCALE">();

// Effective SH-IBL strength for this frame: the configured strength (ImGui
// slider / MC2_STATIC_PROP_IBL_SH_STRENGTH env / per-mission
// data/visual_tuning.json "staticPropIblStrength") scaled by the night curve.
//
// NOTE ON SOURCING NIGHT STATE. This deliberately calls gos_GetNightState()
// directly instead of reusing the nFactor computed in the uniform-upload
// blocks below. Those blocks only fetch real night state when one of
// MC2_STATIC_PROP_NIGHT_TAGS / _FIXTURE_LIGHTS / _FIXTURE_GLOW is active and
// otherwise leave nFactor at 0.0. _FIXTURE_LIGHTS happens to default ON today,
// so piggy-backing would work by luck -- and would silently degrade to a no-op
// the moment a user set MC2_STATIC_PROP_FIXTURE_LIGHTS=0. This fix must not
// depend on an unrelated gate's default.
float iblShStrengthNightScaled() {
    if (!iblShEnabled()) return 0.0f;
    if (s_iblNightFloor >= 1.0f) return g_iblShStrength;   // killswitch: exact
    bool  isNight = false;
    float nFactor = 0.0f;
    ::gos_GetNightState(&isNight, &nFactor);
    if (nFactor <= 0.0f) return g_iblShStrength;           // day: exact
    return g_iblShStrength * (1.0f - (1.0f - s_iblNightFloor) * nFactor);
}

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular gate.
// Default-OFF; "=1" / any non-"0" value enables. Matches V-IBL-STATIC-1
// pre-flip env-parse pattern. When OFF, the per-frame upload pushes
// u_pbrV1Strength=0.0f -> shader `if (u_pbrV1Strength > 0.0)` short-circuits
// before any u_cameraWorldPos access -> mathematically byte-identical
// (strength=0 -> lit += specular * 0 = lit unchanged).
bool pbrV1Enabled() {   // first-use read, see staticPropAmbientV1Enabled()
    static const bool on = mc2::gate::get<"MC2_STATIC_PROP_PBR_V1">();
    return on;
}

// STATICPROP-MATERIAL-ORM-1: gate for per-bucket linear ORM (occlusion-
// roughness-metallic) texture-array slots + sidecar feed. Default-OFF; "=1"
// / any non-"0", non-empty value enables. Mirrors the s_pbrV1Enabled env-parse
// idiom. When OFF, zero ORM arrays are built and zero extra sidecar I/O occurs
// so the gate-OFF path is byte-identical.
extern const bool s_ormSlotsEnabled = mc2::gate::get<"MC2_STATICPROP_MATERIAL_PBR_SLOTS">();

// V-MATERIAL-PBR-2 safety interlock: when MC2_VIEW_UNIFORMS=0 the shader's
// PBR block is excluded by `#if defined(MC2_USE_VIEW_UNIFORMS)` (compile-
// time guard). This runtime flag is the belt to the compile-time
// suspenders: even if a build slips through with the guard misconfigured,
// strength is force-zeroed here so the path stays dormant. Cached once
// at process start; mirrors the IBL env-gate idiom.
extern const bool s_viewUniformsDisabled = !mc2::gate::get<"MC2_VIEW_UNIFORMS">();

// V-MATERIAL-PBR-2-DIAG: diagnostic visualizer gate. Default-OFF; "=1" / any
// non-"0" value enables. Mirrors s_pbrV1Enabled parse pattern. When ON AND
// PBR gate is also ON, shader replaces lit with cyan (sunFound=true) /
// magenta (false) instead of running Schlick math. Diagnostic-only; never
// commits a behavior change to the default path.
extern const bool s_pbrV1DiagSunFound = mc2::gate::get<"MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND">();
// Note: definition of `g_iblShStrength` lives at file scope above the
// anonymous namespace (line ~191) so it has external linkage. The lambda-
// initialized s_iblShEnabled stays local here; the slider value is global.

// V-IBL-STATIC-2: per-mission SH set selection.
//   - s_currentShSet points at the active set; initialized to "default"
//     (kIblShSets[0]) so every code path -- including pre-mission frames
//     and code paths that never call setMissionForIbl (editor, savegame
//     load before mission name is plumbed) -- uploads the V-IBL-STATIC-1
//     baseline coefficients. Never nullptr.
//   - setMissionForIbl(name) is called from Mission::init right after
//     onMapLoad(); it consults the compile-time registry (kMissionShMap
//     in IblShRegistry.h) and falls back to "default" for unknown names.
//   - Optional env override MC2_STATIC_PROP_IBL_SH_SET=<name> wins over
//     the registry when set + matches an existing set; unset/empty/typo
//     -> registry. Resolved once at process start.
extern const RenderCore::IblShSet* s_currentShSet = &RenderCore::kIblShSets[0];
static const std::string s_iblShSetEnvOverrideText = mc2::gate::str<"MC2_STATIC_PROP_IBL_SH_SET">();   // "" when unset or empty
extern const char* s_iblShSetEnvOverride = s_iblShSetEnvOverrideText.empty() ? nullptr : s_iblShSetEnvOverrideText.c_str();

// ── IBL-SUN-ROTATE-1 ────────────────────────────────────────────────────────
// Rotate the SH-IBL ambient probe about world-up so its bright lobe lands on
// the MISSION SUN, instead of staying pinned wherever the source HDRI's own sun
// happened to be.
//
// THE DEFECT, measured offline in tools/class_light_band_harness Stage F (no
// launch, no capture -- it is arithmetic over the shipped tables):
//   - There is ONE coefficient set for the whole game. RenderCore/
//     IblShRegistry.h kIblShSetCount == 1, and kMissionShMap[] holds a single
//     {nullptr,nullptr} placeholder -- so its COUNT IS 1, not 0, and every one
//     of the 24 shipping missions resolves to "default" through lookupShSet().
//   - Its bright axis is therefore a constant: the L1 band IS the radiance's
//     first directional moment, so the axis is normalize([3],[1],[2]) under the
//     shader's binding = Stuff azimuth +0.344 deg on every mission.
//   - The MEDIAN mission misses that axis by 106 deg; the worst is mc2_14 at
//     176.7 deg. On 17 of the 24 the probe is strictly BRIGHTER on the wall
//     facing AWAY from the sun than on the wall facing it. The swing it is
//     mis-aiming is 21.05:1 across the sphere.
//
// WHY THIS IS AN OVERSIGHT AND NOT A DESIGN CHOICE. The VISIBLE skybox drawn
// from the SAME HDRI is already sun-synced: gos_postprocess.cpp:5201-5218 scans
// the EXR for its baked sun azimuth (gos_hdri.cpp) and sets
// `skyYaw = hdriBakedSunAz_ - sunAzGL` so the HDRI's sun lands on the mission
// sun. The probe DERIVED FROM THAT SKY is the one consumer that skips the step.
// IblShRegistry.h's own creating commit says future slices would populate
// kMissionShMap; none ever did, and the V-IBL default-ON flip (2026-05-27) made
// the unrotated probe live behaviour on every mission.
//
// WHY THE FIX IS HERE AND NOT IN THE SHADER. A rotation about the basis POLE is
// the closed-form case of SH rotation -- no Wigner matrices. Grouping
// static_prop.vert::evalShL2's terms by their (x,z) angular dependence, with
// x = s*cos(a), z = s*sin(a):
//     m=0, INVARIANT : [0] L00, [1] L1-1 (the POLE term under the Y-up
//                      binding: evalShL2 multiplies u_iblSh[1] by n.y), [6] L20
//     m=1, angle  t  : ([3] L11 ~ x, [2] L10 ~ z) and ([4] L2-2 ~ xy, [5] L2-1 ~ yz)
//     m=2, angle 2t  : ([8] L22 ~ x^2-z^2, [7] L21 ~ xz)
//   each pair mixing as (A,B) -> (A cos mt - B sin mt, A sin mt + B cos mt).
// tools/class_light_band_harness test_sh_y_rotation_equals_rotating_the_normal
// proves to 1e-12 that this equals counter-rotating the NORMAL, so the whole
// fix is nine CPU-side coefficient updates once per sun change -- zero
// per-vertex cost, and shaders/static_prop.vert is not touched at all.
//
// DEFAULT OFF, deliberately. Nobody has seen this render: engine launches are
// hard-blocked in this lane. It moves the clamped prop multiplier by up to
// -0.70 on shadow-side walls (see the floor below), which is a large visible
// change that wants a graded A/B before it ships on.
//
// GRADE IT ON mc2_17 (mean move 0.181, 54.6% of wall azimuths unclamped -- the
// most headroom in tier1) or mc2_24 (0.231, the largest tier1 move). NOT on
// mc2_10 or mc2_22: both are ambient-192 AND sun-192, so ~91% of wall azimuths
// are already at the clamp and the rotation moves a mean of 0.0037 / 0.0123 --
// one byte. mc2_05 moves most overall (0.358) but its sun is below the horizon,
// where "aim at the sun" is not a well-posed request.
// Canonical DEFAULT-OFF opt-in reader.
// Read at first use, see staticPropAmbientV1Enabled().
static bool iblSunRotateEnabled() {
    static const bool on = mc2::gate::get<"MC2_STATIC_PROP_IBL_SUN_ROTATE">();
    return on;
}

// ── IBL-SUN-ROTATE-1's FLOOR, independently adjustable ──────────────────────
//
// WHY A ROTATION NEEDS A FLOOR AT ALL. A rotation is an isometry of the sphere,
// so it cannot create a darker value than the probe already had -- min and max
// over the sphere are invariant. What it changes is WHICH wall gets the dark
// end, and that is the whole problem: once the probe is aimed correctly, the
// probe's dark lobe COINCIDES with the wall that also has NdotL == 0. Before
// the rotation those two darknesses sat on different walls and partially
// covered for each other; after it they stack. Measured worst drops on the
// clamped prop multiplier are -0.705 (mc2_13), -0.704 (mc2_17), -0.692
// (mc2_12) and -0.692 (mc2_18) -- and mc2_12/13/17/18 all carry ambient
// 31/255 == 0.1216, i.e. almost no other fill to catch a face when the probe
// leaves it. An earlier IBL_NIGHT_SCALE floor of 0.0 was rejected for exactly
// this near-black failure; this slice is not going to reintroduce it.
//
// THE SHAPE: a lower bound on the probe's own irradiance, applied as ONE
// UNIFORM LIFT on L00 -- the only isotropic coefficient in evalShL2 (its
// spherical mean is exactly c4*L00, because c5 == c3/3 makes the L20 pair's
// mean vanish). Consequences, all of them wanted:
//   - the AIM IS UNTOUCHED. L1 and L2 are the directional bands and this does
//     not read or write them, so the bright axis lands on the sun to 1e-9
//     whatever the floor is. The floor cannot quietly undo the fix.
//   - it can only LIFT. There is no floor value that makes any direction darker
//     than the plain rotation, so the floor is safe to tune in either direction.
//   - it is UNIFORM across R,G,B (one lift sized by the worst channel) rather
//     than per-channel, which would flatten the dark side's colour: the baked
//     probe's dark end is blue-dominant (min 0.232 R / 0.360 G / 0.477 B) and a
//     per-channel clamp would neutralise exactly that.
//   - the minimum is ROTATION-INVARIANT, so the lift is computed ONCE from the
//     baked table and never per mission.
//
// THE DEFAULT VALUE, 0.45, and where it comes from -- an in-engine anchor, not
// taste. terrain_lod_chunk.frag's own lighting cannot go below
// mix(0.35,1.20,0.02) == 0.367 (pinned by class_light_band_harness
// test_terrain_floor_is_not_0_35). So the GROUND a building stands on has a
// hard 0.367 floor while the building's shadow-side wall has none -- which is
// the "prop reads black against lit ground" complaint in one line. Solving
//     ambient + floor * strength >= 0.367
// at the darkest shipping case (ambient 31/255 == 0.1216 on mc2_12/13/17/18)
// and the shipped daytime IBL strength 0.55 gives floor >= 0.446. 0.45 is that,
// rounded up. On the baked table the resulting lift is +0.218 irradiance, which
// against a bright end of 4.896 is +4.5% -- almost entirely inside the clamp.
//
// "0" or unset-to-0 disables the floor and gives the bare rotation; that is the
// arm to use when measuring how much of a graded difference the floor is worth.
// Bounded [0,2] (out-of-range numbers clamp); text that is not a number parses as
// 0 and so reads as the bare rotation, only NaN reads as the 0.45 default --
// never a silent giant lift.
// Read at first use, see staticPropAmbientV1Enabled().
static float iblSunRotateFloor() {
    static const float v = mc2::gate::get<"MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR">();
    return v;
}

// shaders/static_prop.vert::evalShL2, transcribed verbatim (constants c1..c5
// read from the shader body, coefficient binding read from the same lines).
// Used ONLY for the one-off spherical-minimum search below -- the per-vertex
// evaluation stays in the shader. double, not float, so the harness port can
// agree with it exactly.
static double iblEvalShL2Cpu(const float (*C)[3], int ch,
                             double nx, double ny, double nz) {
    const double c1 = 0.429043, c2 = 0.511664, c3 = 0.743125,
                 c4 = 0.886227, c5 = 0.247708;
    return c1 * C[8][ch] * (nx * nx - nz * nz)
         + c3 * C[6][ch] * (ny * ny)
         + c4 * C[0][ch]
         - c5 * C[6][ch]
         + 2.0 * c1 * C[4][ch] * (nx * ny)
         + 2.0 * c1 * C[5][ch] * (ny * nz)
         + 2.0 * c1 * C[7][ch] * (nx * nz)
         + 2.0 * c2 * C[3][ch] * nx
         + 2.0 * c2 * C[1][ch] * ny
         + 2.0 * c2 * C[2][ch] * nz;
}

// The smallest UNIFORM lift, in irradiance units, that brings every channel's
// spherical minimum up to iblSunRotateFloor(). Searched numerically rather than
// baked as a constant on purpose: RenderCore/IblShCoeffs.h is regenerated by
// tools/ibl/project_sh.py, and a hardcoded lift would go silently stale the
// next time it is. 129*256*3 evaluations, once per coefficient table, off the
// render path.
static double iblSunRotateLift(const float (*C)[3]) {
    if (iblSunRotateFloor() <= 0.0f || C == nullptr) return 0.0;
    const double kPi = 3.14159265358979323846;
    double worstDeficit = 0.0;
    const int kLat = 128, kLon = 256;
    for (int i = 0; i <= kLat; ++i) {
        const double th = kPi * (double)i / (double)kLat;
        const double sth = std::sin(th), cth = std::cos(th);
        for (int j = 0; j < kLon; ++j) {
            const double ph = 2.0 * kPi * (double)j / (double)kLon;
            const double nx = sth * std::cos(ph), ny = cth, nz = sth * std::sin(ph);
            for (int ch = 0; ch < 3; ++ch) {
                const double deficit =
                    (double)iblSunRotateFloor() - iblEvalShL2Cpu(C, ch, nx, ny, nz);
                if (deficit > worstDeficit) worstDeficit = deficit;
            }
        }
    }
    return worstDeficit;
}

static float  s_iblShRotated[9][3]      = {};
static bool   s_iblShRotatedValid       = false;
static float  s_iblShRotatedSun[3]      = { 0.0f, 0.0f, 0.0f };

// The 9x3 coefficient block to upload this flush.
//
// GATE OFF returns the shipped table pointer unchanged -- the identical bytes
// the pre-slice build uploads, not a copy that happens to compare equal.
const float* iblShCoeffsForUpload() {
    const float (*src)[3] = s_currentShSet ? s_currentShSet->coeffs
                                           : RenderCore::kIblShCoeffs;
    if (!iblSunRotateEnabled()) return &src[0][0];

    // NOTE: no local `extern` re-declaration here, unlike the neighbouring
    // upload blocks. This function lives INSIDE the anonymous namespace, where a
    // block-scope extern declares `(anonymous namespace)::gos_GetTerrainLightDir`
    // and links to nothing -- LNK2019, caught on the first build of this slice.
    // gameos.hpp (included at the top of this file) already declares the real
    // one at global scope; `::` says so unambiguously.
    float sx = 0.0f, sy = 0.0f, sz = 1.0f;
    ::gos_GetTerrainLightDir(&sx, &sy, &sz);

    if (s_iblShRotatedValid && src == s_iblShRotatedSrc &&
        sx == s_iblShRotatedSun[0] && sy == s_iblShRotatedSun[1] &&
        sz == s_iblShRotatedSun[2]) {
        return &s_iblShRotated[0][0];
    }

    // THE FRAME. static_prop.vert feeds evalShL2 the STUFF-space normal
    // (`worldNormal`, :652) -- NOT the MC2-swapped `worldNormalMC2` (:664) that
    // the diffuse term uses. So the sun, which arrives in the MC2 frame from
    // gos_GetTerrainLightDir, has to be carried back into Stuff before its
    // azimuth means anything to the probe. The swap is
    // stuff -> mc2 : (x,y,z) -> (-x, z, y)   [static_prop.vert:664, mech.vert:162]
    // and it is its own inverse, so mc2 -> stuff is the same expression:
    // stuff.x = -mc2.x, stuff.y = mc2.z, stuff.z = mc2.y. Stuff is Y-up, so its
    // horizontal plane is x-z and azimuth = atan2(stuff.z, stuff.x)
    //                                     = atan2(mc2.y, -mc2.x).
    const double sunHoriz2 = (double)sx * sx + (double)sy * sy;
    double t = 0.0;
    if (sunHoriz2 > 1e-12) {
        const double sunAz = std::atan2((double)sy, -(double)sx);
        // The baked probe's bright axis IS its L1 band. R channel: all three
        // agree to within 0.5 deg (harness F2), and one angle has to serve all
        // three or the channels would shear apart.
        const double probeAz = std::atan2((double)src[2][0], (double)src[3][0]);
        t = sunAz - probeAz;
    }

    const double c1t = std::cos(t),       s1t = std::sin(t);
    const double c2t = std::cos(2.0 * t), s2t = std::sin(2.0 * t);
    for (int ch = 0; ch < 3; ++ch) {
        for (int i = 0; i < 9; ++i) s_iblShRotated[i][ch] = src[i][ch];
        // m=0 ([0] L00, [1] L1-1 pole term, [6] L20) are invariant -- copied above.
        const double a3 = src[3][ch], b2 = src[2][ch];   // m=1 (L11 ~x, L10 ~z)
        s_iblShRotated[3][ch] = (float)(a3 * c1t - b2 * s1t);
        s_iblShRotated[2][ch] = (float)(a3 * s1t + b2 * c1t);
        const double a4 = src[4][ch], b5 = src[5][ch];   // m=1 (L2-2 ~xy, L2-1 ~yz)
        s_iblShRotated[4][ch] = (float)(a4 * c1t - b5 * s1t);
        s_iblShRotated[5][ch] = (float)(a4 * s1t + b5 * c1t);
        const double a8 = src[8][ch], b7 = src[7][ch];   // m=2 (L22 ~x^2-z^2, L21 ~xz)
        s_iblShRotated[8][ch] = (float)(a8 * c2t - b7 * s2t);
        s_iblShRotated[7][ch] = (float)(a8 * s2t + b7 * c2t);
    }

    // The floor. Rotation-invariant, so it is searched once per source table
    // and reused for every sun angle.
    static const float (*s_liftSrc)[3] = nullptr;
    static double s_liftValue = 0.0;
    if (s_liftSrc != src) { s_liftValue = iblSunRotateLift(src); s_liftSrc = src; }
    if (s_liftValue > 0.0) {
        const double dL00 = s_liftValue / 0.886227;   // c4: L00's weight in evalShL2
        for (int ch = 0; ch < 3; ++ch)
            s_iblShRotated[0][ch] = (float)((double)src[0][ch] + dL00);
    }

    s_iblShRotatedSrc    = src;
    s_iblShRotatedSun[0] = sx;
    s_iblShRotatedSun[1] = sy;
    s_iblShRotatedSun[2] = sz;
    s_iblShRotatedValid  = true;
    return &s_iblShRotated[0][0];
}

// V-MATERIAL-DEBUG-1: per-fragment material debug view mode for the
// StaticPropOpaque lane. Default 0 = OFF (byte-identical to legacy output —
// shader short-circuits via `if (u_debugMaterialMode != 0) { ...; return; }`).
// Env var MC2_STATIC_PROP_DEBUG_MATERIAL accepts integer 0..N:
//   0 = off (default)
//   1 = albedo (raw texture)
//   2 = materialIdx (hashed palette)
//   3 = normal (worldNormal as RGB)
//   4 = texArrayLayer (hashed palette)
//   5 = roughnessFactor as grayscale (V-MATERIAL-PBR-1)
//   6 = metallicFactor as grayscale  (V-MATERIAL-PBR-1)
// Values outside the implemented range render as hot-pink in the shader.
// Resolved once at process start (atoi semantics: invalid -> 0).
int s_staticPropDebugMaterialMode = mc2::gate::get<"MC2_STATIC_PROP_DEBUG_MATERIAL">();
bool s_materialKtxEnabled = mc2::gate::get<"MC2_MATERIAL_KTX">();   // MC2_MATERIAL_KTX=1
// MC2_STATICPROP_TEX_TIER: 128 | 256 | 512 (default) | 1024
// Selects which cooked tier directory (data/tgl/<tier>/<name>.ktx2) is loaded
// when MC2_MATERIAL_KTX=1. Launcher sets this from the Graphics / Texture Quality
// slider; engine reads it once at process start.
int texTier() {   // first-use read, see staticPropAmbientV1Enabled()
    static const int tier = []() -> int {
        const int v = mc2::gate::get<"MC2_STATICPROP_TEX_TIER">();   // row default 512
        return (v == 128 || v == 256 || v == 1024) ? v : 512;
    }();
    return tier;
}
// COMPRESSION-BC7-STATICPROP-1: gate for BC7-compressed KTX2 static-prop arrays.
// Default-OFF; effective only when s_materialKtxEnabled is also true. When the
// per-group BC7 conditions (all-or-nothing + uniform-dim) are not met, the group
// falls back to the existing RGBA8 path → byte-identical to gate-OFF.
bool s_staticPropBc7Enabled = mc2::gate::get<"MC2_STATICPROP_BC7">(); // MC2_STATICPROP_BC7=1
// Tracks whether finalizeGeometry() produced a correctly-sized sidecar.
// Reset to false at the start of every finalizeGeometry() call; set to true
// only after the sidecar loop completes with size == emitted count.
// Initialized false: no table built yet.
bool s_materialGpuSidecarValid = false;
std::vector<uint32_t>                s_packetMaterialIdx;  // per draw slot
std::vector<RenderCore::MaterialGpu> s_materialGpuTable;   // deduplicated
// V-MATERIAL-STATIC-0: inventory snapshot — one row per s_materialGpuTable entry.
// Built in lockstep with s_materialGpuTable; cleared in onMapUnload. Read-only.
std::vector<StaticPropMaterialInventoryEntry> s_materialInventory;
// v2: per-packet texArrayLayer sidecar. Indexed by global packet index.
// Copied from layerForPacket[] in finalizeGeometry() before that local is discarded.
// Valid until next finalizeGeometry() or onMapUnload(). -1 for unavailable packets.
std::vector<int32_t> s_packetTexArrayLayer;
// FENCE-UVSCALE-1: per-packet uvScaleX/Y sidecar, same indexing/lifetime as
// s_packetTexArrayLayer above. Copied from uvScaleXByPacket/uvScaleYByPacket
// in finalizeGeometry() (already computed there for the GL PerDrawEntry path,
// gos_static_prop_batcher.h uvScaleX/Y) so the VK color-prop island can sample
// the same texture sub-region GL does instead of assuming 1.0 (ADJUDICATION-1
// FOLLOW-UP fence residual: narrow/gapped panels vs GL's continuous ribbon).
// Also consumed by SHADOW-PROP-ALPHA-ATLAS-1 (atlas-variant prop shadow bind:
// shadow alpha-test samples the same array sub-region as the color pass).
std::vector<float> s_packetUvScaleX;
std::vector<float> s_packetUvScaleY;
// VK-P11 STATIC-PROP MATERIAL: per-type group-relative albedo layer + alpha-class,
// persisted for mc2_color_prop_island_get_material_feed (the VK island copy-once
// imports s_texArrayOff and samples at typeAlbedoLayer[typeID]). Populated in
// finalizeGeometry() from layerForType[] + s_types[t].alphaClass; cleared in
// onMapUnload(). -1 = type has no alpha-OFF albedo texture.
std::vector<int32_t> s_typeAlbedoLayer;
std::vector<int32_t> s_typeAlphaClass;
// v2.1: per-type instance count snapshot from the most recent flush().
// Keyed by typeID. Cleared in onMapUnload().
// Same prior-frame semantics as s_recipeHasSubstrateRecord / hasCullRecord:
//   flush(frame N) snapshots -> extraction(frame N+1) reads -> 0 on frame 1.
std::unordered_map<uint32_t, uint32_t> s_typeInstanceCountPrevFrame;
bool s_reinjectedThisFrame = false;  // item-4 once-per-frame guard
// STATICPROP-MATERIAL-SSBO-OWNER-1: static-prop MaterialGpu table SSBO (binding 5),
// narrowed behind a GpuBufferOwner identity record (logical id + lifetime + debug
// name + GLuint value). GL calls happen at the same sites with the same
// args/order/flags/slot; the raw handle is reached only via owner.backendHandle.
// Lifetime Mission: created in finalizeGeometry(), torn down on onMapUnload();
// rebuilt per map. Registered at create (upload), invalidated on destroy.
RenderCore::GpuBufferOwner           s_materialGpuSsbo{
    RenderCore::RenderResourceId::StaticPropMaterialGpuBuffer,
    RenderCore::RenderResourceLifetime::Mission,
    "StaticPropMaterialGpuBuffer",
    0u};

GLuint s_texArrayOff               = 0;  // alpha-OFF group GL_TEXTURE_2D_ARRAY
GLuint s_texArrayOn                = 0;  // alpha-ON  group GL_TEXTURE_2D_ARRAY
std::vector<GLuint>               s_bucketArrays;       // owns GL handles (delete all on teardown)
std::vector<StaticPropBucketInfo> s_bucketInfo;
std::vector<int32_t>              s_packetBucketIndex;  // per global packet; -1 if unassigned
std::vector<uint8_t>              s_slotBucketIndex;    // per sorted draw slot
std::vector<uint32_t>             s_bucketCmdCount;     // per bucket, in sorted-slot order

// STATICPROP-MATERIAL-ORM-1 — per-bucket LINEAR ORM (occlusion-roughness-
// metallic) sibling arrays. INVARIANT: s_ormBucketArrays.size() ==
// s_bucketArrays.size(), and ORM layer k in bucket b corresponds to the SAME
// unique as albedo layer k in bucket b (metallicRoughnessTex == albedoTex).
// Each ORM array is DENSE/bindable: layers without a real sidecar get a 1x1-
// replicated neutral (255,255,255,255) texel. s_ormLayerHasMap[b][k] records
// whether bucket b layer k had a REAL ORM map (drives the material flag).
// All three vectors stay empty when the gate (s_ormSlotsEnabled) is OFF.
std::vector<GLuint>               s_ormBucketArrays;    // owns GL handles (delete all on teardown)
std::vector<std::vector<bool>>    s_ormLayerHasMap;     // per bucket, per bucket-relative layer
GLuint s_permutationSsbo           = 0;  // sortedSlot[typeID] mapping (binding 15)
GLuint s_staticPropProgramCoalesce = 0;  // coalesce variant (Step 7.5)

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1: depth-prepass program object — same
// static_prop.vert (invariant gl_Position), but the cheap static_prop_depth.frag
// that does ONLY the alpha-test discard (no color/lighting/object-id). It is a
// DISTINCT GL program object, so its uniform locations differ from the color
// program's — the prepass must use THIS program's u_drawIDBase, not the color
// program's. 0 = the program failed to link this session → flushDepthPrepass()
// must no-op (color path unaffected).
GLuint s_staticPropDepthProgram = 0;
DepthCoalesceLocs s_locsDepthCoalesce;

void*  s_coalesceInstanceMap = nullptr;
GLsync s_coalesceFence[RING_FRAMES] = {};

std::vector<uint32_t> s_sortedTypeOrder;
uint32_t s_alphaOffCount = 0;
uint32_t s_alphaOnCount  = 0;
size_t   s_offGroupTotalBytes = 0;
size_t   s_onGroupTotalBytes  = 0;
size_t   s_coalescePerFrameInstanceBytes = 0;  // populated in Step 5.7

// 2026-05-11 per-packet rework (multi-packet correctness fix). Each indirect
// draw command corresponds to ONE packet (not one type). Multi-packet types
// in stock content (S_admin_Core has 44 packets, fences/doors with separate
// alpha-mesh and frame packets) need per-packet textures and per-packet
// alpha-test bits — the previous per-type architecture collapsed all packets
// to the first packet's texture, dropping detail and alpha. See
// memory/substrate_coalesce_armed_multi_packet_limitation.md.
//
// Sorted-packet order: alpha-OFF packets first, alpha-ON packets second.
// A packet's alpha-class group comes from its TYPE's alphaClass (OR-reduce
// of its packets' alpha-test bits) — all packets of one type render in the
// same group so they can read instance data from the same per-group SSBO
// range; the fragment's per-packet materialFlags then drives discard.
std::vector<uint32_t> s_sortedPacketOrder;     // global packet indices in [OFF | ON] order
uint32_t s_alphaOffCmdCount = 0;               // number of packets in alpha-OFF group
uint32_t s_alphaOnCmdCount  = 0;               // number of packets in alpha-ON group
GLuint   s_cmdToBucketSsbo  = 0;               // uint typeID per cmd (binding 7, patch shader)

std::vector<DWORD> s_coalescePinnedNodes;

bool s_coalesceLayoutReady = false;
bool s_coalesceEnabled     = false;
bool s_coalesceArmed       = false;

bool s_coalesceFirstFlushDone = false;

// Step 2.1 — env-resolved-once + extension-probe-persisting flags.
// Set inside coalesce_resetEnvOnce() / loadProgramsIfNeeded(), never cleared
// across mission loads (process-lifetime values).
bool s_coalesceEnvDisabled = false;
bool s_hasShaderDrawParams = false;
CoalesceForceDisarm s_coalesceForceDisarm = CoalesceForceDisarm::None;

// Step 12.1 — trace gate for noisy non-lifecycle COALESCE diagnostics.
// Lifecycle events (armed/disarmed/ready/permutation_state when forced)
// are always-on; future per-frame diagnostics gated behind this env.
extern const bool s_coalesceTrace = mc2::gate::get<"MC2_SUBSTRATE_COALESCE_TRACE">();

// --- Global instance pool (slice 1, plan v3.8 step group 3). ---
// s_globalInstanceCap: per-ring-frame instance capacity for the global pool.
// Override via MC2_STATIC_PROP_GLOBAL_CAP env; defaults to STATIC_PROP_GLOBAL_CAP_DEFAULT.
// s_baseInstanceByCmdSsbo: persistent-mapped SSBO, one uint32_t baseInstance per
// draw command per ring frame. Binding BASE_INSTANCE_SSBO_BINDING (16).
// Allocated inside finalizeGeometry under !s_globalPoolLegacy after the per-packet
// sort builds s_alphaOffCmdCount + s_alphaOnCmdCount.
extern const uint32_t s_globalInstanceCap = []() {
    if (mc2::gate::isNonEmpty<"MC2_STATIC_PROP_GLOBAL_CAP">()) {
        const uint32_t cap = static_cast<uint32_t>(mc2::gate::get<"MC2_STATIC_PROP_GLOBAL_CAP">());
        if (cap > 0) return cap;
    }
    return STATIC_PROP_GLOBAL_CAP_DEFAULT;
}();

// M2a POPULATION-SPLIT state (defined early so onMapUnload :1729 can tear the
// SSBO down; the build/fill helpers + accessors live near s_baseInstanceForType).
std::vector<uint32_t> s_staticBaseInstanceForType;
uint64_t              s_staticBaseBuiltGen     = 0xFFFFFFFFFFFFFFFFull;
GLuint                s_staticInstanceSsbo     = 0;
void*                 s_staticInstanceMap      = nullptr;   // GL_MAP_PERSISTENT|COHERENT
size_t                s_staticInstanceBytes    = 0;         // == cap * sizeof(instance)
uint64_t              s_staticInstanceFillGen  = 0xFFFFFFFFFFFFFFFFull;
GLuint                       s_staticIndirectCmdBuf = 0;
std::vector<StaticDrawCmd>   s_staticDrawCmds;          // CPU mirror
uint64_t                     s_staticCmdBuiltGen = 0xFFFFFFFFFFFFFFFFull;
// Task 4 criterion 2: fence covering the last StaticPopulation draw. The single-
// region s_staticInstanceSsbo is overwritten in place on a dirty fill; wait on
// this before overwriting so a prior frame's in-flight draw has finished reading
// (GPU executes in submission order -> the latest fence covers all earlier).
GLsync                       s_staticDrawFence = nullptr;

// Gate: MC2_STATIC_POP_SPLIT env AND the G2 frozen-records path armed. Defined
// early (before onMapUnload / the draw / reinject) so all use sites resolve.
bool staticPopSplitArmed() {
    static const bool s_on = mc2::gate::get<"MC2_STATIC_POP_SPLIT">();
    return s_on && GpuStaticPropRegistry::frozenRecordsArmed();
}

GLuint   s_baseInstanceByCmdSsbo          = 0;
void*    s_baseInstanceByCmdMap           = nullptr;
size_t   s_baseInstanceByCmdBytesPerFrame = 0;

uint32_t s_offGroupCountThisFrame  = 0;
size_t   s_offGroupBytesThisFrame  = 0;
size_t   s_onGroupBytesThisFrame   = 0;
size_t   s_totalUsedBytesThisFrame = 0;

// BUILDING-GLASS-1: ordered-dither translucency for partial-alpha (glass)
// texels on ALPHA_TEST packets. Default ON; MC2_SP_GLASS_DITHER=0 kills it
// (shader falls back to the legacy binary a<0.5 discard, byte-identical).
extern const bool s_glassDitherEnabled = mc2::gate::get<"MC2_SP_GLASS_DITHER">();
ProgramLocs s_locsLegacy;
ProgramLocs s_locsCoalesce;

// --- Step 2.6: identity-permutation alloc helper (plan v3.8). ---
// Called UNCONDITIONALLY by Step 5.4 before the Step 5.5 early-return so
// that even disarmed missions have a non-zero s_permutationSsbo with
// identity contents. Step 10.3 binds slot 15 every patch dispatch
// regardless of armed state; an identity permutation makes legacy fallback
// behave as if no remapping happened.
//
// Lifecycle: GL_STATIC_DRAW (matches spec §9 lifecycle table). The buffer
// is finalize-uploaded; Step 5.12 may overwrite it once via glBufferSubData
// in the same finalize pass, then it is read-only thereafter.
void allocPermutationSsboAsIdentity(uint32_t typeCount) {
    if (typeCount == 0) return;
    std::vector<uint32_t> identity(typeCount);
    for (uint32_t i = 0; i < typeCount; ++i) identity[i] = i;
    // TIER2-EXCLUDED: dead-path
    if (s_permutationSsbo == 0) glGenBuffers(1, &s_permutationSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
    MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                 typeCount * sizeof(uint32_t),
                 identity.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// --- Step 5.10.c rollback helper (plan v3.7). ---
// Factored out of finalizeGeometry()'s per-group texture-array build so
// Step 12B.5's forced-`size_mismatch` test hook reuses the same code.
// One rollback site, two callers (natural failure + forced disarm).
//
// Per v2r15 out3-MIN-F rollback contract: delete BOTH texture arrays if
// either is non-zero (the OFF group may have succeeded before the ON
// group fails — the half-built array still leaks if not deleted), then
// unpin only the CURRENT group's temp pins. Do NOT touch the other
// group's already-promoted pins in s_coalescePinnedNodes — those release
// at onMapUnload().
//
// Caller responsibility: set s_coalesceLayoutReady / s_coalesceEnabled /
// s_coalesceArmed to false AFTER calling the helper, then return. The
// helper deliberately does NOT mutate the flags so Step 12B.5 (which
// logs a different "(forced)" reason string before invoking the
// rollback) keeps logging-vs-state-change ordering symmetric with the
// natural failure path.
// STATICPROP-MATERIAL-ORM-1 — pick the BPTC internalformat. Albedo arrays are
// sRGB; ORM arrays are LINEAR data (occlusion/roughness/metallic are not color)
// so they MUST use the non-sRGB variant or the values would be gamma-decoded.
GLenum bptcInternalFormatFor(bool isSrgb) {
    return isSrgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                  : GL_COMPRESSED_RGBA_BPTC_UNORM;
}

// COMPRESSION-BC7-STATICPROP-2 — release all bucketed arrays + reset the
// bucket bookkeeping vectors. No-op (vectors empty) when the gate is OFF, so
// gate-OFF teardown is byte-identical.
void staticPropReleaseBuckets() {
    for (GLuint a : s_bucketArrays) {
        if (a) glDeleteTextures(1, &a);
    }
    s_bucketArrays.clear();
    s_bucketInfo.clear();
    s_packetBucketIndex.clear();
    s_slotBucketIndex.clear();
    s_bucketCmdCount.clear();
    // STATICPROP-MATERIAL-ORM-1 — release the per-bucket ORM sibling arrays.
    // Empty (no-op) when the gate is OFF.
    for (GLuint a : s_ormBucketArrays) {
        if (a) glDeleteTextures(1, &a);
    }
    s_ormBucketArrays.clear();
    s_ormLayerHasMap.clear();
}

void coalesceRollbackTexBuild(std::vector<DWORD>& tempPins) {
    if (s_texArrayOff) { glDeleteTextures(1, &s_texArrayOff); s_texArrayOff = 0; }
    if (s_texArrayOn)  { glDeleteTextures(1, &s_texArrayOn);  s_texArrayOn  = 0; }
    staticPropReleaseBuckets();
    if (mcTextureManager) {
        for (DWORD nodeIdx : tempPins) {
            mcTextureManager->unpinNode(nodeIdx);
        }
    }
    tempPins.clear();
}
}  // namespace gos_static_prop_batcher_detail
