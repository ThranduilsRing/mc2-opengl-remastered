#include "RenderCore/PropLodState.h"
#include "GameOS/gameos/static_prop_recipe_admission.h"
#include "doctest.h"
#include <cstdint>
#include <random>
#include <cmath>
#include <unordered_map>
#include "RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include <cstring>
#include <atomic>
#include <array>
#include "mc2fmt.h"
#include "diagnostic_trace.h"

static void propLodEraseBakedLight(int32_t) {}
#define mc2EraseBakedStaticLight propLodEraseBakedLight
namespace {
// Compile the production lifecycle functions verbatim. Only unrelated renderer,
// texture and shape services are stand-ins; LOD state and slot bindings are real.
using DWORD = uint32_t;
struct TG_InstanceId { uint64_t value = 1; };
struct TG_MultiShape {
    bool usesTypeMaterials() const { return true; }
    TG_InstanceId stableId() const { return {}; }
    long GetNumTextures() const { return 0; }
    DWORD GetTextureHandle(long) const { return 0xffffffffu; }
    void setCachedFrame(uint32_t) {}
};
struct TglInstanceRegistry {
    static TglInstanceRegistry& get() { static TglInstanceRegistry r; return r; }
    TG_MultiShape* resolve(TG_InstanceId) { return nullptr; }
};
namespace mc2::textures {
bool ready() { return false; }
void pin(DWORD) {}
void unpin(DWORD) {}
}
namespace mc2::fmt { using ::mc2::fmt::print_err; }
std::atomic<uint32_t> g_mc2FrameCounter{0};
PropLodStateMap s_lodState;
std::unordered_map<uint32_t, PropLodSlots> s_persistentLodSlots;
std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_persistentStaticStore;
std::vector<int> s_types(1);
uint64_t s_persistentStaticGen = 0, s_staticBaseBuiltGen = 0;
// Non-LOD resources touched by onMapUnload; no graphics service is invoked.
std::vector<int> s_coalescePinnedNodes, s_packetTexArrayLayer, s_packetUvScaleX, s_packetUvScaleY,
    s_typeInstanceCountPrevFrame, s_packetMaterialIdx, s_materialGpuTable, s_materialInventory,
    s_sortedPacketOrder, s_bucketsByType, s_lodGroups, s_lodTargetUses, s_lodMembers, s_lodEvaluated,
    s_staticBaseInstanceForType;
bool s_materialGpuSidecarValid = false;
int s_alphaOffCmdCount = 0, s_alphaOnCmdCount = 0;
class GpuStaticPropBatcher {
    std::array<int, 2> albedoArrays_{};
    int albedoKey_ = 0;
public:
    static GpuStaticPropBatcher& instance() { static GpuStaticPropBatcher b; return b; }
    void clearPersistentStatic();
    void appendPersistentStaticRange(const GpuStaticPropInstance*, uint32_t);
    void forgetLodState(const GpuStaticPropInstance&);
    void onMapUnload();
    void onMapLoad() {} // resource initialization is outside the lifecycle seam
};
namespace GpuStaticPropRegistry {
#include "prop_lod_range.inc"
std::vector<GpuStaticPropInstance> s_recipes;
std::vector<RecipeRange> s_recipeRanges;
std::vector<uint32_t> s_recipeRangeFreeList;
std::unordered_map<uint32_t, int32_t> s_typeIDToRecipeIndex;
bool s_recipeRecycle = true;
// Production admission policy (real header); registerRecipe/invalidate depend on it.
::GpuStaticPropRegistry::StaticPropRecipeAdmission s_recipeAdmission;
uint64_t s_registryGeneration = 0, s_totalPinCalls = 0, s_contentRevision = 0;
void releasePinsForRange(RecipeRange&) {}
void staticPropRegistryClearMaterialCache() {}
#define SP_TRACE(...) ((void)0)
#define TEX_LC_PIN(...) ((void)0)
#include "prop_lod_register.inc"
#include "prop_lod_invalidate.inc"
#undef TEX_LC_PIN
#undef SP_TRACE
}
#include "prop_lod_store.inc"
#include "prop_lod_forget.inc"
#include "prop_lod_unload.inc"

struct PropLodLifecycle {
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    PropLodLifecycle() { reset(); }
    ~PropLodLifecycle() { reset(); }
    void reset() {
        batcher.onMapUnload();
        GpuStaticPropRegistry::s_recipes.clear();
        GpuStaticPropRegistry::s_recipeRanges.clear();
        GpuStaticPropRegistry::s_recipeRangeFreeList.clear();
        GpuStaticPropRegistry::s_recipeAdmission.reset();
        GpuStaticPropRegistry::s_typeIDToRecipeIndex.clear();
    }
    static GpuStaticPropInstance prop(float x) {
        GpuStaticPropInstance p{};
        p.modelMatrix[3] = x;
        return p;
    }
    PropLodSlots& bind() {
        auto& slots = s_persistentLodSlots[0];
        slots.bind(s_lodState, std::span<const GpuStaticPropInstance>(s_persistentStaticStore[0]), propLodKey);
        return slots;
    }
    void append(std::initializer_list<GpuStaticPropInstance> props) {
        batcher.appendPersistentStaticRange(props.begin(), static_cast<uint32_t>(props.size()));
    }
};
}
#undef mc2EraseBakedStaticLight

