// Vulkan CPU object-light rows, baked static slots and per-frame dedup caches.
// Declared in the class headers; shared names in txmmgr_internal.h.
#include "txmmgr_internal.h"
#include "vk_object_lights.h"
#include <optional>
#include "mc2fmt.h"

// PERF FIX 2026-05-07: hash-based dedup map. The previous linear-scan
// implementation walked all existing entries doing 900-byte memcmp per
// comparison. Tracy capture (728 trees, default config) showed 8.5 ms
// total per frame — every actor scanning every prior actor's terrain-
// light-scaled struct, all unique due to per-actor aRGB. Hash-first
// approach: O(1) average lookup; memcmp only on hash match (collision
// verify). Map is reset by resetLightData() at frame start to mirror
// lightDataStructuresCount=0 reset.
// Hash collisions: 64-bit FNV-1a over ~1KB struct. Birthday-paradox
// probability ~10⁻⁷ per 728-actor frame; even on collision, memcmp
// fails-safe by falling through to the append path. Logical duplicates
// from collisions are correct (same-data → same-result), just waste a
// UBO slot. Acceptable.
namespace {
    std::vector<TG_HWLightsData> lightData_;
    uint32_t lightDataStructuresCapacity = 0;
    uint32_t lightDataStructuresCount = 0;
    bool s_active = false;
    // FRAME-JOBS-2 precondition: single mutex protecting both dedup maps AND
    // the lightData_ grow path. One lock covers the whole light-slot allocation
    // sequence (map lookup -> grow-if-needed -> table write -> map insert) so
    // the check-then-act is atomic from the callers' perspective.
    // recursive_mutex: addLightDataStructureWithPerActorColor calls
    // addLightDataStructure on the same thread while holding this lock.
    static std::recursive_mutex s_lightDataMapMu;

    static std::unordered_map<uint64_t, uint32_t> s_lightDataDedupMap;

    struct CachedSceneLightTemplate {
        TG_HWLightsData data;
        uint32_t actorLightSlot;
    };

    static std::unordered_map<uint64_t, CachedSceneLightTemplate> s_sceneLightTemplateMap;
    static uint32_t s_sceneLightTemplateFrame = 0xFFFFFFFFu;

