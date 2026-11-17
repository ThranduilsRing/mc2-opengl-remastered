#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "object_light_row.h"
#include "static_light_row_guard.h"

#include <string>
#include <vector>

namespace {
namespace olr = object_light_row;

static std::vector<olr::LightInput> makeWorldLights(unsigned nPool = 3)
{
    std::vector<olr::LightInput> v;

    olr::LightInput sun{};
    sun.type   = olr::kTypeInfinite;
    sun.active = true;
    sun.aRGB   = 0xFFC8B48Cu;
    for (int i = 0; i < 12; ++i) sun.l2w[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    sun.l2w[3] = 100.0f; sun.l2w[7] = 250.0f; sun.l2w[11] = -40.0f;
    sun.fwd[0] = 0.31f; sun.fwd[1] = -0.86f; sun.fwd[2] = 0.40f;
    sun.falloff[0] = 0.0f; sun.falloff[1] = 0.0f; sun.falloff[2] = 0.0f;
    v.push_back(sun);

    olr::LightInput amb{};
    amb.type   = olr::kTypeAmbient;
    amb.active = true;
    amb.aRGB   = 0xFF404860u;

    for (int i = 0; i < 12; ++i) amb.l2w[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    amb.fwd[0] = 0.0f; amb.fwd[1] = 0.0f; amb.fwd[2] = 1.0f;
    v.push_back(amb);

    for (unsigned p = 0; p < nPool; ++p) {
        olr::LightInput pt{};
        pt.type   = (p % 2) ? olr::kTypeSpot : olr::kTypePoint;
        pt.active = true;
        pt.aRGB   = 0xFF000000u | (0x201010u * (p + 1));
        for (int i = 0; i < 12; ++i) pt.l2w[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        pt.l2w[3]  = 10.0f * (float)(p + 1);
        pt.l2w[7]  = 20.0f * (float)(p + 1);
        pt.l2w[11] = 3.0f  * (float)(p + 1);
        pt.fwd[0] = 0.0f; pt.fwd[1] = -1.0f; pt.fwd[2] = 0.0f;
        pt.falloff[0] = 5.0f; pt.falloff[1] = 80.0f; pt.falloff[2] = 1.0f / 75.0f;
        v.push_back(pt);
    }
    return v;
}

static const uint32_t kTerrainRGB = 0xFF9E8C6Eu;

static olr::Query buildingQuery(const std::vector<olr::LightInput>& lights)
{
    olr::Query q;
    q.lights    = lights.data();
    q.numLights = (uint32_t)lights.size();
    q.policy    = olr::kClassBuilding;
    q.sunSlot.index    = 0;
    q.sunSlot.recolour = true;
    q.sunSlot.aRGB     = kTerrainRGB;
    q.terrainLightSample = 0.82f;
    return q;
}

static olr::Query treeQuery(const std::vector<olr::LightInput>& lights)
{
    olr::Query q;
    q.lights    = lights.data();
    q.numLights = (uint32_t)lights.size();
    q.policy    = olr::kClassTree;
    q.ambientSlot.index    = 1;
    q.ambientSlot.recolour = true;
    q.ambientSlot.aRGB     = kTerrainRGB;
    q.sunSlot.index         = 0;
    q.sunSlot.forceInactive = true;
    q.terrainLightSample = 0.82f;
    return q;
}

static olr::Query moverQuery(const std::vector<olr::LightInput>& lights,
                             olr::ClassPolicy cls)
{
    olr::Query q = buildingQuery(lights);
    q.policy = cls;
    return q;
}

static void legacyRow(const std::vector<olr::LightInput>& lights,
                      const olr::Query& q, olr::Row* out)
{
    std::vector<olr::LightInput> mutated = lights;
    olr::applyLegacyMutations(mutated.data(), (uint32_t)mutated.size(), q);

    olr::Query raw = olr::withoutPolicy(q);
    raw.lights    = mutated.data();
    raw.numLights = (uint32_t)mutated.size();
    olr::BuildObjectLightRow(raw, out);
}

static void pureRow(const olr::Query& q, olr::Row* out)
{
    olr::BuildObjectLightRow(q, out);
}

static bool rowsEqual(const olr::Row& a, const olr::Row& b)
{
    return std::memcmp(&a, &b, sizeof(olr::Row)) == 0;
}

static std::string firstDiff(const olr::Row& a, const olr::Row& b)
{
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
    for (size_t i = 0; i < sizeof(olr::Row); ++i) {
        if (pa[i] != pb[i]) {
            const char* field = "?";
            if      (i < offsetof(olr::Row, lightDir))     field = "lightToWorld";
            else if (i < offsetof(olr::Row, lightColor))   field = "lightDir";
            else if (i < offsetof(olr::Row, lightFalloff)) field = "lightColor";
            else if (i < offsetof(olr::Row, numLights_))   field = "lightFalloff";
            else                                          field = "numLights_/pad";
            return std::string("first byte diff at offset ") + std::to_string(i) +
                   " (" + field + "), legacy=" + std::to_string((int)pa[i]) +
                   " pure=" + std::to_string((int)pb[i]);
        }
    }
    return "identical";
}

static bool byteIdentity(const olr::Query& q,
                         const std::vector<olr::LightInput>& lights,
                         const char* label)
{
    olr::Row legacy{}, pure{};
    legacyRow(lights, q, &legacy);
    pureRow(q, &pure);
    if (!rowsEqual(legacy, pure)) {
        FAIL_CHECK((std::string(label) + ": " + firstDiff(legacy, pure)));
        return false;
    }
    return true;
}

TEST_CASE("object_light_row_harness/building_policy_is_byte_identical") {
    auto lights = makeWorldLights();
    byteIdentity(buildingQuery(lights), lights, "building");
    return;
}

TEST_CASE("object_light_row_harness/tree_policy_is_byte_identical") {
    auto lights = makeWorldLights();
    byteIdentity(treeQuery(lights), lights, "tree");
    return;
}

TEST_CASE("object_light_row_harness/mech_policy_is_byte_identical") {
    auto lights = makeWorldLights();
    byteIdentity(moverQuery(lights, olr::kClassMech), lights, "mech");
    return;
}

TEST_CASE("object_light_row_harness/vehicle_policy_is_byte_identical") {
    auto lights = makeWorldLights();
    byteIdentity(moverQuery(lights, olr::kClassVehicle), lights, "vehicle");
    return;
}

TEST_CASE("object_light_row_harness/generic_prop_policy_is_byte_identical") {
    auto lights = makeWorldLights();
    byteIdentity(moverQuery(lights, olr::kClassGenericProp), lights, "generic prop");
    return;
}

TEST_CASE("object_light_row_harness/byte_identity_across_light_counts") {
    for (unsigned pool = 0; pool <= 40; ++pool) {
        auto lights = makeWorldLights(pool);
        const std::string tag = "pool=" + std::to_string(pool);
        if (!byteIdentity(buildingQuery(lights), lights, ("bldg " + tag).c_str())) break;
        if (!byteIdentity(treeQuery(lights),     lights, ("tree " + tag).c_str())) break;
    }
    return;
}

TEST_CASE("object_light_row_harness/byte_identity_with_holes") {
    auto lights = makeWorldLights(6);
    for (size_t hole = 0; hole < lights.size(); ++hole) {
        auto v = lights;
        v[hole].active = false;
        const std::string tag = "hole=" + std::to_string(hole);
        byteIdentity(buildingQuery(v), v, ("bldg " + tag).c_str());
        byteIdentity(treeQuery(v),     v, ("tree " + tag).c_str());
    }
    return;
}

TEST_CASE("object_light_row_harness/byte_identity_across_env_switches") {
    auto lights = makeWorldLights(2);
    for (int fixColumn = 0; fixColumn < 2; ++fixColumn) {
        for (int fixture = 0; fixture < 2; ++fixture) {
            olr::Query b = buildingQuery(lights);
            b.posColumnFix = (fixColumn != 0);
            b.clampFixture = (fixture != 0);
            olr::Query tr = treeQuery(lights);
            tr.posColumnFix = (fixColumn != 0);
            tr.clampFixture = (fixture != 0);
            const std::string tag = "col=" + std::to_string(fixColumn) +
                                    " fixture=" + std::to_string(fixture);
            byteIdentity(b,  lights, ("bldg " + tag).c_str());
            byteIdentity(tr, lights, ("tree " + tag).c_str());
        }
    }
    return;
}

TEST_CASE("object_light_row_harness/row_is_independent_of_bake_order") {
    auto lights = makeWorldLights(4);

    olr::Row aloneBldg{};
    pureRow(buildingQuery(lights), &aloneBldg);

    olr::Row scratch{}, afterTree{};
    pureRow(treeQuery(lights), &scratch);
    pureRow(buildingQuery(lights), &afterTree);
    CHECK((rowsEqual(aloneBldg, afterTree)));

    pureRow(treeQuery(lights), &scratch);
    pureRow(treeQuery(lights), &scratch);
    olr::Row afterTwoTrees{};
    pureRow(buildingQuery(lights), &afterTwoTrees);
    CHECK((rowsEqual(aloneBldg, afterTwoTrees)));

    if (!rowsEqual(aloneBldg, afterTree) || !rowsEqual(aloneBldg, afterTwoTrees))
        FAIL_CHECK(("bake ORDER changed a building row: " + firstDiff(aloneBldg, afterTree)));
    return;
}

TEST_CASE("object_light_row_harness/row_is_independent_of_bake_time") {
    auto lights = makeWorldLights(4);
    olr::Row first{};
    pureRow(buildingQuery(lights), &first);

    for (int i = 0; i < 1000; ++i) {
        olr::Row noise{};
        pureRow(treeQuery(lights), &noise);
    }
    olr::Row thousandth{};
    pureRow(buildingQuery(lights), &thousandth);
    CHECK((rowsEqual(first, thousandth)));
    return;
}

TEST_CASE("object_light_row_harness/tree_policy_removes_sun_from_row") {
    auto lights = makeWorldLights(2);

    olr::Row tree{};
    pureRow(treeQuery(lights), &tree);
    CHECK((tree.numLights_ == 3));
    CHECK((tree.lightDir[0][3] == (float)olr::kTypeAmbient));
    for (int s = 0; s < tree.numLights_; ++s)
        CHECK((tree.lightDir[s][3] != (float)olr::kTypeInfinite));

    olr::Row bldg{};
    pureRow(buildingQuery(lights), &bldg);
    CHECK((bldg.numLights_ == 4));
    CHECK((bldg.lightDir[0][3] == (float)olr::kTypeInfinite));
    return;
}

static void effectiveLight(const olr::Query& q, uint32_t i,
                           bool* activeOut, uint32_t* aRGBOut)
{
    const olr::LightInput& L = q.lights[i];
    bool     a = L.active;
    uint32_t c = L.aRGB;
    if (q.sunSlot.index == (int32_t)i) {
        if (q.sunSlot.forceInactive) a = false;
        if (q.sunSlot.recolour)      c = q.sunSlot.aRGB;
    }
    if (q.ambientSlot.index == (int32_t)i) {
        if (q.ambientSlot.forceInactive) a = false;
        if (q.ambientSlot.recolour)      c = q.ambientSlot.aRGB;
    }
    *activeOut = a;
    *aRGBOut   = c;
}

static uint32_t modelDecomposeFirstActiveLightColor(const olr::Query& q,
                                                    olr::Row* row,
                                                    uint32_t colorXor = 0u)
{
    for (uint32_t i = 0; i < q.numLights; ++i) {
        if (!q.lights) break;
        bool a = false; uint32_t startLight = 0;
        effectiveLight(q, i, &a, &startLight);
        if (!a) continue;
        startLight ^= colorXor;
        row->lightColor[0][0] = ((startLight >> 16) & 0x000000ffu) / 255.0f;
        row->lightColor[0][1] = ((startLight >>  8) & 0x000000ffu) / 255.0f;
        row->lightColor[0][2] = ((startLight      ) & 0x000000ffu) / 255.0f;
        row->lightColor[0][3] = 1.0f;
        return 0;
    }
    return 0xFFFFFFFFu;
}

TEST_CASE("object_light_row_harness/decompose_slot0_is_a_noop_after_gather") {
    for (unsigned pool = 0; pool <= 8; ++pool) {
        auto lights = makeWorldLights(pool);
        const uint32_t n = (uint32_t)lights.size();

        for (uint32_t hole = 0; hole <= n; ++hole) {
            auto v = lights;
            if (hole < n) v[hole].active = false;

            olr::Query qs[5] = {
                buildingQuery(v),
                treeQuery(v),
                moverQuery(v, olr::kClassMech),
                moverQuery(v, olr::kClassVehicle),
                moverQuery(v, olr::kClassGenericProp),
            };

            for (int k = 0; k < 5; ++k) {
                const olr::Query& q = qs[k];
                olr::Row gathered{};
                pureRow(q, &gathered);

                olr::Row decomposed = gathered;
                const uint32_t rv = modelDecomposeFirstActiveLightColor(q, &decomposed);

                if (gathered.numLights_ == 0) {

                    CHECK((rv == 0xFFFFFFFFu));
                    CHECK((rowsEqual(gathered, decomposed)));
                    continue;
                }

                CHECK((rv == 0u));

                if (!rowsEqual(gathered, decomposed)) {
                    FAIL_CHECK((std::string("decompose CHANGED the row: policy=") +
                           std::to_string((int)q.policy) +
                           " pool=" + std::to_string(pool) +
                           " hole=" + std::to_string(hole) + " " +
                           firstDiff(gathered, decomposed)));
                    FAIL_CHECK(("test returned false")); return;
                }

                olr::Row perturbed = gathered;
                modelDecomposeFirstActiveLightColor(q, &perturbed, 0x00000001u);
                CHECK((!rowsEqual(gathered, perturbed)));
            }
        }
    }
    return;
}

TEST_CASE("object_light_row_harness/decompose_clobbers_slot0_only_across_a_light_state_change") {
    auto lights = makeWorldLights(2);

    const uint32_t kBrightRGB = 0xFFC0C0C0u;
    const uint32_t kDimRGB    = 0xFF3F3F3Fu;

    olr::Query qA = buildingQuery(lights);
    qA.sunSlot.aRGB = kBrightRGB;
    olr::Row rowA{};
    pureRow(qA, &rowA);
    CHECK((rowA.lightDir[0][3] == (float)olr::kTypeInfinite));
    CHECK((rowA.lightColor[0][0] == 192.0f / 255.0f));

    olr::Query qB = treeQuery(lights);
    qB.ambientSlot.aRGB = kDimRGB;

    olr::Row crossed = rowA;
    const uint32_t rv = modelDecomposeFirstActiveLightColor(qB, &crossed);
    CHECK((rv == 0u));
    CHECK((!rowsEqual(rowA, crossed)));

    CHECK((crossed.lightColor[0][0] == 63.0f / 255.0f));
    CHECK((crossed.lightDir[0][3] == (float)olr::kTypeInfinite));

    bool activeMaskDiffers = false;
    for (uint32_t i = 0; i < (uint32_t)lights.size(); ++i) {
        bool aA = false, aB = false; uint32_t cA = 0, cB = 0;
        effectiveLight(qA, i, &aA, &cA);
        effectiveLight(qB, i, &aB, &cB);
        if (aA != aB) { activeMaskDiffers = true; break; }
    }
    CHECK((activeMaskDiffers));

    olr::Row sameState = rowA;
    modelDecomposeFirstActiveLightColor(qA, &sameState);
    CHECK((rowsEqual(rowA, sameState)));
    return;
}

TEST_CASE("object_light_row_harness/recolour_lands_in_the_declared_slot") {
    auto lights = makeWorldLights(1);

    olr::Row bldg{};
    pureRow(buildingQuery(lights), &bldg);
    CHECK((bldg.lightColor[0][0] == ((kTerrainRGB >> 16) & 0xFF) / 255.0f));
    CHECK((bldg.lightColor[0][1] == ((kTerrainRGB >>  8) & 0xFF) / 255.0f));
    CHECK((bldg.lightColor[0][2] == ((kTerrainRGB      ) & 0xFF) / 255.0f));

    CHECK((bldg.lightColor[1][0] == ((lights[1].aRGB >> 16) & 0xFF) / 255.0f));

    olr::Row tree{};
    pureRow(treeQuery(lights), &tree);
    CHECK((tree.lightColor[0][0] == ((kTerrainRGB >> 16) & 0xFF) / 255.0f));
    return;
}

TEST_CASE("object_light_row_harness/pos_column_fix") {
    auto lights = makeWorldLights(1);
    olr::Query q = buildingQuery(lights);

    q.posColumnFix = true;
    olr::Row on{};
    pureRow(q, &on);

    CHECK((on.lightToWorld[2][12] == on.lightToWorld[2][3]));
    CHECK((on.lightToWorld[2][13] == on.lightToWorld[2][7]));
    CHECK((on.lightToWorld[2][14] == on.lightToWorld[2][11]));
    CHECK((on.lightToWorld[2][12] == 10.0f));
    CHECK((on.lightToWorld[2][15] == 1.0f));

    q.posColumnFix = false;
    olr::Row off{};
    pureRow(q, &off);
    CHECK((off.lightToWorld[2][12] == 0.0f));
    CHECK((off.lightToWorld[2][13] == 0.0f));
    CHECK((off.lightToWorld[2][14] == 0.0f));
    CHECK((off.lightToWorld[2][15] == 1.0f));
    return;
}

TEST_CASE("object_light_row_harness/abi_cap_is_never_exceeded") {
    auto lights = makeWorldLights(200);
    olr::Row r{};
    pureRow(buildingQuery(lights), &r);
    CHECK((r.numLights_ == (int32_t)olr::kMaxLights));

    olr::Query q = buildingQuery(lights);
    q.clampFixture = true;
    olr::Row f{};
    pureRow(q, &f);
    CHECK((f.numLights_ == (int32_t)olr::kMaxLights));
    return;
}

TEST_CASE("object_light_row_harness/clamp_fixture_fills_to_cap") {
    auto lights = makeWorldLights(1);
    olr::Query q = buildingQuery(lights);
    q.clampFixture = true;
    olr::Row r{};
    pureRow(q, &r);
    CHECK((r.numLights_ == (int32_t)olr::kMaxLights));
    for (uint32_t s = 3; s < olr::kMaxLights; ++s) {
        CHECK((r.lightDir[s][3] == (float)olr::kTypePoint));
        CHECK((r.lightFalloff[s][1] == 1000.0f));
    }
    return;
}

TEST_CASE("object_light_row_harness/tail_slots_are_left_untouched_on_reuse") {
    auto lights = makeWorldLights(1);

    olr::Row reused{};
    std::memset(&reused, 0xAB, sizeof(reused));
    pureRow(buildingQuery(lights), &reused);

    CHECK((reused.numLights_ == 3));
    const unsigned char* p = reinterpret_cast<const unsigned char*>(reused.lightToWorld[3]);
    bool tailPreserved = true;
    for (size_t i = 0; i < 16 * sizeof(float); ++i)
        if (p[i] != 0xAB) { tailPreserved = false; break; }
    CHECK((tailPreserved));

    CHECK((reused.pad[0] == (int32_t)0xABABABAB));
    return;
}

TEST_CASE("object_light_row_harness/build_does_not_mutate_its_inputs") {
    auto lights = makeWorldLights(4);
    const auto before = lights;

    olr::Row r{};
    pureRow(treeQuery(lights), &r);
    pureRow(buildingQuery(lights), &r);

    CHECK((std::memcmp(before.data(), lights.data(),
                            before.size() * sizeof(olr::LightInput)) == 0));

    CHECK((lights[0].active));
    return;
}

TEST_CASE("object_light_row_harness/determinism") {
    auto lights = makeWorldLights(5);
    olr::Row a{}, b{};
    pureRow(treeQuery(lights), &a);
    pureRow(treeQuery(lights), &b);
    CHECK((rowsEqual(a, b)));
    return;
}

TEST_CASE("object_light_row_harness/empty_and_null_light_lists") {
    olr::Row r{};
    std::memset(&r, 0x5A, sizeof(r));

    olr::Query q;
    olr::BuildObjectLightRow(q, &r);
    CHECK((r.numLights_ == 0));

    std::vector<olr::LightInput> none;
    olr::Query q2 = buildingQuery(none);
    q2.lights = nullptr;
    olr::BuildObjectLightRow(q2, &r);
    CHECK((r.numLights_ == 0));

    olr::BuildObjectLightRow(q, nullptr);
    return;
}

TEST_CASE("object_light_row_harness/comparator_detects_injected_divergence") {
    auto lights = makeWorldLights(2);

    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.aRGB ^= 0x00000001u;
        pureRow(q, &pure);
        CHECK((!rowsEqual(legacy, pure)));
    }

    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.index = 1;
        pureRow(q, &pure);
        CHECK((!rowsEqual(legacy, pure)));
    }

    {
        olr::Query q = treeQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.forceInactive = false;
        pureRow(q, &pure);
        CHECK((!rowsEqual(legacy, pure)));
    }

    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        pureRow(olr::withoutPolicy(q), &pure);
        CHECK((!rowsEqual(legacy, pure)));
    }

    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.aRGB = 0xFF000000u;
        pureRow(q, &pure);
        CHECK((firstDiff(legacy, pure).find("lightColor") != std::string::npos));
    }
    return;
}

