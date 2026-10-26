// Minimal assert-based runner. Returns nonzero on any failure.
// Fixtures dir comes from argv[1]; the compiled binary lives in the build tree.
#include "model_override_registry.h"
#include <cstdio>
#include <string>
#include "loose_asset_priority.h"

static int failures = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++failures; } }while(0)

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "fixtures";
    auto fx = [&](const char* f){ return dir + "/" + f; };

    {   // An unstamped user KTX2 wins even when a stock TGA has the same stem.
        const auto fixture = std::filesystem::temp_directory_path() / "mc2-sidecar-priority-fixture";
        std::filesystem::create_directories(fixture);
        const auto sidecar = fixture / "building.ktx2";
        const auto source = fixture / "building.tga";
        const auto stamp = sidecar.string() + ".cookstamp";
        { std::ofstream out(source); out << "stock"; }
        { std::ofstream out(sidecar); out << "hello"; }
        CHECK(!verifiedLooseStockAsset(sidecar));
        CHECK(looseAssetDiffers(sidecar.string(), {}));
        // FNV-1a-64 standard vector in a deploy-only stock receipt.
        { std::ofstream out(stamp); out << "mc2-stock-v1 a430d84680aabd0b\n"; }
        CHECK(verifiedLooseStockAsset(sidecar));
        CHECK(!verifiedLooseStockAsset(source));
        { std::ofstream out(sidecar); out << "jello"; } // same-size user edit
        CHECK(!verifiedLooseStockAsset(sidecar));
        CHECK(looseAssetDiffers(sidecar.string(), {}));
        std::filesystem::remove(source); // archived-only sources do not change the verdict
        CHECK(!verifiedLooseStockAsset(sidecar));
        { std::ofstream out(stamp); out << "invalid"; }
        CHECK(!verifiedLooseStockAsset(sidecar));
        std::filesystem::remove(sidecar);
        CHECK(!verifiedLooseStockAsset(sidecar));
        std::filesystem::remove(stamp);
        std::filesystem::remove(fixture);
    }

    {   // A legacy overlay of a kit building must win; extracted stock must not.
        const auto fixture = std::filesystem::temp_directory_path() / "mc2-building-priority-fixture";
        std::filesystem::create_directories(fixture / "mods/test/data/tgl");
        const std::vector<unsigned char> stock{'s', 't', 'o', 'c', 'k'};
        for (const char* name : {"chemicalplant.ini", "chemicalplant.tgl", "chemicalplant.ase",
                                 "chemicalplant.glb", "chemicalplantd.tgl", "chemicalplant.agl", "cp.tga", "cp.ktx2"}) {
            const auto path = fixture / "mods/test/data/tgl" / name;
            { std::ofstream out(path, std::ios::binary); out << "stock"; }
            CHECK(!looseAssetDiffers(path.string().c_str(), stock));
            { std::ofstream out(path, std::ios::binary); out << "modded building"; }
            CHECK(looseAssetDiffers(path.string().c_str(), stock));
            CHECK(looseAssetDiffers(path.string().c_str(), {}));
            std::filesystem::remove(path);
            CHECK(!looseAssetDiffers(path.string().c_str(), stock));
        }
        // Root manifests merge after the kit, with provider-relative sources retained.
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("valid.json"), "mods/mc2-industrial-kit/data/model_overrides") == 1);
        CHECK(r.mergeFromFile(fx("valid.json"), "data/model_overrides") == 1);
        CHECK(r.resolve("staticProp", "example_name")->manifestDir == "data/model_overrides");
        CHECK(r.mergeFromFile(fx("valid.json"), "mods/test/data/model_overrides") == 1);
        CHECK(r.resolve("staticProp", "example_name")->manifestDir == "mods/test/data/model_overrides");
    }

    {   ModelOverrideRegistry r;
        int n = r.loadFromFile(fx("valid.json"), dir);
        CHECK(n == 1);
        CHECK(r.count() == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "example_name");
        CHECK(m != nullptr);
        if (m) {
            CHECK(m->overrideClass == "staticprop");
            CHECK(m->appearanceName == "example_name");
            CHECK(m->scale == 1.0f);
            CHECK(!m->lateCoverageFringe);
            CHECK(m->sourceRelPath == "source/props/example.glb");
        }
        CHECK(r.resolve("STATICPROP", "Example_Name") != nullptr);
        CHECK(r.resolve("tree", "example_name") == nullptr);
        CHECK(r.resolve(nullptr, "example_name") == nullptr);
        CHECK(r.resolve("staticProp", nullptr) == nullptr);
    }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("not_renderonly.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("bad_fallback.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("bad_scale.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("unsafe_paths.json"), dir) == 0); }
    {   ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("dup.json"), dir) == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "dup_name");
        CHECK(m != nullptr);
        if (m) CHECK(m->sourceRelPath == "first.glb");
    }
    {   ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("non_object_entry.json"), dir) == 1);
        CHECK(r.resolve("tree", "ok") != nullptr);
    }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("malformed.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("does_not_exist.json"), dir) == 0); }
    {   ModelOverrideRegistry r; CHECK(r.loadFromFile(fx("type_mismatch.json"), dir) == 0); }
    {   ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("pbr.json"), dir) == 13);
        const auto* good = r.resolve("staticProp", "valid");
        CHECK(good && good->materials.size() == 1);
        CHECK(good && good->lateCoverageFringe);
        if (good && !good->materials.empty()) {
            CHECK(good->materials[0].materialClass == 4);
            CHECK(good->materials[0].uvScale[1] == 3);
        }
        for (const char* name : {"unreviewed", "bad_class", "unsafe", "bad_uv", "bad_scalar", "dielectric_metal", "bad_version", "duplicate", "huge_slot", "huge_version", "rust_metal"}) {
            const auto* bad = r.resolve("staticProp", name);
            CHECK(bad != nullptr); // bad detail keeps the geometry override
            if (bad) CHECK(bad->materials.empty());
        }
        const auto* two = r.resolve("staticProp", "two_slots");
        CHECK(two && two->materials.size() == 2);
        ModelOverrideRegistry empty;
        CHECK(empty.loadFromFile(fx("valid.json"), dir) == 1);
        const auto* unmapped = empty.resolve("staticProp", "example_name");
        CHECK(unmapped && unmapped->materials.empty());
        CHECK(r.mergeFromFile(fx("pbr.json"), "another-provider") == 13);
        const auto* replaced = r.resolve("staticProp", "valid");
        CHECK(replaced && replaced->manifestDir == "another-provider");
    }
    {   // materials v2, destroyed chains, condition variants (content pipeline packages).
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("content_v2.json"), dir) == 5);
        const auto* h = r.resolve("staticProp", "hangar");
        CHECK(h != nullptr);
        if (h) {
            CHECK(h->destroyed.sourceRelPath == "hangar/pristine_destroyed_lod0.glb");
            CHECK(h->conditions.size() == 2);                       // "ruined" dropped
            const auto* worn = h->condition("worn");
            CHECK(worn && worn->intact.sourceRelPath == "hangar/worn_intact_lod0.glb");
            CHECK(worn && worn->destroyed.sourceRelPath == "hangar/worn_destroyed_lod0.glb");
            const auto* abandoned = h->condition("abandoned");
            CHECK(abandoned && !abandoned->destroyed.present());    // falls back to the pristine destroyed chain
            CHECK(h->condition("") == nullptr);
            CHECK(h->materials.size() == 3);
            if (h->materials.size() == 3) {
                CHECK(h->materials[0].materialClass == 2 && h->materials[0].alphaMode == 0);
                CHECK(h->materials[0].normalPath == "hangar/detail/0_normal.ktx2");
                CHECK(h->materials[1].materialClass == 3 && h->materials[1].metallic == 1.0f);   // class default
                CHECK(h->materials[1].roughness == 0.45f && h->materials[1].ormPath.empty());
                CHECK(h->materials[2].alphaMode == 2 && h->materials[2].alphaCutoff == 0.4f);
                CHECK(h->materials[2].metallic == 0.7f);
            }
        }
        for (const char* name : {"concrete_metal", "ktx_in_v1", "blend"}) {
            const auto* m = r.resolve("staticProp", name);
            CHECK(m && m->materials.empty());          // never metal from a non-metal class; v1 stays TGA-only
        }
        const auto* bd = r.resolve("staticProp", "bad_destroyed");
        CHECK(bd && !bd->destroyed.present());

        CHECK(r.mergeConditionsFromFile(fx("conditions.json")) == 3);   // "derelict" dropped
        CHECK(r.selectedCondition("mc2_01", "hangar") == "abandoned");
        CHECK(r.selectedCondition("data/missions/MC2_01.fit", "Quonset") == "worn");   // wildcard, path + case
        CHECK(r.selectedCondition("mc2_10", "hangar").empty());                       // explicit pristine
        CHECK(r.selectedCondition("mc2_24", "hangar").empty());                       // default pristine
    }
    {   // VFS mount order: lowest priority first; active last; env deps highest-first; de-duplicated.
        const auto order = modelOverrideMountOrder("camp", "addon_a, addon_b;base", {"base"});
        CHECK(order.size() == 4);
        if (order.size() == 4) {
            CHECK(order[0] == "mods/addon_b");
            CHECK(order[1] == "mods/addon_a");
            CHECK(order[2] == "mods/base");
            CHECK(order[3] == "mods/camp");
        }
        const auto stockAddons = modelOverrideMountOrder("", "content_pack", {});
        CHECK(stockAddons.size() == 1 && stockAddons[0] == "mods/content_pack");
        CHECK(modelOverrideMountOrder("", "", {}).empty());
    }
    printf(failures ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
