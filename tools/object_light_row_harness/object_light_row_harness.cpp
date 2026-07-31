// tools/object_light_row_harness/object_light_row_harness.cpp
// SUBSYSTEM-HARNESS-ARC / LIGHT-ROW-PURE-1
//
// Proves the claim ARCH-LIGHTING slice 1 rests on, OFFLINE, with no game launch
// and no human eyes:
//
//   For every per-class policy, the row produced by BuildObjectLightRow() from
//   an UNMUTATED light list plus an EXPLICIT DECLARED POLICY is byte-identical
//   to the row the legacy path produced by MUTATING the shared global light
//   list and gathering with no policy.
//
// If those two rows memcmp equal, the declared policy faithfully replaces the
// mutation, and "pixels unchanged" follows by construction rather than by
// review — every consumer downstream of the row sees the same 3600 bytes.
//
// Includes mclib/object_light_row.h directly: no engine, no GL, no Stuff, no
// gameos. Same idiom as tools/camera_frustum_harness.
//
// Build (standalone):
//   cmake -S tools/object_light_row_harness -B build64-h-object_light_row_harness -A x64
//   cmake --build build64-h-object_light_row_harness --config RelWithDebInfo

#include "contract_harness.h"
#include "object_light_row.h"

#include <string>
#include <vector>

using namespace contract_harness;
namespace olr = object_light_row;

