#include "fx_runtime.h"
#include "fx_emitters.h"
#include "fx_tubes.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace mc2::fx {
glm::mat4 event_transform(const Event& event) {
    glm::mat4 matrix{1.0f};
    std::memcpy(glm::value_ptr(matrix), event.local_to_parent.data(), 12 * sizeof(float));
    return matrix;
}
Instance::Instance(std::span<const EffectRecord> records, size_t id,
    RandomFraction& random, unsigned flags)
    : records_(records), record_(id < records.size() ? records[id] : throw std::out_of_range("FX effect ID")),
      random_(random), flags_(flags) {
    const auto type = ordinal();
    if (type > 14 || type == 1 || type == 3 || type == 9)
        throw std::invalid_argument("FX abstract/unknown playback class");
    if (type >= 2 && type <= 8) particles_.resize(record_.population_limit);
    if (type == 13) particles_.resize(record_.geometry.size());
    if (type == 12) {
        profiles_.resize(record_.population_limit);
        const auto count = tube_template(record_.profile_type).size() * record_.population_limit;
        tube_vertices_.resize(count); tube_uvs_.resize(count); tube_colors_.resize(count);
        initialize_tube();
    }
    if (type == 11) singleton_.radius = record_.radius;
}
Instance::~Instance() = default;
float Instance::curve(std::string_view name, float age, float seed) const {
    const auto* value = record_.find_curve(name);
    if (!value) throw std::invalid_argument("FX missing curve: " + std::string(name));
    return value->evaluate(age, seed);
}
void Instance::set_looped(bool looped) {
    if (looped) flags_ |= loop_flag;
    else flags_ &= ~loop_flag;
}
float Instance::child_seed() {
    const float minimum = curve("m_minimumChildSeed", age_, seed_);
    const float range = curve("m_maximumChildSeed", age_, seed_) - minimum;
    return std::clamp(random_() * range + minimum, 0.0f, 1.0f);
}
void Instance::start(const Update& info) {
    if (!is_executed() || last_ran_ == -1) last_ran_ = info.time;
    flags_ |= execute_flag;
    seed_ = info.seed == -1 ? random_() : info.seed;
    if (info.age == -1) {
        const float lifetime = std::max(curve("m_lifeSpan", seed_, 0), 0.033333f);
        age_rate_ = 1.0f / lifetime;
        age_ = 0;
    } else { age_ = info.age; age_rate_ = info.age_rate; }
    local_to_world_ = local_to_parent * info.parent_to_world;
    event_ = 0;
    const auto type = ordinal();
    if (type >= 2 && type <= 8) start_emitters();
    if (type == 10) {
        singleton_.half_y = curve("m_halfHeight", age_, seed_);
        singleton_.half_x = singleton_.half_y * curve("m_aspectRatio", age_, seed_);
        singleton_.radius = static_cast<float>(std::sqrt(static_cast<double>(
            singleton_.half_x * singleton_.half_x + singleton_.half_y * singleton_.half_y)));
    }
    if (type == 12) start_tube();
    if (type == 13) start_debris();
    if (type == 14) light_.active = true;
}
bool Instance::base_execute(const Update& info) {
    if (info.seed != -1) seed_ = info.seed;
    const float new_age = age_ + static_cast<float>(info.time - last_ran_) * age_rate_;
    bounds_ = {};
    local_to_world_ = local_to_parent * info.parent_to_world;
    // Events test the OLD age. Equal-time events retain catalog order.
    while (event_ < record_.events.size() && record_.events[event_].time <= age_) {
        const auto& event = record_.events[event_++];
        unsigned mode = event.flags & simulation_mask;
        if (mode == parent_space) mode = flags_ & simulation_mask;
        auto child = std::make_unique<Instance>(records_, event.effect_id, random_, execute_flag | mode);
        child->local_to_parent = event_transform(event);
        Update child_info{info.time, local_to_world_, child_seed()};
        child->start(child_info);
        children_.push_back(std::move(child));
    }
    Update child_info{info.time, local_to_world_};
    for (auto& child : children_) {
        const bool alive = child->execute(child_info);
        const auto parent_bounds = math::multiply(child->bounds(), local_to_parent);
        bounds_ = math::unite(bounds_, parent_bounds);
        if (!alive) child.reset();
    }
    std::erase(children_, nullptr);
    last_ran_ = info.time;
    age_ = new_age;
    if (age_ >= 1) {
        if (flags_ & loop_flag) start(info);
        else if (has_finished()) kill();
    }
    return is_executed();
}
bool Instance::execute(const Update& info) {
    if (!is_executed()) return false;
    if (ordinal() >= 2 && ordinal() <= 8) execute_emitters(info);
    if (ordinal() == 12) { execute_tube(info); return is_executed(); }
    if (ordinal() == 13) execute_debris(info);
    if (!base_execute(info)) return false;
    if (ordinal() >= 2 && ordinal() <= 8) emitter_bounds();
    if (ordinal() == 13) emitter_bounds();
    if (ordinal() == 10 || ordinal() == 11) singleton_execute();
    if (ordinal() == 14) {
        light_.color = {curve("m_red", age_, seed_), curve("m_green", age_, seed_), curve("m_blue", age_, seed_)};
        light_.origin = local_to_world_;
        light_.intensity = curve("m_intensity", age_, seed_);
        light_.inner = record_.find_curve("m_innerRadius") ? curve("m_innerRadius", age_, seed_) : 0;
        light_.outer = record_.find_curve("m_outerRadius") ? curve("m_outerRadius", age_, seed_) : 25;
    }
    return true;
}
void Instance::stop() { age_ = 1; event_ = record_.events.size(); }
void Instance::kill() {
    if ((ordinal() >= 2 && ordinal() <= 8) || ordinal() == 13) {
        for (size_t i = 0; i < active_; ++i) destroy_particle(i);
        active_ = 0; births_ = 0;
    }
    if (ordinal() == 12) { active_ = 0; births_ = 0; }
    for (auto& child : children_) child->kill();
    flags_ &= ~execute_flag;
    light_.active = false;
}
bool Instance::has_finished() const {
    if (is_executed() && age_ < 1) return false;
    if (!children_.empty()) return false;
    if ((ordinal() >= 2 && ordinal() <= 8) || ordinal() == 12) return active_ == 0;
    return true;
}
void Instance::include_box(const math::Aabb& box, bool minimum_radius) {
    math::Obb local_bounds;
    local_bounds.axisExtents = 0.5f * (box.max - box.min);
    math::buildTranslation(local_bounds.localToParent, box.min + local_bounds.axisExtents);
    local_bounds.sphereRadius = glm::length(local_bounds.axisExtents);
    if (minimum_radius && local_bounds.sphereRadius < math::SmallValue) local_bounds.sphereRadius = 0.01f;
    bounds_ = math::unite(bounds_, math::multiply(local_bounds, local_to_parent));
}
void Instance::singleton_execute() {
    auto& p = singleton_;
    p.color = {curve("m_red", age_, seed_), curve("m_green", age_, seed_),
        curve("m_blue", age_, seed_), curve("m_alpha", age_, seed_)};
    p.scale = curve("m_scale", age_, seed_);
    math::Obb own;
    own.sphereRadius = p.radius * p.scale;
    if (own.sphereRadius < math::SmallValue) own.sphereRadius = 0.01f;
    own.localToParent = local_to_parent;
    bounds_ = math::unite(bounds_, own);
    if (ordinal() == 10) {
        p.uvs = card_uvs(record_, age_, seed_, "m_index");
        p.vertices = {{p.scale * p.half_x, -p.scale * p.half_y, 0},
            {-p.scale * p.half_x, -p.scale * p.half_y, 0},
            {-p.scale * p.half_x, p.scale * p.half_y, 0},
            {p.scale * p.half_x, p.scale * p.half_y, 0}};
    }
}
}
