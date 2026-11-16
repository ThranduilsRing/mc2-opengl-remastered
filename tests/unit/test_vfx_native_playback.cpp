#include "doctest.h"
#include "test_repo_root.h"
#include "gosfxheaders.hpp"
#include "Gates/gate_cold.h"
#include "fx/fx_geometry.h"
#include "fx/fx_tubes.h"
#include "particles/spec_library.h"
#include "particles/light_manager.h"
#include "particles/batcher.h"
#include "mlr/gosimagepool.hpp"
#include "mlr/mlrsortbyorder.hpp"
#include "GameOS/gameos/gos_vfx_mesh_bridge.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

void vfx_test_bind_rng(std::uint32_t* state);
void vfx_test_clear_mesh_capture();
const std::vector<GosVfxMeshInstance>& vfx_test_mesh_instances();
void vfx_test_capture_particles(std::vector<mc2::particles::GpuParticle>* capture);

namespace {
namespace fx = mc2::fx;
namespace math = mc2::math;
const std::string stock_fixture_path = MC2_TEST_FIXTURE_DIR + "/vfx/mc2.fx";

bool stock_fixture_missing() {
    std::error_code error;
    if (std::filesystem::is_regular_file(stock_fixture_path, error)) return false;
    std::printf("SKIP fx_playback/stock: stock VFX fixture unavailable: %s\n", stock_fixture_path.c_str());
    return true;
}

// Public member pointers expose protected oracle observations without casting
// a real base instance to a fabricated derived object or changing legacy code.
struct EffectView : gosFX::Effect {
    static constexpr auto age = &EffectView::m_age, rate = &EffectView::m_ageRate, seed = &EffectView::m_seed;
    static constexpr auto last = &EffectView::m_lastRan;
    static constexpr auto local = &EffectView::m_localToParent;
    static constexpr auto children = &EffectView::m_children;
    static constexpr auto event = &EffectView::m_event;
};
struct CloudView : gosFX::ParticleCloud {
    static constexpr auto active = &CloudView::m_activeParticleCount;
    static constexpr auto births = &CloudView::m_birthAccumulator;
};
struct SingletonView : gosFX::Singleton {
    static constexpr auto color = &SingletonView::m_color;
    static constexpr auto scale = &SingletonView::m_scale, radius = &SingletonView::m_radius;
};
struct PointView : gosFX::PointCloud {
    static constexpr auto positions = &PointView::m_P_localTranslation;
    static constexpr auto colors = &PointView::m_P_color;
};
struct CardView : gosFX::Card {
    static constexpr auto vertices = &CardView::m_vertices;
    static constexpr auto uvs = &CardView::m_uvs;
};
template<class T> struct CloudGeometry : T {
    static constexpr auto vertices = &CloudGeometry::m_P_vertices;
    static constexpr auto colors = &CloudGeometry::m_P_color;
};
struct CardCloudView : gosFX::CardCloud {
    static constexpr auto uvs = &CardCloudView::m_P_uvs;
};
struct TubeView : gosFX::Tube {
    static constexpr auto active = &TubeView::m_activeProfileCount;
    static constexpr auto head = &TubeView::m_headProfile, tail = &TubeView::m_tailProfile;
    static constexpr auto births = &TubeView::m_birthAccumulator;
    static constexpr auto vertices = &TubeView::m_P_vertices;
    static constexpr auto colors = &TubeView::m_P_colors;
    static constexpr auto uvs = &TubeView::m_P_uvs;
    static constexpr auto indices = &TubeView::m_P_indices;
    static constexpr auto triangles = &TubeView::m_triangleCount;
};
struct LightView : gosFX::PointLight { static constexpr auto light = &LightView::m_light; };

template<class T> void exact(const T& actual, const T& expected, const char* field) {
    REQUIRE_MESSAGE(std::memcmp(&actual, &expected, sizeof(T)) == 0, std::string(field));
}
glm::vec4 rgba(const Stuff::RGBAColor& color) { return {color.red, color.green, color.blue, color.alpha}; }

struct Lights : mc2::particles::LightManager {
    std::vector<std::unique_ptr<int>> tokens;
    std::map<gosFX::Light*, Info> samples;
    gosFX::Light* MakePointLight(const char*) override {
        tokens.push_back(std::make_unique<int>(0));
        return reinterpret_cast<gosFX::Light*>(tokens.back().get());
    }
    void ChangeLight(gosFX::Light* light, Info* info) override { samples[light] = *info; }
    void DeleteLight(gosFX::Light* light) override { samples.erase(light); }
};
struct Platform {
    mc2::gate::ScopedOverride<"MC2_DISABLE_GOSFX"> mlr{"0"};
    mc2::gate::ScopedOverride<"MC2_GPU_PARTICLES"> gpu{"1"};
    mc2::gate::ScopedOverride<"MC2_VFX_ORACLE_SHAPE"> shapes{"1"};
    mc2::gate::ScopedOverride<"MC2_VFX_ORACLE_TUBE"> tubes{"1"};
    uint32_t seed = 0x39f20d17;
    Lights lights;
    Platform() {
        mc2::gate::markMainEntered();
        vfx_test_bind_rng(&seed);
        Stuff::InitializeClasses();
        MidLevelRenderer::InitializeClasses(64 * 1024, 16 * 1024, 1024, 4096, true);
        gosFX::InitializeClasses();
        mc2::particles::LightManager::Instance = &lights;
        gos_PushCurrentHeap(MidLevelRenderer::Heap);
        MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(new MidLevelRenderer::TGAFilePool(""));
        gos_PopCurrentHeap();
    }
    ~Platform() {
        mc2::particles::LightManager::Instance = nullptr;
        gosFX::TerminateClasses(); MidLevelRenderer::TerminateClasses(); Stuff::TerminateClasses();
        vfx_test_bind_rng(nullptr);
    }
};
struct Library {
    std::optional<fx::Catalog> catalog;
    std::unique_ptr<gosFX::EffectLibrary> legacy;
    explicit Library(std::vector<uint8_t>& bytes) {
        std::string error;
        catalog = fx::read_catalog(bytes, error);
        REQUIRE_MESSAGE(catalog, error);
        Stuff::MemoryStream stream(bytes.data(), static_cast<DWORD>(bytes.size()));
        gos_PushCurrentHeap(gosFX::Heap);
        legacy = std::make_unique<gosFX::EffectLibrary>(); legacy->Load(&stream);
        gos_PopCurrentHeap();
        gosFX::EffectLibrary::Instance = legacy.get();
        REQUIRE(mc2::particles::SpecLibrary::Instance()->Count() == catalog->effects().size());
    }
    ~Library() { gosFX::EffectLibrary::Instance = nullptr; legacy.reset(); }
};
struct RandomCopy {
    Stuff::Random::State state = Stuff::Random::GetState();
    float fraction() {
        const auto other = state.index >= 147 ? state.index - 147 : state.index + 103;
        const int value = state.numbers[state.index] ^ state.numbers[other];
        state.numbers[state.index] = value;
        if (++state.index == 250) state.index = 0;
        ++state.draws;
        return static_cast<float>(value) / 32768.0f;
    }
    void check() {
        const auto expected = Stuff::Random::GetState();
        REQUIRE(state.index == expected.index); REQUIRE(state.draws == expected.draws);
        REQUIRE(state.numbers == expected.numbers);
    }
};

template<class Cloud> void cloud_state(Cloud& legacy, const fx::Instance& native) {
    const auto active = legacy.*CloudView::active;
    REQUIRE(active == native.active_count());
    exact(native.birth_accumulator(), legacy.*CloudView::births, "birth accumulator");
    for (int slot = 0; slot < active; ++slot) {
        CAPTURE(slot);
        const auto& p = native.particles()[slot]; const auto& old = *legacy.GetParticle(slot);
        exact(p.age, old.m_age, "particle age");
        if (p.age >= 1) continue;
        exact(p.age_rate, old.m_ageRate, "particle rate"); exact(p.seed, old.m_seed, "particle seed");
        exact(p.velocity, old.m_localLinearVelocity, "particle velocity");
        if constexpr (std::is_same_v<Cloud, gosFX::PointCloud>) {
            exact(p.position, (legacy.*PointView::positions)[slot], "point position");
            exact(p.color, rgba((legacy.*PointView::colors)[slot]), "point color");
        } else {
            exact(p.position, old.m_localTranslation, "particle position");
            exact(p.rotation, old.m_localRotation, "particle rotation");
            exact(p.angular_velocity, old.m_angularVelocity, "particle spin");
            exact(p.scale, old.m_scale, "particle scale"); exact(p.radius, old.m_radius, "particle radius");
            if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                exact(p.half_x, old.m_halfX, "card half X"); exact(p.half_y, old.m_halfY, "card half Y");
                for (int i = 0; i < 4; ++i) {
                    const auto uv = (legacy.*CardCloudView::uvs)[slot * 4 + i];
                    exact(p.uvs[i], glm::vec2(uv.x, uv.y), "card UV");
                }
            }
            if constexpr (std::is_same_v<Cloud, gosFX::ShardCloud>) exact(p.angle, old.m_angle, "shard angle");
            if constexpr (std::is_same_v<Cloud, gosFX::PertCloud>)
                for (size_t i = 0; i < p.vertices.size(); ++i) {
                    CAPTURE(i);
                    for (int axis = 0; axis < 3; ++axis) {
                        CAPTURE(axis); CAPTURE(std::bit_cast<uint32_t>(p.vertices[i][axis])); CAPTURE(std::bit_cast<uint32_t>(old.m_vertices[i][axis]));
                        exact(p.vertices[i][axis], old.m_vertices[i][axis], "pert template");
                    }
                }
        }
    }
}
void state(gosFX::Effect& legacy, const fx::Instance& native, Lights& lights) {
    CAPTURE(native.record().name);
    REQUIRE(legacy.GetSpecification()->GetClassID() == native.record().class_id);
    REQUIRE(std::string(static_cast<const char*>(legacy.GetSpecification()->m_name)) == native.record().name);
    REQUIRE(legacy.GetSimulationFlags() == native.flags());
    exact(native.local_to_parent, legacy.*EffectView::local, "effect local transform");
    REQUIRE(legacy.IsExecuted() == native.is_executed());
    REQUIRE(legacy.HasFinished() == native.has_finished());
    exact(native.age(), legacy.*EffectView::age, "effect age");
    exact(native.age_rate(), legacy.*EffectView::rate, "effect rate");
    exact(native.seed(), legacy.*EffectView::seed, "effect seed");
    exact(native.last_ran(), legacy.*EffectView::last, "effect time");
    exact(native.local_to_world(), legacy.m_localToWorld, "effect world transform");
    REQUIRE(native.next_event() == legacy.*EffectView::event);
    const auto type = native.record().class_id - fx::first_effect_class;
    switch (type) {
    case 2: cloud_state(static_cast<gosFX::PointCloud&>(legacy), native); break;
    case 4: cloud_state(static_cast<gosFX::ShardCloud&>(legacy), native); break;
    case 5: cloud_state(static_cast<gosFX::PertCloud&>(legacy), native); break;
    case 6: cloud_state(static_cast<gosFX::CardCloud&>(legacy), native); break;
    case 7: cloud_state(static_cast<gosFX::ShapeCloud&>(legacy), native); break;
    case 8: {
        auto& cloud = static_cast<gosFX::EffectCloud&>(legacy);
        cloud_state(cloud, native);
        for (size_t i = 0; i < native.active_count(); ++i) {
            const auto* old = cloud.GetParticle(static_cast<unsigned>(i));
            REQUIRE(bool(old->m_effect) == bool(native.particles()[i].effect));
            if (old->m_effect) state(*old->m_effect, *native.particles()[i].effect, lights);
        }
        break;
    }
    case 10: case 11: {
        auto& singleton = static_cast<gosFX::Singleton&>(legacy);
        if (native.is_executed()) {
            exact(native.singleton().color, rgba(singleton.*SingletonView::color), "singleton color");
            exact(native.singleton().scale, singleton.*SingletonView::scale, "singleton scale");
        }
        break;
    }
    case 12: {
        auto& tube = static_cast<gosFX::Tube&>(legacy);
        REQUIRE(native.active_count() == tube.*TubeView::active);
        exact(native.birth_accumulator(), tube.*TubeView::births, "tube births");
        for (size_t i = 0; i < native.profiles().size(); ++i) {
            const auto& p = native.profiles()[i]; const auto& old = *tube.GetProfile(static_cast<unsigned>(i));
            // Unused legacy profiles are not initialized. Walk the live ring only.
            size_t slot = native.head_profile(); bool live = false;
            for (size_t n = 0; n < native.active_count(); ++n) { live |= slot == i; slot = slot ? slot - 1 : native.profiles().size() - 1; }
            if (!live) continue;
            exact(p.age, old.m_age, "profile age"); exact(p.age_rate, old.m_ageRate, "profile rate");
            exact(p.seed, old.m_seed, "profile seed"); exact(p.direction, old.m_direction, "profile direction");
            exact(p.profile_to_world, old.m_profileToWorld, "profile origin");
        }
        break;
    }
    case 13: {
        auto& debris = static_cast<gosFX::DebrisCloud&>(legacy);
        for (size_t i = 0; i < native.particles().size(); ++i) {
            const auto& p = native.particles()[i]; const auto& old = *debris.GetParticle(static_cast<unsigned>(i));
            exact(p.age, old.m_age, "debris age");
            if (p.age >= 1) continue;
            exact(p.seed, old.m_seed, "debris seed"); exact(p.age_rate, old.m_ageRate, "debris rate");
            exact(p.debris_transform, old.m_localToParent, "debris transform");
            exact(p.velocity, old.m_linearVelocity, "debris velocity");
            exact(p.angular_velocity, old.m_angularVelocity, "debris spin");
        }
        break;
    }
    case 14: {
        auto* light = static_cast<gosFX::PointLight&>(legacy).*LightView::light;
        REQUIRE(bool(light) == native.light().active);
        if (light && lights.samples.contains(light)) {
            const auto& old = lights.samples.at(light); const auto& sample = native.light();
            exact(sample.color, glm::vec3(old.m_color.red, old.m_color.green, old.m_color.blue), "light color");
            exact(sample.origin, old.m_origin, "light origin"); exact(sample.intensity, old.m_intensity, "light intensity");
            exact(sample.inner, old.m_inner, "light inner"); exact(sample.outer, old.m_outer, "light outer");
        }
        break;
    }
    default: break;
    }
    const auto& children = legacy.*EffectView::children;
    REQUIRE(children.size() == native.children().size());
    for (size_t i = 0; i < children.size(); ++i) state(*children[i], *native.children()[i], lights);
}