    static inline uint64_t fnv1a_64_bytes(uint64_t h, const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    static inline uint64_t fnv1a_64_struct(const void* data, size_t bytes) {
        // FNV-1a 64-bit. Walk in uint64_t chunks for fewer iterations
        // (~110 iters for ~900 bytes vs ~900 iters byte-at-a-time).
        const uint64_t* p64 = static_cast<const uint64_t*>(data);
        const size_t chunks = bytes / sizeof(uint64_t);
        const size_t tail   = bytes % sizeof(uint64_t);
        uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
        for (size_t i = 0; i < chunks; ++i) {
            h ^= p64[i];
            h *= 0x100000001b3ULL;            // FNV prime
        }
        if (tail) {
            const uint8_t* tailp = reinterpret_cast<const uint8_t*>(&p64[chunks]);
            for (size_t i = 0; i < tail; ++i) {
                h ^= tailp[i];
                h *= 0x100000001b3ULL;
            }
        }
        return h;
    }

    static inline uint64_t fnv1a_64_u32(uint64_t h, uint32_t v) {
        return fnv1a_64_bytes(h, &v, sizeof(v));
    }

    static inline uint64_t fnv1a_64_bool(uint64_t h, bool v) {
        const uint8_t b = v ? 1 : 0;
        return fnv1a_64_bytes(h, &b, sizeof(b));
    }

    static inline std::optional<uint32_t> decomposeFirstActiveLightColor(TG_HWLightsData* lights) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != nullptr) && listOfLights[iLight]->active) {
                const DWORD startLight = listOfLights[iLight]->GetaRGB();
                lights->lightColor[0][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
                lights->lightColor[0][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
                lights->lightColor[0][2] = ((startLight) & 0x000000ff) / 255.0f;
                lights->lightColor[0][3] = 1.0f;
                return 0u;
            }
        }

        return std::nullopt;
    }

    static inline std::optional<uint32_t> firstActiveLightSourceIndex() {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        if (!listOfLights)
            return std::nullopt;

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            if ((listOfLights[iLight] != nullptr) && listOfLights[iLight]->active)
                return iLight;
        }
        return std::nullopt;
    }

    static uint64_t sceneLightTemplateKey(uint32_t actorLightSourceIndex) {
        const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
        const DWORD numLights = TG_Shape::s_numLights;

        uint64_t h = 0xcbf29ce484222325ULL;
        h = fnv1a_64_u32(h, g_mc2FrameCounter);
        h = fnv1a_64_u32(h, numLights);

        const uintptr_t listPtr = reinterpret_cast<uintptr_t>(listOfLights);
        h = fnv1a_64_bytes(h, &listPtr, sizeof(listPtr));

        for (DWORD iLight = 0; iLight < numLights; ++iLight) {
            const TG_LightPtr light = listOfLights ? listOfLights[iLight] : nullptr;
            h = fnv1a_64_bool(h, light != nullptr);
            if (!light)
                continue;

            h = fnv1a_64_bool(h, light->active);
            if (!light->active)
                continue;

            h = fnv1a_64_u32(h, light->lightType);
            h = fnv1a_64_bytes(h, &light->lightToWorld, sizeof(light->lightToWorld));
            h = fnv1a_64_bytes(h, &light->closeDistance, sizeof(light->closeDistance));
            h = fnv1a_64_bytes(h, &light->farDistance, sizeof(light->farDistance));
            h = fnv1a_64_bytes(h, &light->oneOverDistance, sizeof(light->oneOverDistance));

            // Per-actor terrain scaling mutates the first active light color
            // before CacheGpuLightData(). Other active light colors remain part
            // of the scene key. If future gameplay makes more colors per-actor,
            // widen this patch/key rule instead of reusing the template.
            if (iLight != actorLightSourceIndex) {
                const DWORD argb = light->GetaRGB();
                h = fnv1a_64_u32(h, argb);
            }
        }

        return h;
    }

    // [LIGHTBRIDGE v1] C5/C6 populate sizing recon (env-gated, measure-only,
    // demote-not-delete). The handoff's prescribed shape_emit_ns counter was
    // grep-proven to wrap only the C1 legacy leaf (tgl.cpp scope, gated
    // !eligibleForGpuObjects) which is structurally dead for the GPU-batched
    // population this slice targets; the C5/C6 path
    // (addLightDataStructureWithPerActorColor, the sole caller route from
    // GatherGpuObjectLightDataOnly) had NO armed-path attribution. This is
    // that attribution: ONE std::chrono pair per call (NOT a per-call Tracy
    // zone / not nested -> ~30-50ns/call observer effect, << the claimed
    // multi-ms lever; the 6-nested-scope cost-split apparatus that inflated
    // terrain numbers is the anti-pattern this deliberately avoids), summed
    // per-frame, drained at resetLightData() (frame-start). Gated on the SAME
    // MC2_OBJECT_RECON_TRACY the handoff capture protocol already sets, so the
    // protocol is unchanged. tmpl_hit counts the template-cache-hit calls
    // whose trailing FNV+memcmp is the redundancy the slice retires.
    static bool     s_lbInit = false;
    static bool     s_lbEnabled = false;
    static uint64_t s_lbFrameNs = 0,    s_lbMonoNs = 0;
    static uint64_t s_lbFrameCalls = 0, s_lbMonoCalls = 0;
    static uint64_t s_lbFrameHit = 0,   s_lbMonoHit = 0;
    static uint64_t s_lbFrameMiss = 0,  s_lbMonoMiss = 0;
    static uint64_t s_lbFrameNo = 0,    s_lbMonoNo = 0;
    static uint32_t s_lbFirstDataFrame = 0;

    static inline void lbInitFromEnv() {
        if (s_lbInit) return;
        s_lbInit = true;
        s_lbEnabled = mc2::gate::get<"MC2_OBJECT_RECON_TRACY">();
        if (s_lbEnabled) {
            MC2_DIAG_PUTS("[LIGHTBRIDGE v1] event=enabled note=c5c6_populate_sizing_active");
            std::fflush(stdout);
        }
    }

    struct LbScope {
        std::chrono::steady_clock::time_point t0;
        LbScope() : t0(std::chrono::steady_clock::now()) {}
        ~LbScope() {
            s_lbFrameNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
            ++s_lbFrameCalls;
        }
    };

    static void lbDrainPerFrame(uint32_t frame) {
        if (!s_lbInit) lbInitFromEnv();
        const bool hadData = (s_lbFrameCalls != 0);
        if (hadData && s_lbFirstDataFrame == 0) s_lbFirstDataFrame = frame;

        if (s_lbEnabled && hadData) {
            char buf[512];
            mc2::fmt::to_buf(buf, "[LIGHTBRIDGE v1] frame={:d} populate={{ns:{:d},calls:{:d},tmpl_hit:{:d},tmpl_miss:{:d},no_actor_light:{:d}}}", mc2::fmt::as_u32(static_cast<unsigned>(frame)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbFrameNs)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbFrameCalls)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbFrameHit)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbFrameMiss)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbFrameNo)));
            MC2_DIAG_PUTS(buf); crashbundle_append(buf); std::fflush(stdout);
        }

        s_lbMonoNs   += s_lbFrameNs;   s_lbMonoCalls += s_lbFrameCalls;
        s_lbMonoHit  += s_lbFrameHit;  s_lbMonoMiss  += s_lbFrameMiss;
        s_lbMonoNo   += s_lbFrameNo;
        s_lbFrameNs = s_lbFrameCalls = s_lbFrameHit = s_lbFrameMiss = s_lbFrameNo = 0;

        if (frame > 0 && (frame % 600) == 0 && s_lbMonoCalls != 0) {
            char buf[512];
            mc2::fmt::to_buf(buf, "[LIGHTBRIDGE v1] summary={:d} populate={{ns:{:d},calls:{:d},tmpl_hit:{:d},tmpl_miss:{:d},no_actor_light:{:d}}} ns_per_call={:d} first_data_frame={:d}", mc2::fmt::as_u32(static_cast<unsigned>(frame)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbMonoNs)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbMonoCalls)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbMonoHit)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbMonoMiss)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_lbMonoNo)), mc2::fmt::as_u64(static_cast<unsigned long long>((s_lbMonoCalls ? s_lbMonoNs / s_lbMonoCalls : 0))), mc2::fmt::as_u32(static_cast<unsigned>(s_lbFirstDataFrame)));
            MC2_DIAG_PUTS(buf); crashbundle_append(buf); std::fflush(stdout);
        }
    }

    // ---- [LIGHTBRIDGE v1] SUBSTITUTIVE REPOINT (slice, not recon) ----------
    // Retires the per-call 1792B fnv1a_64_struct + 1792B memcmp for the
    // C5/C6/C7 GPU-object populate. On the 99.3% template-cache-hit common
    // path the slot is resolved by a tiny (templateKey + per-actor-aRGB)
    // key instead. See docs/superpowers/plans/
    // 2026-05-17-addlightdatastructure-bridge-retirement.md. Kill-switch
    // MC2_LIGHTBRIDGE (default ON; =0 restores legacy FNV/memcmp bit-for-
    // bit). Slot cache is per-frame, cleared in resetLightData alongside
    // s_lightDataDedupMap (same frame-start slot-reset invariant).
    struct LightSlotEntry { uint64_t tmpl; uint32_t actorARGB; uint32_t slot; };
    static std::unordered_map<uint64_t, LightSlotEntry> s_lightSlotByActorKey;

    static bool s_lbRepointInit = false;
    static bool s_lbRepointEnabled = true;          // default ON
    static bool s_lbFirstPopulateLogged = false;
    static bool s_lbC6FastpathLogged = false;

    static inline void lbRepointInitFromEnv() {
        if (s_lbRepointInit) return;
        s_lbRepointInit = true;
        // OFF only on explicit "0"; any other value (or unset) = ON.
        s_lbRepointEnabled = mc2::gate::get<"MC2_LIGHTBRIDGE">();
        MC2_DIAGF("[LIGHTBRIDGE v1] event=enabled mode=%s\n",
                    s_lbRepointEnabled ? "repoint" : "legacy");
        std::fflush(stdout);
    }

    // ---- [LIGHTBAKE v1] static-actor mission-load lighting bake --------
    // Static bdactor bldg/tree actors have a mission-constant per-actor
    // light (position-derived getTerrainLight + frozen sun/nightFactor;
    // no dynamic emitters -- lighting_is_mission_load_static_no_dynamic_
    // emitters.md).
    // ★ PROP-BAKE-TIME-1 (2026-07-30): that invariant is TRUE on 45 of the 46
    // stock missions and FALSE on mc2_10, which authors live rain. Weather
    // rewrites Camera::ambient*/light* every frame as dayAmbient/rainLightLevel
    // (code/weather.cpp) and mc2_10's storm ramps rainLightLevel 3.0 -> 4.0,
    // so its published ambient walks 63/255 -> 48/255 across the mission while
    // every baked row stays frozen at whatever it read. Proven offline by
    // tools/object_light_row_harness ::mc2_10_storm_ambient_is_not_mission_constant
    // (and bounded by ::non_rainy_missions_are_immune_to_bake_time); full
    // derivation and the designed-but-unshipped repair are in the PROP-BAKE-TIME-1
    // block above mc2CacheOrBakeStaticGpuLight in mclib/bdactor.cpp. Do not cite
    // the sentence above as unconditional.
    // Lazily bake the post-decompose TG_HWLightsData once
    // (keyed by monotonic-never-reused registry recipeIndex) then re-emit
    // the constant into a per-frame slot WITHOUT the per-frame
    // GatherLights+decompose+template recompute (the retired CPU zone).
    // Shape-C precedent: recompute dies, the O(1) per-frame slot WRITE
    // stays. Post-SSBO this is the full design (no window/partition --
    // see docs/superpowers/plans/2026-05-17-static-lighting-bake-SIMPLIFIED.md).
    // s_bakedStaticLight = mission-scoped struct SOURCE (recipeIndex ->
    // baked TG_HWLightsData; cleared on mission unload + per-recipe on
    // invalidate; NOT per-frame). [LIGHTBAKE v2] persistent static table:
    // each static recipe owns a PERMANENT lightData_ slot == recipeIndex
    // in [0..s_staticLightHighWater); written ONCE at bake (CPU mirror),
    // re-shipped idempotently by the unchanged per-frame whole-buffer
    // upload (no per-frame addLightDataStructure / FNV / memcmp). S =
    // max(recipeIndex)+1; resetLightData rebases the dynamic count to S
    // so dynamic appends never collide into [0..S).
    static std::unordered_map<int32_t, TG_HWLightsData> s_bakedStaticLight;
    static uint32_t                                      s_staticLightHighWater = 0;
    // [LIGHTSLOT v1] diagnostic-only counters (Task 0 cardinality gate). Pure
    // counting, no behavior change. Read via mc2LightSlot* accessors below.
    static uint64_t s_lightSlotDedupHits = 0;   // addLightDataStructure FNV+memcmp matches
    static uint64_t s_lightSlotActorKeyHits = 0; // per-actor-color slot-cache hits
    static bool s_bakeInit = false;
    static bool s_bakeEnabled = true;          // default ON
    static bool s_bakeFirstLogged = false;

    static inline void bakeInitFromEnv() {
        if (s_bakeInit) return;
        s_bakeInit = true;
        s_bakeEnabled = mc2::gate::get<"MC2_LIGHTBAKE">();
        MC2_DIAGF("[LIGHTBAKE v1] event=enabled mode=%s\n",
                    s_bakeEnabled ? "bake" : "passthrough");
        std::fflush(stdout);
        if (mc2_diag::tagEnabled("LIGHTBAKE_PROOF")) {
            char diag_buf[128];
            mc2::fmt::to_buf(diag_buf, "{{\"event\":\"enabled\",\"mode\":\"{}\"}}", mc2::fmt::safe(s_bakeEnabled ? "bake" : "passthrough"));
            mc2_diag::writeEvent("LIGHTBAKE_PROOF", 1, 0, diag_buf);
        }
    }
}

