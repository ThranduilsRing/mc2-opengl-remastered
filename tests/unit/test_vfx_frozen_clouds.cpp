#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate_cold.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include "mlr/mlrsortbyorder.hpp"
#include "mlr/gosimagepool.hpp"
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
#include "mlr/mlr_i_mt_pmesh.hpp"
#include "mlr/mlrshape.hpp"
#include "mlr/mlrtexture.hpp"
#include "mlr/mlrtexturepool.hpp"
#include "RenderCore/FramePackets.h"
#include "code/render_view.h"

namespace {
std::vector<GosVfxMeshInstance> meshInstances;
std::vector<std::vector<uint16_t>> meshIndices;
std::vector<std::vector<float>> meshPositions, meshUvs;
std::vector<mc2::particles::GpuParticle>* playbackParticles = nullptr;
}
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload* uploads, unsigned count,
    const GosVfxMeshInstance* instances, unsigned instanceCount)
{
    for (unsigned i = 0; i < count; ++i) {
        meshIndices.emplace_back(uploads[i].indices, uploads[i].indices + uploads[i].indexCount);
        meshPositions.emplace_back(uploads[i].positions, uploads[i].positions + uploads[i].vertexCount * 3);
        meshUvs.emplace_back(uploads[i].uvs, uploads[i].uvs + uploads[i].vertexCount * 2);
    }
    meshInstances.insert(meshInstances.end(), instances, instances + instanceCount);
}