//-----------------------------------------------------------------------------
// A stand-in for the world's shared light list: slot 0 = sun (INFINITE),
// slot 1 = ambient, then a handful of POINT/SPOT pool lights, mirroring what
// Camera::updateLights compacts into activeLights[] on a night map.
//-----------------------------------------------------------------------------
static std::vector<olr::LightInput> makeWorldLights(unsigned nPool = 3)
{
    std::vector<olr::LightInput> v;

    olr::LightInput sun{};
    sun.type   = olr::kTypeInfinite;
    sun.active = true;
    sun.aRGB   = 0xFFC8B48Cu;                 // warm daylight
    for (int i = 0; i < 12; ++i) sun.l2w[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    sun.l2w[3] = 100.0f; sun.l2w[7] = 250.0f; sun.l2w[11] = -40.0f;
    sun.fwd[0] = 0.31f; sun.fwd[1] = -0.86f; sun.fwd[2] = 0.40f;
    sun.falloff[0] = 0.0f; sun.falloff[1] = 0.0f; sun.falloff[2] = 0.0f;
    v.push_back(sun);

    olr::LightInput amb{};
    amb.type   = olr::kTypeAmbient;
    amb.active = true;
    amb.aRGB   = 0xFF404860u;
    // The adapter substitutes Identity for AMBIENT, so model that here.
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

// The terrain-light sample a class latches and pushes into a slot.
static const uint32_t kTerrainRGB = 0xFF9E8C6Eu;

static olr::Query buildingQuery(const std::vector<olr::LightInput>& lights)
{
    olr::Query q;
    q.lights    = lights.data();
    q.numLights = (uint32_t)lights.size();
    q.policy    = olr::kClassBuilding;
    q.sunSlot.index    = 0;                // eye->setLightColor(0, lightRGB)
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
    q.ambientSlot.index    = 1;            // eye->setLightColor(1, lightRGB)
    q.ambientSlot.recolour = true;
    q.ambientSlot.aRGB     = kTerrainRGB;
    q.sunSlot.index         = 0;           // getWorldLight(0)->active = false
    q.sunSlot.forceInactive = true;
    q.terrainLightSample = 0.82f;
    return q;
}

static olr::Query moverQuery(const std::vector<olr::LightInput>& lights,
                             olr::ClassPolicy cls)
{
    olr::Query q = buildingQuery(lights);
    q.policy = cls;                        // mech / vehicle: same sun recolour
    return q;
}

// Produce the LEGACY row: mutate a COPY of the shared list exactly as the
// appearance class did, then gather with NO policy.
static void legacyRow(const std::vector<olr::LightInput>& lights,
                      const olr::Query& q, olr::Row* out)
{
    std::vector<olr::LightInput> mutated = lights;      // the shared world list
    olr::applyLegacyMutations(mutated.data(), (uint32_t)mutated.size(), q);

    olr::Query raw = olr::withoutPolicy(q);
    raw.lights    = mutated.data();
    raw.numLights = (uint32_t)mutated.size();
    olr::BuildObjectLightRow(raw, out);
}

// Produce the PURE row: unmutated list + declared policy.
static void pureRow(const olr::Query& q, olr::Row* out)
{
    olr::BuildObjectLightRow(q, out);
}

static bool rowsEqual(const olr::Row& a, const olr::Row& b)
{
    return std::memcmp(&a, &b, sizeof(olr::Row)) == 0;
}

// Report the first differing byte offset, to make a failure actionable.
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

//=============================================================================
// THE CORE CLAIM: declared policy == mutation, byte for byte, per class.
//=============================================================================
static bool byteIdentity(TestCtx& t, const olr::Query& q,
                         const std::vector<olr::LightInput>& lights,
                         const char* label)
{
    olr::Row legacy{}, pure{};
    legacyRow(lights, q, &legacy);
    pureRow(q, &pure);
    if (!rowsEqual(legacy, pure)) {
        t.fail(std::string(label) + ": " + firstDiff(legacy, pure));
        return false;
    }
    return true;
}

static bool test_building_policy_is_byte_identical(TestCtx& t)
{
    auto lights = makeWorldLights();
    byteIdentity(t, buildingQuery(lights), lights, "building");
    return t.failures == 0;
}

static bool test_tree_policy_is_byte_identical(TestCtx& t)
{
    auto lights = makeWorldLights();
    byteIdentity(t, treeQuery(lights), lights, "tree");
    return t.failures == 0;
}

static bool test_mech_policy_is_byte_identical(TestCtx& t)
{
    auto lights = makeWorldLights();
    byteIdentity(t, moverQuery(lights, olr::kClassMech), lights, "mech");
    return t.failures == 0;
}

static bool test_vehicle_policy_is_byte_identical(TestCtx& t)
{
    auto lights = makeWorldLights();
    byteIdentity(t, moverQuery(lights, olr::kClassVehicle), lights, "vehicle");
    return t.failures == 0;
}

static bool test_generic_prop_policy_is_byte_identical(TestCtx& t)
{
    auto lights = makeWorldLights();
    byteIdentity(t, moverQuery(lights, olr::kClassGenericProp), lights, "generic prop");
    return t.failures == 0;
}

// Byte-identity must hold across the whole light-count domain, including the
// 32-light ABI cap and the >16 region LIGHT-CLAMP-RAISE-STAGE1-1 opened up.
static bool test_byte_identity_across_light_counts(TestCtx& t)
{
    for (unsigned pool = 0; pool <= 40; ++pool) {
        auto lights = makeWorldLights(pool);
        const std::string tag = "pool=" + std::to_string(pool);
        if (!byteIdentity(t, buildingQuery(lights), lights, ("bldg " + tag).c_str())) break;
        if (!byteIdentity(t, treeQuery(lights),     lights, ("tree " + tag).c_str())) break;
    }
    return t.failures == 0;
}

// Inactive lights anywhere in the list shift the packing; identity must survive
// that, because that is precisely what the tree's sun-off window does.
static bool test_byte_identity_with_holes(TestCtx& t)
{
    auto lights = makeWorldLights(6);
    for (size_t hole = 0; hole < lights.size(); ++hole) {
        auto v = lights;
        v[hole].active = false;
        const std::string tag = "hole=" + std::to_string(hole);
        byteIdentity(t, buildingQuery(v), v, ("bldg " + tag).c_str());
        byteIdentity(t, treeQuery(v),     v, ("tree " + tag).c_str());
    }
    return t.failures == 0;
}

// Both row-production env switches are explicit Query inputs; identity must
// hold on every combination, not just the shipped defaults.
static bool test_byte_identity_across_env_switches(TestCtx& t)
{
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
            byteIdentity(t, b,  lights, ("bldg " + tag).c_str());
            byteIdentity(t, tr, lights, ("tree " + tag).c_str());
        }
    }
    return t.failures == 0;
}

