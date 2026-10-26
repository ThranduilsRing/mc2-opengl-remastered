#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate_cold.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include "mlr/mlrsortbyorder.hpp"
#include "mlr/gosimagepool.hpp"
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
#include "GameOS/gameos/gos_vfx_mesh_bridge.h"
#include "mlr/mlr_i_pmesh.hpp"
#include "mlr/mlrshape.hpp"

namespace {
std::vector<GosVfxMeshInstance> meshInstances;
std::vector<std::vector<uint16_t>> meshIndices;
}
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload* uploads, unsigned count,
    const GosVfxMeshInstance* instances, unsigned instanceCount)
{
    for (unsigned i = 0; i < count; ++i)
        meshIndices.emplace_back(uploads[i].indices, uploads[i].indices + uploads[i].indexCount);
    meshInstances.insert(meshInstances.end(), instances, instances + instanceCount);
}

void vfx_test_bind_rng(std::uint32_t* state);
extern "C" unsigned long long mc2_vfx_getEmitTotal();
extern "C" unsigned long long mc2_vfx_getRecordsFlushedTotal();

namespace {
using mc2::particles::GpuParticle;
struct BridgeCapture {
    std::vector<GpuParticle> records;
    std::vector<mc2::particles::GroupInfo> groups;
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
    int clippedVertices() const {
        if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>) return 0;
        else return this->m_cloudImplementation->GetNumGOSVertices();
    }
    void distinguishParticles()
    {
        // Distinct channels and slots expose zero-fill, swapped fields and dead-slot packing.
        mc2::math::buildTranslation(this->m_localToParent, glm::vec3(0.5f, -0.25f, 0.75f));
        for (unsigned slot = 0; slot < active(); ++slot) {
            auto* p = this->GetParticle(slot);
            const float n = static_cast<float>(slot + 1);
            p->m_age = n * 0.125f;
            const glm::vec3 position(n * 0.25f, n * -0.375f, n * 0.5f);
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
                    // Distinct live atlas rectangles expose resampling at age/seed zero.
                    const float u = n * 0.125f, v = n * 0.25f;
                    this->m_P_uvs[slot * 4] = {u, v + 0.125f};
                    this->m_P_uvs[slot * 4 + 1] = {u + 0.25f, v + 0.125f};
                    this->m_P_uvs[slot * 4 + 2] = {u + 0.25f, v};
                    this->m_P_uvs[slot * 4 + 3] = {u, v};
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
            if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>)
                appendBytes(bytes, p->m_age < 1.0f);
            else appendBytes(bytes, this->m_cloudImplementation->IsOn(slot));
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
    std::vector<GpuParticle> expectedPayload(const glm::mat4& parent, const glm::vec3& camera = glm::vec3(0))
    {
        std::vector<GpuParticle> result;
        glm::mat4 localToWorld;
        localToWorld = this->m_localToParent * parent;
        for (unsigned slot = 0; slot < active(); ++slot) {
            const auto* p = this->GetParticle(slot);
            if (p->m_age >= 1.0f) continue;
            glm::vec3 world;
            if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>)
                world = mc2::math::transformPoint(this->m_P_localTranslation[slot], localToWorld);
            else
                world = mc2::math::transformPoint(p->m_localTranslation, localToWorld);
            constexpr unsigned colorsPerParticle = std::is_same_v<Cloud, gosFX::ShardCloud> ? 3 : 1;
            const auto& color = this->m_P_color[slot * colorsPerParticle];
            if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                glm::mat4 cardToCloud;
                mc2::math::buildRotation(cardToCloud, p->m_localRotation);
                const auto* spec = this->GetSpecification();
                const auto direction = mc2::math::transformPoint(camera, glm::inverse(localToWorld)) - p->m_localTranslation;
                if (spec->m_alignZUsingX)
                    mc2::math::alignLocalAxisToWorldVector(cardToCloud, direction, Stuff::Z_Axis,
                        spec->m_alignZUsingY ? Stuff::Y_Axis : Stuff::X_Axis,
                        spec->m_alignZUsingY ? Stuff::X_Axis : -1);
                else if (spec->m_alignZUsingY)
                    mc2::math::alignLocalAxisToWorldVector(cardToCloud, direction, Stuff::Z_Axis, Stuff::Y_Axis, -1);
                mc2::math::buildTranslation(cardToCloud, p->m_localTranslation);
                const float x = p->m_scale * p->m_halfX, y = p->m_scale * p->m_halfY;
                const glm::vec3 corners[] = {{x,-y,0}, {-x,-y,0}, {-x,y,0}, {x,y,0}};
                for (unsigned corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
                    GpuParticle vertex{};
                    const auto wp = mc2::math::transformPoint(mc2::math::transformPoint(corners[corner], cardToCloud), localToWorld);
                    vertex.position[0] = wp.x; vertex.position[1] = wp.y; vertex.position[2] = wp.z;
                    vertex.color[0] = color.red; vertex.color[1] = color.green;
                    vertex.color[2] = color.blue; vertex.color[3] = color.alpha;
                    vertex.velocity[0] = this->m_P_uvs[slot*4+corner].x;
                    vertex.velocity[1] = this->m_P_uvs[slot*4+corner].y;
                    MidLevelRenderer::MLRState state;
                    state.Combine(MidLevelRenderer::MLRState{}, spec->m_state);
                    const bool additive = state.GetAlphaMode() == MidLevelRenderer::MLRState::OneOneMode ||
                                          state.GetAlphaMode() == MidLevelRenderer::MLRState::AlphaOneMode;
                    vertex.size = p->m_scale * std::max(p->m_halfX, p->m_halfY);
                    vertex.kind_flags = 8u | 64u | (additive ? 2u : 0u) |
                        (state.GetTextureWrapMode() == MidLevelRenderer::MLRState::TextureClamp ? 16u : 0u) |
                        (state.GetTextureHandle() == 0 ? 32u : 0u);
                    result.push_back(vertex);
                }
                continue;
            }
            for (unsigned corner = 0; corner < 3; ++corner) {
                GpuParticle record{};
                if constexpr (std::is_same_v<Cloud, gosFX::ShardCloud>) {
                    glm::mat4 shard;
                    mc2::math::buildRotation(shard, p->m_localRotation);
                    const auto* spec = this->GetSpecification();
                    const auto direction = mc2::math::transformPoint(camera, glm::inverse(localToWorld)) - p->m_localTranslation;
                    if (spec->m_alignZUsingX)
                        mc2::math::alignLocalAxisToWorldVector(shard, direction, Stuff::Z_Axis,
                            spec->m_alignZUsingY ? Stuff::Y_Axis : Stuff::X_Axis,
                            spec->m_alignZUsingY ? Stuff::X_Axis : -1);
                    else if (spec->m_alignZUsingY)
                        mc2::math::alignLocalAxisToWorldVector(shard, direction, Stuff::Z_Axis, Stuff::Y_Axis, -1);
                    mc2::math::buildTranslation(shard, p->m_localTranslation);
                    const float radius = p->m_scale * p->m_radius;
                    const glm::vec3 corners[] = {{0, -0.5f * radius, 0},
                        {radius * p->m_angle, 0.5f * radius, 0},
                        {-radius * p->m_angle, 0.5f * radius, 0}};
                    world = mc2::math::transformPoint(mc2::math::transformPoint(corners[corner], shard), localToWorld);
                }
                record.position[0] = world.x;
                record.position[1] = world.y;
                record.position[2] = world.z;
                const auto& tint = this->m_P_color[slot * colorsPerParticle + (colorsPerParticle == 3 ? corner : 0)];
                record.color[0] = tint.red;
                record.color[1] = tint.green;
                record.color[2] = tint.blue;
                record.color[3] = this->GetSpecification()->m_state.GetAlphaMode() ==
                    MidLevelRenderer::MLRState::OneZeroMode ? 1.0f : tint.alpha;
                record.kind_flags = 8u | 32u;
                if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>) {
                    record.velocity[0] = corner == 1 ? 2.4f : 0.0f;
                    record.velocity[1] = corner == 2 ? 2.4f : 0.0f;
                    record.kind_flags |= 128u;
                }
                result.push_back(record);
            }
        }
        return result;
    }
    void checkLiveUvs(const BridgeCapture& capture)
    {
        if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
            for (const auto& group : capture.groups) CHECK(group.atlasColumns == 0xffffffffu);
        }
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
    glm::mat4 transform = glm::mat4(1.0f);
    mc2::math::buildTranslation(transform, glm::vec3(0.0f, 0.0f, 10.0f));
    mc2::math::Obb bounds = mc2::math::Obb();
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
    glm::mat4 projection;
    mc2::math::setPerspective(projection, 1.0f, 100.0f, 1.2f, 1.0f);
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
        auto expected = [&] {
            if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>)
                if (gpu) return cloud.expectedPayload(transform);
            return std::vector<GpuParticle>{};
        }();
        constexpr unsigned stride = std::is_same_v<Cloud, gosFX::CardCloud> ? 6 : 3;
        REQUIRE(expected.size() == (gpu ? live * stride : 0));
        std::vector<GpuParticle> firstPayload;
        std::array<std::vector<char>, 2> firstMlrPayload;
        glm::mat4 camera = glm::mat4(1.0f);
        if (outside) mc2::math::buildTranslation(camera, glm::vec3(1000.0f, 0.0f, 0.0f));
        for (int repeat = 0; repeat < 8; ++repeat) {
            CAPTURE(repeat);
            CAPTURE(live);
            CAPTURE(outside);
            CAPTURE(gpu);
            clipper.StartDraw(camera, projection, fog, nullptr, state, nullptr);
            if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>)
                if (gpu) expected = cloud.expectedPayload(transform, mc2::math::getTranslation(camera));
            sorter->payload.clear();
            draw.m_clippingFlags = repeat % 2 ? 0x3f : 0;
            glm::mat4 worldToClip;
            worldToClip = clipper.GetWorldToCameraMatrix() * projection;
            Stuff::Vector4D projected;
            projected.MultiplyPoint(mc2::math::getTranslation(transform), worldToClip);
            MidLevelRenderer::MLRClippingState culled;
            culled.Clip4dVertex(&projected);
            CHECK((culled.GetClippingState() != 0) == outside);
            CHECK(Stuff::Random::GetState() == frozen);
            const auto emitted = mc2_vfx_getEmitTotal();
            cloud.Draw(&draw);
            CHECK(mc2_vfx_getEmitTotal() - emitted == (gpu ? live * stride : 0));
            CHECK(Stuff::Random::GetState() == frozen);
            CHECK(Stuff::Random::GetDrawCount() == frozen.draws);
            CHECK(cloud.frozenState() == particles);
            if (std::is_same_v<Cloud, gosFX::PertCloud> && !gpu && live == 4) {
                // MC2's legacy clouds bypass clip rejection, even with clipping flags set.
                CHECK(cloud.clippedVertices() > 0);
                REQUIRE_FALSE(sorter->payload.empty());
            }
            if constexpr (!std::is_same_v<Cloud, gosFX::PertCloud>)
                CHECK(sorter->payload.empty()); // accepted cards never enter MLR, including with the kill-switch off
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
            borrowedCapture->groups.clear();
            borrowedCapture->calls = 0;
            batcher.Flush();
            CHECK(mc2_vfx_getRecordsFlushedTotal() - flushed == (gpu ? live * stride : 0));
            const auto& actual = borrowedCapture->records;
            CHECK(borrowedCapture->calls == (gpu && live ? 1 : 0));
            CHECK(payloadMatches(actual, expected));
            if (gpu) cloud.checkLiveUvs(*borrowedCapture);
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
    const mc2::particles::GroupInfo* groups, unsigned groupCount)
{
    if (!borrowedCapture) return;
    ++borrowedCapture->calls;
    borrowedCapture->records.insert(borrowedCapture->records.end(), records, records + count);
    borrowedCapture->groups.insert(borrowedCapture->groups.end(), groups, groups + groupCount);
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/mesh submaterials and polygon fans preserve RNG")
{
    mc2::gate::ScopedOverride<"MC2_VFX_ORACLE_SHAPE"> native{"1"};
    using namespace MidLevelRenderer;
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    auto* shape = new MLRShape(2);
    for (unsigned i = 0; i < 2; ++i) {
        auto* prim = new MLR_I_PMesh;
        const glm::vec3 positions[] = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}};
        unsigned short indices[] = {0,1,2,3};
        unsigned char lengths[] = {4};
        prim->SetCoordData(positions, 4);
        prim->SetSubprimitiveLengths(lengths, 1);
        prim->SetIndexData(indices, 4);
        MLRState state;
        state.SetTextureHandle(11 + i);
        state.SetAlphaMode(i == 0 ? MLRState::OneOneMode : MLRState::AlphaInvAlphaMode);
        prim->SetReferenceState(state);
        shape->Add(prim);
        prim->DetachReference();
    }
    gos_PopCurrentHeap();
    DrawScalableShapeInformation draw;
    draw.shape = shape;
    glm::mat4 transform(1.0f);
    mc2::math::buildTranslation(transform, glm::vec3(17,23,31));
    draw.shapeToWorld = &transform;
    glm::vec3 scale(2.0f);
    draw.scaling = &scale;
    Stuff::RGBAColor color(0.2f,0.4f,0.6f,0.8f);
    draw.paintMe = &color;
    const auto frozen = Stuff::Random::GetState();
    meshIndices.clear(); meshInstances.clear();
    REQUIRE(gosFX::DrawNativeShape(draw));
    REQUIRE(meshIndices.size() == 2);
    REQUIRE(meshInstances.size() == 2);
    for (const auto& indices : meshIndices)
        CHECK(indices == std::vector<uint16_t>{0,1,2,0,2,3});
    CHECK(meshInstances[0].mlrTexHandle == 11);
    CHECK(meshInstances[1].mlrTexHandle == 12);
    CHECK(meshInstances[0].blendMode == 2);
    CHECK(meshInstances[1].blendMode == 0);
    CHECK(meshInstances[0].modelToWorld[12] == 17);
    CHECK(meshInstances[0].modelToWorld[13] == 23);
    CHECK(meshInstances[0].modelToWorld[14] == 31);
    CHECK(meshInstances[0].scale == 2.0f);
    CHECK(meshInstances[1].rgba[3] == 0.8f);
    meshIndices.clear(); meshInstances.clear();
    REQUIRE(gosFX::DrawNativeShape(draw));
    CHECK(meshIndices.empty());
    CHECK(meshInstances.size() == 2);
    CHECK(Stuff::Random::GetState() == frozen);
    shape->DetachReference();
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/CardCloud draw flush and clip leave RNG unchanged")
{
    checkFrozenCloud<gosFX::CardCloud>();
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