static float absf32(float v) { return v < 0.0f ? -v : v; }

static unsigned char weatherAmbientByte(unsigned char dayAmbient,
                                        float rainLevel, float rainLightLevel)
{
    if (rainLevel      <= 0.0f) return dayAmbient;
    if (rainLightLevel <= 1.0f) return dayAmbient;
    return (unsigned char)(float(dayAmbient) / rainLightLevel);
}

static void rowAtAmbientByte(unsigned char amb, olr::Row* out)
{
    auto lights = makeWorldLights();
    lights[1].aRGB = 0xFF000000u | (uint32_t(amb) << 16)
                                 | (uint32_t(amb) << 8)
                                 |  uint32_t(amb);
    olr::Query q = buildingQuery(lights);
    pureRow(q, out);
}

TEST_CASE("object_light_row_harness/ambient_state_changes_the_baked_row") {

    const unsigned char kA = 192;
    const unsigned char kB = 63;

    olr::Row rowA{}, rowB{};
    rowAtAmbientByte(kA, &rowA);
    rowAtAmbientByte(kB, &rowB);

    CHECK((!rowsEqual(rowA, rowB)));

    CHECK((firstDiff(rowA, rowB).find("lightColor") != std::string::npos));

    for (int c = 0; c < 3; ++c) {
        CHECK((rowA.lightColor[1][c] == float(kA) / 255.0f));
        CHECK((rowB.lightColor[1][c] == float(kB) / 255.0f));
        CHECK(((rowA.lightColor[1][c] - rowB.lightColor[1][c])
                        == float(kA - kB) / 255.0f));
    }

    for (int c = 0; c < 3; ++c)
        CHECK((rowA.lightColor[0][c] == rowB.lightColor[0][c]));

    {
        olr::Row same1{}, same2{}, off1{};
        rowAtAmbientByte(kB, &same1);
        rowAtAmbientByte(kB, &same2);
        rowAtAmbientByte((unsigned char)(kB + 1), &off1);
        CHECK((rowsEqual(same1, same2)));
        CHECK((!rowsEqual(same1, off1)));
    }
    return;
}