// Cross-TU kill-switch view (C6 repoint lives in msl.cpp). Free function
// to avoid pulling render_submissions.h into msl.cpp. Lazy-inits the env read.
bool mc2LightBridgeRepointEnabled()
{
    lbRepointInitFromEnv();
    return s_lbRepointEnabled;
}

// ---- [LIGHTBAKE v1] cross-TU free fns (bdactor/msl/registry call these;
// free fns avoid pulling render_submissions.h into those TUs) ----
bool mc2LightBakeEnabled()
{
    bakeInitFromEnv();
    return s_bakeEnabled;
}

// [LIGHTBAKE-PROOF v1] read-only S accessor. s_staticLightHighWater lives in the
// anonymous namespace above but is visible throughout this TU (same pattern as
// mc2GetBakedStaticLight reading s_bakedStaticLight). Exposed as a cross-TU free
// fn for the stability trace below and the upload-split caller (Task 4) — no
// render_submissions.h coupling into the consuming TUs.
uint32_t mc2StaticLightHighWater()
{
    return s_staticLightHighWater;
}

#ifdef MC2_VULKAN_ISLAND
// VK-MECH-MATERIAL-LIGHTING-1: global fog-color snapshot (RGB, GL scene-UBO order),
// set once per frame at the scene-UBO upload site. Read by the VK mech island feed.
static float s_vkSceneFogRGB[3] = {0.0f, 0.0f, 0.0f};
void mc2SetSceneFogColorRGB(float r, float g, float b) { s_vkSceneFogRGB[0]=r; s_vkSceneFogRGB[1]=g; s_vkSceneFogRGB[2]=b; }
extern "C" void mc2GetSceneFogColorRGB(float out[3]) { out[0]=s_vkSceneFogRGB[0]; out[1]=s_vkSceneFogRGB[1]; out[2]=s_vkSceneFogRGB[2]; }
#endif

