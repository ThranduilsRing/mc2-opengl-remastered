#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate.h"
#include "Gates/gate_name.h"
#include "contract_test_support.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include "particles/spec_library.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

void vfx_test_bind_rng(std::uint32_t* state);

namespace {
constexpr size_t EFFECT_SIZE = 232;  // step 5 (2026-10-18): the 48-byte Stuff LinearMatrix4D members are 64-byte glm::mat4
struct Platform {
    ContractEnvironment deterministic{mc2::gate::name<"MC2_DETERMINISTIC_RNG">(), "1"};
    ContractEnvironment fixedStep{mc2::gate::name<"MC2_SMOKE_FIXED_TIMESTEP">(), "1"};
    ContractEnvironment identity{mc2::gate::name<"MC2_SIM_IDENTITY_TRACE">(), "1"};
    std::uint32_t seed = mc2_det_rng::hashMissionName("mc2_03");
    Platform()
    {
        mc2::gate::markMainEntered();
        vfx_test_bind_rng(&seed);
        Stuff::InitializeClasses();
        MidLevelRenderer::InitializeClasses(64 * 1024, 16 * 1024, 1024, 4096, true);
        gosFX::InitializeClasses();
    }
    ~Platform()
    {
        gosFX::TerminateClasses();
        MidLevelRenderer::TerminateClasses();
        Stuff::TerminateClasses();
        vfx_test_bind_rng(nullptr);
    }
};

class Parent final : public gosFX::Effect {
public:
    explicit Parent(Specification* spec) : Effect(DefaultData, spec, ExecuteFlag) {}
    void adopt(gosFX::EffectPtr child) { m_children.push_back(std::move(child)); }
    bool empty() { return m_children.empty(); }
    size_t count() const { return m_children.size(); }
};

// A child that counts its destructions, so a test can prove each child is freed exactly once by its one owner.
int g_childDestroyed = 0;
class CountedChild final : public gosFX::Effect {
public:
    explicit CountedChild(Specification* spec) : Effect(DefaultData, spec, ExecuteFlag) {}
    ~CountedChild() { ++g_childDestroyed; }
};
}

TEST_CASE_FIXTURE(Platform, "CardCloud layout after retiring its MLR owner")
{
    // The pre-G2 ParticleCloud object wrote its array at 0xd8. Enlarging Effect
    // silently linked incompatible constructors and destructors in the failing VS build.
    // B8a: m_children became std::vector<EffectPtr> (was a Stuff::ChainOf); the sizes moved with it.
    // Step 5: the two matrix members are glm::mat4 (64 bytes, was the 48-byte affine block): Effect grew by 32.
    CHECK(sizeof(gosFX::Effect) == EFFECT_SIZE);
    CHECK(sizeof(gosFX::Effect::ExecuteInfo) == 40);
    // B4: m_data became std::vector<char> (24 bytes, was the 16-byte DAO); ParticleCloud = vector + int + Scalar.
    CHECK(sizeof(gosFX::ParticleCloud) == EFFECT_SIZE + sizeof(std::vector<char>) + 8);
    CHECK(sizeof(gosFX::CardCloud) == EFFECT_SIZE + sizeof(std::vector<char>) + 32);
}

TEST_CASE_FIXTURE(Platform, "CardCloud births and retirement through Effect Execute")
{
    REQUIRE(mc2::gate::get<"MC2_DETERMINISTIC_RNG">());
    REQUIRE(mc2::gate::get<"MC2_SMOKE_FIXED_TIMESTEP">());
    REQUIRE(mc2::gate::get<"MC2_SIM_IDENTITY_TRACE">());
    for (unsigned capacity : {1u, 32u, 128u}) {
        CAPTURE(capacity);
        gosFX::Effect::Specification parentSpec;
        parentSpec.BuildDefaults();
        parentSpec.m_lifeSpan.SetCurve(10.0f);
        gosFX::CardCloud::Specification spec;
        spec.BuildDefaults();
        spec.m_maxParticleCount = capacity;
        spec.m_startingPopulation.SetCurve(static_cast<float>(capacity));
        spec.m_particlesPerSecond.SetCurve(0.0f);
        spec.m_lifeSpan.SetCurve(0.1f);
        spec.m_pLifeSpan.m_ageCurve.SetCurve(0.1f);
        const glm::mat4 transform = glm::mat4(1.0f);
        mc2::math::Obb bounds = mc2::math::Obb();
        gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
        Parent parent(&parentSpec);
        parent.Start(&info);
        auto* cloud = gosFX::CardCloud::Make(&spec, gosFX::Effect::ExecuteFlag);
        parent.adopt(gosFX::EffectPtr{cloud});
        cloud->Start(&info);
        const auto before = Stuff::Random::GetDrawCount();
        info.m_time = 1.0 / 30.0;
        REQUIRE(parent.Execute(&info));
        REQUIRE_FALSE(parent.empty());
        REQUIRE(Stuff::Random::GetDrawCount() > before);
        REQUIRE(cloud->GetParticle(0)->m_age < 1.0f);
        if (mc2::particles::Batcher::is_enabled()) {
            gosFX::Effect::DrawInfo draw{};
            draw.m_parentToWorld = &transform;
            const auto frozen = Stuff::Random::GetState();
            for (int repeat = 0; repeat < 8; ++repeat) {
                cloud->Draw(&draw);
                mc2::particles::Batcher::Instance().Flush();
                CHECK(Stuff::Random::GetState() == frozen);
            }
        }
        for (int frame = 2; frame <= 60 && !parent.empty(); ++frame) {
            info.m_time = frame / 30.0;
            REQUIRE(parent.Execute(&info));
        }
        CHECK(parent.empty());
    }
}

