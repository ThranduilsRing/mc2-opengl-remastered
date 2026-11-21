#include "doctest.h"
#include "../../GameOS/gameos/visual_tuning_profile.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options.h"
#include "file.h"
#include "vfs.h"
#include "../../GameAdapters/MissionEnvironmentAdapter.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

// Real profile loader/resolver; only the renderer setters are replaced.
namespace {
float exposure=1, softness=2.5f, water=0, brightness=1, additive=1, lit=0.7f, specular=0.05f;
int waterReflections=2;
std::array<float,4> grass{-0.02f,0.06f,0.22f,0.40f}, dirt{-0.02f,0.06f,0.22f,0.45f};
VisualTuningSource source(std::string text) { return {"data/visual_tuning.json","fixture",std::move(text),true}; }
struct TempProfile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mc2-environment-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
    explicit TempProfile(const char* text) { write(text); }
    void write(const char* text) { std::ofstream f(path); f << text; REQUIRE(bool(f)); }
    ~TempProfile() { std::error_code ec; std::filesystem::remove(path,ec); }
};
}
float g_iblShStrength=0.5f;
float gos_GetExposure() { return exposure; }
void gos_SetExposure(float v) { exposure=v; }
float gos_GetTerrainShadowSoftness() { return softness; }
void gos_SetTerrainShadowSoftness(float v) { softness=v; }
float gos_GetWaterSkyTintStrength() { return water; }
void gos_SetWaterSkyTintStrength(float v) { water=v; }
int gos_GetWaterReflectionLegacy() { return waterReflections; }
void gos_SetWaterReflectionQuality(int v) { waterReflections=v; }
extern "C" float gos_vfx_getBrightness() { return brightness; }
extern "C" void gos_vfx_setBrightness(float v) { brightness=v; }
extern "C" float gos_vfx_getAdditiveBrightness() { return additive; }
extern "C" void gos_vfx_setAdditiveBrightness(float v) { additive=v; }
extern "C" float gos_vfx_getLitStrength() { return lit; }
extern "C" void gos_vfx_setLitStrength(float v) { lit=v; }
extern "C" float batcher_getMechSpecularStrength() { return specular; }
extern "C" void batcher_setMechSpecularStrength(float v) { specular=v; }
class gosPostProcess;
gosPostProcess* getGosPostProcess() { return nullptr; }  // cloud knobs: no post-process in the unit tier
void mc2SetCloudWaterScale(float) {}
void gos_SetTerrainClassGrass(float a,float b,float c,float d) { grass={a,b,c,d}; }
void gos_GetTerrainClassGrass(float* a,float* b,float* c,float* d) { *a=grass[0];*b=grass[1];*c=grass[2];*d=grass[3]; }
void gos_SetTerrainClassDirt(float a,float b,float c,float d) { dirt={a,b,c,d}; }
void gos_GetTerrainClassDirt(float* a,float* b,float* c,float* d) { *a=dirt[0];*b=dirt[1];*c=dirt[2];*d=dirt[3]; }

TEST_CASE("MissionEnvironment/unit fill and building spill profile bounds and mission reset") {
    const auto profile = source(R"({"defaults":{"unitAmbientScale":1.2,"buildingSpillIntensityScale":1},"missions":{"a":{"unitAmbientScale":1.4,"buildingSpillIntensityScale":0.5},"invalid":{"unitAmbientScale":2.1,"buildingSpillIntensityScale":-1}}})");
    const auto a = visualTuning_resolve("a", {profile});
    CHECK(a.unitAmbientScale == 1.4f);
    CHECK(a.buildingSpillIntensityScale == 0.5f);
    const auto b = visualTuning_resolve("b", {profile});
    CHECK(b.unitAmbientScale == 1.2f);
    CHECK(b.buildingSpillIntensityScale == 1.0f);
    CHECK(a.effectiveHash != b.effectiveHash);
    const auto invalid = visualTuning_resolve("invalid", {profile});
    CHECK(invalid.unitAmbientScale == 1.2f);
    CHECK(invalid.buildingSpillIntensityScale == 1.0f);
}