TEST_CASE_FIXTURE(PropLodLifecycle, "prop_lod_seams: registry erase and recycle bumps binding revision") {
    TG_MultiShape shape;
    const auto a = prop(10), b = prop(20), c = prop(30);
    const auto contentRevision = GpuStaticPropRegistry::s_contentRevision;
    const int32_t index = GpuStaticPropRegistry::registerRecipe(&shape, {a});
    REQUIRE(index >= 0);
    CHECK(GpuStaticPropRegistry::s_contentRevision == contentRevision + 1);
    append({a, b});
    const auto erasedSlot = bind().indices[0];
    s_lodState[propLodKey(a)] = 2;
    s_lodState[propLodKey(b)] = 1;
    const auto revision = s_lodState.revision();
    GpuStaticPropRegistry::invalidate(index);
    CHECK(GpuStaticPropRegistry::s_contentRevision == contentRevision + 2);
    CHECK(s_lodState.revision() > revision);
    CHECK(s_lodState.find(propLodKey(a)) == nullptr);
    CHECK(GpuStaticPropRegistry::registerRecipe(&shape, {c}) == index);
    CHECK(s_lodState.resolve(propLodKey(c)) == erasedSlot);
    s_lodState[propLodKey(c)] = 4;
    // Unchanged store count: only the erase revision can force this rebind.
    CHECK(s_lodState.dense(bind().indices[0]) == 0);
    CHECK(s_lodState.dense(bind().indices[1]) == 1);
    CHECK(s_lodState[propLodKey(c)] == 4);
}

TEST_CASE_FIXTURE(PropLodLifecycle, "prop_lod_seams: destroying a tree removes its decoration copies") {
    TG_MultiShape shape;
    const auto stock=prop(10), copy=prop(100);
    const int32_t index=GpuStaticPropRegistry::registerRecipe(&shape,{stock,copy});
    REQUIRE(index>=0);
    REQUIRE(GpuStaticPropRegistry::s_recipeRanges[index].count==2);
    s_lodState[propLodKey(stock)]=1;
    s_lodState[propLodKey(copy)]=2;
    const auto begin=GpuStaticPropRegistry::s_recipeRanges[index].begin;
    GpuStaticPropRegistry::invalidate(index);
    CHECK(GpuStaticPropRegistry::s_recipeRanges[index].count==0);
    CHECK(s_lodState.find(propLodKey(stock))==nullptr);
    CHECK(s_lodState.find(propLodKey(copy))==nullptr);
    const GpuStaticPropInstance empty{};
    CHECK(std::memcmp(&GpuStaticPropRegistry::s_recipes[begin],&empty,sizeof(empty))==0);
    CHECK(std::memcmp(&GpuStaticPropRegistry::s_recipes[begin+1],&empty,sizeof(empty))==0);
}