// Playback oracle shares the actual legacy draw sink, including nested meshes.
void vfx_test_clear_mesh_capture()
{
    meshInstances.clear(); meshIndices.clear(); meshPositions.clear(); meshUvs.clear();
}
const std::vector<GosVfxMeshInstance>& vfx_test_mesh_instances() { return meshInstances; }
void vfx_test_capture_particles(std::vector<mc2::particles::GpuParticle>* capture) { playbackParticles = capture; }

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
            appendBytes(bytes, p->m_age < 1.0f);
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
            if constexpr (std::is_same_v<Cloud, gosFX::PertCloud>) {
                glm::mat4 model;
                mc2::math::buildRotation(model, p->m_localRotation);
                const auto* spec = this->GetSpecification();
                const auto direction = mc2::math::transformPoint(camera, glm::inverse(localToWorld)) - p->m_localTranslation;
                if (spec->m_alignZUsingX)
                    mc2::math::alignLocalAxisToWorldVector(model, direction, Stuff::Z_Axis,
                        spec->m_alignZUsingY ? Stuff::Y_Axis : Stuff::X_Axis,
                        spec->m_alignZUsingY ? Stuff::X_Axis : -1);
                else if (spec->m_alignZUsingY)
                    mc2::math::alignLocalAxisToWorldVector(model, direction, Stuff::Z_Axis, Stuff::Y_Axis, -1);
                mc2::math::buildTranslation(model, p->m_localTranslation);
                for (unsigned v = 1; v + 1 < spec->m_vertices; ++v) {
                    for (unsigned corner : {0u, v+1, v}) {
                        const auto world = mc2::math::transformPoint(
                            mc2::math::transformPoint(p->m_vertices[corner] * p->m_scale, model), localToWorld);
                        const auto& tint = this->m_P_color[slot*2 + (corner ? 1 : 0)];
                        GpuParticle vertex{};
                        vertex.position[0] = world.x; vertex.position[1] = world.y; vertex.position[2] = world.z;
                        vertex.color[0] = tint.red; vertex.color[1] = tint.green; vertex.color[2] = tint.blue;
                        vertex.color[3] = 1.0f; // opaque fixture
                        vertex.kind_flags = 8u | 32u;
                        result.push_back(vertex);
                    }
                }
                continue;
            }
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
                    const bool additive = state.GetAlphaMode() == MidLevelRenderer::MLRState::OneOneMode;
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
void checkFrozenCloud(bool alignX = false, bool alignY = false, unsigned sides = 3,
    MidLevelRenderer::MLRState::AlphaMode alphaMode = MidLevelRenderer::MLRState::OneZeroMode)
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
    spec.m_state.SetAlphaMode(alphaMode);
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
    const bool gpu = std::is_same_v<Cloud, gosFX::PertCloud>
        ? mc2::particles::Batcher::is_enabled() && mc2::particles::Batcher::is_oracle_shape_enabled()
        : mc2::particles::Batcher::is_enabled();
    auto& batcher = mc2::particles::Batcher::Instance();
    batcher.Flush();

    auto repeatDraw = [&](unsigned live, bool outside) {
        const auto frozen = Stuff::Random::GetState();
        const auto particles = cloud.frozenState();
        auto expected = [&] {
            if (gpu) return cloud.expectedPayload(transform);
            return std::vector<GpuParticle>{};
        }();
        const unsigned stride = std::is_same_v<Cloud, gosFX::PertCloud> ? sides * 3 :
            std::is_same_v<Cloud, gosFX::CardCloud> ? 6 : 3;
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
            CHECK(sorter->payload.empty()); // retired renderers stay absent with either gate vector
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
            if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                if (!actual.empty()) {
                    REQUIRE(borrowedCapture->groups.size() == 1);
                    CHECK(borrowedCapture->groups[0].blendMode ==
                        (alphaMode == MidLevelRenderer::MLRState::OneOneMode ? 2 :
                         alphaMode == MidLevelRenderer::MLRState::AlphaOneMode ? 1 : 0));
                }
            }
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
    if (playbackParticles) playbackParticles->insert(playbackParticles->end(), records, records + count);
    if (!borrowedCapture) return;
    ++borrowedCapture->calls;
    borrowedCapture->records.insert(borrowedCapture->records.end(), records, records + count);
    borrowedCapture->groups.insert(borrowedCapture->groups.end(), groups, groups + groupCount);
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_mesh/submaterials and polygon fans preserve RNG")
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
    alignas(16) unsigned char memory[3][8192]{};
    RenderCore::FrameSlotArenas slots;
    slots.init(memory[0], memory[1], memory[2], sizeof(memory[0]));
    REQUIRE(slots.retain(1));
    gosFX::FrozenNativeShape cold;
    REQUIRE(gosFX::FreezeNativeShape(draw, slots.begin(1), cold));
    CHECK(meshInstances.empty()); // capture performs no bridge submission
    gosFX::DrawFrozenNativeShape(cold);
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
    // The live caller also freezes into the scoped retained sequence.
    mc2::render_view::FrameDraws frame;
    const auto before = slots.begin(1).bytesUsed();
    {
        mc2::render_view::DrawFrameScope scope(frame, &slots.begin(1));
        REQUIRE(gosFX::DrawNativeShape(draw));
    }
    CHECK(slots.begin(1).bytesUsed() > before);
    CHECK(meshIndices.empty());
    CHECK(meshInstances.size() == 2);
    gosFX::FrozenNativeShape hot;
    REQUIRE(gosFX::FreezeNativeShape(draw, slots.begin(1), hot));
    shape->DetachReference();
    draw.shape = nullptr;
    transform = glm::mat4(0.0f); scale = glm::vec3(99.0f); color.alpha = 0.0f;
    REQUIRE(slots.retain(2));
    auto later = slots.begin(2).allocArray<unsigned char>(8192, "later frame");
    std::fill_n(later.data, later.count, 0xcd);
    CHECK_FALSE(slots.retain(4));
    meshIndices.clear(); meshInstances.clear(); meshPositions.clear(); meshUvs.clear();
    gosFX::DrawFrozenNativeShape(cold);
    REQUIRE(meshIndices.size() == 2);
    CHECK(meshIndices[0] == std::vector<uint16_t>{0,1,2,0,2,3});
    CHECK(meshPositions[0] == std::vector<float>{0,0,0,1,0,0,1,1,0,0,1,0});
    CHECK(meshUvs[0] == std::vector<float>(8, 0.0f));
    CHECK(meshInstances[0].modelToWorld[12] == 17);
    CHECK(meshInstances[0].scale == 2.0f);
    CHECK(meshInstances[1].rgba[3] == 0.8f);
    meshIndices.clear(); meshInstances.clear();
    gosFX::DrawFrozenNativeShape(hot);
    CHECK(meshIndices.empty());
    REQUIRE(meshInstances.size() == 2);
    CHECK(meshInstances[0].mlrTexHandle == 11);
    CHECK(meshInstances[1].blendMode == 0);
    CHECK(Stuff::Random::GetState() == frozen);
    CHECK(slots.retire(2));
    CHECK(slots.retire(1));
}