TEST_CASE("object_light_row_harness/mc2_10_storm_ambient_is_not_mission_constant") {
    const unsigned char kDayAmbient = 192;
    const float         kStartRain  = 3.0f;
    const float         kRainCap    = 4.0f;

    const unsigned char P = kDayAmbient;

    CHECK((weatherAmbientByte(kDayAmbient, kStartRain, 3.001f) == 63));
    CHECK((weatherAmbientByte(kDayAmbient, kStartRain, 3.02f)  == 63));
    CHECK((weatherAmbientByte(kDayAmbient, kStartRain, 3.04f)  == 63));
    const unsigned char R = weatherAmbientByte(kDayAmbient, kStartRain, 3.02f);

    CHECK((weatherAmbientByte(kDayAmbient, kRainCap, kRainCap) == 48));
    const unsigned char S = weatherAmbientByte(kDayAmbient, kRainCap, kRainCap);

    olr::Row rowP{}, rowR{}, rowS{};
    rowAtAmbientByte(P, &rowP);
    rowAtAmbientByte(R, &rowR);
    rowAtAmbientByte(S, &rowS);
    CHECK((rowP.lightColor[1][0] == 192.0f / 255.0f));
    CHECK((rowR.lightColor[1][0] ==  63.0f / 255.0f));
    CHECK((rowS.lightColor[1][0] ==  48.0f / 255.0f));

    CHECK((!rowsEqual(rowP, rowR)));
    CHECK((!rowsEqual(rowR, rowS)));
    CHECK((!rowsEqual(rowP, rowS)));

    const float kQuarterStep = 0.25f / 255.0f;
    CHECK((absf32((rowP.lightColor[1][0] - rowS.lightColor[1][0])
                       - 144.0f / 255.0f) < kQuarterStep));
    CHECK((absf32((rowR.lightColor[1][0] - rowS.lightColor[1][0])
                       -  15.0f / 255.0f) < kQuarterStep));
    CHECK((rowP.lightColor[1][0] > rowS.lightColor[1][0]));
    CHECK((rowR.lightColor[1][0] > rowS.lightColor[1][0]));
    return;
}