TEST_CASE("MissionEnvironment/version migration preserves exact shipped tuning") {
    std::ifstream file(std::filesystem::path(MC2_REPO_ROOT)/"data/visual_tuning.json");
    REQUIRE(bool(file));
    const std::string text((std::istreambuf_iterator<char>(file)),{});
    auto old=nlohmann::json::parse(text); old.erase("schemaVersion"); old.erase("colorContract");
    for (const auto* mission : {"mc2_01","mc2_10"}) {
        const auto legacy=visualTuning_resolve(mission,{source(old.dump())});
        const auto current=visualTuning_resolve(mission,{source(text)});
        CHECK(legacy.legacyTuning==current.legacyTuning);
        CHECK(legacy.effectiveHash==current.effectiveHash);
        CHECK(legacy.sourceSchemaVersion==1);
        CHECK(current.sourceSchemaVersion==2);
        CHECK(current.legacyTuning.at("staticPropIblStrength")==0.6f);
        CHECK(current.legacyTuning.at("mechSpecularStrength")==0.05f);
        CHECK(current.output.exposureMultiplier==1.0f);
        CHECK_FALSE(current.legacyTuning.contains("mechAmbientStrength"));
    }
}

TEST_CASE("MissionEnvironment/default mission session precedence and missing mission reset") {
    auto layer=source(R"({"defaults":{"exposure":1.2,"vfxBrightness":2},"missions":{"a":{"exposure":1.5,"mechSpecularStrength":0.8}}})");
    const auto a=visualTuning_resolve("a",{layer},{{"exposure",2.0f}});
    CHECK(a.output.exposureMultiplier==2.0f);
    const auto b=visualTuning_resolve("b",{layer});
    CHECK(b.output.exposureMultiplier==1.2f);
    CHECK(b.legacyTuning.at("mechSpecularStrength")==0.05f);
    CHECK(visualTuning_resolve("c",{}).legacyTuning.at("vfxBrightness")==1.0f);
}

TEST_CASE("MissionEnvironment/output defaults neutral and mission grade resets") {
    const auto profile=source(R"({"missions":{"warm":{"gradeR":1.05,"gradeShadowsB":1.04,"vignette":0.18,"topWarmthR":1.1,"topWarmthStrength":0.25}}})");
    const auto warm=visualTuning_resolve("warm",{profile});
    CHECK(warm.output.grade[0]==1.05f);
    CHECK(warm.output.shadows[2]==1.04f);
    CHECK(warm.output.vignette==0.18f);
    CHECK(warm.output.topWarmthStrength==0.25f);
    const auto neutral=visualTuning_resolve("day",{profile});
    for (const auto* rgb : {&neutral.output.grade,&neutral.output.shadows,
             &neutral.output.highlights,&neutral.output.topWarmth})
        CHECK(*rgb==std::array<float,3>{1,1,1});
    CHECK(neutral.output.vignette==0);
    CHECK(neutral.output.topWarmthStrength==0);
    CHECK(neutral.effectiveHash!=warm.effectiveHash);
}

TEST_CASE("MissionEnvironment/reject invalid versions colors types and ranges without stale values") {
    for (const auto* bad : {"{", R"({"schemaVersion":99,"defaults":{"exposure":2}})",
            R"({"schemaVersion":2.5})", R"({"colorContract":"physical-but-not-yet-supported"})"}) {
        const auto env=visualTuning_resolve("b",{source(bad)});
        CHECK(env.output.exposureMultiplier==1.0f); CHECK_FALSE(env.diagnostics.empty());
    }
    const auto env=visualTuning_resolve("b",{source(R"({"defaults":{"exposure":"2","vfxBrightness":-2,"staticPropIblStrength":1e300,"unknown":1,"mechSpecularStrength":0.4}})")},
        {{"exposure",std::numeric_limits<float>::quiet_NaN()}});
    CHECK(env.output.exposureMultiplier==1.0f);
    CHECK(env.legacyTuning.at("vfxBrightness")==1.0f);
    CHECK(env.legacyTuning.at("staticPropIblStrength")==0.5f);
    CHECK(env.legacyTuning.at("mechSpecularStrength")==0.4f);
    CHECK(env.diagnostics.size()==5);
}