//=============================================================================
// THE STRUCTURAL CLAIM: PROP-BAKE-ORDER-VS-TIME-1 cannot exist here.
//=============================================================================

// A row must depend ONLY on its own query — not on what was built before it.
// The legacy design could not offer this: the tree deactivated the ONE shared
// sun across its own bake, so a prop baking inside that window silently
// inherited sun-inactive. Here, building a tree row between two building rows
// cannot perturb them, because nothing is mutated.
static bool test_row_is_independent_of_bake_order(TestCtx& t)
{
    auto lights = makeWorldLights(4);

    olr::Row aloneBldg{};
    pureRow(buildingQuery(lights), &aloneBldg);

    // Interleave: tree, building, tree, building — the exact adjacency the
    // legacy sun-off window made hazardous.
    olr::Row scratch{}, afterTree{};
    pureRow(treeQuery(lights), &scratch);
    pureRow(buildingQuery(lights), &afterTree);
    CH_CHECK(t, rowsEqual(aloneBldg, afterTree));

    pureRow(treeQuery(lights), &scratch);
    pureRow(treeQuery(lights), &scratch);
    olr::Row afterTwoTrees{};
    pureRow(buildingQuery(lights), &afterTwoTrees);
    CH_CHECK(t, rowsEqual(aloneBldg, afterTwoTrees));

    if (t.failures)
        t.fail("bake ORDER changed a building row: " + firstDiff(aloneBldg, afterTree));
    return t.failures == 0;
}

// A row must depend only on its query, not on WHEN it is built. Same query
// built 1st and 1000th must be identical — the TIME half of the ORDER-vs-TIME
// ambiguity that made the defect undiagnosable.
static bool test_row_is_independent_of_bake_time(TestCtx& t)
{
    auto lights = makeWorldLights(4);
    olr::Row first{};
    pureRow(buildingQuery(lights), &first);

    for (int i = 0; i < 1000; ++i) {
        olr::Row noise{};
        pureRow(treeQuery(lights), &noise);
    }
    olr::Row thousandth{};
    pureRow(buildingQuery(lights), &thousandth);
    CH_CHECK(t, rowsEqual(first, thousandth));
    return t.failures == 0;
}

// The mechanism itself, asserted directly: under the tree policy the sun is
// absent from the row, so the AMBIENT light becomes row slot 0. Under the
// building policy the sun IS row slot 0.
//
// PROP-BAKE-SUNWINDOW-1 CORRECTION: this comment used to continue "...— which
// is what makes decomposeFirstActiveLightColor (txmmgr.cpp) overwrite the
// ambient instead of the sun and collapse the term ~3x". That step is FALSE and
// was never modelled here, so nothing caught it. The gather COMPACTS (num_lights
// advances only inside the active branch, mclib/txmmgr.cpp:2837/:2983), so with
// the sun filtered out row slot 0 already holds the AMBIENT's colour — and
// decompose writes that same light's colour back into slot 0. It is a byte
// no-op, not a collapse. Proven below by
// test_decompose_slot0_is_a_noop_after_gather.
//
// What the tree policy really does to the row is remove the sun from it
// (numLights_ 4 -> 3, no INFINITE slot). That is the whole difference, and it
// is a look the user has ratified for foliage. The point of this file is that
// the divergence is now a DECLARED policy on one query rather than a global
// window with a lifetime.
static bool test_tree_policy_removes_sun_from_row(TestCtx& t)
{
    auto lights = makeWorldLights(2);

    olr::Row tree{};
    pureRow(treeQuery(lights), &tree);
    CH_CHECK(t, tree.numLights_ == 3);                       // sun filtered out
    CH_CHECK(t, tree.lightDir[0][3] == (float)olr::kTypeAmbient);
    for (int s = 0; s < tree.numLights_; ++s)
        CH_CHECK(t, tree.lightDir[s][3] != (float)olr::kTypeInfinite);

    olr::Row bldg{};
    pureRow(buildingQuery(lights), &bldg);
    CH_CHECK(t, bldg.numLights_ == 4);
    CH_CHECK(t, bldg.lightDir[0][3] == (float)olr::kTypeInfinite);
    return t.failures == 0;
}

