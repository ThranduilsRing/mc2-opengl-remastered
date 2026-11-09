#include "fx_tubes.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace mc2::fx {
namespace {
std::pair<float, float> range(const CurveData& curve) {
    float low = curve.evaluate(0), high = low;
    const auto include = [&](float value) { low = std::min(low, value); high = std::max(high, value); };
    if (curve.kind == CurveKind::complex) {
        low = high = curve.keys.empty() ? 0 : curve.effective_key(curve.keys.size() - 1).value;
        for (size_t i = 0; i < curve.keys.size(); ++i) {
            const auto key = curve.effective_key(i);
            const float dt = i + 1 == curve.keys.size() ? std::max(1.0f - key.time, 0.0f) : curve.keys[i + 1].time - key.time;
            include(key.value);
            if (dt >= math::SmallValue) include(key.slope * dt + key.value);
        }
    } else {
        include(curve.evaluate(1));
    }
    if (curve.kind == CurveKind::spline) {
        const float slope = curve.coefficients[1], b = curve.coefficients[2], a = curve.coefficients[3];
        const auto test = [&](float t) { if (t > 0 && t < 1) include(curve.evaluate(t)); };
        if (a == 0) { if (b != 0) test(-0.5f * slope / b); }
        else {
            float da = 3 * a, db = 2 * b, r = db * db - 4 * da * slope;
            if (r >= 0) {
                da = 0.5f / da; db = -db * da;
                r = static_cast<float>(std::sqrt(static_cast<double>(r))) * da;
                test(db - r); test(db + r);
            }
        }
    }
    return {low, high};
}
std::pair<float, float> range(const Curve& curve) {
    auto [low, high] = range(curve.age);
    // Legacy range analysis includes the seed curve even when evaluation is unseeded.
    if (!curve.seed) return {low, high};
    const auto [slo, shi] = range(*curve.seed);
    return {std::min({low * slo, low * shi, high * slo, high * shi}),
        std::max({low * slo, low * shi, high * slo, high * shi})};
}
}
void Instance::initialize_tube() {
    auto [min_scale, max_scale] = range(*record_.find_curve("m_pUSize"));
    const auto [min_offset, max_offset] = range(*record_.find_curve("m_pUOffset"));
    const float last = static_cast<float>(record_.population_limit) - 1;
    float lower = std::min(min_scale * last, 0.0f) + min_offset;
    float upper = max_scale * last + max_offset;
    // Specification loading repairs an excessive U span before playback.
    // Keep that repair per instance; the serialized L1 record stays immutable.
    if (upper - lower >= 198.0f) {
        tube_u_size_ = static_cast<float>(std::floor((198.0f - (max_offset - min_offset)) / last));
        auto repaired = *record_.find_curve("m_pUSize");
        repaired.age = {}; repaired.age.coefficients[0] = *tube_u_size_;
        std::tie(min_scale, max_scale) = range(repaired);
        lower = std::min(min_scale * last, 0.0f) + min_offset;
        upper = max_scale * last + max_offset;
        if (upper - lower >= 198.0f) throw std::invalid_argument("FX unrepairable tube U span");
    }
    lower += 99;
    if (lower < 0) { tube_u_bias_ = static_cast<float>(std::floor(0.5f - lower)); return; }
    upper -= 99;
    if (upper > 0) tube_u_bias_ = static_cast<float>(std::floor(0.5f - upper));
}
std::vector<glm::vec3> tube_template(uint32_t type) {
    switch (type) {
    case 0: case 1: return {{1, 0, 0}, {-1, 0, 0}};
    case 2: return {{0, 0, 1}, {-0.8660254f, 0, -0.5f}, {0.8660254f, 0, -0.5f}, {0, 0, 1}};
    case 3: return {{0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}, {0, 0, 1}};
    case 4: return {{0, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}};
    case 5: return {{0, 0, 1}, {-0.9510565f, 0, 0.309017f}, {-0.58778525f, 0, -0.8090167f},
        {0.58778525f, 0, -0.8090167f}, {0.9510565f, 0, 0.309017f}, {0, 0, 1}};
    case 6: return {{0, 0, 1}, {-0.8660254f, 0, 0.5f}, {-0.8660254f, 0, -0.5f},
        {0, 0, -1}, {0.8660254f, 0, -0.5f}, {0.8660254f, 0, 0.5f}, {0, 0, 1}};
    case 7: return {{0, 0, -1}, {0, 0, 1}};
    default: throw std::invalid_argument("FX tube profile type");
    }
}
std::vector<uint16_t> tube_indices(uint32_t type, bool inside, size_t profiles) {
    std::vector<uint16_t> out;
    const auto count = tube_template(type).size();
    for (size_t i = 0; i + 1 < profiles; ++i) {
        const auto base = i * count;
        for (size_t panel = 0; panel + 1 < count; ++panel) {
            const auto a = type == 4 ? base : base + panel;
            const auto b = base + panel + 1, c = b + count, d = a + count;
            for (const auto index : inside ? std::array<size_t, 6>{b, a, c, d, c, a}
                                          : std::array<size_t, 6>{a, b, c, c, d, a})
                out.push_back(static_cast<uint16_t>(index));
        }
    }
    return out;
}
void Instance::start_tube() { births_ = 1; }
void Instance::create_profile(size_t index, const glm::mat4& origin) {
    auto& p = profiles_[index];
    p.age = 0; p.seed = child_seed();
    p.age_rate = 1.0f / std::max(curve("m_pLifeSpan", age_, p.seed), 0.0333333f);
    p.profile_to_world = origin;
    const float minimum = curve("m_minimumDeviation", age_, p.seed);
    const float deviation = std::max(curve("m_maximumDeviation", age_, p.seed) - minimum, 0.0f);
    const float angle = minimum + random_() * deviation;
    const float sine = glm::sin(angle), cosine = glm::cos(angle);
    p.direction = {random_() >= 0.5f ? sine : -sine, 0, cosine};
}
bool Instance::animate_profile(size_t index, size_t compact, const glm::mat4& inverse, math::Sphere& bounds) {
    const auto& p = profiles_[index];
    if (p.age >= 1) return false;
    const float scale = curve("m_pScale", p.age, p.seed);
    const float displacement = curve("m_pDisplacement", p.age, p.seed);
    glm::mat4 matrix{1.0f};
    matrix[0][0] = matrix[1][1] = matrix[2][2] = scale;
    math::buildTranslation(matrix, p.direction * displacement);
    matrix = matrix * p.profile_to_world;
    matrix = matrix * inverse;
    bounds.center = math::getTranslation(matrix); bounds.radius = scale;
    const auto vertices = tube_template(record_.profile_type);
    float u = curve("m_pUOffset", p.age, p.seed), v = curve("m_pVOffset", p.age, p.seed);
    const float du = tube_u_size_ ? *tube_u_size_ : curve("m_pUSize", p.age, p.seed);
    const float dv = curve("m_pVSize", p.age, p.seed);
    u += du * compact + tube_u_bias_;
    const glm::vec4 color{curve("m_pRed", p.age, p.seed), curve("m_pGreen", p.age, p.seed),
        curve("m_pBlue", p.age, p.seed), curve("m_pAlpha", p.age, p.seed)};
    for (size_t i = 0; i < vertices.size(); ++i) {
        const size_t dest = compact * vertices.size() + i;
        if (record_.profile_type != 1) tube_vertices_[dest] = math::transformPoint(vertices[i], matrix);
        const float template_v = record_.profile_type == 4 ? (i ? 1.0f : 0.0f) :
            static_cast<float>(i) / static_cast<float>(vertices.size() - 1);
        tube_uvs_[dest] = {u, template_v * dv + v}; tube_colors_[dest] = color;
    }
    return true;
}
void Instance::execute_tube(const Update& info) {
    const auto world = local_to_parent * info.parent_to_world;
    const auto inverse = glm::inverse(world);
    const float dt = static_cast<float>(info.time - last_ran_), previous = age_;
    age_ += dt * age_rate_;
    if (age_ >= 1) births_ = 0;
    else {
        births_ += dt * std::max(curve("m_profilesPerSecond", age_, seed_), 0.0f);
        if (births_ >= 1 && active_ < profiles_.size()) {
            if (!active_) {
                if (profiles_.size() < 2) throw std::invalid_argument("FX tube requires two profiles");
                head_ = 1; tail_ = profiles_.size() - 1;
                create_profile(0, world); active_ = 2;
            } else {
                profiles_[head_].profile_to_world = world;
                head_ = (head_ + 1) % profiles_.size();
            }
            create_profile(head_, world);
            births_ -= static_cast<float>(std::floor(births_));
        } else if (head_ < profiles_.size()) profiles_[head_].profile_to_world = world;
    }
    if (!active_) { age_ = previous; base_execute(info); return; }
    math::Aabb box;
    size_t index = head_, compact = 0;
    do {
        auto& p = profiles_[index];
        p.age += dt * p.age_rate;
        math::Sphere sphere;
        if (!animate_profile(index, compact, inverse, sphere)) { tail_ = index; break; }
        const math::Aabb part{sphere.center - glm::vec3(sphere.radius), sphere.center + glm::vec3(sphere.radius)};
        box = compact ? math::unite(box, part) : part;
        ++compact;
        index = index ? index - 1 : profiles_.size() - 1;
    } while (index != tail_);
    active_ = compact;
    age_ = previous;
    if (base_execute(info)) include_box(box, false);
}
}
