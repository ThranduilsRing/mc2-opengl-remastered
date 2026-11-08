#include "fx_curve.h"
#include <algorithm>

namespace mc2::fx {
CurveData read_curve_data(Reader& reader, CurveKind kind, unsigned version) {
    CurveData result;
    result.kind = kind;
    if (kind != CurveKind::complex) {
        const auto count = kind == CurveKind::constant ? 1 : kind == CurveKind::linear ? 2 : 4;
        for (int i = 0; i < count; ++i) result.coefficients[i] = reader.scalar();
    } else {
        if (version < 15) result.legacy_duration = reader.scalar();
        const auto count = reader.count(12);
        if (!count) reader.fail("empty complex curve");
        result.keys.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            CurveKey key{reader.scalar(), reader.scalar(), reader.scalar()};
            if (!result.keys.empty() && key.time < result.keys.back().time)
                reader.fail("unordered curve keys");
            result.keys.push_back(key);
        }
    }
    return result;
}

void write_curve_data(Writer& writer, const CurveData& curve, unsigned version) {
    if (curve.kind != CurveKind::complex) {
        const auto count = curve.kind == CurveKind::constant ? 1 : curve.kind == CurveKind::linear ? 2 : 4;
        for (int i = 0; i < count; ++i) writer.scalar(curve.coefficients[i]);
    } else {
        if (version < 15) writer.scalar(curve.legacy_duration);
        writer.integer(static_cast<uint32_t>(curve.keys.size()));
        for (const auto& key : curve.keys) {
            writer.scalar(key.time); writer.scalar(key.slope); writer.scalar(key.value);
        }
    }
}

Curve read_curve(Reader& reader, std::string_view name, CurveKind age,
    std::optional<CurveKind> seed, unsigned version) {
    Curve curve;
    curve.name = name;
    curve.age = read_curve_data(reader, age, version);
    if (seed) { curve.seed = read_curve_data(reader, *seed, version); curve.seeded = reader.boolean(); }
    return curve;
}

void write_curve(Writer& writer, const Curve& curve, unsigned version) {
    write_curve_data(writer, curve.age, version);
    if (curve.seed) { write_curve_data(writer, *curve.seed, version); writer.boolean(curve.seeded); }
}

CurveKey CurveData::effective_key(size_t index) const {
    auto key = keys.at(index);
    if (index + 1 == keys.size()) {
        // gosFX repairs a negative tail during Load, even for v17. Keep authored
        // coefficients for byte-exact rebuild, apply the repair only for playback.
        const float dt = std::max(1.0f - key.time, 0.0f);
        const float low = dt < 1e-4f ? key.value : std::min(key.value, key.slope * dt + key.value);
        if (low < 0 && dt > 0) key.slope = -key.value / dt;
    }
    return key;
}

float CurveData::evaluate(float age) const {
    if (kind == CurveKind::complex) {
        if (keys.empty()) return 0;
        const auto next = std::upper_bound(keys.begin(), keys.end(), age,
            [](float value, const CurveKey& key) { return value < key.time; });
        const auto index = next == keys.begin() ? 0 : static_cast<size_t>(std::prev(next) - keys.begin());
        const auto key = effective_key(index);
        return key.slope * (age - key.time) + key.value;
    }
    return ((coefficients[3] * age + coefficients[2]) * age + coefficients[1]) * age + coefficients[0];
}

float Curve::evaluate(float age_value, float seed_value) const {
    const float value = age.evaluate(age_value);
    return seeded && seed ? value * seed->evaluate(seed_value) : value;
}
}