template<class Cloud> void cloud_geometry(Cloud& cloud, const glm::mat4& world, std::vector<fx::Vertex>& out) {
    const int count = cloud.*CloudView::active;
    for (int slot = 0; slot < count; ++slot) {
        if (cloud.GetParticle(slot)->m_age >= 1) continue;
        unsigned vertices = 3;
        if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) vertices = 4;
        if constexpr (std::is_same_v<Cloud, gosFX::PertCloud>) vertices = cloud.GetSpecification()->m_vertices;
        for (unsigned i = 0; i < vertices; ++i) {
            const auto position = (cloud.*CloudGeometry<Cloud>::vertices)[slot * vertices + i];
            const unsigned color_index = std::is_same_v<Cloud, gosFX::ShardCloud> ? slot * 3 + i :
                std::is_same_v<Cloud, gosFX::PertCloud> ? slot * 2 + (i ? 1 : 0) : slot;
            const auto color = rgba((cloud.*CloudGeometry<Cloud>::colors)[color_index]);
            glm::vec2 uv{0};
            if constexpr (std::is_same_v<Cloud, gosFX::CardCloud>) {
                const auto old = (cloud.*CardCloudView::uvs)[slot * 4 + i]; uv = {old.x, old.y};
            }
            out.push_back({math::transformPoint(position, world), color, uv});
        }
    }
}
void old_vertices(gosFX::Effect& effect, const glm::mat4& parent, const glm::vec3& camera,
    std::vector<fx::Vertex>& out, std::vector<std::vector<uint16_t>>& indices) {
    const auto world = effect.*EffectView::local * parent;
    const auto type = effect.GetSpecification()->GetClassID() - fx::first_effect_class;
    if (type == 2) {
        auto& cloud = static_cast<gosFX::PointCloud&>(effect);
        for (int i = 0; i < cloud.*CloudView::active; ++i) if (cloud.GetParticle(i)->m_age < 1)
            out.push_back({math::transformPoint((cloud.*PointView::positions)[i], world), rgba((cloud.*PointView::colors)[i]), {}});
    } else if (type == 4) cloud_geometry(static_cast<gosFX::ShardCloud&>(effect), world, out);
    else if (type == 5) cloud_geometry(static_cast<gosFX::PertCloud&>(effect), world, out);
    else if (type == 6) cloud_geometry(static_cast<gosFX::CardCloud&>(effect), world, out);
    else if (type == 8) {
        auto& cloud = static_cast<gosFX::EffectCloud&>(effect);
        for (int i = 0; i < cloud.*CloudView::active; ++i) if (auto* child = cloud.GetParticle(i)->m_effect) old_vertices(*child, parent, camera, out, indices);
    } else if (type == 10) {
        auto& card = static_cast<gosFX::Card&>(effect);
        const auto* spec = card.GetSpecification();
        auto model = world;
        const auto direction = camera - math::getTranslation(model);
        if (spec->m_alignZUsingX) math::alignLocalAxisToWorldVector(model, direction, 2,
            spec->m_alignZUsingY ? 1 : 0, spec->m_alignZUsingY ? 0 : -1);
        else if (spec->m_alignZUsingY) math::alignLocalAxisToWorldVector(model, direction, 2, 1, -1);
        for (int i = 0; i < 4; ++i) {
            const auto uv = (card.*CardView::uvs)[i];
            out.push_back({math::transformPoint((card.*CardView::vertices)[i], model), rgba(card.*SingletonView::color), {uv.x, uv.y}});
        }
    } else if (type == 12) {
        auto& tube = static_cast<gosFX::Tube&>(effect);
        const auto count = tube.*TubeView::active;
        if (count > 1) indices.emplace_back(tube.*TubeView::indices, tube.*TubeView::indices + (tube.*TubeView::triangles) * 3);
        if (count > 1) for (size_t i = 0; i < count * fx::tube_template(static_cast<uint32_t>(tube.GetSpecification()->m_profileType)).size(); ++i) {
            const auto uv = (tube.*TubeView::uvs)[i];
            out.push_back({math::transformPoint((tube.*TubeView::vertices)[i], world), rgba((tube.*TubeView::colors)[i]), {uv.x, uv.y}});
        }
    }
    for (const auto& child : effect.*EffectView::children) old_vertices(*child, world, camera, out, indices);
}
void geometry(gosFX::Effect& old, const fx::Instance& native, const glm::mat4& parent, const glm::vec3& camera,
    MidLevelRenderer::MLRClipper& clipper) {
    const auto frozen = Stuff::Random::GetState();
    const auto before = native.age();
    auto packets = fx::generate_geometry(native, parent, camera);
    glm::mat4 view{1}, projection;
    math::buildTranslation(view, camera); math::setPerspective(projection, 1, 100, 1.2f, 1);
    MidLevelRenderer::MLRState render_state;
    clipper.StartDraw(view, projection, Stuff::RGBAColor(0, 0, 0, 1), nullptr, render_state, nullptr);
    gosFX::Effect::DrawInfo draw{}; draw.m_parentToWorld = &parent; draw.m_clipper = &clipper;
    mc2::particles::Batcher::Instance().Flush();
    std::vector<mc2::particles::GpuParticle> submitted;
    vfx_test_capture_particles(&submitted);
    vfx_test_clear_mesh_capture(); old.Draw(&draw);
    mc2::particles::Batcher::Instance().Flush();
    vfx_test_capture_particles(nullptr);
    std::vector<fx::Vertex> expected, actual;
    std::vector<std::vector<uint16_t>> tube_indices;
    old_vertices(old, parent, camera, expected, tube_indices);
    for (const auto& packet : packets) if (packet.kind != fx::GeometryKind::mesh)
        actual.insert(actual.end(), packet.vertices.begin(), packet.vertices.end());
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        exact(actual[i].position, expected[i].position, "generated position");
        exact(actual[i].color, expected[i].color, "generated color"); exact(actual[i].uv, expected[i].uv, "generated UV");
    }
    size_t particle_index = 0, tube_index = 0;
    for (const auto& packet : packets) {
        if (packet.kind == fx::GeometryKind::mesh) continue;
        if (packet.effect->class_id == gosFX::TubeClassID) {
            REQUIRE(tube_index < tube_indices.size());
            REQUIRE(packet.indices == tube_indices[tube_index++]); continue;
        }
        const auto check_vertex = [&](size_t index, glm::vec2 uv) {
            REQUIRE(particle_index < submitted.size());
            const auto& vertex = submitted[particle_index++];
            exact(packet.vertices[index].position, glm::make_vec3(vertex.position), "submitted position/order");
            exact(uv, glm::make_vec2(vertex.velocity), "submitted UV/order");
        };
        if (packet.kind == fx::GeometryKind::points) {
            for (size_t i = 0; i < packet.vertices.size(); ++i) {
                check_vertex(i, {0, 0}); check_vertex(i, {2.4f, 0}); check_vertex(i, {0, 2.4f});
            }
        } else if (packet.kind == fx::GeometryKind::quads) {
            for (size_t i = 0; i < packet.vertices.size(); i += 4)
                for (size_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) check_vertex(i + corner, packet.vertices[i + corner].uv);
        } else if (!packet.indices.empty()) {
            for (auto index : packet.indices) check_vertex(index, packet.vertices[index].uv);
        } else for (size_t i = 0; i < packet.vertices.size(); ++i) check_vertex(i, packet.vertices[i].uv);
    }
    REQUIRE(particle_index == submitted.size()); REQUIRE(tube_index == tube_indices.size());
    size_t mesh_index = 0;
    const auto& instances = vfx_test_mesh_instances();
    for (const auto& packet : packets) if (packet.kind == fx::GeometryKind::mesh)
        for (const auto& primitive : packet.shape->primitives) {
            if (primitive.positions.empty() || primitive.indices.empty()) continue;
            REQUIRE(mesh_index < instances.size());
            const auto& old_mesh = instances[mesh_index++];
            for (int axis = 0; axis < 3; ++axis) for (int k = 0; k < 4; ++k)
                exact(packet.model[axis][k], old_mesh.modelToWorld[k * 4 + axis], "mesh model");
            exact(packet.scale.x, old_mesh.scale, "mesh scale");
            auto color = packet.color; if (old_mesh.blendMode == 3) color.a = 1;
            exact(color, glm::make_vec4(old_mesh.rgba), "mesh paint");
        }
    REQUIRE(mesh_index == instances.size());
    REQUIRE(Stuff::Random::GetState() == frozen); exact(native.age(), before, "draw changed age");
    // A second extraction catches mutations hidden in frame construction.
    const auto repeat = fx::generate_geometry(native, parent, camera);
    REQUIRE(repeat.size() == packets.size());
    for (size_t i = 0; i < repeat.size(); ++i) {
        REQUIRE(repeat[i].vertices.size() == packets[i].vertices.size());
        for (size_t j = 0; j < repeat[i].vertices.size(); ++j) exact(repeat[i].vertices[j], packets[i].vertices[j], "unstable geometry");
    }
}