TEST_CASE("object_light_row_harness/non_rainy_missions_are_immune_to_bake_time") {

    const unsigned char kAuthored[] = { 192, 128, 106, 95, 74, 31 };

    for (unsigned char day : kAuthored) {

        const unsigned char P = day;
        const unsigned char R = weatherAmbientByte(day, 0.0f, 3.02f);
        const unsigned char S = weatherAmbientByte(day, 0.0f, 4.0f);
        CHECK((R == day));
        CHECK((S == day));

        olr::Row rowP{}, rowR{}, rowS{};
        rowAtAmbientByte(P, &rowP);
        rowAtAmbientByte(R, &rowR);
        rowAtAmbientByte(S, &rowS);
        CHECK((rowsEqual(rowP, rowR)));
        CHECK((rowsEqual(rowR, rowS)));

        const unsigned char injR = weatherAmbientByte(day, 3.0f, 3.02f);
        olr::Row injRow{};
        rowAtAmbientByte(injR, &injRow);
        CHECK((!rowsEqual(rowP, injRow)));
    }
    return;
}

namespace slr = static_light_row;

TEST_CASE("object_light_row_harness/render_liveness_does_not_preserve_dynamic_rows") {
    // LookoutTower, blown-arm fixture: the last update allocated 998, then
    // settled presentation skipped updates while render continued every frame.
    CHECK_FALSE(slr::needsResubmit(998, 997, 404, 404));
    CHECK(slr::needsResubmit(998, 997, 404, 406));
    // Reuse is invalid even if unrelated arm/actor appends make 998 in range.
    CHECK(slr::needsResubmit(998, 997, 406, 407));
    CHECK(slr::needsResubmit(997, 997, 406, 407));
    CHECK_FALSE(slr::needsResubmit(996, 997, 404, 407));
    CHECK_FALSE(slr::needsResubmit(191, 997, 404, 407));
    CHECK(slr::needsResubmit(slr::kNoLightIndex, 997, 404, 407));
}

