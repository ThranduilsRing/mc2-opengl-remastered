#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate_cold.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include "mlr/mlrsortbyorder.hpp"
#include "mlr/gosimagepool.hpp"
#include "mlr/mlrpointcloud.hpp"
#include "mlr/mlrtrianglecloud.hpp"
#include "mlr/mlrcardcloud.hpp"
#include "mlr/mlrngoncloud.hpp"
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>
#include <array>
#include <cstdlib>
#include <fstream>

void vfx_test_bind_rng(std::uint32_t* state);
extern "C" unsigned long long mc2_vfx_getEmitTotal();
extern "C" unsigned long long mc2_vfx_getRecordsFlushedTotal();

namespace {
using mc2::particles::GpuParticle;
struct BridgeCapture {
    std::vector<GpuParticle> records;
    unsigned calls = 0;
    std::ofstream snapshot;
};
BridgeCapture* borrowedCapture = nullptr;

template<class T>
void appendBytes(std::vector<char>& bytes, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* begin = reinterpret_cast<const char*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(value));
}

// Capture the submitted geometry and state, because a vertex count cannot detect drift.
class PayloadSorter final : public MidLevelRenderer::MLRSortByOrder {
public:
    using MLRSortByOrder::MLRSortByOrder;
    std::vector<char> payload;
    void AddEffect(MidLevelRenderer::MLREffect* effect,
                   const MidLevelRenderer::MLRState& state) override
    {
        appendBytes(payload, state.GetRenderStateFlags());
        appendBytes(payload, state.GetProcessStateFlags());
        appendBytes(payload, state.GetRenderDeltaMask());
        appendBytes(payload, state.GetProcessDeltaMask());
        appendBytes(payload, state.GetRenderPermissionMask());
        appendBytes(payload, state.GetProcessPermissionMask());
        appendBytes(payload, effect->GetSortDataMode());
        appendBytes(payload, effect->GetNumGOSVertices());
        for (int i = 0; i < effect->GetNumGOSVertices(); ++i) {
            const auto& v = effect->GetGOSVertices()[i];
            for (float field : {v.x, v.y, v.z, v.rhw}) appendBytes(payload, field);
            appendBytes(payload, v.argb);
            appendBytes(payload, v.frgb);
            appendBytes(payload, v.u);
            appendBytes(payload, v.v);
        }
        MLRSortByOrder::AddEffect(effect, state);
    }
};

bool payloadMatches(std::span<const GpuParticle> actual, std::span<const GpuParticle> expected)
{
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        if (!std::equal(std::begin(a.position), std::end(a.position), std::begin(e.position)) ||
            !std::equal(std::begin(a.color), std::end(a.color), std::begin(e.color)) ||
            !std::equal(std::begin(a.velocity), std::end(a.velocity), std::begin(e.velocity)) ||
            a.size != e.size || a.age != e.age || a.lifetime != e.lifetime ||
            a.atlasIndex != e.atlasIndex || a.kind_flags != e.kind_flags || a._pad0 != e._pad0)
            return false;
    }
    return true;
}

struct FrozenPlatform {
    // Fresh children set this before MLR first reads its process-lifetime cache.
    mc2::gate::ScopedOverride<"MC2_DISABLE_GOSFX"> mlrEnabled{"0"};
    std::uint32_t seed = mc2_det_rng::hashMissionName("mc2_03");
    BridgeCapture capture;
    FrozenPlatform()
    {
        borrowedCapture = &capture;
        if (const auto* path = std::getenv("VFX_FROZEN_SNAPSHOT_FILE")) {
            capture.snapshot.open(path, std::ios::binary | std::ios::app);
            REQUIRE(capture.snapshot.is_open());
        }
        mc2::gate::markMainEntered();
        vfx_test_bind_rng(&seed);
        Stuff::InitializeClasses();
        MidLevelRenderer::InitializeClasses(64 * 1024, 16 * 1024, 1024, 4096, true);
        gosFX::InitializeClasses();
    }
    ~FrozenPlatform()
    {
        gosFX::TerminateClasses();
        MidLevelRenderer::TerminateClasses();
        Stuff::TerminateClasses();
        vfx_test_bind_rng(nullptr);
        borrowedCapture = nullptr;
    }
};