namespace {
struct ProbeImagePool : MidLevelRenderer::TGAFilePool {
    ProbeImagePool() : TGAFilePool("") {}
    std::size_t entries() const { return imageHash.size(); }
};
}

TEST_CASE_FIXTURE(FrozenPlatform, "gos_image_pool/remove, unload and reload keep the name map consistent")
{
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    {
        ProbeImagePool pool;
        auto* a = pool.GetImage("tex_a");
        CHECK(pool.GetImage("tex_a") == a);
        auto* b = pool.GetImage("tex_b");
        CHECK(b != a);
        CHECK(pool.entries() == 2);
        // Removing an image erases only its own entry; a later lookup makes a fresh one.
        pool.RemoveImage(a);
        CHECK(pool.entries() == 1);
        CHECK(pool.GetImage("tex_b") == b);
        CHECK(pool.GetImage("tex_a") != nullptr);
        CHECK(pool.entries() == 2);
        // An unpooled image with a pooled name leaves the pooled entry alone.
        pool.RemoveImage(new MidLevelRenderer::GOSImage("tex_b"));
        CHECK(pool.GetImage("tex_b") == b);
        CHECK(pool.entries() == 2);
        pool.UnLoadImages();
        CHECK(pool.entries() == 0);
        CHECK(pool.GetImage("tex_b") != nullptr);
        CHECK(pool.entries() == 1);
    } // the destructor deletes what is left
    gos_PopCurrentHeap();
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/CardCloud draw flush and clip leave RNG unchanged")
{
    checkFrozenCloud<gosFX::CardCloud>();
    for (auto blend : {MidLevelRenderer::MLRState::OneOneMode, MidLevelRenderer::MLRState::AlphaOneMode})
        checkFrozenCloud<gosFX::CardCloud>(false, false, 3, blend);
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
TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/PertCloud variable polygons preserve live payload and RNG")
{
    for (unsigned sides : {3u, 7u}) {
        CAPTURE(sides);
        for (const auto alignment : {std::pair{false, false}, std::pair{true, false},
                                    std::pair{false, true}, std::pair{true, true}})
            checkFrozenCloud<gosFX::PertCloud>(alignment.first, alignment.second, sides);
    }
}


TEST_CASE("vfx_frozen/card budget counts quads and resets after flush")
{
    if (!mc2::particles::Batcher::is_enabled()) {
        mc2::particles::Batcher batcher(2);
        BridgeCapture capture;
        borrowedCapture = &capture;
        batcher.Emit(GpuParticle{});
        batcher.Flush();
        borrowedCapture = nullptr;
        CHECK(capture.records.empty());
        return;
    }
    mc2::particles::Batcher batcher(2);
    BridgeCapture capture;
    borrowedCapture = &capture;
    struct Reset { ~Reset() { borrowedCapture = nullptr; } } reset;
    GpuParticle card{};
    card.kind_flags = 64u;
    for (int frame = 0; frame < 2; ++frame) {
        batcher.BeginGroup(0, 0, 0, 1, 1, 0, 0xffffffffu);
        for (int vertex = 0; vertex < 12; ++vertex) batcher.Emit(card);
        CHECK_FALSE(batcher.inspect_overflowReported());
        batcher.Emit(card);
        CHECK(batcher.inspect_overflowReported());
        batcher.Flush();
        CHECK(capture.records.size() == 12u * (frame + 1));
    }
    batcher.BeginGroup(0, 0, 0, 1, 1);
    batcher.Emit(GpuParticle{});
    for (int vertex = 0; vertex < 6; ++vertex) batcher.Emit(card);
    CHECK_FALSE(batcher.inspect_overflowReported());
    batcher.Emit(GpuParticle{});
    CHECK(batcher.inspect_overflowReported());
    batcher.Flush();
    CHECK(capture.records.size() == 31);
}

// B6 holder round-trips: the converted std::vector holders must Save and Load back to the same bytes.
#include "tools/mc2fx/mc2fx_core.h"
namespace {
// Textured states serialize the texture name through MLRTexturePool::Instance, which
// MLRTexturePool::TerminateClass deletes; borrow it for the test and hand it back empty.
struct BorrowTexturePool {
    MidLevelRenderer::MLRTexturePool pool{new MidLevelRenderer::TGAFilePool("")};
    BorrowTexturePool() { MidLevelRenderer::MLRTexturePool::Instance = &pool; }
    ~BorrowTexturePool() { MidLevelRenderer::MLRTexturePool::Instance = nullptr; }
};

const MidLevelRenderer::Vector2DScalar kPassUvs[3][4] = {
    {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}},
    {{0.5f, 0.25f}, {0.75f, 0.25f}, {0.75f, 0.5f}, {0.5f, 0.5f}},
    {{-2.0f, 3.0f}, {4.0f, -5.0f}, {6.5f, 7.25f}, {-8.0f, 9.0f}},
};
const char* const kPassTextures[3] = {"b6_base", "b6_detail", "b6_glow"};

// Quad subprimitive: MLRShape's triangle-mesh conversion leaves it as the saved class.
MidLevelRenderer::MLR_I_MT_PMesh* makeTexturedMultipassQuad(MidLevelRenderer::MLRTexturePool& pool, float offset)
{
    using namespace MidLevelRenderer;
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    auto* mesh = new MLR_I_MT_PMesh;
    const glm::vec3 positions[] = {{offset,0,0}, {offset+1,0,0}, {offset+1,1,0}, {offset,1,0}};
    unsigned short indices[] = {0,1,2,3};
    unsigned char lengths[] = {4};
    mesh->SetCoordData(positions, 4);
    mesh->SetSubprimitiveLengths(lengths, 1);
    mesh->SetIndexData(indices, 4);
    for (int pass = 0; pass < 3; ++pass) {
        mesh->SetTexCoordData(kPassUvs[pass], 4, pass);
        MLRState state;
        state.SetTextureHandle(pool.Add(kPassTextures[pass], 0)->GetTextureHandle());
        state.SetAlphaMode(pass == 0 ? MLRState::OneZeroMode : MLRState::OneOneMode);
        mesh->SetReferenceState(state, pass);
    }
    gos_PopCurrentHeap();
    return mesh;
}

// Everything written so far (GetPointer is the current position, so rewind first).
std::vector<unsigned char> streamBytes(Stuff::DynamicMemoryStream& stream)
{
    const auto used = stream.GetBytesUsed();
    stream.Rewind();
    const auto* begin = static_cast<const unsigned char*>(stream.GetPointer());
    return {begin, begin + used};
}
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/B6 holder: textured multipass MLR_I_MT_PMesh saves and loads back identically")
{
    using namespace MidLevelRenderer;
    BorrowTexturePool textures;
    auto* mesh = makeTexturedMultipassQuad(textures.pool, 0.0f);
    REQUIRE(mesh->GetNumPasses() == 3);

    Stuff::DynamicMemoryStream saved(16);
    WriteMLRVersion(&saved);
    mesh->Save(&saved);
    const auto savedBytes = streamBytes(saved);

    // The existing raw-mesh fixture also exercises the independent data boundary.
    mc2::fx::Reader nativeReader(savedBytes);
    const auto nativeVersion = mc2::fx::read_mlr_version(nativeReader);
    mc2::fx::Writer shapeBytes;
    shapeBytes.integer(1);
    shapeBytes.bytes.insert(shapeBytes.bytes.end(), savedBytes.begin() + nativeReader.offset(), savedBytes.end());
    mc2::fx::Reader shapeReader(shapeBytes.bytes);
    const auto nativeShape = mc2::fx::read_shape(shapeReader, nativeVersion.value);
    mc2::fx::Writer rebuiltShape;
    mc2::fx::write_shape(rebuiltShape, nativeShape, nativeVersion.value);
    CHECK(rebuiltShape.bytes == shapeBytes.bytes);
    REQUIRE(nativeShape.primitives.size() == 1);
    CHECK(nativeShape.primitives[0].materials.size() == 3);
    CHECK(nativeShape.primitives[0].bounds.minimum == mc2::fx::Vec3{0, 0, 0});
    CHECK(nativeShape.primitives[0].bounds.maximum == mc2::fx::Vec3{1, 1, 0});
    for (size_t pass = 0; pass < 3; ++pass) {
        const auto& primitive = nativeShape.primitives[0];
        CHECK(primitive.materials[pass].texture == kPassTextures[pass]);
        const auto& uv = pass ? primitive.extra_texcoords[pass - 1] : primitive.texcoords;
        for (size_t vertex = 0; vertex < 4; ++vertex) {
            CHECK(uv[vertex][0] == kPassUvs[pass][vertex].x);
            CHECK(uv[vertex][1] == kPassUvs[pass][vertex].y);
        }
    }

    Stuff::MemoryStream reader(const_cast<unsigned char*>(savedBytes.data()), savedBytes.size());
    const int version = ReadMLRVersion(&reader);
    Stuff::RegisteredClass::ClassID classId;
    reader >> classId;
    REQUIRE(classId == MLR_I_MT_PMeshClassID);
    auto* loaded = MLR_I_MT_PMesh::Make(&reader, version);
    CHECK(reader.GetBytesRemaining() == 0);    // every pass's state and UVs consumed, nothing written

    REQUIRE(loaded->GetNumPasses() == 3);
    for (int pass = 0; pass < 3; ++pass) {
        CAPTURE(pass);
        const MidLevelRenderer::Vector2DScalar* uvs = nullptr;
        int count = 0;
        loaded->GetTexCoordData(&uvs, &count, pass);
        REQUIRE(count == 4);
        CHECK(std::memcmp(uvs, kPassUvs[pass], sizeof(kPassUvs[pass])) == 0);
        const MLRState& state = loaded->GetReferenceState(pass);
        CHECK(state.GetTextureHandle() == mesh->GetReferenceState(pass).GetTextureHandle());
        CHECK(state.GetAlphaMode() == mesh->GetReferenceState(pass).GetAlphaMode());
    }

    Stuff::DynamicMemoryStream resaved(16);
    WriteMLRVersion(&resaved);
    loaded->Save(&resaved);
    CHECK(streamBytes(resaved) == savedBytes);

    // Copy() collapses to one pass and releases the owned pass slots.
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    loaded->Copy(mesh);
    gos_PopCurrentHeap();
    CHECK(loaded->GetNumPasses() == 1);

    loaded->DetachReference();
    mesh->DetachReference();
}

TEST_CASE_FIXTURE(FrozenPlatform, "vfx_frozen/B6 holder: debris cloud specification saves and loads back identically")
{
    using namespace MidLevelRenderer;
    BorrowTexturePool textures;
    std::vector<unsigned char> savedBytes;
    {
        gosFX::DebrisCloud__Specification spec;
        spec.BuildDefaults();
        // Multi-key ComplexCurves exercise the CurveKey holders.
        auto& speed = spec.m_startingSpeed.m_ageCurve;
        const int mid = speed.InsertKey(0.5f);
        speed[mid].m_value = 4.0f;
        speed[mid].m_slope = -1.5f;
        spec.m_pDrag.m_seedCurve.InsertKey(0.25f);
        spec.m_pDrag.m_seedCurve.InsertKey(0.75f);
        REQUIRE(spec.m_pDrag.m_seedCurve.GetKeyCount() >= 3);
        spec.centerOfForce = glm::vec3(1.0f, 2.0f, 3.0f);
        const float radii[] = {0.5f, 2.0f, 1.25f};
        for (int i = 0; i < 3; ++i) {
            gos_PushCurrentHeap(MidLevelRenderer::Heap);
            auto* shape = new MLRShape(1);
            gos_PopCurrentHeap();
            auto* prim = makeTexturedMultipassQuad(textures.pool, static_cast<float>(i));
            shape->Add(prim);
            prim->DetachReference();
            spec.debrisPieces.push_back(shape);    // the spec destructor detaches it
            spec.debrisPositions.push_back(glm::vec3(static_cast<float>(i), -1.0f, 0.5f));
            mc2::math::Sphere sphere;
            sphere.center = glm::vec3(0.5f + i, 0.5f, 0.0f);
            sphere.radius = radii[i];
            spec.debrisSpheres.push_back(sphere);
            spec.debrisSeed.push_back(0.0f);
        }
        Stuff::DynamicMemoryStream saved(16);
        gosFX::WriteGFXVersion(&saved);
        spec.Save(&saved);
        savedBytes = streamBytes(saved);
    }

    Stuff::MemoryStream reader(savedBytes.data(), savedBytes.size());
    // Existing debris fixture: wrap its single legacy spec as a one-entry catalog.
    auto catalogBytes = savedBytes;
    catalogBytes.insert(catalogBytes.begin() + 8, {1, 0, 0, 0});
    std::string nativeError;
    const auto nativeCatalog = mc2::fx::read_catalog(catalogBytes, nativeError);
    REQUIRE_MESSAGE(nativeCatalog, nativeError);
    CHECK(nativeCatalog->rebuild() == catalogBytes);
    CHECK_MESSAGE(mc2fx::compareLegacy(*nativeCatalog, catalogBytes, nativeError), nativeError);

    const int gfxVersion = gosFX::ReadGFXVersion(&reader);
    Stuff::RegisteredClass::ClassID classId;
    reader >> classId;
    REQUIRE(classId == gosFX::DebrisCloudClassID);
    auto* loaded = gosFX::DebrisCloud__Specification::Make(&reader, gfxVersion);
    CHECK(reader.GetBytesRemaining() == 0);

    REQUIRE(loaded->debrisPieces.size() == 3);
    REQUIRE(loaded->debrisSpheres.size() == 3);
    CHECK(loaded->debrisSpheres[1].radius == 2.0f);
    CHECK(loaded->debrisPositions[2] == glm::vec3(2.0f, -1.0f, 0.5f));
    CHECK(loaded->debrisSeed[0] == 0.0f);    // smallest radius
    CHECK(loaded->debrisSeed[1] == 1.0f);    // largest radius
    CHECK(loaded->debrisSeed[2] == doctest::Approx(0.5f));
    CHECK(loaded->m_startingSpeed.m_ageCurve.GetKeyCount() == 2);
    CHECK(loaded->m_startingSpeed.m_ageCurve[1].m_value == 4.0f);
    for (auto* shape : loaded->debrisPieces) {
        REQUIRE(shape->GetNum() == 1);
        const auto* prim = shape->Find(0);
        REQUIRE(prim->GetClassData() == MLR_I_MT_PMesh::DefaultData);
    }

    Stuff::DynamicMemoryStream resaved(16);
    gosFX::WriteGFXVersion(&resaved);
    loaded->Save(&resaved);
    CHECK(streamBytes(resaved) == savedBytes);
    delete loaded;
}

TEST_CASE("vfx_frozen/Event copy constructor copies m_localToParent")
{
    gosFX::Event source;
    source.m_time = 1.5f;
    source.m_flags = 3u;
    source.m_effectID = 7u;
    source.m_localToParent = glm::mat4(1.0f);
    source.m_localToParent[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    gosFX::Event copy(source);
    CHECK(copy.m_time == 1.5f);
    CHECK(copy.m_flags == 3u);
    CHECK(copy.m_effectID == 7u);
    CHECK(copy.m_localToParent == source.m_localToParent);
    CHECK(copy.m_localToParent[3][0] == 1.0f);
    CHECK(copy.m_localToParent[3][2] == 3.0f);
}