TEST_CASE("object_light_row_harness/light_index_resolution_falls_back_on_sentinel") {
    const uint32_t kS = slr::kNoLightIndex;

    CHECK((slr::resolve(true, 7u, 99u) == 7u));

    CHECK((slr::resolve(true, kS, 99u) == 99u));

    CHECK((slr::resolve(false, 7u, 99u) == 99u));

    CHECK((slr::resolve(true, kS, kS) == kS));
    CHECK((slr::resolve(false, kS, kS) == kS));
    return;
}

TEST_CASE("object_light_row_harness/zero_light_row_is_classified_not_silently_shaded") {
    const int kOutOfRange = -1;

    CHECK((slr::classify(12u, 1) == slr::kHealthy));
    CHECK((slr::classify(0u,  4) == slr::kHealthy));
    CHECK((!slr::isZeroRow(12u, 1)));

    CHECK((slr::classify(slr::kNoLightIndex, 3) == slr::kSentinel));
    CHECK((slr::isZeroRow(slr::kNoLightIndex, 3)));

    CHECK((slr::classify(slr::kNoLightIndex, kOutOfRange) == slr::kSentinel));

    CHECK((slr::classify(4096u, kOutOfRange) == slr::kOutOfRange));
    CHECK((slr::isZeroRow(4096u, kOutOfRange)));

    CHECK((slr::classify(31u, 0) == slr::kEmptyRow));
    CHECK((slr::isZeroRow(31u, 0)));

    CHECK((std::string(slr::className(slr::kSentinel))   == "sentinel"));
    CHECK((std::string(slr::className(slr::kOutOfRange)) == "out_of_range"));
    CHECK((std::string(slr::className(slr::kEmptyRow))   == "empty_row"));
    CHECK((std::string(slr::className(slr::kHealthy))    == "healthy"));
    return;
}