//=============================================================================
// PROP-BAKE-SUNWINDOW-1 — the slot-0 "clobber" step, modelled and FALSIFIED.
//
// Three source files (mclib/bdactor.cpp's [BAKEORD v1] block,
// mclib/object_light_row.h's header, and this harness) asserted the same causal
// step in prose and NOTHING executed it:
//
//   "with the sun filtered out of the row the AMBIENT becomes row slot 0, and
//    decomposeFirstActiveLightColor unconditionally overwrites lightColor[0] —
//    that is the ~3x ambient collapse"
//
// The overwrite is real; the collapse is not. Both the gather and decompose
// select THE SAME light (the first ACTIVE one) off THE SAME list, so decompose
// writes back the bytes the gather just wrote. The model below is a faithful
// transcription of mclib/txmmgr.cpp:1480-1496 and the tests run it, so the claim
// is now executed rather than narrated.
//=============================================================================

// The effective state of one source light AFTER the class policy is applied —
// i.e. exactly what the legacy code left in the shared global list, which is
// what the real decompose reads. (applyLegacyMutations proves these agree; this
// is the same substitution expressed per-light so decompose can be modelled
// without materialising a mutated vector.)
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

// Faithful model of mclib/txmmgr.cpp:1480 decomposeFirstActiveLightColor().
// Returns 0 when it found a light (and patched slot 0), 0xFFFFFFFF when it did
// not — the same two return values the original uses to drive `perActor`.
// `colorXor` exists only for the blindness proof: 0 is the faithful model.
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

// THE FALSIFICATION. For every class policy, every light count, and every
// single-inactive-light hole (which is precisely what the tree's sun-off window
// makes of the list), gather-then-decompose is byte-identical to gather.
//
// Blindness guards, because four checks shipped this week that passed their own
// selftests while seeing nothing:
//   - assert decompose actually FOUND a light (returned 0, not the sentinel),
//   - assert the row is non-empty,
//   - and re-run the comparator against a one-bit-perturbed decompose and
//     require it to FAIL. A comparator that cannot see that is not evidence.
static bool test_decompose_slot0_is_a_noop_after_gather(TestCtx& t)
{
    for (unsigned pool = 0; pool <= 8; ++pool) {
        auto lights = makeWorldLights(pool);
        const uint32_t n = (uint32_t)lights.size();

        for (uint32_t hole = 0; hole <= n; ++hole) {      // hole == n => no hole
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
                    // Degenerate only if every light was filtered; then decompose
                    // must report the sentinel and touch nothing.
                    CH_CHECK(t, rv == 0xFFFFFFFFu);
                    CH_CHECK(t, rowsEqual(gathered, decomposed));
                    continue;
                }

                // Guard 1+2: the check is looking at a populated row and the
                // modelled decompose really ran.
                CH_CHECK(t, rv == 0u);

                if (!rowsEqual(gathered, decomposed)) {
                    t.fail(std::string("decompose CHANGED the row: policy=") +
                           std::to_string((int)q.policy) +
                           " pool=" + std::to_string(pool) +
                           " hole=" + std::to_string(hole) + " " +
                           firstDiff(gathered, decomposed));
                    return false;
                }

                // Guard 3: prove the comparator can see a divergence here.
                olr::Row perturbed = gathered;
                modelDecomposeFirstActiveLightColor(q, &perturbed, 0x00000001u);
                CH_CHECK(t, !rowsEqual(gathered, perturbed));
            }
        }
    }
    return t.failures == 0;
}

