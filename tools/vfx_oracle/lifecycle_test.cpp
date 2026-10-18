#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate.h"
#include "contract_test_support.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include <cstdint>

void vfx_test_bind_rng(std::uint32_t* state);

namespace {
constexpr size_t EFFECT_SIZE = 200;
struct Platform {
    ContractEnvironment deterministic{"MC2_DETERMINISTIC_RNG", "1"};
    ContractEnvironment fixedStep{"MC2_SMOKE_FIXED_TIMESTEP", "1"};
    ContractEnvironment identity{"MC2_SIM_IDENTITY_TRACE", "1"};
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

TEST_CASE_FIXTURE(Platform, "CardCloud retains the pre-G2 runtime ABI")
{
    // The pre-G2 ParticleCloud object wrote its array at 0xd8. Enlarging Effect
    // silently linked incompatible constructors and destructors in the failing VS build.
    // B8a: m_children became std::vector<EffectPtr> (was a Stuff::ChainOf); the sizes moved with it.
    CHECK(sizeof(gosFX::Effect) == EFFECT_SIZE);
    CHECK(sizeof(gosFX::Effect::ExecuteInfo) == 40);
    CHECK(sizeof(gosFX::ParticleCloud) == EFFECT_SIZE + 24);
    CHECK(sizeof(gosFX::CardCloud) == EFFECT_SIZE + 56);
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
        const Stuff::LinearMatrix4D transform = Stuff::LinearMatrix4D::Identity;
        Stuff::OBB bounds = Stuff::OBB::Identity;
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
    const Stuff::LinearMatrix4D transform = Stuff::LinearMatrix4D::Identity;

    SUBCASE("a parent destroyed with live children frees each child once")
    {
        g_childDestroyed = 0;
        {
            Stuff::OBB bounds = Stuff::OBB::Identity;
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
            Stuff::OBB bounds = Stuff::OBB::Identity;
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
            Stuff::OBB bounds = Stuff::OBB::Identity;
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