bool mc2GetBakedStaticLight(int32_t recipeIndex, TG_HWLightsData& out)
{
    if (recipeIndex < 0) return false;
    auto it = s_bakedStaticLight.find(recipeIndex);
    if (it == s_bakedStaticLight.end()) return false;
    out = it->second;
    return true;
}

// [LIGHTBRIDGE-BAKED-PROBE-1] Presence-only probe — avoids 1792B struct copy
// when the callee (EmitBakedGpuLightData) only needs the key (recipeIndex).
// Use this in touch()/touchSerialCommit() hot paths instead of mc2GetBakedStaticLight.
bool mc2IsBakedStaticLightPresent(int32_t recipeIndex)
{
    if (recipeIndex < 0) return false;
    return s_bakedStaticLight.count(recipeIndex) != 0;
}

void mc2SetBakedStaticLight(int32_t recipeIndex, const TG_HWLightsData& in)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight[recipeIndex] = in;
    if (!s_bakeFirstLogged) {
        s_bakeFirstLogged = true;
        MC2_DIAGF("[LIGHTBAKE v1] event=first_bake recipe=%d\n", recipeIndex);
        std::fflush(stdout);
    }
}

// Erased on destruction/LOD multi-swap (via invalidateStaticRegistration
// -> GpuStaticPropRegistry::invalidate) so the next CacheGpuLightData
// lazily re-bakes the same position-derived constant for the new multi.
void mc2EraseBakedStaticLight(int32_t recipeIndex)
{
    if (recipeIndex < 0) return;
    s_bakedStaticLight.erase(recipeIndex);
}