TEST_CASE_FIXTURE(PropLodLifecycle, "prop_lod_seams: persistent rebuild handles same-count reorder and replacement") {
    const auto a = prop(10), b = prop(20), c = prop(30);
    append({a, b});
    bind();
    s_lodState[propLodKey(a)] = 1;
    s_lodState[propLodKey(b)] = 2;
    const auto revision = s_lodState.revision();
    batcher.clearPersistentStatic();
    CHECK(s_persistentLodSlots.empty());
    CHECK(s_persistentStaticStore[0].empty());
    CHECK(s_persistentStaticGen == UINT64_MAX);
    SUBCASE("save reload reorders equal-count recipes") { append({b, a}); }
    SUBCASE("save reload replaces an equal-count recipe") { append({c, a}); }
    CHECK(s_lodState.revision() == revision); // no erase to rescue stale bindings
    const auto& store = s_persistentStaticStore[0];
    const auto& slots = bind();
    for (size_t i = 0; i < store.size(); ++i)
        CHECK(s_lodState.dense(slots.indices[i]) == s_lodState[propLodKey(store[i])]);
    CHECK(s_lodState.dense(slots.indices[1]) == 1);
}

TEST_CASE_FIXTURE(PropLodLifecycle, "prop_lod_seams: persistent dynamic transitions discard cached bindings") {
    const auto a = prop(10), b = prop(20);
    append({a});
    bind();
    s_lodState[propLodKey(a)] = 2;
    // Persistent -> dynamic: rebuild removes A; its keyed hysteresis survives.
    batcher.clearPersistentStatic();
    append({b});
    CHECK(s_lodState.dense(bind().indices[0]) == 0);
    CHECK(s_lodState[propLodKey(a)] == 2);
    s_lodState[propLodKey(a)] = 1; // dynamic keyed route advances hysteresis
    // Dynamic -> persistent: append must invalidate even an existing binding.
    append({a});
    CHECK(s_persistentLodSlots.count(0) == 0);
    CHECK(s_lodState.dense(bind().indices[1]) == 1);
}

TEST_CASE_FIXTURE(PropLodLifecycle, "prop_lod_seams: mission teardown clears hysteresis and persistent bindings") {
    const auto a = prop(10);
    append({a});
    bind();
    s_lodState[propLodKey(a)] = 2;
    const auto revision = s_lodState.revision();
    batcher.onMapUnload();
    CHECK(s_lodState.size() == 0);
    CHECK(s_lodState.revision() > revision);
    CHECK(s_persistentLodSlots.empty());
    CHECK(s_persistentStaticStore.empty());
    append({a});
    CHECK(s_lodState.dense(bind().indices[0]) == 0);
}

TEST_CASE("prop_lod_state: dense bindings survive growth and invalidate on erase or reload") {
    PropLodStateMap state;
    PropLodSlots slots;
    const std::vector<uint64_t> keys{3, 7, 3};
    auto bind = [&] { slots.bind(state, std::span<const uint64_t>(keys), [](uint64_t k) { return k; }); };
    bind();
    CHECK(slots.indices[0] == slots.indices[2]);
    state.dense(slots.indices[0]) = 2;
    for (uint64_t k = 10; k < 10000; ++k) state[k] = 1;
    bind();
    CHECK(state.dense(slots.indices[0]) == 2);
    state.erase(3);
    state[123456] = 4; // recycle the erased dense slot before rebinding
    bind();
    CHECK(state.dense(slots.indices[0]) == 0);
    CHECK(state[123456] == 4);
    state.clear(); bind();
    CHECK(state.dense(slots.indices[1]) == 0);
}