TEST_CASE_FIXTURE(Platform, "Effect is the one owner of its children: create, finish, kill, destroy")
{
    gosFX::Effect::Specification parentSpec;
    parentSpec.BuildDefaults();
    parentSpec.m_lifeSpan.SetCurve(10.0f);
    gosFX::Effect::Specification shortSpec;
    shortSpec.BuildDefaults();
    shortSpec.m_lifeSpan.SetCurve(0.05f);
    gosFX::Effect::Specification longSpec;
    longSpec.BuildDefaults();
    longSpec.m_lifeSpan.SetCurve(10.0f);
    const glm::mat4 transform = glm::mat4(1.0f);

    SUBCASE("a parent destroyed with live children frees each child once")
    {
        g_childDestroyed = 0;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            for (int i = 0; i < 3; ++i) {
                auto child = std::make_unique<CountedChild>(&longSpec);
                child->Start(&info);
                parent.adopt(std::move(child));
            }
            CHECK(parent.count() == 3);
            CHECK(g_childDestroyed == 0);
        }
        CHECK(g_childDestroyed == 3);
    }
    SUBCASE("a finished child is released by Execute, in place, once; live siblings keep their order")
    {
        g_childDestroyed = 0;
        gosFX::Effect* survivor = nullptr;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            auto shortChild = std::make_unique<CountedChild>(&shortSpec);
            shortChild->Start(&info);
            parent.adopt(std::move(shortChild));
            auto longChild = std::make_unique<CountedChild>(&longSpec);
            longChild->Start(&info);
            survivor = longChild.get();
            parent.adopt(std::move(longChild));
            for (int frame = 1; frame <= 10; ++frame) {
                info.m_time = frame / 30.0;
                REQUIRE(parent.Execute(&info));
            }
            CHECK(g_childDestroyed == 1);
            CHECK(parent.count() == 1);
            CHECK_FALSE(parent.HasFinished());
            CHECK(survivor->IsExecuted());
        }
        CHECK(g_childDestroyed == 2);
    }
    SUBCASE("Kill stops the children without freeing them; the owner frees them once")
    {
        g_childDestroyed = 0;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            for (int i = 0; i < 2; ++i) {
                auto child = std::make_unique<CountedChild>(&longSpec);
                child->Start(&info);
                parent.adopt(std::move(child));
            }
            parent.Kill();
            CHECK_FALSE(parent.IsExecuted());
            CHECK(parent.count() == 2);
            CHECK(g_childDestroyed == 0);
        }
        CHECK(g_childDestroyed == 2);
    }
    SUBCASE("an effect that owns a render cloud is created and destroyed repeatedly")
    {
        gosFX::CardCloud::Specification spec;
        spec.BuildDefaults();
        spec.m_maxParticleCount = 8;
        for (int cycle = 0; cycle < 8; ++cycle) {
            gosFX::EffectPtr cloud{gosFX::CardCloud::Make(&spec, gosFX::Effect::ExecuteFlag)};
            REQUIRE(cloud);
        }
    }
}