// THE RESIDUAL HAZARD, pinned. Slot 0 CAN be clobbered — but only if the row
// being decomposed was gathered under a DIFFERENT light-list state than the one
// decompose reads. In production that is reachable at exactly one place:
// mclib/txmmgr.cpp:2086, where a cached per-frame scene-light TEMPLATE is copied
// into the caller's row and then patched. What keeps it unreachable is
// sceneLightTemplateKey (mclib/txmmgr.cpp:1512) hashing EVERY light's `active`
// bit and every non-actor light's aRGB, so two rows sharing a key necessarily
// compact identically and slot 0 means the same light in both.
//
// This test builds the forbidden pairing deliberately and asserts it WOULD
// collapse the ambient ~3x, which is the honest form of the mechanism the three
// prose comments got wrong: it is a template-key invariant, not a bake-order
// window. If someone ever drops `active` from that key, this test says what
// breaks.
static bool test_decompose_clobbers_slot0_only_across_a_light_state_change(TestCtx& t)
{
    auto lights = makeWorldLights(2);

    // The two constants PROP-RECIPE-BAKE-1 actually measured on mc2_10:
    // BRIGHT = 192/255 = 0.7529, DIM = 63/255 = 0.2471. Using them makes the
    // asserted damage literally the reported 3x, and keeps the two states
    // distinct — an earlier draft of this test gave both classes the SAME
    // terrain sample and the "clobber" then produced identical bytes, i.e. the
    // fixture, not the mechanism, decided the verdict.
    const uint32_t kBrightRGB = 0xFFC0C0C0u;   // 0.7529
    const uint32_t kDimRGB    = 0xFF3F3F3Fu;   // 0.2471

    // State A — sun ACTIVE (a building bake): row slot 0 is the SUN.
    olr::Query qA = buildingQuery(lights);
    qA.sunSlot.aRGB = kBrightRGB;
    olr::Row rowA{};
    pureRow(qA, &rowA);
    CH_CHECK(t, rowA.lightDir[0][3] == (float)olr::kTypeInfinite);
    CH_CHECK(t, rowA.lightColor[0][0] == 192.0f / 255.0f);

    // State B — sun DISABLED (inside a tree's window): first active is AMBIENT.
    olr::Query qB = treeQuery(lights);
    qB.ambientSlot.aRGB = kDimRGB;

    // The forbidden pairing: a row gathered in state A, decomposed in state B.
    olr::Row crossed = rowA;
    const uint32_t rv = modelDecomposeFirstActiveLightColor(qB, &crossed);
    CH_CHECK(t, rv == 0u);
    CH_CHECK(t, !rowsEqual(rowA, crossed));
    // ...and the damage is exactly "the ambient's colour replaced the sun's",
    // 0.7529 -> 0.2471, the 3x the field report describes.
    CH_CHECK(t, crossed.lightColor[0][0] == 63.0f / 255.0f);
    CH_CHECK(t, crossed.lightDir[0][3] == (float)olr::kTypeInfinite);   // still typed sun

    // The guard that makes it unreachable, asserted as a property: the two
    // states differ in an `active` bit, which sceneLightTemplateKey hashes, so
    // they cannot share a template.
    bool activeMaskDiffers = false;
    for (uint32_t i = 0; i < (uint32_t)lights.size(); ++i) {
        bool aA = false, aB = false; uint32_t cA = 0, cB = 0;
        effectiveLight(qA, i, &aA, &cA);
        effectiveLight(qB, i, &aB, &cB);
        if (aA != aB) { activeMaskDiffers = true; break; }
    }
    CH_CHECK(t, activeMaskDiffers);

    // Same-state decompose on the same row: no-op. (The contrast that makes the
    // line above meaningful rather than decorative.)
    olr::Row sameState = rowA;
    modelDecomposeFirstActiveLightColor(qA, &sameState);
    CH_CHECK(t, rowsEqual(rowA, sameState));
    return t.failures == 0;
}