TEST_CASE("MissionEnvironment/hash is stable and zero exposure has no invalid logarithm") {
    const auto a=visualTuning_resolve("a",{source(R"({"defaults":{"exposure":0,"mechSpecularStrength":0.3}})")});
    const auto b=visualTuning_resolve("a",{source(R"({"defaults":{"mechSpecularStrength":0.3,"exposure":0}})")});
    CHECK(a.effectiveHash==b.effectiveHash);
    CHECK_FALSE(a.output.exposureEV.has_value());
    CHECK(a.output.exposureMultiplier==0);
    CHECK(a.effectiveHash!=visualTuning_resolve("a",{}).effectiveHash);
}

TEST_CASE("MissionEnvironment/real loader resets renderer state on mission and missing file") {
    TempProfile file(R"({"defaults":{"exposure":2,"mechSpecularStrength":0.8,"terrainClassGrassGmRLo":-0.5}})");
    mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> path(file.path.string().c_str());
    mc2::gate::ScopedOverride<"MC2_MECH_SPECULAR_STRENGTH"> specularEnv(nullptr);
    visualTuning_applyProfile("not-authored");
    CHECK(exposure==2.0f); CHECK(specular==0.8f); CHECK(grass[0]==-0.5f);
    RenderCore::LegacyMissionLighting lighting; lighting.skyNumber=14; lighting.night=true;
    visualTuning_setMissionLighting(lighting);
    CHECK(visualTuning_environment().sky.shIdentity=="mission-ambient-night");
    lighting.ambientColor={0.24f,0.20f,0.16f};
    visualTuning_setMissionLighting(lighting);
    const auto& nightSky=visualTuning_environment().sky;
    CHECK(nightSky.hemisphereSky[0]==doctest::Approx(0.06f));
    CHECK(nightSky.hemisphereGround[0]==doctest::Approx(0.03f));
    for (size_t c=0;c<3;++c) {
        CHECK(0.886227f*nightSky.diffuseSh[0][c]+2.0f*0.511664f*nightSky.diffuseSh[1][c]
            ==doctest::Approx(nightSky.hemisphereSky[c]));
        for (size_t i=2;i<9;++i) CHECK(nightSky.diffuseSh[i][c]==0);
    }
    file.write(R"({"defaults":{"exposure":1.5}})");
    visualTuning_applyProfile("also-not-authored");
    CHECK(exposure==1.5f); CHECK(specular==0.05f); CHECK(grass[0]==-0.02f);
    CHECK(visualTuning_environment().lighting.skyNumber==0);
    CHECK_FALSE(visualTuning_environment().lighting.night);
    std::filesystem::remove(file.path);
    visualTuning_applyProfile("not-authored");
    CHECK(exposure==1.0f); CHECK(specular==0.05f);
}

TEST_CASE("MissionEnvironment/save migrates without losing unrelated fields or missions") {
    TempProfile file(R"({"custom":{"retain":true},"defaults":{"exposure":1},"missions":{"other":{"exposure":1.4},"a":{"customField":"retain"}}})");
    mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> path(file.path.string().c_str());
    visualTuning_applyProfile("a"); exposure=1.7f; waterReflections=1;
    REQUIRE(visualTuning_saveCurrentToMission());
    std::ifstream input(file.path); nlohmann::json saved; input>>saved;
    CHECK(saved["schemaVersion"]==2);
    CHECK(saved["custom"]["retain"]==true);
    CHECK(saved["missions"]["other"]["exposure"]==1.4);
    CHECK(saved["missions"]["a"]["customField"]=="retain");
    CHECK(saved["missions"]["a"]["exposure"].get<float>()==1.7f);
    CHECK(saved["missions"]["a"]["waterReflections"].get<int>()==1);
}