namespace {
// Live CRT heap blocks (the memory instrumentation of the teardown cases): a teardown that leaks grows this from one cycle to the
// next. ASan replaces the CRT heap, so under MC2_ASAN the count is skipped and ASan itself is the use-after-free/double-free oracle.
long liveHeapBlocks()
{
#if defined(_WIN32) && !defined(MC2_ASAN)
    _HEAPINFO entry{};
    long used = 0;
    while (_heapwalk(&entry) == _HEAPOK)
        used += entry._useflag == _USEDENTRY ? 1 : 0;
    return used;
#else
    return 0;
#endif
}

// A two-spec library the way a mission loads one (a stream through EffectLibrary::Load): 0 = a plain effect, 1 = an
// EffectCloud whose every particle owns an effect made from spec 0.
struct LoadedLibrary {
    std::unique_ptr<gosFX::EffectLibrary> library;
    LoadedLibrary(float childLife, float particleLife)
    {
        // Born large and rewound, as tools/mc2fx saveBlob.
        Stuff::DynamicMemoryStream stream(64u * 1024u);
        stream.Rewind();
        gosFX::WriteGFXVersion(&stream);
        stream << 2u;
        gosFX::Effect::Specification child;
        child.BuildDefaults();
        child.m_lifeSpan.SetCurve(childLife);
        child.Save(&stream);
        gosFX::EffectCloud::Specification cloud;
        cloud.BuildDefaults();
        cloud.m_maxParticleCount = 6;
        cloud.m_startingPopulation.SetCurve(6.0f);
        cloud.m_particlesPerSecond.SetCurve(0.0f);
        cloud.m_lifeSpan.SetCurve(10.0f);
        cloud.m_pLifeSpan.m_ageCurve.SetCurve(particleLife);
        cloud.m_particleEffectID = 0;
        cloud.Save(&stream);
        stream.Rewind();
        gos_PushCurrentHeap(gosFX::Heap);
        library = std::make_unique<gosFX::EffectLibrary>();
        library->Load(&stream);
        gos_PopCurrentHeap();
        gosFX::EffectLibrary::Instance = library.get();
    }
    ~LoadedLibrary()
    {
        gosFX::EffectLibrary::Instance = nullptr;
        library.reset(); // unload: the specs go with the library
    }
};

// Starts the cloud, runs `frames` 30 Hz frames and returns how many of its particles own a live effect afterwards.
int runCloud(gosFX::EffectPtr& cloud, int frames)
{
    const glm::mat4 transform = glm::mat4(1.0f);
    mc2::math::Obb bounds = mc2::math::Obb();
    gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
    cloud->Start(&info);
    for (int frame = 1; frame <= frames; ++frame) {
        info.m_time = frame / 30.0;
        if (!cloud->Execute(&info))
            break;
    }
    auto* effects = static_cast<gosFX::EffectCloud*>(cloud.get());
    int owning = 0;
    for (unsigned i = 0; i < 6; ++i)
        owning += effects->GetParticle(i)->m_effect != nullptr ? 1 : 0;
    return owning;
}
}

TEST_CASE_FIXTURE(Platform, "Library unload after its effects: effects made from a loaded library die before the specs")
{
    for (int cycle = 0; cycle < 4; ++cycle) {
        LoadedLibrary loaded(10.0f, 10.0f);
        REQUIRE(mc2::particles::SpecLibrary::Instance()->Count() == 2);
        gosFX::EffectPtr cloud = gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag);
        gosFX::EffectPtr plain = gosFX::EffectLibrary::Instance->MakeEffect(0, gosFX::Effect::ExecuteFlag);
        REQUIRE(cloud);
        REQUIRE(plain);
        CHECK(runCloud(cloud, 2) == 6); // every particle owns a live effect when the owners go
        cloud.reset();
        plain.reset();
    } // ~LoadedLibrary: the library (and its specs) unload after the last effect
}

TEST_CASE_FIXTURE(Platform, "EffectCloud particle teardown frees each particle's effect once on every path")
{
    struct Path { const char* name; float childLife, particleLife; int frames, owningAtEnd; };
    const Path paths[] = {
        {"cloud destroyed with live particles (~EffectCloud)", 10.0f, 10.0f, 3, 6},
        // The particle's effect runs on the particle's age, so the two age out together (AnimateParticle frees the effect when its
        // Execute ends, DestroyParticle frees whatever is left).
        {"particles and their effects age out (AnimateParticle / DestroyParticle)", 10.0f, 0.05f, 30, 0},
    };
    for (const Path& path : paths) {
        const std::string pathName = path.name;
        CAPTURE(pathName);
        LoadedLibrary loaded(path.childLife, path.particleLife);
        long afterWarmUp = 0;
        for (int cycle = 0; cycle < 5; ++cycle) {
            int owning = 0;
            {
                gosFX::EffectPtr cloud = gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag);
                owning = runCloud(cloud, path.frames);
            }
            CHECK(owning == path.owningAtEnd);
            const long blocks = liveHeapBlocks();
            if (cycle == 1)
                afterWarmUp = blocks;
            else if (cycle > 1)
                CHECK(blocks == afterWarmUp); // nothing a cycle allocates survives it
        }
    }
}

TEST_CASE_FIXTURE(Platform, "Mission reload with live effects: teardown then unload, repeated, leaves nothing behind")
{
    long afterWarmUp = 0;
    for (int mission = 0; mission < 5; ++mission) {
        {
            LoadedLibrary loaded(10.0f, 10.0f);
            std::vector<gosFX::EffectPtr> live; // the mission's effect owners (actors, appearances, weapons)
            for (int i = 0; i < 3; ++i) {
                live.push_back(gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag));
                CHECK(runCloud(live.back(), 2) == 6);
                live.push_back(gosFX::EffectLibrary::Instance->MakeEffect(0, gosFX::Effect::ExecuteFlag));
            }
            live.clear(); // Mission::destroy: the owners go first, still executing
        } // then the texture-manager flush deletes the library
        const long blocks = liveHeapBlocks();
        if (mission == 1)
            afterWarmUp = blocks;
        else if (mission > 1)
            CHECK(blocks == afterWarmUp);
    }
    CHECK(gosFX::EffectLibrary::Instance == nullptr);
}