//=============================================================================
// Packing contract — the parts a downstream consumer decodes.
//=============================================================================

// The recoloured slot must carry the terrain sample, and nothing else may move.
static bool test_recolour_lands_in_the_declared_slot(TestCtx& t)
{
    auto lights = makeWorldLights(1);

    olr::Row bldg{};
    pureRow(buildingQuery(lights), &bldg);
    CH_CHECK(t, bldg.lightColor[0][0] == ((kTerrainRGB >> 16) & 0xFF) / 255.0f);
    CH_CHECK(t, bldg.lightColor[0][1] == ((kTerrainRGB >>  8) & 0xFF) / 255.0f);
    CH_CHECK(t, bldg.lightColor[0][2] == ((kTerrainRGB      ) & 0xFF) / 255.0f);
    // Ambient (row slot 1) must be untouched by a sun-slot policy.
    CH_CHECK(t, bldg.lightColor[1][0] == ((lights[1].aRGB >> 16) & 0xFF) / 255.0f);

    // Tree: sun gone, so ambient is row slot 0 AND carries the terrain sample.
    olr::Row tree{};
    pureRow(treeQuery(lights), &tree);
    CH_CHECK(t, tree.lightColor[0][0] == ((kTerrainRGB >> 16) & 0xFF) / 255.0f);
    return t.failures == 0;
}

// LIGHT-POS-COLUMN-1: the 3x4 AffineMatrix4D trap. Translation arrives at
// floats [3]/[7]/[11]; consumers read m[3].xyz == [12][13][14].
static bool test_pos_column_fix(TestCtx& t)
{
    auto lights = makeWorldLights(1);
    olr::Query q = buildingQuery(lights);

    q.posColumnFix = true;
    olr::Row on{};
    pureRow(q, &on);
    // Row slot 2 is the POINT light (sun, ambient, point).
    CH_CHECK(t, on.lightToWorld[2][12] == on.lightToWorld[2][3]);
    CH_CHECK(t, on.lightToWorld[2][13] == on.lightToWorld[2][7]);
    CH_CHECK(t, on.lightToWorld[2][14] == on.lightToWorld[2][11]);
    CH_CHECK(t, on.lightToWorld[2][12] == 10.0f);
    CH_CHECK(t, on.lightToWorld[2][15] == 1.0f);

    q.posColumnFix = false;
    olr::Row off{};
    pureRow(q, &off);
    CH_CHECK(t, off.lightToWorld[2][12] == 0.0f);
    CH_CHECK(t, off.lightToWorld[2][13] == 0.0f);
    CH_CHECK(t, off.lightToWorld[2][14] == 0.0f);
    CH_CHECK(t, off.lightToWorld[2][15] == 1.0f);
    return t.failures == 0;
}

// The 32-light ABI cap: never write past slot 31, and stop counting at 32.
static bool test_abi_cap_is_never_exceeded(TestCtx& t)
{
    auto lights = makeWorldLights(200);
    olr::Row r{};
    pureRow(buildingQuery(lights), &r);
    CH_CHECK(t, r.numLights_ == (int32_t)olr::kMaxLights);

    // Fixture must not overrun either, on top of an already-full row.
    olr::Query q = buildingQuery(lights);
    q.clampFixture = true;
    olr::Row f{};
    pureRow(q, &f);
    CH_CHECK(t, f.numLights_ == (int32_t)olr::kMaxLights);
    return t.failures == 0;
}