TEST_CASE("object_light_row_harness/guard_catches_every_zero_row_route") {
    const uint32_t kS   = slr::kNoLightIndex;
    const uint32_t kGood = 5u, kPast = 9999u;
    const int kOOR = -1;

    struct Case {
        bool     perInstance;
        uint32_t rangeIdx;
        uint32_t multiIdx;
        int      numLightsAtResolved;
        bool     expectZeroRow;
    };
    const Case cases[] = {

        { true,  kGood, kGood, 2, false },

        { true,  kS,    kGood, 2, false },

        { false, kGood, kGood, 2, false },

        { true,  kS,    kS,    3, true  },
        { false, kS,    kS,    3, true  },

        { true,  kPast, kGood, kOOR, true },

        { true,  kS,    kGood, 0, true },

        { false, kGood, kS,    2, true },
    };

    unsigned zeroRows = 0;
    for (const Case& c : cases) {
        const uint32_t resolved = slr::resolve(c.perInstance, c.rangeIdx, c.multiIdx);
        const bool bad = slr::isZeroRow(resolved, c.numLightsAtResolved);
        CHECK((bad == c.expectZeroRow));
        if (bad) ++zeroRows;
    }

    CHECK((zeroRows == 5u));

    unsigned disagreements = 0;
    for (const Case& c : cases) {
        const uint32_t resolved = slr::resolve(c.perInstance, c.rangeIdx, c.multiIdx);
        const bool unchecked_ships_it = true;
        const bool guard_rejects_it   = slr::isZeroRow(resolved, c.numLightsAtResolved);
        if (unchecked_ships_it && guard_rejects_it) ++disagreements;
    }
    CHECK((disagreements == 5u));
    return;
}