TEST_CASE("MissionEnvironment/save keeps the raw legacy water value, not the effective one") {
    TempProfile file(R"({"missions":{"a":{}}})");
    mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> path(file.path.string().c_str());
    visualTuning_applyProfile("a"); waterReflections=2;  // legacy 2 while the Off option would make the effective value 1
    REQUIRE(visualTuning_saveCurrentToMission());
    std::ifstream input(file.path); nlohmann::json saved; input>>saved;
    CHECK(saved["missions"]["a"]["waterReflections"].get<int>()==2);
}

TEST_CASE("MissionEnvironment/invalid files cannot be overwritten by save") {
    for (const auto* bad : {"{",R"({"schemaVersion":99})"}) {
        TempProfile file(bad);
        mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> path(file.path.string().c_str());
        visualTuning_applyProfile("a");
        CHECK_FALSE(visualTuning_saveCurrentToMission());
        std::ifstream input(file.path);
        CHECK(std::string((std::istreambuf_iterator<char>(input)),{})==bad);
    }
}

namespace {
struct MountedProfiles {
    std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("mc2-environment-vfs-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    MountedProfiles() {
        REQUIRE_FALSE(std::filesystem::exists(root));
        std::filesystem::create_directory(root);
        std::filesystem::current_path(root);
        put("data/visual_tuning.json",R"({"defaults":{"exposure":1.1}})");
        put("mods/env-a/mod.json",R"({"id":"env-a","name":"A"})");
        put("mods/env-b/mod.json",R"({"id":"env-b","name":"B"})");
        put("mods/env-active/mod.json",R"({"id":"env-active","name":"Active","dependencies":["env-a","env-b"]})");
        put("mods/env-a/data/visual_tuning.json",R"({"defaults":{"exposure":1.2}})");
        put("mods/env-b/data/visual_tuning.json",R"({"defaults":{"exposure":1.3}})");
        put("mods/env-active/data/visual_tuning.json",R"({"defaults":{"exposure":1.4},"missions":{"other":{"exposure":1.6}}})");
    }
    static void put(const std::filesystem::path& path,const char* text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path); file<<text; REQUIRE(bool(file));
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream file(path); return {std::istreambuf_iterator<char>(file),{}};
    }
    ~MountedProfiles() {
        mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> clear(nullptr);
        InitModSearchPaths("");
        std::filesystem::current_path(previous);
        // Only this fixture's newly created absolute child of the temp directory is owned.
        if (root.is_absolute() && root.parent_path()==std::filesystem::temp_directory_path() &&
            root.filename().string().starts_with("mc2-environment-vfs-")) {
            std::error_code error; std::filesystem::remove_all(root,error);
        }
    }
};
}

TEST_CASE("MissionEnvironment/real VFS active dependency order base fallback and editable provider") {
    MountedProfiles files;
    mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> active("env-active");
    mc2::gate::ScopedOverride<"MC2_MOD_DEPS"> deps(nullptr);
    mc2::gate::ScopedOverride<"MC2_REBUILD_MOD_CACHE"> rebuild("1");
    mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> session(nullptr);
    InitModSearchPaths("mods");
    visualTuning_applyProfile("a");
    CHECK(exposure==1.4f);
    const auto base=files.read("data/visual_tuning.json");
    const auto dep=files.read("mods/env-b/data/visual_tuning.json");
    exposure=1.7f;
    REQUIRE(visualTuning_saveCurrentToMission());
    auto saved=nlohmann::json::parse(files.read("mods/env-active/data/visual_tuning.json"));
    CHECK(saved["missions"]["other"]["exposure"]==1.6);
    CHECK(saved["missions"]["a"]["exposure"].get<float>()==1.7f);
    CHECK(files.read("data/visual_tuning.json")==base);
    CHECK(files.read("mods/env-b/data/visual_tuning.json")==dep);
    for (const auto& [removed,expected] : std::vector<std::pair<std::string,float>>{
            {"env-active",1.2f},{"env-a",1.3f},{"env-b",1.1f}}) {
        std::filesystem::remove("mods/"+removed+"/data/visual_tuning.json");
        InitModSearchPaths("mods");
        visualTuning_applyProfile("a");
        CHECK(exposure==expected);
        CHECK_FALSE(visualTuning_saveCurrentToMission());
    }
}