template<class Cloud>
class FrozenCloud final : public Cloud {
public:
    explicit FrozenCloud(typename Cloud::Specification* spec)
        : Cloud(spec, gosFX::Effect::ExecuteFlag) {}
    bool execute(gosFX::Effect::ExecuteInfo* info) { return this->Execute(info); }
    unsigned active() const { return this->m_activeParticleCount; }
    void expire(unsigned slot) { this->DestroyParticle(slot); }
    int clippedVertices() const { return this->m_cloudImplementation->GetNumGOSVertices(); }
    void distinguishParticles()
    {
        // Distinct channels and slots expose zero-fill, swapped fields and dead-slot packing.
        this->m_localToParent.BuildTranslation(Stuff::Point3D(0.5f, -0.25f, 0.75f));
        for (unsigned slot = 0; slot < active(); ++slot) {
            auto* p = this->GetParticle(slot);
            const float n = static_cast<float>(slot + 1);
            p->m_age = n * 0.125f;
            const Stuff::Point3D position(n * 0.25f, n * -0.375f, n * 0.5f);
            if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>) {
                this->m_P_localTranslation[slot] = position;
            } else {
                p->m_localTranslation = position;
                p->m_scale = 0.5f + n * 0.25f;
                p->m_radius = 1.0f + n * 0.5f;
                p->m_localRotation.x = p->m_localRotation.y = 0.0f;
                p->m_localRotation.z = std::sin(n * 0.125f);
                p->m_localRotation.w = std::cos(n * 0.125f);
                if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                    p->m_halfX = 0.5f + n * 0.25f;
                    p->m_halfY = 1.0f + n * 0.125f;
                    p->m_radius = Stuff::Sqrt(p->m_halfX * p->m_halfX + p->m_halfY * p->m_halfY);
                }
            }
            const unsigned colorsPerParticle = colorStride();
            for (unsigned vertex = 0; vertex < colorsPerParticle; ++vertex) {
                const float offset = static_cast<float>(vertex) * 0.03125f;
                this->m_P_color[slot * colorsPerParticle + vertex] =
                    Stuff::RGBAColor(n * 0.125f + offset, n * 0.0625f + offset,
                                     n * 0.03125f + offset, 1.0f - n * 0.125f - offset);
            }
        }
    }
    unsigned colorStride()
    {
        if constexpr (std::is_same_v<Cloud, gosFX::PertCloud>) return 2;
        return std::is_same_v<Cloud, gosFX::ShardCloud> ? 3 : 1;
    }
    std::vector<char> observedState()
    {
        std::vector<char> bytes;
        appendBytes(bytes, active());
        appendBytes(bytes, this->m_age);
        appendBytes(bytes, this->m_seed);
        appendBytes(bytes, this->m_lastRan);
        for (unsigned slot = 0; slot < active(); ++slot) {
            const auto* p = this->GetParticle(slot);
            appendBytes(bytes, p->m_age);
            appendBytes(bytes, p->m_seed);
            appendBytes(bytes, p->m_ageRate);
            appendBytes(bytes, this->m_cloudImplementation->IsOn(slot));
            for (unsigned c = 0; c < colorStride(); ++c) {
                const auto& color = this->m_P_color[slot * colorStride() + c];
                for (float v : {color.red, color.green, color.blue, color.alpha}) appendBytes(bytes, v);
            }
        }
        const auto rng = Stuff::Random::GetState();
        appendBytes(bytes, rng.numbers);
        appendBytes(bytes, rng.index);
        appendBytes(bytes, rng.draws);
        return bytes;
    }
    std::vector<GpuParticle> expectedPayload(const Stuff::LinearMatrix4D& parent)
    {
        std::vector<GpuParticle> result;
        Stuff::LinearMatrix4D localToWorld;
        localToWorld.Multiply(this->m_localToParent, parent);
        for (unsigned slot = 0; slot < active(); ++slot) {
            const auto* p = this->GetParticle(slot);
            if (p->m_age >= 1.0f) continue;
            Stuff::Point3D world;
            if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>)
                world.Multiply(this->m_P_localTranslation[slot], localToWorld);
            else
                world.Multiply(p->m_localTranslation, localToWorld);
            constexpr unsigned colorsPerParticle = std::is_same_v<Cloud, gosFX::ShardCloud> ? 3 : 1;
            const auto& color = this->m_P_color[slot * colorsPerParticle];
            GpuParticle record{};
            record.position[0] = world.x;
            record.position[1] = world.y;
            record.position[2] = world.z;
            record.color[0] = color.red;
            record.color[1] = color.green;
            record.color[2] = color.blue;
            record.color[3] = color.alpha;
            record.age = static_cast<float>(p->m_age);
            record.lifetime = 1.0f;
            if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>) {
                record.size = 4.0f;
            } else {
                if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                    record.velocity[0] = p->m_scale * p->m_halfX;
                    record.velocity[1] = p->m_scale * p->m_halfY;
                    record.size = p->m_scale * Stuff::Sqrt(p->m_halfX * p->m_halfX + p->m_halfY * p->m_halfY);
                    Stuff::UnitQuaternion parentRotation, rotation;
                    parentRotation = parent;
                    rotation.Multiply(p->m_localRotation, parentRotation);
                    record.velocity[2] = 2.0f * std::atan2(rotation.z, rotation.w);
                } else {
                    record.size = p->m_scale * p->m_radius;
                    record.velocity[0] = record.velocity[1] = record.size;
                    record.velocity[2] = 2.0f * std::atan2(p->m_localRotation.z, p->m_localRotation.w);
                }
            }
            result.push_back(record);
        }
        return result;
    }
    std::vector<char> frozenState()
    {
        std::vector<char> result;
        auto append = [&result](const auto* data, unsigned count) {
            const auto* bytes = reinterpret_cast<const char*>(data);
            result.insert(result.end(), bytes, bytes + sizeof(*data) * count);
        };
        for (unsigned slot = 0; slot < active(); ++slot)
            append(this->GetParticle(slot), 1);
        const unsigned colorsPerParticle = colorStride();
        append(this->m_P_color, active() * colorsPerParticle);
        if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>)
            append(this->m_P_localTranslation, active());
        append(&this->m_age, 1);
        append(&this->m_seed, 1);
        append(&this->m_lastRan, 1);
        return result;
    }
};