// LIGHTBAKE-REBAKE-ON-READY-1: SOFT invalidation -- drop only the cache map so
// every recipe lazily re-gathers with the now-final mission lighting. The
// permanent slot indices (s_staticLightHighWater) are preserved: re-bakes write
// the SAME recipeIndex slots via mc2WriteStaticLightSlot, so staticReg
// lightDataIndex values held by instances stay valid throughout.
void mc2InvalidateBakedStaticLightCache()
{
    s_bakedStaticLight.clear();
}

// Mission unload: recipeIndex restarts per mission -> a stale entry
// would alias a different actor. Drop the whole mission-scoped map.
void mc2ClearAllBakedStaticLight()
{
    // Co-located with GpuStaticPropRegistry::destroy s_recipeRanges.clear()
    // (gos_static_prop_registry.cpp) via mission.cpp -> next mission's
    // recipeIndex restarts at 0 against a fresh prefix.
    s_bakedStaticLight.clear();
    s_staticLightHighWater = 0;
}

// [G1-STATIC-EAGER-LIGHT v1] Probe helper: returns true when lightData_[recipeIndex]
// has been written by a real bake (numLights_ > 0). Default-constructed slots and
// the G1 ambient fallback both set numLights_=1, so this is specifically the check
// "has the slot ever been written by mc2WriteStaticLightSlot (static prefix only)".
// Used by MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE to count never-baked alive props.
// Checks the persistent static prefix [0..s_staticLightHighWater) only:
// slot beyond S means registerRecipe ran but bakeStaticLightSlot was never called.
bool mc2IsStaticLightSlotBaked(int32_t recipeIndex)
{
    if (recipeIndex < 0 || !mc2::vkLights::active()) return false;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri >= s_staticLightHighWater) return false;  // slot never written
    TG_HWLightsData slot{};
    if (!mc2::vkLights::copyLightSlot(ri, slot)) return false;
    return slot.numLights_ > 0;
}

// PROP-RECIPE-BAKE-1 (diagnostic, read-only). Summarize a permanent static
// light slot the way static_prop.vert actually consumes it, so a probe can
// compare a dark prop against a lit building on the SAME ObjectLights row
// semantics rather than on raw struct bytes:
//   amb[] = SUM of every TG_LIGHT_AMBIENT lightColor row  -> the shader's
//           orientation-free `ambientTerm`.
//   sun[] = FIRST TG_LIGHT_INFINITE/INFINITEWITHFALLOFF lightColor row ->
//           the shader's `sunColor`, which is scaled by NdotL.
// Mirrors the scan at shaders/static_prop.vert exactly (type lives in
// lightDir[i][3], i.e. light_dir[i].w on the GLSL side).
// Returns false when the slot was never written by mc2WriteStaticLightSlot.
bool mc2PeekStaticLightSlotSummary(int32_t recipeIndex, int* outNumLights,
                                   float amb[3], float sun[3])
{
    if (outNumLights) *outNumLights = 0;
    if (amb) amb[0] = amb[1] = amb[2] = 0.0f;
    if (sun) sun[0] = sun[1] = sun[2] = 0.0f;
    if (recipeIndex < 0 || !mc2::vkLights::active()) return false;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri >= s_staticLightHighWater) return false;   // slot never written
    TG_HWLightsData slot{};
    if (!mc2::vkLights::copyLightSlot(ri, slot)) return false;
    if (outNumLights) *outNumLights = slot.numLights_;
    const int n = (slot.numLights_ < MAX_HW_LIGHTS_IN_WORLD)
                ? slot.numLights_ : MAX_HW_LIGHTS_IN_WORLD;
    bool sunFound = false;
    for (int i = 0; i < n; ++i) {
        const int lt = static_cast<int>(slot.lightDir[i][3]);
        if (lt == TG_LIGHT_AMBIENT) {
            if (amb) { amb[0] += slot.lightColor[i][0];
                       amb[1] += slot.lightColor[i][1];
                       amb[2] += slot.lightColor[i][2]; }
        } else if (!sunFound &&
                   (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF)) {
            if (sun) { sun[0] = slot.lightColor[i][0];
                       sun[1] = slot.lightColor[i][1];
                       sun[2] = slot.lightColor[i][2]; }
            sunFound = true;
        }
    }
    return true;
}

