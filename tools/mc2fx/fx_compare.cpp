// Offline oracle only. No production native reader depends on these headers.
#include "mc2fx_core.h"
#include "gosfxheaders.hpp"
#include "mlr/mlrheaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace mc2fx {
namespace {
using namespace mc2::fx;
using namespace MidLevelRenderer;

struct Compare {
    size_t events = 0, curves = 0, materials = 0, meshes = 0, bounds = 0;
    std::string context;
    void require(bool condition, const std::string& field) const {
        if (!condition) throw std::runtime_error(context + ": " + field);
    }
    void scalar(float a, float b, const std::string& field) const {
        require(std::isfinite(a) && std::isfinite(b) &&
            std::abs(a - b) <= 1e-5f * std::max({1.0f, std::abs(a), std::abs(b)}), field);
    }
    template<size_t N, class T> void vector(const std::array<float, N>& a, const T& b, const char* field) const {
        for (size_t i = 0; i < N; ++i) require(a[i] == b[i], field);
    }
};

CurveData decoded(gosFX::ConstantCurve& curve) {
    CurveData out; out.coefficients[0] = curve.m_value; return out;
}
CurveData decoded(gosFX::LinearCurve& curve) {
    auto out = decoded(static_cast<gosFX::ConstantCurve&>(curve));
    out.kind = CurveKind::linear; out.coefficients[1] = curve.m_slope; return out;
}
CurveData decoded(gosFX::SplineCurve& curve) {
    auto out = decoded(static_cast<gosFX::LinearCurve&>(curve));
    out.kind = CurveKind::spline; out.coefficients[2] = curve.m_b; out.coefficients[3] = curve.m_a; return out;
}
CurveData decoded(gosFX::ComplexCurve& curve) {
    CurveData out; out.kind = CurveKind::complex;
    for (int i = 0; i < curve.GetKeyCount(); ++i) out.keys.push_back({curve[i].m_time, curve[i].m_slope, curve[i].m_value});
    return out;
}
template<class C> Curve decoded(const char* name, C& curve) { return {name, decoded(curve), {}, false}; }
template<class C, class S, gosFX::Curve::CurveType T>
Curve decoded(const char* name, gosFX::SeededCurveOf<C, S, T>& curve) {
    return {name, decoded(curve.m_ageCurve), decoded(curve.m_seedCurve), curve.m_seeded};
}

std::vector<Curve> legacy_curves(gosFX::Effect__Specification& spec) {
    std::vector<Curve> out;
    const auto add = [&](const char* name, auto& curve) { out.push_back(decoded(name, curve)); };
#define FIELD(s, n) add(#n, (s).n)
    FIELD(spec, m_lifeSpan); FIELD(spec, m_minimumChildSeed); FIELD(spec, m_maximumChildSeed);
    const auto id = spec.GetClassID() - first_effect_class;
    if (id >= 1 && id <= 8) {
        auto& s = static_cast<gosFX::ParticleCloud__Specification&>(spec);
        FIELD(s, m_startingPopulation); FIELD(s, m_particlesPerSecond);
        FIELD(s, m_emitterSizeX); FIELD(s, m_emitterSizeY); FIELD(s, m_emitterSizeZ);
        FIELD(s, m_minimumDeviation); FIELD(s, m_maximumDeviation); FIELD(s, m_startingSpeed); FIELD(s, m_pLifeSpan);
        FIELD(s, m_pEtherVelocityX); FIELD(s, m_pEtherVelocityY); FIELD(s, m_pEtherVelocityZ);
        FIELD(s, m_pAccelerationX); FIELD(s, m_pAccelerationY); FIELD(s, m_pAccelerationZ); FIELD(s, m_pDrag);
        FIELD(s, m_pRed); FIELD(s, m_pGreen); FIELD(s, m_pBlue); FIELD(s, m_pAlpha);
        if (id >= 3) {
            auto& spin = static_cast<gosFX::SpinningCloud__Specification&>(spec);
            FIELD(spin, m_pSpin); FIELD(spin, m_pScale);
        }
    }
    if (id >= 9 && id <= 11) {
        auto& s = static_cast<gosFX::Singleton__Specification&>(spec);
        FIELD(s, m_red); FIELD(s, m_green); FIELD(s, m_blue); FIELD(s, m_alpha); FIELD(s, m_scale);
    }
    switch (id) {
    case 4: { auto& s = static_cast<gosFX::ShardCloud__Specification&>(spec); FIELD(s, m_size); FIELD(s, m_angularity); break; }
    case 5: { auto& s = static_cast<gosFX::PertCloud__Specification&>(spec);
        FIELD(s, m_size); FIELD(s, m_perturbation); FIELD(s, m_pCenterRed); FIELD(s, m_pCenterGreen);
        FIELD(s, m_pCenterBlue); FIELD(s, m_pCenterAlpha); break; }
    case 6: { auto& s = static_cast<gosFX::CardCloud__Specification&>(spec);
        FIELD(s, m_halfHeight); FIELD(s, m_aspectRatio); FIELD(s, m_pIndex); FIELD(s, m_UOffset);
        FIELD(s, m_VOffset); FIELD(s, m_USize); FIELD(s, m_VSize); break; }
    case 10: { auto& s = static_cast<gosFX::Card__Specification&>(spec);
        FIELD(s, m_halfHeight); FIELD(s, m_aspectRatio); FIELD(s, m_index); FIELD(s, m_UOffset);
        FIELD(s, m_VOffset); FIELD(s, m_USize); FIELD(s, m_VSize); break; }
    case 12: { auto& s = static_cast<gosFX::Tube__Specification&>(spec);
        FIELD(s, m_profilesPerSecond); FIELD(s, m_pLifeSpan); FIELD(s, m_emitterSizeX); FIELD(s, m_emitterSizeY);
        FIELD(s, m_emitterSizeZ); FIELD(s, m_minimumDeviation); FIELD(s, m_maximumDeviation);
        FIELD(s, m_pDisplacement); FIELD(s, m_pScale); FIELD(s, m_pRed); FIELD(s, m_pGreen); FIELD(s, m_pBlue);
        FIELD(s, m_pAlpha); FIELD(s, m_pUOffset); FIELD(s, m_pVOffset); FIELD(s, m_pUSize); FIELD(s, m_pVSize); break; }
    case 13: { auto& s = static_cast<gosFX::DebrisCloud__Specification&>(spec);
        FIELD(s, m_minimumDeviation); FIELD(s, m_maximumDeviation); FIELD(s, m_startingSpeed); FIELD(s, m_pLifeSpan);
        FIELD(s, m_pEtherVelocityY); FIELD(s, m_pAccelerationY); FIELD(s, m_pDrag); FIELD(s, m_pAlpha); FIELD(s, m_pSpin); break; }
    case 14: { auto& s = static_cast<gosFX::PointLight__Specification&>(spec);
        FIELD(s, m_red); FIELD(s, m_green); FIELD(s, m_blue); FIELD(s, m_intensity);
        FIELD(s, m_innerRadius); FIELD(s, m_outerRadius); break; }
    default: break;
    }
#undef FIELD
    return out;
}

void compare_data(Compare& c, const CurveData& native, const CurveData& legacy, const std::string& field) {
    c.require(native.kind == legacy.kind, field + " kind");
    for (size_t i = 0; i < 4; ++i) c.scalar(native.coefficients[i], legacy.coefficients[i], field + " coefficient");
    c.require(native.keys.size() == legacy.keys.size(), field + " key count");
    for (size_t i = 0; i < native.keys.size(); ++i) {
        const auto key = native.effective_key(i);
        c.scalar(key.time, legacy.keys[i].time, field + " key time");
        c.scalar(key.slope, legacy.keys[i].slope, field + " key slope");
        c.scalar(key.value, legacy.keys[i].value, field + " key value");
    }
}

void compare_material(Compare& c, Material native, const MLRState& legacy, int version,
    unsigned gfx_version = 17, bool effect = false, bool depth_off = false, bool default_priority = false) {
    using S = MLRState;
    auto& words = native.words;
    if (version <= 3) {
        words[0] &= ~(S::TextureWrapMask | S::SpecularMask);
        words[1] &= ~(S::TextureWrapMask | S::SpecularMask);
        words[2] |= S::TextureWrapMask | S::SpecularMask;
    }
    if (effect && gfx_version < 14) words[2] |= S::TextureMask;
    if (effect && (words[0] & S::AlphaMask) != S::OneZeroMode && (words[3] & S::PriorityMask) < S::AlphaPriority) {
        words[3] = (words[3] & ~S::PriorityMask) | S::AlphaPriority;
        words[4] |= S::PriorityMask;
    }
    if (default_priority) { words[3] = (words[3] & ~S::PriorityMask) | S::DefaultPriority; words[4] |= S::PriorityMask; }
    if (depth_off) { words[0] &= ~S::ZBufferWriteMask; words[1] |= S::ZBufferWriteMask; }
    std::array<uint32_t, 6> actual = {uint32_t(legacy.GetRenderStateFlags()), uint32_t(legacy.GetRenderDeltaMask()),
        uint32_t(legacy.GetRenderPermissionMask()), uint32_t(legacy.GetProcessStateFlags()),
        uint32_t(legacy.GetProcessDeltaMask()), uint32_t(legacy.GetProcessPermissionMask())};
    // Runtime pool handles replace the authored texture bits. Compare identities separately.
    words[0] &= ~S::TextureMask; actual[0] &= ~S::TextureMask;
    c.require(words == actual, "material state/masks");
    if (native.words[0] || !native.texture.empty()) {
        if (!native.texture.empty()) {
            auto* texture = (*MLRTexturePool::Instance)[legacy.GetTextureHandle()];
            c.require(texture && native.texture == static_cast<const char*>(texture->GetTextureName()), "texture name");
            c.require(native.texture_instance == uint32_t((texture->GetHint() << 4) | texture->GetTextureInstance()), "texture instance");
        }
    }
    ++c.materials;
}

template<class T> void compare_colors(Compare& c, const MeshPrimitive& native, MLRPrimitiveBase* legacy) {
    Stuff::RGBAColor* colors = nullptr; int count = 0;
    static_cast<T*>(legacy)->GetColorData(&colors, &count);
    c.require(size_t(count) == native.colors.size(), "mesh colors count");
    for (int i = 0; i < count; ++i) {
        const auto value = native.colors[i];
        c.require(colors[i].red == float((value >> 16) & 255) / 256 &&
            colors[i].green == float((value >> 8) & 255) / 256 && colors[i].blue == float(value & 255) / 256 &&
            colors[i].alpha == float(value >> 24) / 256, "mesh packed colors");
    }
}
template<class T> void compare_normals(Compare& c, const MeshPrimitive& native, MLRPrimitiveBase* legacy) {
    glm::vec3* normals = nullptr; int count = 0;
    static_cast<T*>(legacy)->GetNormalData(&normals, &count);
    c.require(size_t(count) == native.normals.size(), "mesh normal count");
    for (int i = 0; i < count; ++i) c.vector(native.normals[i], normals[i], "mesh normal");
}

void compare_shape(Compare& c, const MeshShape& native, MLRShape* legacy, int version) {
    c.require(legacy && size_t(legacy->GetNum()) == native.primitives.size(), "shape primitive count");
    for (size_t i = 0; i < native.primitives.size(); ++i) {
        const auto& mesh = native.primitives[i]; auto* old = legacy->Find(static_cast<int>(i));
        c.require(mesh.class_id == old->GetClassID(), "mesh class ordinal");
        c.require(mesh.draw_mode == uint32_t(old->GetSortDataMode()), "mesh draw mode");
        glm::vec3* positions = nullptr; int count = 0; old->GetCoordData(&positions, &count);
        c.require(size_t(count) == mesh.positions.size(), "mesh vertex count");
        Vec3 minimum{}, maximum{};
        for (int j = 0; j < count; ++j) {
            c.vector(mesh.positions[j], positions[j], "mesh position");
            for (size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = j ? std::min(minimum[axis], positions[j][axis]) : positions[j][axis];
                maximum[axis] = j ? std::max(maximum[axis], positions[j][axis]) : positions[j][axis];
            }
        }
        c.require(mesh.bounds.minimum == minimum && mesh.bounds.maximum == maximum && mesh.bounds.empty == (count == 0), "mesh bounds");
        ++c.bounds;
        unsigned short* indices = nullptr; static_cast<MLRIndexedPrimitiveBase*>(old)->GetIndexData(&indices, &count);
        c.require(size_t(count) == mesh.indices.size(), "mesh index count");
        for (int j = 0; j < count; ++j) c.require(indices[j] == mesh.indices[j], "mesh index");
        unsigned char* lengths = nullptr; old->GetSubprimitiveLengths(&lengths, &count);
        c.require(size_t(count) == mesh.lengths.size(), "mesh polygon count");
        for (int j = 0; j < count; ++j) c.require(lengths[j] == mesh.lengths[j], "mesh polygon length");
        Vector2DScalar* uv = nullptr; old->GetTexCoordData(&uv, &count);
        c.require(size_t(count) == mesh.texcoords.size(), "mesh UV count");
        for (int j = 0; j < count; ++j) c.require(uv[j].x == mesh.texcoords[j][0] && uv[j].y == mesh.texcoords[j][1], "mesh UV");
        const auto layout = mesh_layout(mesh.class_id);
        c.require(size_t(old->GetNumPasses()) == mesh.materials.size(), "mesh material count");
        for (size_t pass = 0; pass < mesh.materials.size(); ++pass)
            compare_material(c, mesh.materials[pass], old->GetReferenceState(static_cast<int>(pass)), version, 17, false,
                pass == 1 && (layout.dual || layout.detail), layout.dual || (layout.detail && !layout.triangles));
        if (layout.multi) {
            auto* multi = static_cast<MLR_I_MT_PMesh*>(old);
            for (size_t pass = 1; pass < mesh.materials.size(); ++pass) {
                const Vector2DScalar* values = nullptr;
                multi->GetTexCoordData(&values, &count, static_cast<int>(pass));
                const auto& expected = mesh.extra_texcoords[pass - 1];
                c.require(size_t(count) == expected.size(), "multipass UV count");
                for (int j = 0; j < count; ++j) c.require(values[j].x == expected[j][0] && values[j].y == expected[j][1], "multipass UV");
            }
        }
        if (layout.detail) {
            std::array<float, 6> data{};
            if (layout.triangles) static_cast<MLR_I_DeT_TMesh*>(old)->GetDetailData(data[0], data[1], data[2], data[3], data[4], data[5]);
            else static_cast<MLR_I_DeT_PMesh*>(old)->GetDetailData(data[0], data[1], data[2], data[3]);
            c.require(data == mesh.detail, "mesh detail parameters");
        }
        switch (mesh.class_id) {
        case 88: case 89: compare_colors<MLR_I_C_PMesh>(c, mesh, old); break;
        case 91: case 92: compare_colors<MLR_I_C_DT_PMesh>(c, mesh, old); break;
        case 97: case 98: compare_colors<MLR_I_C_DeT_PMesh>(c, mesh, old); break;
        case 101: case 102: compare_colors<MLR_I_C_TMesh>(c, mesh, old); break;
        case 108: case 109: compare_colors<MLR_I_C_DT_TMesh>(c, mesh, old); break;
        case 110: case 111: compare_colors<MLR_I_C_DeT_TMesh>(c, mesh, old); break;
        default: break;
        }
        switch (mesh.class_id) {
        case 89: compare_normals<MLR_I_L_PMesh>(c, mesh, old); break;
        case 92: compare_normals<MLR_I_L_DT_PMesh>(c, mesh, old); break;
        case 98: compare_normals<MLR_I_L_DeT_PMesh>(c, mesh, old); break;
        case 102: compare_normals<MLR_I_L_TMesh>(c, mesh, old); break;
        case 109: compare_normals<MLR_I_L_DT_TMesh>(c, mesh, old); break;
        case 111: compare_normals<MLR_I_L_DeT_TMesh>(c, mesh, old); break;
        default: break;
        }
        ++c.meshes;
    }
}

// Legal derived inspectors: invoke the real protected legacy constructors and
// expose their parsed shape/radius without aliasing a base object as a subclass.
struct ShapeOracle : gosFX::Shape__Specification {
    ShapeOracle(Stuff::MemoryStream* s, int v) : Shape__Specification(s, v) {}
    MLRShape* shape() const { return m_shape; }
    float radius() const { return m_radius; }
};
struct ShapeCloudOracle : gosFX::ShapeCloud__Specification {
    ShapeCloudOracle(Stuff::MemoryStream* s, int v) : ShapeCloud__Specification(s, v) {}
    MLRShape* shape() const { return m_shape; }
    float radius() const { return m_radius; }
};
struct OracleScope {
    bool triangles = ConvertToTriangleMeshes;
    OracleScope() { ConvertToTriangleMeshes = false; gos_PushCurrentHeap(gosFX::Heap); }
    ~OracleScope() { gos_PopCurrentHeap(); ConvertToTriangleMeshes = triangles; }
};

void compare_scalars(Compare& c, const EffectRecord& native, gosFX::Effect__Specification& spec) {
    const auto id = native.class_id - first_effect_class;
    if (id >= 1 && id <= 8) {
        auto& cloud = static_cast<gosFX::ParticleCloud__Specification&>(spec);
        c.require(native.population_limit == uint32_t(cloud.m_maxParticleCount), "maximum particle count");
        if (id >= 3) {
            auto& spin = static_cast<gosFX::SpinningCloud__Specification&>(spec);
            c.require(native.alignment == std::array<bool, 4>{spin.m_randomStartingRotation, spin.m_alignZUsingX,
                spin.m_alignZUsingY, spin.m_alignYUsingVelocity}, "spinning alignment");
        }
    }
    if (id >= 9 && id <= 11) {
        auto& singleton = static_cast<gosFX::Singleton__Specification&>(spec);
        c.require(native.alignment[0] == singleton.m_alignZUsingX && native.alignment[1] == singleton.m_alignZUsingY, "singleton alignment");
    }
    if (id == 5) c.require(native.vertex_count == static_cast<gosFX::PertCloud__Specification&>(spec).m_vertices, "PertCloud vertices");
    if (id == 6) c.require(native.animated == static_cast<gosFX::CardCloud__Specification&>(spec).m_animated, "card animation");
    if (id == 10) c.require(native.animated == static_cast<gosFX::Card__Specification&>(spec).m_animated, "card animation");
    if (id == 8) c.require(native.child_effect == static_cast<gosFX::EffectCloud__Specification&>(spec).m_particleEffectID, "child effect");
    if (id == 12) {
        auto& tube = static_cast<gosFX::Tube__Specification&>(spec);
        c.require(native.population_limit == uint32_t(tube.m_maxProfileCount) && native.profile_type == uint32_t(tube.m_profileType) &&
            native.inside_out == tube.m_insideOut, "tube profile fields");
    }
    if (id == 14) {
        auto& light = static_cast<gosFX::PointLight__Specification&>(spec);
        const char* light_map = light.m_lightMap;
        c.require(native.two_sided == light.m_twoSided && native.light_map == (light_map ? light_map : ""), "light fields");
    }
}
}

