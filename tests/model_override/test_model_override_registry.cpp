// Minimal assert-based runner. Returns nonzero on any failure.
// Fixtures dir comes from argv[1]; the compiled binary lives in the build tree.
#include "model_override_registry.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++failures; } }while(0)

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "fixtures";
    auto fx = [&](const char* f){ return dir + "/" + f; };

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
        CHECK(r.loadFromFile(fx("pbr.json"), dir) == 10);
        const auto* good = r.resolve("staticProp", "valid");
        CHECK(good && good->materials.size() == 1);
        if (good && !good->materials.empty()) {
            CHECK(good->materials[0].materialClass == 4);
            CHECK(good->materials[0].uvScale[1] == 3);
        }
        for (const char* name : {"unreviewed", "bad_class", "unsafe", "bad_uv", "bad_scalar", "dielectric_metal", "bad_version", "duplicate"}) {
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
        CHECK(r.mergeFromFile(fx("pbr.json"), "another-provider") == 10);
        const auto* replaced = r.resolve("staticProp", "valid");
        CHECK(replaced && replaced->manifestDir == "another-provider");
    }
    printf(failures ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