// [LIGHTBAKE v2] Persistent static slot write. Replaces the retired
// per-frame mc2SubmitBakedLightSlot (which re-ran addLightDataStructure
// -> 1792B FNV + 1792B memcmp every frame per recipe). Called ONCE per
// recipe at bake (and again only on invalidate re-bake): mirror the
// constant into the permanent CPU slot lightData_[recipeIndex] and
// advance S. The unchanged per-frame whole-buffer upload then ships it
// to the GPU every frame idempotently (resetLightData never memsets
// lightData_ contents). No GL call, no FNV, no memcmp.
void mc2WriteStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0 || !mc2::vkLights::active()) return;
    mc2::vkLights::bakeStaticLightSlot(recipeIndex, baked);
}

// [LIGHTSLOT v1] Task 0 cardinality-gate accessors (diagnostic only).
// B = permanent baked static slots (high-water); H components = dedup +
// actor-key cache hits; U-source = total live light-table count.
uint32_t mc2LightSlotBakedHighWater()   { return s_staticLightHighWater; }
uint64_t mc2LightSlotDedupHits()         { return s_lightSlotDedupHits; }
uint64_t mc2LightSlotActorKeyHits()      { return s_lightSlotActorKeyHits; }
uint32_t mc2LightSlotTableCount()        { return mc2::vkLights::active() ? mc2::vkLights::getLightStructCount() : 0u; }

uint32_t mc2::vkLights::addLightDataStructure(TG_HWLightsData* light_data)
{
    ZoneScopedN("addLightDataStructure scan");
    // FRAME-JOBS-2 precondition: lock covers map lookup + grow-if-needed +
    // table write + map insert as a single atomic sequence.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    const uint64_t hash = fnv1a_64_struct(light_data, sizeof(TG_HWLightsData));
    auto it = s_lightDataDedupMap.find(hash);
    if (it != s_lightDataDedupMap.end()) {
        const uint32_t slot = it->second;
        // Verify with memcmp on hash match (collision safety).
        if (slot < lightDataStructuresCount &&
            0 == memcmp(lightData_.data() + slot, light_data, sizeof(TG_HWLightsData))) {
            ++s_lightSlotDedupHits;   // [LIGHTSLOT v1] content dedup matched an existing slot
            return slot;
        }
        // Hash collision (vanishingly rare) — fall through to append.
        // We don't update the map; future lookups of the colliding hash
        // will continue to find the existing slot via memcmp-verify.
    }

    if(lightDataStructuresCount + 1 >= lightDataStructuresCapacity)
    {
        lightDataStructuresCapacity += 128;
        lightData_.resize(lightDataStructuresCapacity);
    }

    lightData_[lightDataStructuresCount] = *light_data;
    uint32_t rv = lightDataStructuresCount;
    lightDataStructuresCount++;
    s_lightDataDedupMap.emplace(hash, rv);  // O(1) avg insert
    return rv;
}