// MC2_LIGHT_CLAMP_FIXTURE fills [n,32) with deterministic synthetic points.
static bool test_clamp_fixture_fills_to_cap(TestCtx& t)
{
    auto lights = makeWorldLights(1);
    olr::Query q = buildingQuery(lights);
    q.clampFixture = true;
    olr::Row r{};
    pureRow(q, &r);
    CH_CHECK(t, r.numLights_ == (int32_t)olr::kMaxLights);
    for (uint32_t s = 3; s < olr::kMaxLights; ++s) {
        CH_CHECK(t, r.lightDir[s][3] == (float)olr::kTypePoint);
        CH_CHECK(t, r.lightFalloff[s][1] == 1000.0f);
    }
    return t.failures == 0;
}

// BYTE-IDENTITY REQUIREMENT: the legacy gather writes only [0, numLights) into
// a caller-owned struct that is REUSED (leaf->lightData_). Zeroing the tail
// would silently diverge from the shipped rows in exactly the bytes a memcmp
// is watching. Assert the tail is preserved.
static bool test_tail_slots_are_left_untouched_on_reuse(TestCtx& t)
{
    auto lights = makeWorldLights(1);

    olr::Row reused{};
    std::memset(&reused, 0xAB, sizeof(reused));
    pureRow(buildingQuery(lights), &reused);

    // sun + ambient + 1 pool light, all active.
    CH_CHECK(t, reused.numLights_ == 3);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(reused.lightToWorld[3]);
    bool tailPreserved = true;
    for (size_t i = 0; i < 16 * sizeof(float); ++i)
        if (p[i] != 0xAB) { tailPreserved = false; break; }
    CH_CHECK(t, tailPreserved);
    // pad[] is likewise never written by the packer.
    CH_CHECK(t, reused.pad[0] == (int32_t)0xABABABAB);
    return t.failures == 0;
}

// Purity: the query's light list is an input, never an output.
static bool test_build_does_not_mutate_its_inputs(TestCtx& t)
{
    auto lights = makeWorldLights(4);
    const auto before = lights;

    olr::Row r{};
    pureRow(treeQuery(lights), &r);        // the policy that "disables" the sun
    pureRow(buildingQuery(lights), &r);

    CH_CHECK(t, std::memcmp(before.data(), lights.data(),
                            before.size() * sizeof(olr::LightInput)) == 0);
    // Specifically: the sun is still active in the source list.
    CH_CHECK(t, lights[0].active);
    return t.failures == 0;
}

// Determinism: identical query, identical bytes, every time.
static bool test_determinism(TestCtx& t)
{
    auto lights = makeWorldLights(5);
    olr::Row a{}, b{};
    pureRow(treeQuery(lights), &a);
    pureRow(treeQuery(lights), &b);
    CH_CHECK(t, rowsEqual(a, b));
    return t.failures == 0;
}

// Degenerate inputs must not crash or write garbage.
static bool test_empty_and_null_light_lists(TestCtx& t)
{
    olr::Row r{};
    std::memset(&r, 0x5A, sizeof(r));

    olr::Query q;                          // null list, zero lights
    olr::BuildObjectLightRow(q, &r);
    CH_CHECK(t, r.numLights_ == 0);

    std::vector<olr::LightInput> none;
    olr::Query q2 = buildingQuery(none);
    q2.lights = nullptr;
    olr::BuildObjectLightRow(q2, &r);
    CH_CHECK(t, r.numLights_ == 0);

    olr::BuildObjectLightRow(q, nullptr);  // must not crash
    return t.failures == 0;
}