template<class Cloud>
void checkFrozenCloud(bool alignX = false, bool alignY = false, unsigned sides = 3)
{
    auto spec = [sides] {
        if constexpr (std::is_same_v<Cloud, gosFX::PertCloud>) return typename Cloud::Specification(sides);
        else return typename Cloud::Specification();
    }();
    spec.BuildDefaults();
    spec.m_maxParticleCount = 4;
    spec.m_startingPopulation.SetCurve(4.0f);
    spec.m_particlesPerSecond.SetCurve(0.0f);
    spec.m_lifeSpan.SetCurve(2.0f);
    spec.m_pLifeSpan.m_ageCurve.SetCurve(0.25f);
    spec.m_emitterSizeX.SetCurve(0.0f);
    spec.m_emitterSizeY.SetCurve(0.0f);
    spec.m_emitterSizeZ.SetCurve(0.0f);
    spec.m_startingSpeed.m_ageCurve.SetCurve(0.0f);
    spec.m_state.SetTextureHandle(0);
    spec.m_state.SetAlphaMode(MidLevelRenderer::MLRState::OneZeroMode);
    if constexpr (!std::is_same_v<Cloud, gosFX::PointCloud>) {
        spec.m_alignZUsingX = alignX;
        spec.m_alignZUsingY = alignY;
    }
    Stuff::LinearMatrix4D transform = Stuff::LinearMatrix4D::Identity;
    transform.BuildTranslation(Stuff::Point3D(0.0f, 0.0f, 10.0f));
    Stuff::OBB bounds = Stuff::OBB::Identity;
    gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds, 0.5f);
    FrozenCloud<Cloud> cloud(&spec);
    cloud.Start(&info);
    const auto beforeBirth = Stuff::Random::GetDrawCount();
    info.m_time = 1.0 / 30.0;
    REQUIRE(cloud.execute(&info));
    REQUIRE(cloud.active() == 4);
    REQUIRE(Stuff::Random::GetDrawCount() > beforeBirth);
    cloud.distinguishParticles();

    // MLRTexturePool owns and deletes its image pool.
    MidLevelRenderer::MLRTexturePool textures(new MidLevelRenderer::TGAFilePool(""));
    auto* sorter = new PayloadSorter(&textures);
    MidLevelRenderer::MLRClipper clipper(nullptr, sorter);
    Stuff::Matrix4D projection;
    projection.SetPerspective(1.0f, 100.0f, Stuff::Radian(1.2f), 1.0f);
    const Stuff::RGBAColor fog(0.0f, 0.0f, 0.0f, 1.0f);
    MidLevelRenderer::MLRState state;
    gosFX::Effect::DrawInfo draw{};
    draw.m_parentToWorld = &transform;
    draw.m_clipper = &clipper;
    const bool gpu = mc2::particles::Batcher::is_enabled() && !std::is_same_v<Cloud, gosFX::PertCloud>;
    auto& batcher = mc2::particles::Batcher::Instance();
    batcher.Flush();

    auto repeatDraw = [&](unsigned live, bool outside) {
        const auto frozen = Stuff::Random::GetState();
        const auto particles = cloud.frozenState();
        const auto expected = [&] {
            if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>)
                if (gpu) return cloud.expectedPayload(transform);
            return std::vector<GpuParticle>{};
        }();
        REQUIRE(expected.size() == (gpu ? live : 0));
        std::vector<GpuParticle> firstPayload;
        std::array<std::vector<char>, 2> firstMlrPayload;
        Stuff::LinearMatrix4D camera = Stuff::LinearMatrix4D::Identity;
        if (outside) camera.BuildTranslation(Stuff::Point3D(1000.0f, 0.0f, 0.0f));
        for (int repeat = 0; repeat < 8; ++repeat) {
            CAPTURE(repeat);
            CAPTURE(live);
            CAPTURE(outside);
            CAPTURE(gpu);
            clipper.StartDraw(camera, projection, fog, nullptr, state, nullptr);
            sorter->payload.clear();
            draw.m_clippingFlags = repeat % 2 ? 0x3f : 0;
            Stuff::Matrix4D worldToClip;
            worldToClip.Multiply(clipper.GetWorldToCameraMatrix(), projection);
            Stuff::Vector4D projected;
            projected.Multiply(Stuff::Point3D(transform), worldToClip);
            MidLevelRenderer::MLRClippingState culled;
            culled.Clip4dVertex(&projected);
            CHECK((culled.GetClippingState() != 0) == outside);
            CHECK(Stuff::Random::GetState() == frozen);
            const auto emitted = mc2_vfx_getEmitTotal();
            cloud.Draw(&draw);
            CHECK(mc2_vfx_getEmitTotal() - emitted == (gpu ? live : 0));
            CHECK(Stuff::Random::GetState() == frozen);
            CHECK(Stuff::Random::GetDrawCount() == frozen.draws);
            CHECK(cloud.frozenState() == particles);
            if (!gpu && live == 4) {
                // MC2's legacy clouds bypass clip rejection, even with clipping flags set.
                CHECK(cloud.clippedVertices() > 0);
                REQUIRE_FALSE(sorter->payload.empty());
            }
            if (repeat < 2) firstMlrPayload[repeat] = sorter->payload;
            CHECK(sorter->payload == firstMlrPayload[repeat % 2]);
            if (borrowedCapture->snapshot.is_open()) {
                auto bytes = cloud.observedState();
                bytes.insert(bytes.end(), sorter->payload.begin(), sorter->payload.end());
                const auto size = static_cast<std::uint64_t>(bytes.size());
                borrowedCapture->snapshot.write(reinterpret_cast<const char*>(&size), sizeof(size));
                borrowedCapture->snapshot.write(bytes.data(), bytes.size());
                REQUIRE(borrowedCapture->snapshot.good());
            }
            clipper.RenderNow();
            CHECK(Stuff::Random::GetState() == frozen);
            batcher.ResolveTextures();
            const auto flushed = mc2_vfx_getRecordsFlushedTotal();
            borrowedCapture->records.clear();
            borrowedCapture->calls = 0;
            batcher.Flush();
            CHECK(mc2_vfx_getRecordsFlushedTotal() - flushed == (gpu ? live : 0));
            const auto& actual = borrowedCapture->records;
            CHECK(borrowedCapture->calls == (gpu && live ? 1 : 0));
            CHECK(payloadMatches(actual, expected));
            if (repeat == 0) firstPayload = actual;
            REQUIRE(actual.size() == firstPayload.size());
            if (!actual.empty()) {
                CHECK(std::memcmp(actual.data(), firstPayload.data(), actual.size() * sizeof(GpuParticle)) == 0);
                // Every checked component must reject a same-count corrupted bridge payload.
                auto corrupted = actual;
                corrupted.front() = {};
                CHECK_FALSE(payloadMatches(corrupted, expected));
                for (unsigned component = 0; component < 12; ++component) {
                    CAPTURE(component);
                    corrupted = actual;
                    auto& record = corrupted.front();
                    if (component < 3) record.position[component] += 1.0f;
                    else if (component < 7) record.color[component - 3] += 1.0f;
                    else if (component < 10) record.velocity[component - 7] += 1.0f;
                    else if (component == 10) record.size += 1.0f;
                    else record.age += 1.0f;
                    CHECK_FALSE(payloadMatches(corrupted, expected));
                }
                if (actual.size() > 1) {
                    corrupted = actual;
                    std::swap(corrupted[0], corrupted[1]);
                    CHECK_FALSE(payloadMatches(corrupted, expected));
                }
            }
            CHECK(Stuff::Random::GetState() == frozen);
            CHECK(Stuff::Random::GetDrawCount() == frozen.draws);
            CHECK(cloud.frozenState() == particles);
        }
    };
    repeatDraw(4, false);
    repeatDraw(4, true);
    cloud.expire(1);
    repeatDraw(3, false);
    for (unsigned slot : {0u, 2u, 3u}) cloud.expire(slot);
    repeatDraw(0, false);
    REQUIRE(cloud.active() == 4);
    REQUIRE(cloud.execute(&info));
    REQUIRE(cloud.active() == 0);
    // The authored population remains positive: placeholder fallback would fabricate four particles.
    repeatDraw(0, false);
}
}

