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
    void adopt(gosFX::Effect* child) { m_children.Add(child); }
    bool empty() { return m_children.IsEmpty(); }
};
}

TEST_CASE_FIXTURE(Platform, "CardCloud retains the pre-G2 runtime ABI")
{
    // The pre-G2 ParticleCloud object wrote its array at 0xd8. Enlarging Effect
    // silently linked incompatible constructors and destructors in the failing VS build.
    CHECK(sizeof(gosFX::Effect) == 208);
    CHECK(sizeof(gosFX::Effect::ExecuteInfo) == 40);
    CHECK(sizeof(gosFX::ParticleCloud) == 232);
    CHECK(sizeof(gosFX::CardCloud) == 264);
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
        parent.adopt(cloud);
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