void playback(Library& library, size_t id, int scenario, Lights& lights, MidLevelRenderer::MLRClipper& clipper) {
    RandomCopy random;
    fx::RandomFraction fraction = [&] { return random.fraction(); };
    const unsigned mode = scenario % 3 == 0 ? fx::local_space : scenario % 3 == 1 ? fx::dynamic_world_space : fx::static_world_space;
    const unsigned flags = fx::execute_flag | mode | (scenario == 1 ? fx::loop_flag : 0);
    auto legacy = library.legacy->MakeEffect(static_cast<unsigned>(id), flags);
    fx::Instance native(library.catalog->effects(), id, fraction, flags);
    const float seed = scenario == 0 ? -1 : scenario == 1 ? 0.173f : 0.881f;
    glm::mat4 parent{1}; math::buildTranslation(parent, {0.5f, -0.25f, 10});
    math::buildRotation(parent, math::Euler{0.125f, 0.25f, -0.375f});
    mc2::math::Obb bounds;
    gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &parent, &bounds, seed);
    if (scenario == 3) { info.m_age = 0.25f; info.m_ageRate = 3.25f; }
    legacy->Start(&info);
    native.start({0, parent, seed, info.m_age, info.m_ageRate}); random.check();
    for (int frame = 1; frame <= 36; ++frame) {
        CAPTURE(frame);
        if (scenario == 2 && frame == 6) { legacy->Stop(); native.stop(); }
        if (frame == 24) { legacy->SetLoopOff(); native.set_looped(false); legacy->Stop(); native.stop(); }
        if (frame == 30 || (scenario == 3 && frame == 6)) { legacy->Kill(); native.kill(); state(*legacy, native, lights); random.check(); break; }
        if (scenario == 3 && frame == 4) {
            info.m_seed = 0.625f; legacy->Start(&info);
            native.start({info.m_time, parent, info.m_seed, info.m_age, info.m_ageRate}); random.check();
        }
        info.m_time = frame / 30.0; info.m_seed = scenario == 3 && frame == 3 ? 0.375f : -1;
        math::buildTranslation(parent, {frame * 0.125f, frame * -0.0625f, 10});
        const bool old_alive = legacy->Execute(&info), alive = native.execute({info.m_time, parent, info.m_seed});
        REQUIRE(alive == old_alive); random.check(); state(*legacy, native, lights);
        exact(native.bounds().localToParent, bounds.localToParent, "bounds transform");
        exact(native.bounds().axisExtents, bounds.axisExtents, "bounds extents");
        exact(native.bounds().sphereRadius, bounds.sphereRadius, "bounds radius");
        if (alive && (frame == 1 || frame == 5 || frame == 12)) geometry(*legacy, native, parent,
            frame == 5 ? glm::vec3(9, -4, 6) : glm::vec3(0), clipper);
        random.check();
        if (!alive) break;
    }
}