TEST_CASE("object_light_row_harness/guard_validates_a_different_variable_than_it_forwards") {
    const uint32_t kS = slr::kNoLightIndex;

    CHECK((slr::classifyGuardForward(4u, 4u) == slr::kAgree));

    CHECK((slr::classifyGuardForward(4u, 9u) == slr::kDifferBothValid));

    CHECK((slr::classifyGuardForward(4u, kS) == slr::kForwardedSentinelOnly));

    CHECK((slr::classifyGuardForward(kS, 4u) == slr::kValidatedSentinelOnly));

    CHECK((slr::classifyGuardForward(kS, kS) == slr::kBothSentinel));

    CHECK((slr::guardCorrectionWouldChangeBehaviour(slr::kForwardedSentinelOnly)));
    CHECK((slr::guardCorrectionWouldChangeBehaviour(slr::kValidatedSentinelOnly)));
    CHECK((!slr::guardCorrectionWouldChangeBehaviour(slr::kAgree)));
    CHECK((!slr::guardCorrectionWouldChangeBehaviour(slr::kDifferBothValid)));
    CHECK((!slr::guardCorrectionWouldChangeBehaviour(slr::kBothSentinel)));

    CHECK((std::string(slr::divergenceName(slr::kForwardedSentinelOnly))
                    == "forwarded_sentinel_only"));
    CHECK((std::string(slr::divergenceName(slr::kValidatedSentinelOnly))
                    == "validated_sentinel_only"));
    return;
}

TEST_CASE("object_light_row_harness/missed_case_is_wrong_light_not_a_zero_row") {
    const uint32_t kS = slr::kNoLightIndex;
    const uint32_t kMine = 7u, kSibling = 21u;

    const uint32_t got = slr::resolve(true, kS, kSibling);
    CHECK((got == kSibling));
    CHECK((got != kMine));
    CHECK((!slr::isZeroRow(got, 2)));

    CHECK((slr::isZeroRow(slr::resolve(true, kS, kS), 2)));
    CHECK((slr::classifyGuardForward(kS, kS) == slr::kBothSentinel));

    CHECK((slr::isZeroRow(slr::resolve(true, kS, kSibling), 0)));
    CHECK((slr::classifyGuardForward(kSibling, kS) == slr::kForwardedSentinelOnly));
    return;
}

}
