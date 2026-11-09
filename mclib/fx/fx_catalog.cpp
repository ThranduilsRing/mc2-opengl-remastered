#include "fx_catalog.h"

#include <algorithm>
#include <utility>

namespace mc2::fx {
namespace {
using K = CurveKind;

// Wire kinds are implicit in the versioned specification class, never curve tags.
void read_derived_curves(Reader& r, EffectRecord& effect, unsigned version) {
    const auto ordinal = effect.class_id - first_effect_class;
    const auto curve = [&](std::string_view name, K age, std::optional<K> seed = {}) {
        effect.curves.push_back(read_curve(r, name, age, seed, version));
    };
    if (ordinal >= 1 && ordinal <= 8) {
        curve("m_startingPopulation", K::constant); curve("m_particlesPerSecond", K::complex);
        for (const char* name : {"m_emitterSizeX", "m_emitterSizeY", "m_emitterSizeZ"}) curve(name, K::linear);
        curve("m_minimumDeviation", K::constant); curve("m_maximumDeviation", K::spline);
        curve("m_startingSpeed", K::complex, K::complex); curve("m_pLifeSpan", K::complex, K::spline);
        for (const char* name : {"m_pEtherVelocityX", "m_pEtherVelocityY", "m_pEtherVelocityZ"})
            curve(name, K::constant, K::linear);
        for (const char* name : {"m_pAccelerationX", "m_pAccelerationY", "m_pAccelerationZ"})
            curve(name, K::spline, K::linear);
        curve("m_pDrag", K::complex, K::complex);
        for (const char* name : {"m_pRed", "m_pGreen", "m_pBlue", "m_pAlpha"}) curve(name, K::complex, K::linear);
        effect.population_limit = r.integer();
        if (effect.population_limit > 1048576) r.fail("invalid maximum particle count");
        if (ordinal >= 3) {
            curve("m_pSpin", K::constant, K::linear); curve("m_pScale", K::complex, K::complex);
            for (size_t i = 0; i < (version >= 10 ? 4u : 3u); ++i) effect.alignment[i] = r.boolean();
        }
    }
    if (ordinal >= 9 && ordinal <= 11) {
        for (const char* name : {"m_red", "m_green", "m_blue", "m_alpha"}) curve(name, K::complex, K::linear);
        curve("m_scale", K::complex, K::complex);
        effect.alignment[0] = r.boolean(); effect.alignment[1] = r.boolean();
    }
    switch (ordinal) {
    case 4: curve("m_size", K::complex, K::spline); curve("m_angularity", K::constant, K::spline); break;
    case 5:
        curve("m_size", K::complex, K::spline); curve("m_perturbation", K::constant, K::spline);
        for (const char* name : {"m_pCenterRed", "m_pCenterGreen", "m_pCenterBlue", "m_pCenterAlpha"})
            curve(name, K::complex, K::linear);
        effect.vertex_count = r.integer();
        if (effect.vertex_count < 3 || effect.vertex_count > 65536) r.fail("invalid PertCloud vertex count");
        break;
    case 6: case 10:
        curve("m_halfHeight", ordinal == 6 ? K::complex : K::constant, K::complex);
        curve("m_aspectRatio", ordinal == 6 ? K::complex : K::constant, K::complex);
        if (version < 10) {
            curve("m_UOffset", K::complex, K::linear); curve("m_VOffset", K::complex, K::linear);
        } else {
            curve(ordinal == 6 ? "m_pIndex" : "m_index", K::complex, K::spline);
            curve("m_UOffset", K::constant); curve("m_VOffset", K::constant);
        }
        curve("m_USize", K::constant); curve("m_VSize", K::constant);
        if (version >= 10) effect.animated = r.boolean();
        break;
    case 8: effect.child_effect = r.integer(); break;
    case 12:
        curve("m_profilesPerSecond", K::constant); curve("m_pLifeSpan", K::constant);
        for (const char* name : {"m_emitterSizeX", "m_emitterSizeY", "m_emitterSizeZ"}) curve(name, K::linear);
        curve("m_minimumDeviation", K::constant); curve("m_maximumDeviation", K::complex);
        curve("m_pDisplacement", K::complex, K::spline);
        if (version >= 12) curve("m_pScale", K::complex, K::spline);
        for (const char* name : {"m_pRed", "m_pGreen", "m_pBlue", "m_pAlpha"}) curve(name, K::complex, K::linear);
        curve("m_pUOffset", K::complex, K::spline); curve("m_pVOffset", K::complex, K::spline);
        curve("m_pUSize", K::spline, K::spline); curve("m_pVSize", K::spline, K::spline);
        effect.population_limit = r.integer(); effect.profile_type = r.integer(); effect.inside_out = r.boolean();
        if (effect.population_limit > 1048576 || effect.profile_type > 7) r.fail("invalid tube profile");
        break;
    case 13:
        curve("m_minimumDeviation", K::constant); curve("m_maximumDeviation", K::spline);
        curve("m_startingSpeed", K::complex, K::complex); curve("m_pLifeSpan", K::complex, K::spline);
        curve("m_pEtherVelocityY", K::constant, K::linear); curve("m_pAccelerationY", K::spline, K::linear);
        curve("m_pDrag", K::complex, K::complex); curve("m_pAlpha", K::complex, K::linear);
        curve("m_pSpin", K::constant, K::linear);
        effect.center_of_force = r.scalars<3>();
        break;
    case 14:
        for (const char* name : {"m_red", "m_green", "m_blue", "m_intensity"}) curve(name, K::complex);
        if (version > 15) { curve("m_innerRadius", K::spline); curve("m_outerRadius", K::spline); }
        if (version > 16) effect.light_map = r.string();
        effect.two_sided = r.boolean();
        break;
    default: break;
    }
}

EffectRecord read_effect(Reader& r, unsigned version) {
    EffectRecord effect;
    effect.class_id = r.integer();
    if (effect.class_id < first_effect_class || effect.class_id > first_effect_class + 14)
        r.fail("unsupported effect class ordinal " + std::to_string(effect.class_id));
    if (effect.class_id == first_effect_class + 1 || effect.class_id == first_effect_class + 3 ||
        effect.class_id == first_effect_class + 9) r.fail("abstract effect class has no wire factory");
    effect.name = r.string();
    const auto events = r.count(60);
    effect.events.reserve(events);
    for (uint32_t i = 0; i < events; ++i) {
        Event event;
        event.time = r.scalar(); event.flags = r.integer(); event.effect_id = r.integer();
        event.local_to_parent = r.scalars<12>();
        if (event.time < 0 || event.time > 1) r.fail("event time outside [0,1]");
        effect.events.push_back(event);
    }
    effect.curves.push_back(read_curve(r, "m_lifeSpan", K::constant, {}, version));
    effect.curves.push_back(read_curve(r, "m_minimumChildSeed", K::spline, {}, version));
    effect.curves.push_back(read_curve(r, "m_maximumChildSeed", K::spline, {}, version));
    effect.state_version = read_mlr_version(r);
    effect.material = read_material(r, effect.state_version.value);
    read_derived_curves(r, effect, version);
    const auto ordinal = effect.class_id - first_effect_class;
    if (ordinal == 7 || ordinal == 11 || ordinal == 13) {
        effect.geometry_version = read_mlr_version(r);
        const auto pieces = ordinal == 13 ? r.count(32, 65536) : 1;
        effect.geometry.reserve(pieces);
        for (uint32_t i = 0; i < pieces; ++i) {
            GeometryPiece piece;
            if (ordinal == 13) {
                piece.position = r.scalars<3>(); piece.sphere_center = r.scalars<3>(); piece.sphere_radius = r.scalar();
                if (piece.sphere_radius < 0) r.fail("negative debris sphere radius");
            }
            piece.shape = read_shape(r, effect.geometry_version.value);
            effect.geometry.push_back(std::move(piece));
        }
        if (ordinal != 13) { effect.radius = r.scalar(); if (effect.radius < 0) r.fail("negative shape radius"); }
    }
    return effect;
}

void write_effect(Writer& w, const EffectRecord& effect, unsigned version) {
    w.integer(effect.class_id); w.string(effect.name);
    w.integer(static_cast<uint32_t>(effect.events.size()));
    for (const auto& event : effect.events) {
        w.scalar(event.time); w.integer(event.flags); w.integer(event.effect_id); w.scalars(event.local_to_parent);
    }
    size_t curve_index = 0;
    const auto curves = [&](size_t count) {
        for (size_t i = 0; i < count; ++i) write_curve(w, effect.curves.at(curve_index++), version);
    };
    curves(3);
    write_mlr_version(w, effect.state_version); write_material(w, effect.material, effect.state_version.value);
    const auto ordinal = effect.class_id - first_effect_class;
    if (ordinal >= 1 && ordinal <= 8) {
        curves(20); w.integer(effect.population_limit);
        if (ordinal >= 3) {
            curves(2);
            for (size_t i = 0; i < (version >= 10 ? 4u : 3u); ++i) w.boolean(effect.alignment[i]);
        }
    }
    if (ordinal >= 9 && ordinal <= 11) {
        curves(5); w.boolean(effect.alignment[0]); w.boolean(effect.alignment[1]);
    }
    curves(effect.curves.size() - curve_index);
    switch (ordinal) {
    case 5: w.integer(effect.vertex_count); break;
    case 6: case 10: if (version >= 10) w.boolean(effect.animated); break;
    case 8: w.integer(effect.child_effect); break;
    case 12: w.integer(effect.population_limit); w.integer(effect.profile_type); w.boolean(effect.inside_out); break;
    case 13: w.scalars(effect.center_of_force); break;
    case 14: if (version > 16) w.string(effect.light_map); w.boolean(effect.two_sided); break;
    default: break;
    }
    if (ordinal == 7 || ordinal == 11 || ordinal == 13) {
        write_mlr_version(w, effect.geometry_version);
        if (ordinal == 13) w.integer(static_cast<uint32_t>(effect.geometry.size()));
        for (const auto& piece : effect.geometry) {
            if (ordinal == 13) { w.scalars(piece.position); w.scalars(piece.sphere_center); w.scalar(piece.sphere_radius); }
            write_shape(w, piece.shape, effect.geometry_version.value);
        }
        if (ordinal != 13) w.scalar(effect.radius);
    }
}
}

Catalog::Catalog(unsigned version, std::vector<EffectRecord> effects)
    : version_(version), effects_(std::move(effects)) {}

const Curve* EffectRecord::find_curve(std::string_view name) const {
    const auto it = std::find_if(curves.begin(), curves.end(), [&](const auto& curve) { return curve.name == name; });
    return it == curves.end() ? nullptr : &*it;
}
const EffectRecord* Catalog::find(std::string_view name) const {
    const auto it = std::find_if(effects_.begin(), effects_.end(), [&](const auto& effect) { return effect.name == name; });
    return it == effects_.end() ? nullptr : &*it;
}

std::vector<uint8_t> Catalog::rebuild() const {
    Writer w;
    w.integer(gfx_signature); w.integer(version_); w.integer(static_cast<uint32_t>(effects_.size()));
    for (const auto& effect : effects_) write_effect(w, effect, version_);
    return std::move(w.bytes);
}

std::optional<Catalog> read_catalog(std::span<const uint8_t> bytes, std::string& error) {
    error.clear();
    try {
        Reader r(bytes);
        if (r.integer() != gfx_signature) r.fail("invalid GFX signature");
        const auto version = r.integer();
        if (version < 9 || version > 17) r.fail("unsupported GFX version " + std::to_string(version));
        const auto count = r.count(68, 65536);
        std::vector<EffectRecord> effects;
        effects.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            try { effects.push_back(read_effect(r, version)); }
            catch (const FormatError& e) { throw FormatError(r.offset(), "effect " + std::to_string(i) + ": " + e.what()); }
        }
        if (r.remaining()) r.fail("trailing catalog bytes");
        for (const auto& effect : effects) {
            for (const auto& event : effect.events)
                if (event.effect_id >= count) r.fail("event references missing effect");
            if (effect.class_id == first_effect_class + 8 && effect.child_effect >= count && effect.child_effect != UINT32_MAX)
                r.fail("particle references missing effect");
        }
        return Catalog(version, std::move(effects));
    } catch (const FormatError& e) { error = e.what(); return std::nullopt; }
}
}