std::vector<uint8_t> controlled_catalog(unsigned orientation) {
    gosFX::Effect::Specification parent;
    gosFX::Card::Specification card;
    gosFX::PointCloud::Specification point;
    gosFX::ShardCloud::Specification shard;
    gosFX::PertCloud::Specification pert(6);
    gosFX::CardCloud::Specification cloud;
    gosFX::EffectCloud::Specification effects;
    gosFX::Tube::Specification tube;
    gosFX::PointLight::Specification light;
    std::array<gosFX::Effect::Specification*, 9> specs{&parent, &card, &point, &shard, &pert, &cloud, &effects, &tube, &light};
    for (size_t i = 0; i < specs.size(); ++i) {
        auto& spec = *specs[i]; spec.BuildDefaults();
        spec.m_name = ("controlled" + std::to_string(i)).c_str();
        spec.m_lifeSpan.SetCurve(0.23f);
        spec.m_minimumChildSeed.SetCurve(0.125f); spec.m_maximumChildSeed.SetCurve(0.875f);
        spec.m_state.SetTextureHandle(0);
    }
    for (auto* spec : std::array<gosFX::ParticleCloud::Specification*, 5>{&point, &shard, &pert, &cloud, &effects}) {
        spec->m_maxParticleCount = 6; spec->m_startingPopulation.SetCurve(3.5f);
        spec->m_particlesPerSecond.SetCurve(35.0f);
        spec->m_pLifeSpan.m_ageCurve.SetCurve(0.12f);
        spec->m_emitterSizeX.SetCurve(0.75f); spec->m_emitterSizeY.SetCurve(0.5f); spec->m_emitterSizeZ.SetCurve(0.25f);
        spec->m_minimumDeviation.SetCurve(0.1f); spec->m_maximumDeviation.SetCurve(0.75f);
        spec->m_startingSpeed.m_ageCurve.SetCurve(1.25f);
        spec->m_pDrag.m_ageCurve.SetCurve(0.375f); spec->m_pAccelerationY.m_ageCurve.SetCurve(-0.75f);
        spec->m_pRed.m_ageCurve.SetCurve(0.2f, 0.8f); spec->m_pAlpha.m_ageCurve.SetCurve(0.9f, 0.1f);
    }
    for (auto* spec : std::array<gosFX::SpinningCloud::Specification*, 4>{&shard, &pert, &cloud, &effects}) {
        spec->m_randomStartingRotation = orientation & 1;
        spec->m_alignZUsingX = orientation & 2; spec->m_alignZUsingY = orientation & 4;
        spec->m_alignYUsingVelocity = orientation == 7;
        spec->m_pSpin.m_ageCurve.SetCurve(0.5f);
        spec->m_pScale.m_ageCurve.SetCurve(0.75f, 1.25f);
    }
    card.m_alignZUsingX = orientation & 2; card.m_alignZUsingY = orientation & 4;
    card.m_USize.SetCurve(0.25f); card.m_VSize.SetCurve(0.5f); card.m_index.m_ageCurve.SetCurve(0.0f, 7.0f); card.m_animated = true;
    cloud.m_USize.SetCurve(0.25f); cloud.m_VSize.SetCurve(0.5f); cloud.m_pIndex.m_ageCurve.SetCurve(0.0f, 7.0f); cloud.m_animated = true;
    effects.m_particleEffectID = 1;
    tube.m_maxProfileCount = 6;
    tube.m_profileType = static_cast<gosFX::Tube::Specification::ProfileType>(orientation);
    tube.m_insideOut = orientation & 1;
    tube.m_profilesPerSecond.SetCurve(35); tube.m_pLifeSpan.SetCurve(0.12f);
    if (orientation == 7) {
        tube.m_pUSize.m_ageCurve.SetCurve(50);
        tube.m_pUOffset.m_ageCurve.SetCurve(-12.5f, 0.25f);
    }
    tube.BuildTemplate();
    light.m_red.SetCurve(0.2f, 0.8f); light.m_intensity.SetCurve(0.3f, 2.3f);
    light.m_innerRadius.SetCurve(1.5f); light.m_outerRadius.SetCurve(12.5f);
    // Equal times, inherited simulation, noncommuting transforms and two levels.
    for (unsigned id : {6u, 8u, 2u}) {
        auto event = std::make_unique<gosFX::Event>();
        event->m_time = id == 2 ? 0.5f : 0;
        event->m_flags = gosFX::Effect::ParentSimulationMode; event->m_effectID = id;
        event->m_localToParent = glm::mat4(1);
        math::buildRotation(event->m_localToParent, math::Euler{0.25f, -0.125f, 0.5f});
        math::buildTranslation(event->m_localToParent, {float(id), -0.5f, 0.25f});
        parent.m_events.push_back(std::move(event));
    }
    Stuff::DynamicMemoryStream stream(1024 * 1024);
    stream.Rewind(); gosFX::WriteGFXVersion(&stream); stream << static_cast<unsigned>(specs.size());
    for (auto* spec : specs) spec->Save(&stream);
    const auto size = stream.GetBytesUsed(); stream.Rewind();
    const auto* data = static_cast<const uint8_t*>(stream.GetPointer());
    return {data, data + size};
}
}