TEST_CASE("MissionEnvironment/explicit session profile wins over mounted active profile") {
    MountedProfiles files;
    TempProfile overrideFile(R"({"defaults":{"exposure":2.5}})");
    mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> active("env-active");
    mc2::gate::ScopedOverride<"MC2_MOD_DEPS"> deps(nullptr);
    mc2::gate::ScopedOverride<"MC2_REBUILD_MOD_CACHE"> rebuild("1");
    mc2::gate::ScopedOverride<"MC2_VISUAL_TUNING_FILE"> session(overrideFile.path.string().c_str());
    InitModSearchPaths("mods");
    visualTuning_applyProfile("a");
    CHECK(exposure==2.5f);
    CHECK(GameAdapters::Environment::readProfileSource("data/visual_tuning.json").provider=="mod:env-active");
}

TEST_CASE("MissionEnvironment/loose building and texture overlays beat bundled kit through real VFS") {
    struct RestoreOptions {
        mc2::options::RenderOptions saved = mc2::options::g_current;
        ~RestoreOptions() { mc2::options::g_current = saved; }
    } restore;
    mc2::options::g_current.remasteredBuildings = true;
    MountedProfiles files;
    // The kit mounts only with remastered building art on (shipped default is Classic since 947d077ee).
    struct RemasteredArt {
        mc2::options::RenderOptions saved = mc2::options::current();
        RemasteredArt() { auto o = saved; o.remasteredBuildings = true; mc2::options::publish(o); }
        ~RemasteredArt() { mc2::options::publish(saved); }
    } remasteredArt;
    mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> active(nullptr);
    mc2::gate::ScopedOverride<"MC2_MOD_DEPS"> deps("mc2-industrial-kit,env-a");
    mc2::gate::ScopedOverride<"MC2_REBUILD_MOD_CACHE"> rebuild("1");
    files.put("mods/mc2-industrial-kit/mod.json", R"({"id":"mc2-industrial-kit"})");
    files.put("mods/mc2-industrial-kit/data/tgl/1024/cp_test.ktx2", "kit cooked texture");
    files.put("data/tgl/1024/cp_test.tga", "legacy source texture");
    const auto genBefore = mc2::vfs::generation();
    InitModSearchPaths("mods");
    CHECK(mc2::vfs::generation() != genBefore);   // rank caches (icon_registry pilot sheet rank) key on this
    CHECK(LookupModPriority("data/tgl/1024/cp_test.tga") < LookupModPriority("data/tgl/1024/cp_test.ktx2"));
    CHECK(LookupModPriority("data/tgl/1024/cp_test.ktx2") < LookupModPriority("data/tgl/packed_stock.tga"));
    for (const char* key : {"data/tgl/chemicalplant.tgl", "data/tgl/1024/cp_chemicalplant_0.ktx2"}) {
        const std::string kit = std::string("mods/mc2-industrial-kit/") + key;
        files.put(kit, "kit");
        InitModSearchPaths("mods");
        REQUIRE(LookupModOwner(key));
        CHECK(std::string(LookupModOwner(key)) == "mc2-industrial-kit");
        files.put(key, "legacy building override");
        InitModSearchPaths("mods");
        CHECK(LookupModOwner(key) == nullptr);
        CHECK(LookupModAbsolutePath(key) == nullptr);
        CHECK(fileExists(key) == FILE_ON_DISK);
        File input;
        REQUIRE(input.open(key) == NO_ERR);
        std::string bytes(input.fileSize(), '\0');
        input.read(0, reinterpret_cast<MemoryPtr>(bytes.data()), static_cast<long>(bytes.size()));
        CHECK(bytes == "legacy building override");
        input.close();
        files.put(std::string("mods/env-a/") + key, "mounted user building");
        InitModSearchPaths("mods");
        REQUIRE(LookupModOwner(key));
        CHECK(std::string(LookupModOwner(key)) == "env-a");
    }
}