uint32_t mc2::vkLights::addLightDataStructureWithPerActorColor(TG_HWLightsData* light_data)
{
    gosASSERT(light_data);
    LbScope _lb_;  // [LIGHTBRIDGE v1] C5/C6 populate sizing (RAII, all return paths)
    // FRAME-JOBS-2 precondition: protects s_sceneLightTemplateMap, s_lightSlotByActorKey,
    // and (via the nested addLightDataStructure call) s_lightDataDedupMap + lightData_ grow.
    // recursive_mutex allows the nested call to re-acquire on the same thread.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    if (s_sceneLightTemplateFrame != g_mc2FrameCounter) {
        s_sceneLightTemplateMap.clear();
        s_sceneLightTemplateFrame = g_mc2FrameCounter;
    }

    const auto actorLightSource = firstActiveLightSourceIndex();
    if (!actorLightSource) {
        ++s_lbFrameNo;  // [LIGHTBRIDGE v1] no per-actor light (direct passthrough)
        GatherLightsParameters(light_data);
        return addLightDataStructure(light_data);
    }

    const uint64_t key = sceneLightTemplateKey(*actorLightSource);
    auto it = s_sceneLightTemplateMap.find(key);
    if (it == s_sceneLightTemplateMap.end()) {
        ++s_lbFrameMiss;  // [LIGHTBRIDGE v1] template miss (GatherLightsParameters runs)
        CachedSceneLightTemplate entry;
        GatherLightsParameters(&entry.data);
        entry.actorLightSlot = decomposeFirstActiveLightColor(&entry.data).value_or(0xFFFFFFFFu);
        it = s_sceneLightTemplateMap.emplace(key, entry).first;
    } else {
        ++s_lbFrameHit;  // [LIGHTBRIDGE v1] template hit (trailing FNV+memcmp = retirable redundancy)
    }

    *light_data = it->second.data;
    const bool perActor = (it->second.actorLightSlot != 0xFFFFFFFFu);
    if (perActor)
        (void)decomposeFirstActiveLightColor(light_data);

    // [LIGHTBRIDGE v1] substitutive repoint: on the (template + per-actor-
    // color) cache hit, return the resolved slot directly and SKIP the
    // 1792B fnv1a_64_struct + 1792B memcmp in addLightDataStructure.
    // Symmetry invariant (load-bearing — do not break without re-deriving
    // the key): actorLightSource == firstActiveLightSourceIndex() is the
    // SAME light decompose mutates into lightColor[0][0..3] AND the SAME
    // light sceneLightTemplateKey deliberately excludes from the template
    // key (txmmgr.cpp). actorARGB closes exactly that excluded
    // gap. perActor==false => no decompose => template key alone suffices
    // (actorARGB folds to 0, combined == key).
    if (!s_lbRepointInit) lbRepointInitFromEnv();
    if (s_lbRepointEnabled) {
        const uint32_t actorARGB = perActor
            ? static_cast<uint32_t>(TG_Shape::s_listOfLights[*actorLightSource]->GetaRGB())
            : 0u;
        const uint64_t combined =
            key ^ (static_cast<uint64_t>(actorARGB) * 0x9E3779B97F4A7C15ULL);
        auto sit = s_lightSlotByActorKey.find(combined);
        if (sit != s_lightSlotByActorKey.end() &&
            sit->second.tmpl == key && sit->second.actorARGB == actorARGB) {
            ++s_lightSlotActorKeyHits;  // [LIGHTSLOT v1] per-actor-color slot-cache hit
            return sit->second.slot;   // no FNV, no memcmp
        }
        const uint32_t slot = addLightDataStructure(light_data);
        s_lightSlotByActorKey[combined] =
            LightSlotEntry{ key, actorARGB, slot };
        if (!s_lbFirstPopulateLogged) {
            s_lbFirstPopulateLogged = true;
            MC2_DIAG_PUTS("[LIGHTBRIDGE v1] event=first_populate");
            std::fflush(stdout);
        }
        return slot;
    }

    return addLightDataStructure(light_data);
}

void mc2::vkLights::resetLightData()
{
    // [LIGHTBRIDGE v1] frame-start boundary: flush the just-completed frame's
    // C5/C6 populate sizing (same boundary the dedup-map reset relies on).
    lbDrainPerFrame(g_mc2FrameCounter);
    // FRAME-JOBS-2 precondition: lock during map clears and count rebase.
    std::lock_guard<std::recursive_mutex> _lk(s_lightDataMapMu);

    // [LIGHTBAKE v2] Rebase the DYNAMIC allocator base to S (the static
    // prefix high-water) when the bake is on, so dynamic appends start
    // above [0..S) and addLightDataStructure never returns a slot < S
    // (rv = count, count never < S -> the dedup maps below stay
    // dynamic-only by construction, exactly as before but rebased). With
    // MC2_LIGHTBAKE=0 the persistent table is off -> base 0 (else the
    // dynamic allocator would collide into a non-rebased prefix).
    lightDataStructuresCount = mc2LightBakeEnabled() ? s_staticLightHighWater : 0;
    // PERF FIX 2026-05-07: clear the dedup map alongside the count reset.
    // Both must reset together — dynamic slot indices restart from the
    // base (S or 0) each frame, so any stale hash→slot entries from the
    // prior frame are invalid. (Static [0..S) is NEVER cleared here:
    // s_bakedStaticLight + lightData_[0..S) persist across frames; that
    // is the whole point — resetLightData does not memset lightData_.)
    s_lightDataDedupMap.clear();
    s_sceneLightTemplateMap.clear();
    s_lightSlotByActorKey.clear();  // [LIGHTBRIDGE v1] per-frame slot cache
    s_sceneLightTemplateFrame = 0xFFFFFFFFu;
}