//=============================================================================
// SELF-TEST OF THE CHECK ITSELF.
//
// Four checks shipped today passed their own selftests while blind, two
// satisfied merely by an identifier in a comment. This test proves the
// comparator can actually SEE a divergence: it perturbs one class's policy and
// asserts byteIdentity would fail. Registered inDefault so the green suite
// itself carries the proof.
//=============================================================================
static bool test_comparator_detects_injected_divergence(TestCtx& t)
{
    auto lights = makeWorldLights(2);

    // 1) Wrong colour in the declared policy -> rows must differ.
    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.aRGB ^= 0x00000001u;           // one bit
        pureRow(q, &pure);
        CH_CHECK(t, !rowsEqual(legacy, pure));
    }
    // 2) Policy pointed at the wrong slot -> rows must differ.
    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.index = 1;                      // ambient instead of sun
        pureRow(q, &pure);
        CH_CHECK(t, !rowsEqual(legacy, pure));
    }
    // 3) Tree policy losing its sun-disable -> rows must differ (this is the
    //    exact divergence that would silently re-open PROP-BAKE-ORDER).
    {
        olr::Query q = treeQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.forceInactive = false;
        pureRow(q, &pure);
        CH_CHECK(t, !rowsEqual(legacy, pure));
    }
    // 4) Dropping the policy entirely -> rows must differ.
    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        pureRow(olr::withoutPolicy(q), &pure);
        CH_CHECK(t, !rowsEqual(legacy, pure));
    }
    // 5) And firstDiff must actually name a field, not return "identical".
    {
        olr::Query q = buildingQuery(lights);
        olr::Row legacy{}, pure{};
        legacyRow(lights, q, &legacy);
        q.sunSlot.aRGB = 0xFF000000u;
        pureRow(q, &pure);
        CH_CHECK(t, firstDiff(legacy, pure).find("lightColor") != std::string::npos);
    }
    return t.failures == 0;
}

int main(int argc, char** argv)
{
    Harness h("object_light_row_harness");
    h.add("building_policy_is_byte_identical",   test_building_policy_is_byte_identical);
    h.add("tree_policy_is_byte_identical",       test_tree_policy_is_byte_identical);
    h.add("mech_policy_is_byte_identical",       test_mech_policy_is_byte_identical);
    h.add("vehicle_policy_is_byte_identical",    test_vehicle_policy_is_byte_identical);
    h.add("generic_prop_policy_is_byte_identical", test_generic_prop_policy_is_byte_identical);
    h.add("byte_identity_across_light_counts",   test_byte_identity_across_light_counts);
    h.add("byte_identity_with_holes",            test_byte_identity_with_holes);
    h.add("byte_identity_across_env_switches",   test_byte_identity_across_env_switches);
    h.add("row_is_independent_of_bake_order",    test_row_is_independent_of_bake_order);
    h.add("row_is_independent_of_bake_time",     test_row_is_independent_of_bake_time);
    h.add("tree_policy_removes_sun_from_row",    test_tree_policy_removes_sun_from_row);
    h.add("decompose_slot0_is_a_noop_after_gather",
          test_decompose_slot0_is_a_noop_after_gather);
    h.add("decompose_clobbers_slot0_only_across_a_light_state_change",
          test_decompose_clobbers_slot0_only_across_a_light_state_change);
    h.add("recolour_lands_in_the_declared_slot", test_recolour_lands_in_the_declared_slot);
    h.add("pos_column_fix",                      test_pos_column_fix);
    h.add("abi_cap_is_never_exceeded",           test_abi_cap_is_never_exceeded);
    h.add("clamp_fixture_fills_to_cap",          test_clamp_fixture_fills_to_cap);
    h.add("tail_slots_are_left_untouched_on_reuse", test_tail_slots_are_left_untouched_on_reuse);
    h.add("build_does_not_mutate_its_inputs",    test_build_does_not_mutate_its_inputs);
    h.add("determinism",                         test_determinism);
    h.add("empty_and_null_light_lists",          test_empty_and_null_light_lists);
    h.add("comparator_detects_injected_divergence", test_comparator_detects_injected_divergence);
    return h.run(argc, argv);
}
