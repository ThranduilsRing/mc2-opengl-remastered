#include "fx_emitters.h"
#include <algorithm>
#include <cmath>

namespace mc2::fx {
namespace {
float root(float value) { return static_cast<float>(std::sqrt(static_cast<double>(value))); }
glm::vec3 vec(const Vec3& v) { return {v[0], v[1], v[2]}; }
}
std::array<glm::vec2, 4> card_uvs(const EffectRecord& record, float age, float seed,
    std::string_view index_curve) {
    const auto value = [&](std::string_view name, float a, float s) { return record.find_curve(name)->evaluate(a, s); };
    float u = value("m_UOffset", age, seed), v = value("m_VOffset", age, seed);
    float du = value("m_USize", age, seed), dv = value("m_VSize", age, seed);
    if (record.animated) {
        const auto width = static_cast<uint8_t>(1.0f / value("m_USize", 0, 0));
        if (!width) throw std::invalid_argument("FX zero card atlas width");
        auto column = static_cast<uint8_t>(value(index_curve, age, seed));
        const auto row = static_cast<uint8_t>(column / width);
        column = static_cast<uint8_t>(column - row * width);
        u += du * column; v += dv * row;
    }
    du += u; dv += v;
    return {glm::vec2(u, dv), glm::vec2(du, dv), glm::vec2(du, v), glm::vec2(u, v)};
}
void Instance::start_emitters() { births_ += std::max(curve("m_startingPopulation", age_, seed_), 0.0f); }
void Instance::execute_emitters(const Update& info) {
    const auto inverse = glm::inverse(local_to_parent * info.parent_to_world);
    const float dt = static_cast<float>(info.time - last_ran_);
    const float previous = age_;
    age_ += dt * age_rate_;
    if (age_ >= 1) births_ = 0;
    else births_ += dt * std::max(curve("m_particlesPerSecond", age_, seed_), 0.0f);
    size_t last_live = 0;
    for (size_t i = 0; i < active_; ++i) {
        auto& p = particles_[i];
        if (p.age < 1) {
            p.age += dt * p.age_rate;
            if (animate_particle(i, inverse, info.time)) { last_live = i + 1; continue; }
            destroy_particle(i);
        }
        if (births_ >= 1) {
            create_particle(i);
            if (animate_particle(i, inverse, info.time)) last_live = i + 1;
            else destroy_particle(i);
            births_ -= 1;
        }
    }
    active_ = last_live;
    while (births_ >= 1 && active_ < particles_.size()) {
        const auto i = active_++;
        create_particle(i);
        if (!animate_particle(i, inverse, info.time)) { destroy_particle(i); --active_; }
        births_ -= 1;
    }
    births_ -= static_cast<float>(std::floor(births_));
    age_ = previous;
}
void Instance::create_particle(size_t index) {
    auto& p = particles_[index];
    p.age = 0;
    p.seed = child_seed();
    p.age_rate = 1.0f / std::max(curve("m_pLifeSpan", age_, p.seed), 0.0333333f);
    // Legacy MSVC evaluates the polar constructor arguments right to left.
    // Keep this order explicit, rather than relying on argument evaluation.
    const float range = random_();
    const float pitch = random_() * math::Pi - math::Pi * 0.5f;
    const float yaw = random_() * math::TwoPi;
    const auto position = math::polarToVector({yaw, pitch, range});
    p.position = {position.x * curve("m_emitterSizeX", age_, p.seed),
        position.y * curve("m_emitterSizeY", age_, p.seed), position.z * curve("m_emitterSizeZ", age_, p.seed)};
    const float minimum = curve("m_minimumDeviation", age_, p.seed);
    const float deviation = std::max(curve("m_maximumDeviation", age_, p.seed) - minimum, 0.0f);
    const float velocity_pitch = minimum + (deviation * random_() - math::Pi * 0.5f);
    const float velocity_yaw = random_() * math::TwoPi;
    p.velocity = math::polarToVector({velocity_yaw, velocity_pitch, curve("m_startingSpeed", age_, p.seed)});
    if (ordinal() == 2) return;
    p.angular_velocity = glm::vec3(curve("m_pSpin", age_, p.seed));
    if (record_.alignment[3]) {
        glm::mat4 basis{1.0f};
        math::alignLocalAxisToWorldVector(basis, p.velocity, 1, 0, 2);
        p.rotation = math::quatFromRotation(basis);
    } else if (record_.alignment[0]) {
        const float roll = math::TwoPi * random_();
        const float yaw_rotation = math::Pi * random_();
        const float pitch_rotation = math::TwoPi * random_();
        p.rotation = math::quatFromEuler({pitch_rotation, yaw_rotation, roll});
    } else p.rotation = math::QuatIdentity;
    if (ordinal() == 4) {
        p.radius = curve("m_size", age_, p.seed);
        p.angle = math::sinD(curve("m_angularity", age_, p.seed));
    } else if (ordinal() == 5) {
        p.vertices.resize(record_.vertex_count);
        const float between = math::TwoPi / (record_.vertex_count - 2);
        const float radius = curve("m_size", age_, p.seed);
        int even = 1;
        float bound = 0;
        p.vertices[0] = glm::vec3(0);
        for (size_t j = 1; j + 1 < p.vertices.size(); ++j) {
            const float perturb = even * curve("m_perturbation", age_, p.seed);
            const float temp = perturb + radius;
            p.vertices[j] = {math::sinD(j * between) * temp, math::cosD(j * between) * temp, perturb};
            bound = std::max(bound, temp * temp + perturb * perturb);
            even = -even;
        }
        p.radius = root(bound); p.vertices.back() = p.vertices[1];
    } else if (ordinal() == 6) {
        p.half_y = curve("m_halfHeight", age_, p.seed);
        p.half_x = p.half_y * curve("m_aspectRatio", age_, p.seed);
        p.radius = root(p.half_x * p.half_x + p.half_y * p.half_y);
    } else if (ordinal() == 7) p.radius = record_.radius;
    else if (ordinal() == 8) {
        p.effect = std::make_unique<Instance>(records_, record_.child_effect, random_, execute_flag | dynamic_world_space);
        p.radius = 0;
        math::buildTranslation(p.effect->local_to_parent, p.position);
        math::buildRotation(p.effect->local_to_parent, p.rotation);
        Update info{last_ran_, local_to_world_, p.seed, p.age, p.age_rate};
        p.effect->start(info);
    }
}
bool Instance::animate_particle(size_t index, const glm::mat4& inverse, double time) {
    auto& p = particles_[index];
    if (ordinal() == 8 && p.age >= 1) p.age = 1.0f - math::SmallValue;
    if (p.age >= 1) return false;
    glm::vec3 position = p.position, velocity = p.velocity;
    glm::quat rotation = p.rotation;
    const auto mode = flags_ & simulation_mask;
    if (mode == dynamic_world_space) {
        position = math::transformPoint(position, local_to_world_);
        velocity = math::transformVector(velocity, local_to_world_);
        if (ordinal() != 2) rotation = math::quatFromRotation(math::rotationMatrix(rotation) * local_to_world_);
    }
    const float drag = std::min(-curve("m_pDrag", p.age, p.seed), 0.0f);
    glm::vec3 ether{curve("m_pEtherVelocityX", p.age, p.seed), curve("m_pEtherVelocityY", p.age, p.seed),
        curve("m_pEtherVelocityZ", p.age, p.seed)};
    glm::vec3 force{curve("m_pAccelerationX", p.age, p.seed), curve("m_pAccelerationY", p.age, p.seed),
        curve("m_pAccelerationZ", p.age, p.seed)};
    glm::vec3 accel;
    if (mode == static_world_space) {
        const auto world_to_effect = glm::inverse(local_to_world_);
        ether = math::transformVector(ether, glm::inverse(world_to_effect));
        accel = (velocity - ether) * drag;
        accel += math::transformVector(force, world_to_effect);
    } else {
        accel = (velocity - ether) * drag;
        accel.x += force.x; accel.y += force.y; accel.z += force.z;
    }
    const float dt = static_cast<float>(time - last_ran_);
    velocity = velocity + accel * dt;
    position = position + velocity * dt;
    if (ordinal() != 2 && !record_.alignment[3]) {
        const auto omega = math::quatFromRotationVector(p.angular_velocity * dt);
        rotation = math::composeQuat(omega, rotation);
        rotation = glm::normalize(rotation);
    }
    if (mode == dynamic_world_space) {
        p.velocity = math::transformVector(velocity, inverse);
        p.position = math::transformPoint(position, inverse);
        if (ordinal() != 2) p.rotation = math::quatFromRotation(math::rotationMatrix(rotation) * inverse);
    } else { p.position = position; p.velocity = velocity; p.rotation = rotation; }
    if (ordinal() != 2) {
        if (record_.alignment[3]) {
            glm::mat4 basis{1.0f};
            math::alignLocalAxisToWorldVector(basis, p.velocity, 1, 0, 2);
            p.rotation = math::quatFromRotation(basis);
        }
        p.scale = curve("m_pScale", p.age, p.seed);
    }
    if (ordinal() == 8) {
        math::buildTranslation(p.effect->local_to_parent, p.position);
        math::buildRotation(p.effect->local_to_parent, p.rotation);
        if (p.effect->execute({time, local_to_world_})) {
            const auto& bounds = p.effect->bounds();
            p.radius = glm::length(math::getTranslation(bounds.localToParent)) + bounds.sphereRadius;
            return true;
        }
        p.radius = 0; p.effect.reset(); return false;
    }
    p.color = {curve("m_pRed", p.age, p.seed), curve("m_pGreen", p.age, p.seed),
        curve("m_pBlue", p.age, p.seed), curve("m_pAlpha", p.age, p.seed)};
    if (ordinal() == 5) p.center_color = {curve("m_pCenterRed", p.age, p.seed), curve("m_pCenterGreen", p.age, p.seed),
        curve("m_pCenterBlue", p.age, p.seed), curve("m_pCenterAlpha", p.age, p.seed)};
    if (ordinal() == 6) p.uvs = card_uvs(record_, p.age, p.seed, "m_pIndex");
    return true;
}
void Instance::destroy_particle(size_t index) { particles_[index].age = 1; particles_[index].effect.reset(); }
void Instance::emitter_bounds() {
    if (ordinal() != 13 && !active_) return;
    math::Aabb box;
    bool first = true;
    size_t begin = 0;
    if (ordinal() == 13) {
        // Legacy's first-live search advances twice past dead debris slots.
        // Keep its resulting bounds, including the zero box if it finds none.
        while (begin < active_ && particles_[begin].age >= 1) begin += 2;
    }
    for (size_t i = begin; i < active_; ++i) {
        const auto& p = particles_[i];
        if (p.age >= 1) continue;
        glm::vec3 position = p.position;
        float radius = ordinal() == 2 ? 0 : p.radius * p.scale;
        if (ordinal() == 13) {
            position = math::transformPoint(vec(record_.geometry[i].sphere_center), p.debris_transform);
            radius = record_.geometry[i].sphere_radius;
        }
        const math::Aabb part{position - glm::vec3(radius), position + glm::vec3(radius)};
        box = first ? part : math::unite(box, part);
        first = false;
    }
    include_box(box);
}
void Instance::start_debris() {
    float minimum = 100000000000.0f, maximum = 0;
    for (const auto& piece : record_.geometry) {
        minimum = std::min(minimum, piece.sphere_radius); maximum = std::max(maximum, piece.sphere_radius);
    }
    // The loader uses the sphere-radius range as each fragment's authored seed.
    for (size_t i = 0; i < particles_.size(); ++i) {
        auto& p = particles_[i]; const auto& piece = record_.geometry[i];
        p.debris_transform = glm::mat4(1);
        math::buildTranslation(p.debris_transform, vec(piece.position));
        p.age = 0; p.seed = (piece.sphere_radius - minimum) / (maximum - minimum);
        p.age_rate = 1.0f / std::max(curve("m_pLifeSpan", 0, p.seed), 0.0333333f);
        const auto v = vec(piece.position) - vec(record_.center_of_force);
        const float low = curve("m_minimumDeviation", 0, p.seed), high = curve("m_maximumDeviation", 0, p.seed);
        const float t = random_();
        const float factor = low * (1.0f - t) + high * t;
        p.velocity = math::lerp(math::Up, v, factor);
        p.velocity = glm::normalize(p.velocity);
        p.velocity *= curve("m_startingSpeed", 0, p.seed);
        const float spin = curve("m_pSpin", 0, p.seed);
        p.angular_velocity.x = spin * (2 * random_() - 1);
        p.angular_velocity.y = spin * (2 * random_() - 1);
        p.angular_velocity.z = spin * (2 * random_() - 1);
    }
    active_ = particles_.size();
}
void Instance::execute_debris(const Update& info) {
    const float dt = static_cast<float>(info.time - last_ran_);
    for (auto& p : particles_) {
        if (p.age >= 1) continue;
        p.age = std::min(p.age + dt * p.age_rate, 1.0f);
        if (p.age >= 1) continue;
        p.color.a = curve("m_pAlpha", p.age, p.seed);
        auto translation = math::getTranslation(p.debris_transform);
        auto rotation = math::quatFromRotation(p.debris_transform);
        const float drag = std::min(-curve("m_pDrag", p.age, p.seed), 0.0f);
        const glm::vec3 ether{0, curve("m_pEtherVelocityY", p.age, p.seed), 0};
        const auto world_to_effect = glm::inverse(local_to_world_);
        const auto local_ether = math::transformVector(ether, glm::inverse(world_to_effect));
        auto accel = (p.velocity - local_ether) * drag;
        accel += math::transformVector({0, curve("m_pAccelerationY", p.age, p.seed), 0}, world_to_effect);
        p.velocity = p.velocity + accel * dt;
        translation = translation + p.velocity * dt;
        rotation = math::composeQuat(math::quatFromRotationVector(p.angular_velocity * dt), rotation);
        rotation = glm::normalize(rotation);
        math::buildRotation(p.debris_transform, rotation);
        math::buildTranslation(p.debris_transform, translation);
    }
}
}