extern "C" void gos_particle_bridge_flush(const GpuParticle* records, unsigned count,
    const mc2::particles::GroupInfo*, unsigned)
{
    if (!borrowedCapture) return;
    ++borrowedCapture->calls;
    borrowedCapture->records.insert(borrowedCapture->records.end(), records, records + count);
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/CardCloud draw flush and clip leave RNG unchanged")
{
    checkFrozenCloud<gosFX::CardCloud>();
    if (!mc2::particles::Batcher::is_enabled())
        for (const auto alignment : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}})
            checkFrozenCloud<gosFX::CardCloud>(alignment.first, alignment.second);
}
TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/PointCloud draw flush and clip leave RNG unchanged")
{
    checkFrozenCloud<gosFX::PointCloud>();
}
TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/ShardCloud draw flush and clip leave RNG unchanged")
{
    checkFrozenCloud<gosFX::ShardCloud>();
    if (!mc2::particles::Batcher::is_enabled())
        for (const auto alignment : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}})
            checkFrozenCloud<gosFX::ShardCloud>(alignment.first, alignment.second);
}
TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/PertCloud variable polygons preserve MLR payload and RNG")
{
    for (unsigned sides : {3u, 7u}) {
        CAPTURE(sides);
        for (const auto alignment : {std::pair{false, false}, std::pair{true, false},
                                    std::pair{false, true}, std::pair{true, true}})
            checkFrozenCloud<gosFX::PertCloud>(alignment.first, alignment.second, sides);
    }
}