TEST_CASE("prop_lod_state: cached persistent route matches keyed route through lifecycle and camera motion") {
    PropLodStateMap oldState, denseState;
    PropLodSlots slots;
    std::vector<GpuStaticPropInstance> persistent(4), dynamic;
    for (size_t i = 0; i < persistent.size(); ++i) {
        auto& inst = persistent[i];
        inst.typeID = 5; inst.actorTint = static_cast<uint32_t>(i + 1);
        inst.modelMatrix[3] = static_cast<float>((i / 2) * 200); // duplicate keys, distinct full payload
        inst.fogRGB[2] = static_cast<float>(i); inst.flags = static_cast<uint32_t>(i);
    }
    auto key = [](const GpuStaticPropInstance& inst) { return static_cast<uint64_t>(inst.modelMatrix[3]) * 256 + inst.typeID; };
    const float dist[] = {0, 100, 300};
    const uint32_t target[] = {5, 9, 9}; // destination sharing across tiers
    for (int frame = 0; frame < 90; ++frame) {
        if (frame == 20) { // despawn a duplicate leaf, then reuse its registry slot
            oldState.erase(key(persistent[0])); denseState.erase(key(persistent[0]));
            persistent[0].modelMatrix[3] = 600; slots = {};
        }
        if (frame == 30) { std::reverse(persistent.begin(), persistent.end()); persistent.shrink_to_fit(); slots = {}; }
        if (frame == 40) { dynamic.push_back(persistent.back()); persistent.pop_back(); slots = {}; }
        if (frame == 50) { persistent.push_back(dynamic.back()); dynamic.clear(); slots = {}; }
        if (frame == 60) { oldState.clear(); denseState.clear(); slots = {}; } // mission/save reload
        const float camera = frame < 10 ? 0.0f : static_cast<float>((frame % 30) * 21);
        std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> oldOut, denseOut;
        std::vector<GpuStaticPropInstance>* outputs[] = {&denseOut[5], &denseOut[9], &denseOut[9]};
        slots.bind(denseState, std::span<const GpuStaticPropInstance>(persistent), key);
        auto route = [&](const auto& instances, bool cached) {
            size_t i = 0;
            for (const auto& inst : instances) {
                uint8_t& old = oldState[key(inst)];
                uint8_t& dense = cached ? denseState.dense(slots.indices[i]) : denseState[key(inst)];
                const float distance = std::abs(inst.modelMatrix[3] - camera);
                propLodAdvance(old, distance, dist, 0.08f); propLodAdvance(dense, distance, dist, 0.08f);
                CHECK(old == dense);
                auto a = inst, b = inst; a.typeID = target[old]; b.typeID = target[dense];
                oldOut[a.typeID].push_back(a); outputs[dense]->push_back(b); ++i;
            }
        };
        route(persistent, true); route(dynamic, false);
        CHECK(oldState.size() == denseState.size());
        for (uint32_t type : {5u, 9u}) {
            const auto& a = oldOut[type]; const auto& b = denseOut[type];
            REQUIRE(a.size() == b.size());
            for (size_t i = 0; i < a.size(); ++i) CHECK(std::memcmp(&a[i], &b[i], sizeof(a[i])) == 0);
        }
    }
}

TEST_CASE("prop_lod_state: flat map matches std::unordered_map under insert, erase and grow") {
    PropLodStateMap flat;
    std::unordered_map<uint64_t, uint8_t> ref;
    std::mt19937_64 rng(12345);
    for (int step = 0; step < 200000; ++step) {
        const uint64_t key = ((rng() % 5000) << 8) | (rng() % 4);   // dense low bits like (position hash << 8) ^ typeID
        const int op = static_cast<int>(rng() % 4);
        if (op == 0) { flat.erase(key); ref.erase(key); }
        else { const uint8_t v = static_cast<uint8_t>(rng() % 256); flat[key] = v; ref[key] = v; }
        if (step % 20000 == 0) {
            CHECK(flat.size() == ref.size());
            for (const auto& [k, v] : ref) { const uint8_t* p = flat.find(k); REQUIRE(p); CHECK(*p == v); }
        }
    }
    CHECK(flat.size() == ref.size());
    for (const auto& [k, v] : ref) { const uint8_t* p = flat.find(k); REQUIRE(p); CHECK(*p == v); }
    CHECK(flat.find(0xFFFFFFFFFFFFull) == nullptr);
    flat.clear();
    CHECK(flat.size() == 0);
    CHECK(flat[7] == 0);
}