bool compareLegacy(const Catalog& catalog, const std::vector<unsigned char>& bytes, std::string& report) {
    Compare c;
    try {
        OracleScope scope;
        Stuff::MemoryStream stream(const_cast<unsigned char*>(bytes.data()), bytes.size());
        const int version = gosFX::ReadGFXVersion(&stream);
        unsigned count; stream >> count;
        c.require(count == catalog.effects().size() && unsigned(version) == catalog.version(), "catalog header");
        for (unsigned i = 0; i < count; ++i) {
            const auto& native = catalog.effects()[i];
            c.context = "effect " + std::to_string(i) + " " + native.name;
            std::unique_ptr<gosFX::Effect__Specification> spec;
            if (native.class_id == first_effect_class + 11 || native.class_id == first_effect_class + 7) {
                unsigned id; stream >> id;
                c.require(id == native.class_id, "shape class ordinal");
                if (id == first_effect_class + 11) spec = std::make_unique<ShapeOracle>(&stream, version);
                else spec = std::make_unique<ShapeCloudOracle>(&stream, version);
            } else spec.reset(gosFX::Effect__Specification::Create(&stream, version));
            c.require(spec && spec->GetClassID() == native.class_id, "class ordinal");
            const char* name = spec->m_name;
            c.require(native.name == (name ? name : ""), "name");
            c.require(native.events.size() == spec->m_events.size(), "event count");
            for (size_t j = 0; j < native.events.size(); ++j) {
                const auto& a = native.events[j]; const auto& b = *spec->m_events[j];
                c.require(a.time == b.m_time && a.flags == b.m_flags && a.effect_id == b.m_effectID, "event fields/order");
                for (size_t k = 0; k < 12; ++k) c.require(a.local_to_parent[k] == b.m_localToParent[k / 4][k % 4], "event transform");
                ++c.events;
            }
            const auto old_curves = legacy_curves(*spec);
            if (version >= 17) c.require(native.curves.size() == old_curves.size(), "curve count");
            for (const auto& curve : native.curves) {
                const auto it = std::find_if(old_curves.begin(), old_curves.end(), [&](const auto& old) { return old.name == curve.name; });
                c.require(it != old_curves.end(), "missing curve " + curve.name);
                // v9's UV curves are collapsed to constants by the legacy loader.
                if (version < 10 && (curve.name == "m_UOffset" || curve.name == "m_VOffset")) {
                    c.scalar(curve.evaluate(0, 0), it->evaluate(0, 0), curve.name);
                } else {
                    compare_data(c, curve.age, it->age, curve.name);
                    c.require(curve.seeded == it->seeded && curve.seed.has_value() == it->seed.has_value(), curve.name + " seeded");
                    if (curve.seed) compare_data(c, *curve.seed, *it->seed, curve.name + " seed");
                }
                ++c.curves;
            }
            compare_material(c, native.material, spec->m_state, native.state_version.value, version, true);
            compare_scalars(c, native, *spec);
            if (native.class_id == first_effect_class + 11) {
                auto& shape = static_cast<ShapeOracle&>(*spec);
                c.require(shape.radius() == native.radius, "shape radius");
                compare_shape(c, native.geometry[0].shape, shape.shape(), native.geometry_version.value); ++c.bounds;
            } else if (native.class_id == first_effect_class + 7) {
                auto& shape = static_cast<ShapeCloudOracle&>(*spec);
                c.require(shape.radius() == native.radius, "shape radius");
                compare_shape(c, native.geometry[0].shape, shape.shape(), native.geometry_version.value); ++c.bounds;
            } else if (native.class_id == first_effect_class + 13) {
                auto& debris = static_cast<gosFX::DebrisCloud__Specification&>(*spec);
                c.vector(native.center_of_force, debris.centerOfForce, "center of force");
                c.require(native.geometry.size() == debris.debrisPieces.size(), "debris piece count");
                for (size_t j = 0; j < native.geometry.size(); ++j) {
                    const auto& piece = native.geometry[j];
                    c.vector(piece.position, debris.debrisPositions[j], "debris position");
                    c.vector(piece.sphere_center, debris.debrisSpheres[j].center, "debris sphere center");
                    c.require(piece.sphere_radius == debris.debrisSpheres[j].radius, "debris sphere radius"); ++c.bounds;
                    compare_shape(c, piece.shape, debris.debrisPieces[j], native.geometry_version.value);
                }
            }
        }
        c.require(stream.GetBytesRemaining() == 0, "legacy trailing bytes");
        report = "PASS effects=" + std::to_string(count) + " events=" + std::to_string(c.events) +
            " curves=" + std::to_string(c.curves) + " materials=" + std::to_string(c.materials) +
            " meshes=" + std::to_string(c.meshes) + " bounds=" + std::to_string(c.bounds);
        return true;
    } catch (const std::exception& e) { report = "FAIL " + std::string(e.what()); return false; }
}
}