// [LIGHTBAKE v2] Persistent static slot writer (member: needs private
// lightData_/capacity access). Grow lightData_ so [recipeIndex] is
// addressable (preserving ALL existing contents -- static prefix AND any
// transient dynamic entries -- via the same realloc+memcpy pattern as
// the addLightDataStructure grow), mirror the baked constant into the
// permanent slot, advance S. Called once per recipe at bake / invalidate
// re-bake -- NOT per frame.
void mc2::vkLights::bakeStaticLightSlot(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    if (recipeIndex < 0) return;
    const uint32_t ri = static_cast<uint32_t>(recipeIndex);
    if (ri + 1 >= lightDataStructuresCapacity)
    {
        uint32_t newCap = lightDataStructuresCapacity;
        while (ri + 1 >= newCap) newCap += 128;            // +128 chunks, like the dynamic grow
        // The whole old array survives the resize (static prefix is in [0..S);
        // keeping only `count` would drop persisted static slots).
        lightData_.resize(newCap);
        lightDataStructuresCapacity = newCap;
    }
    lightData_[ri] = baked;                                 // CPU mirror (persists)
    // 2A: the per-leaf permanent light index is NOT recipeIndex — it is the
    // owning RANGE's lightDataIndex (all leaves of a multi-leaf range share one
    // light slot, matching legacy flush). That propagation now lives in the
    // registry's markVisible() (range-aware). The earlier bake-time per-recipe
    // persist was wrong for multi-leaf ranges (compare-oracle caught it) and is
    // removed. The light-table SLOT content here is unchanged.
    if (ri + 1 > s_staticLightHighWater) s_staticLightHighWater = ri + 1;
    // CRITICAL: the frame a recipe FIRST bakes, this-frame's count was
    // set to the OLD (smaller) S at frame-start resetLightData. Without
    // this bump, a later dynamic addLightDataStructure append THIS frame
    // could land on slot `ri` and clobber the just-written permanent
    // static slot (wrong-light, never re-baked). Raising the live count
    // to S keeps same-frame dynamic appends strictly above [0..S). Only
    // skips some low dynamic slots that frame (harmless -- dynamic is
    // per-frame ephemeral, rebuilt from S next resetLightData).
    if (lightDataStructuresCount < s_staticLightHighWater)
        lightDataStructuresCount = s_staticLightHighWater;
}

mc2::vkLights::LightSlotPeek mc2::vkLights::peekLightSlot(uint32_t idx)
{
    LightSlotPeek p = {-1, -1, 0.0f, 0.0f, 0.0f};
    if (idx >= lightDataStructuresCount || idx >= lightData_.size()) return p;
    const TG_HWLightsData& d = lightData_[idx];
    p.numLights = d.numLights_;
    if (d.numLights_ > 0) {
        // light_dir[i].w carries the light type (TG_LIGHT_AMBIENT=0, INFINITE=1,
        // INFINITEWITHFALLOFF=2, POINT=3, SPOT=4, TERRAIN=5). Mirrors GLSL
        // ObjectLights.light_dir[i].w in shaders/include/lighting.hglsl.
        p.firstType   = static_cast<int>(d.lightDir[0][3]);
        p.firstColorR = d.lightColor[0][0];
        p.firstColorG = d.lightColor[0][1];
        p.firstColorB = d.lightColor[0][2];
    }
    return p;
}

// [LIGHTBAKE-PROOF v1] read-only full-slot copy for the parity trace.
bool mc2::vkLights::copyLightSlot(uint32_t idx, TG_HWLightsData& out)
{
    if (idx >= lightDataStructuresCount || idx >= lightData_.size()) return false;
    out = lightData_[idx];
    return true;
}

// Native light feed: read-only CPU rows, valid for the remainder of the frame.
const void* mc2::vkLights::getLightDataPtr()
{
    return lightData_.empty() ? nullptr : lightData_.data();
}

uint64_t mc2::vkLights::getLightDataBytes()
{
    return static_cast<uint64_t>(lightDataStructuresCount) * sizeof(TG_HWLightsData);
}


void mc2::vkLights::start()
{
    lightDataStructuresCapacity = 128;
    lightDataStructuresCount = 0;
    lightData_.assign(lightDataStructuresCapacity, TG_HWLightsData());
    s_active = true;
}

void mc2::vkLights::shutdown()
{
    std::vector<TG_HWLightsData>().swap(lightData_);
    lightDataStructuresCapacity = lightDataStructuresCount = 0;
    s_active = false;
}

bool mc2::vkLights::active() { return s_active; }
uint32_t mc2::vkLights::getLightStructCount() { return lightDataStructuresCount; }