// Trigger paths: RenderCore/PropLodState.h, GameOS/gameos/gos_static_prop_batcher_shadow.cpp.
TEST_CASE("prop_lod_state: projected tree tier uses MC2 coordinates from its first frame") {
    const float m[16] = {1,0,0,0.6f, 0,0,0,0.8f, 0,2,1,0, 0,0,0,-1000};
    const StaticPropOrigin origin{-5376,350,5376}; // MC2 (5376,5376,350), as stored by props
    const float distances[5] = {0,144,216,308.6f,1620};
    const float d = propLodProjectedDistance(m, origin, 2, 1);
    CHECK(d == doctest::Approx(3263.2f));
    CHECK(propLodProjectedDistance(m, origin, 2, 2) == doctest::Approx(6526.4f));
    uint8_t lod = 0;
    propLodAdvance(lod, d, distances, 0.08f);
    CHECK(lod == 4); // Missing the axis conversion clamps the old projected distance to zero.
    // Behind-camera trees retain their projected size instead of all selecting the finest mesh.
    CHECK(propLodProjectedDistance(m, {5376,350,-5376}, 2, 1) == doctest::Approx(4263.2f));
}

TEST_CASE("prop_lod_state: hysteresis band keeps an instance on its LOD at a boundary") {
    const float dist[3] = {0.f, 100.f, 300.f};
    uint8_t lod = 0;
    const std::span<const float> d3(dist);
    propLodAdvance(lod, 105.f, d3, 0.08f); CHECK(lod == 0);   // inside the +8% band
    propLodAdvance(lod, 109.f, d3, 0.08f); CHECK(lod == 1);
    propLodAdvance(lod, 95.f, d3, 0.08f);  CHECK(lod == 1);   // inside the -8% band
    propLodAdvance(lod, 91.f, d3, 0.08f);  CHECK(lod == 0);
    propLodAdvance(lod, 1000.f, d3, 0.08f); CHECK(lod == 2);
}

TEST_CASE("prop_lod_state: zero key and stale erases preserve all value bits") {
    PropLodStateMap map;
    map.erase(0); CHECK(map.find(0) == nullptr);
    CHECK(map[0] == 0); map[0] = 255;
    for (uint64_t k = 1; k < 3000; ++k) map[k] = static_cast<uint8_t>(k);
    map.erase(0); map.erase(0);
    CHECK(map.size() == 2999); CHECK(map.find(0) == nullptr);
    for (uint64_t k = 1; k < 3000; ++k) {
        REQUIRE(map.find(k)); CHECK(*map.find(k) == static_cast<uint8_t>(k));
        map.erase(k); map.erase(k);
    }
    CHECK(map.size() == 0); CHECK(map[0] == 0);
}

TEST_CASE("camera_looks: vehicle thresholds retain both directions inside eight percent") {
    const float thresholds[] = {0.f, 100.f, 250.f};
    uint8_t lod = 0;
    for (int tier = 1; tier < 3; ++tier) {
        const float t = thresholds[tier];
        for (float d = t * 0.93f; d <= t * 1.07f; d += t * 0.01f) {
            propLodAdvance(lod, d, thresholds, 0.08f);
            CHECK(lod == tier - 1);
        }
        propLodAdvance(lod, t * 1.09f, thresholds, 0.08f);
        CHECK(lod == tier);
        for (float d = t * 1.07f; d >= t * 0.93f; d -= t * 0.01f) {
            propLodAdvance(lod, d, thresholds, 0.08f);
            CHECK(lod == tier);
        }
        propLodAdvance(lod, t * 0.91f, thresholds, 0.08f);
        CHECK(lod == tier - 1);
        propLodAdvance(lod, t * 1.09f, thresholds, 0.08f);
    }
}

TEST_CASE("camera_looks: world cell vegetation rank ignores camera distance") {
    std::vector<int> baseline;
    for (float distance : {100.f, 7000.f, 20000.f}) {
        std::vector<int> kept;
        for (int instance = 0; instance < 128; ++instance) {
            const float seed = static_cast<float>(instance) / 128.f;
            const float hash = seed * 17.371f + 0.137f;
            const float rank = hash - std::floor(hash); // GLSL fract, same as vegLodScale
            if (rank < 1.f) kept.push_back(instance); // full cooked map-cell world budget
        }
        if (distance == 100.f) baseline = kept;
        else CHECK(kept == baseline);
    }
    CHECK(baseline.size() == 128);
}