TEST_CASE("fx_playback/spec index matches linear catalog lookup and first duplicate") {
    Platform platform;
    auto check = [](std::vector<uint8_t>& bytes, bool duplicates) {
        Library library(bytes);
        auto* lib = mc2::particles::SpecLibrary::Instance();
        auto reference = [lib](const char* name) -> gosFX::Effect::Specification* {
            for (unsigned i = 0; i < lib->Count(); ++i) {
                auto* spec = lib->At(i);
                if (spec && !S_stricmp(spec->m_name, name)) return spec;
            }
            return nullptr;
        };
        REQUIRE(lib->Count() > 0);
        for (unsigned i = 0; i < lib->Count(); ++i) {
            auto* spec = lib->At(i); if (!spec) continue;
            std::string name = static_cast<const char*>(spec->m_name), upper = name, lower = name;
            for (char& byte : upper) byte = static_cast<char>(std::toupper(static_cast<unsigned char>(byte)));
            for (char& byte : lower) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
            for (const auto& query : {name, upper, lower, name.substr(0, name.size() - 1), name + "~"})
                CHECK(lib->Find(query.c_str()) == reference(query.c_str()));
        }
        for (const char* absent : {"__absent_spec_index__", "Spec Index Duplicat", "Spec Index Duplicate~"}) {
            REQUIRE(reference(absent) == nullptr); CHECK(lib->Find(absent) == nullptr);
        }
        if (duplicates) CHECK(lib->Find("SPEC INDEX DUPLICATE") == lib->At(0));
    };
    std::ifstream file(stock_fixture_path, std::ios::binary); REQUIRE(file.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    check(bytes, false);
    gosFX::Effect::Specification first, second; first.BuildDefaults(); second.BuildDefaults();
    first.m_name = "Spec Index Duplicate"; second.m_name = "sPEC iNDEX dUPLICATE";
    Stuff::DynamicMemoryStream stream(1024 * 1024); gosFX::WriteGFXVersion(&stream); stream << 2u;
    first.Save(&stream); second.Save(&stream); const auto size = stream.GetBytesUsed(); stream.Rewind();
    const auto* begin = static_cast<const uint8_t*>(stream.GetPointer());
    bytes.assign(begin, begin + size); check(bytes, true);
}

TEST_CASE_FIXTURE(Platform, "fx_playback/stock classes match legacy lifecycle RNG bounds and geometry" * doctest::skip(stock_fixture_missing())) {
    size_t begin = 0, end = 0;
    SUBCASE("quarter1") { begin = 0; end = 226; }
    SUBCASE("quarter2") { begin = 226; end = 452; }
    SUBCASE("quarter3") { begin = 452; end = 678; }
    SUBCASE("quarter4") { begin = 678; end = 904; }
    REQUIRE(end > begin);
    std::ifstream file(stock_fixture_path, std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    Library library(bytes);
    REQUIRE(library.catalog->effects().size() == 904);
    MidLevelRenderer::MLRClipper clipper(nullptr, new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance));
    const glm::mat4 camera{1}; glm::mat4 projection;
    math::setPerspective(projection, 1, 100, 1.2f, 1);
    MidLevelRenderer::MLRState render_state;
    clipper.StartDraw(camera, projection, Stuff::RGBAColor(0, 0, 0, 1), nullptr, render_state, nullptr);
    std::set<uint32_t> tested;
    for (size_t id = begin; id < end; ++id) {
        const auto& record = library.catalog->effects()[id];
        tested.insert(record.class_id);
        CAPTURE(id); CAPTURE(record.name);
        for (int scenario = 0; scenario < 3; ++scenario) { CAPTURE(scenario); playback(library, id, scenario, lights, clipper); }
    }
    for (auto id : tested) std::printf("native playback class %u %s: identical\n", id, fx::class_name(id));
    std::printf("native playback stock definitions [%zu,%zu): identical x 3 scenarios\n", begin, end);
}

TEST_CASE_FIXTURE(Platform, "fx_playback/controlled nested loops dead-slot reuse Stop Kill all profile and alignment modes") {
    MidLevelRenderer::MLRClipper clipper(nullptr, new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance));
    const glm::mat4 camera{1}; glm::mat4 projection;
    math::setPerspective(projection, 1, 100, 1.2f, 1);
    MidLevelRenderer::MLRState render_state;
    for (unsigned orientation = 0; orientation < 8; ++orientation) {
        CAPTURE(orientation);
        clipper.StartDraw(camera, projection, Stuff::RGBAColor(0, 0, 0, 1), nullptr, render_state, nullptr);
        auto bytes = controlled_catalog(orientation); Library library(bytes);
        for (size_t id = 0; id < library.catalog->effects().size(); ++id) {
            CAPTURE(id);
            for (int scenario = 0; scenario < 4; ++scenario) { CAPTURE(scenario); playback(library, id, scenario, lights, clipper); }
        }
    }
    std::printf("native playback controlled: 9 classes x 8 modes x 4 scenarios, including PertCloud and PointLight: identical\n");
}
