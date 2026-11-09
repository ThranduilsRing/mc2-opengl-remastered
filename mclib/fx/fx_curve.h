#pragma once

#include "fx_format.h"
#include <optional>

namespace mc2::fx {

enum class CurveKind { constant, linear, spline, complex };
struct CurveKey {
    float time = 0, slope = 0, value = 0;
};
struct CurveData {
    CurveKind kind = CurveKind::constant;
    // Polynomial coefficients, ascending powers: value, slope, b, a.
    std::array<float, 4> coefficients{};
    std::vector<CurveKey> keys;
    float legacy_duration = 0;
    CurveKey effective_key(size_t index) const;
    float evaluate(float age) const;
};
struct Curve {
    std::string name;
    CurveData age;
    std::optional<CurveData> seed;
    bool seeded = false;
    float evaluate(float age_value, float seed_value) const;
};
CurveData read_curve_data(Reader& reader, CurveKind kind, unsigned version);
void write_curve_data(Writer& writer, const CurveData& curve, unsigned version);
Curve read_curve(Reader& reader, std::string_view name, CurveKind age,
    std::optional<CurveKind> seed, unsigned version);
void write_curve(Writer& writer, const Curve& curve, unsigned version);
}
