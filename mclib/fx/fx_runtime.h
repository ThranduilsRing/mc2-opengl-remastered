#pragma once

#include "fx_catalog.h"
#include "math/mc2_xform.h"
#include <functional>
#include <memory>

namespace mc2::fx {
class Instance;

// The caller owns the effect stream, separately from the simulation stream.
// It is intentionally unavailable to geometry generation.
using RandomFraction = std::function<float()>;
inline constexpr unsigned
    execute_flag = 1, loop_flag = 2, local_space = 0,
    dynamic_world_space = 4, static_world_space = 8,
    parent_space = 12, simulation_mask = 12;
struct Update {
    double time = 0;
    glm::mat4 parent_to_world{1.0f};
    float seed = -1, age = -1, age_rate = -1;
};
struct LightSample {
    glm::mat4 origin{1.0f};
    glm::vec3 color{};
    float intensity = 0, inner = 0, outer = 0;
    bool active = false;
};
struct Particle {
    float age = 0, age_rate = 0, seed = 0;
    glm::vec3 position{}, velocity{}, angular_velocity{};
    glm::quat rotation = math::QuatIdentity;
    float scale = 0, radius = 0, half_x = 0, half_y = 0, angle = 0;
    glm::vec4 color{}, center_color{};
    std::array<glm::vec2, 4> uvs{};
    std::vector<glm::vec3> vertices;
    glm::mat4 debris_transform{1.0f};
    std::unique_ptr<Instance> effect;
};
struct Profile {
    float age = 0, age_rate = 0, seed = 0;
    glm::vec3 direction{};
    glm::mat4 profile_to_world{1.0f};
};

// Records and random source must outlive the instance and all its descendants.
// Instances are independent of render/resource services and legacy registries.
class Instance {
public:
    Instance(std::span<const EffectRecord> records, size_t effect_id,
        RandomFraction& random, unsigned flags = execute_flag);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    void start(const Update& info);
    bool execute(const Update& info);
    void stop();
    void kill();
    bool has_finished() const;
    bool is_executed() const { return (flags_ & execute_flag) != 0; }
    void set_looped(bool looped);
    glm::mat4 local_to_parent{1.0f};

    const EffectRecord& record() const { return record_; }
    float age() const { return age_; }
    float age_rate() const { return age_rate_; }
    float seed() const { return seed_; }
    double last_ran() const { return last_ran_; }
    unsigned flags() const { return flags_; }
    size_t next_event() const { return event_; }
    const glm::mat4& local_to_world() const { return local_to_world_; }
    const math::Obb& bounds() const { return bounds_; }
    const auto& children() const { return children_; }
    const auto& particles() const { return particles_; }
    size_t active_count() const { return active_; }
    float birth_accumulator() const { return births_; }
    const auto& profiles() const { return profiles_; }
    size_t head_profile() const { return head_; }
    size_t tail_profile() const { return tail_; }
    const LightSample& light() const { return light_; }
    const Particle& singleton() const { return singleton_; }
    const auto& tube_vertices() const { return tube_vertices_; }
    const auto& tube_uvs() const { return tube_uvs_; }
    const auto& tube_colors() const { return tube_colors_; }
    float curve(std::string_view name, float age, float seed) const;
private:
    unsigned ordinal() const { return record_.class_id - first_effect_class; }
    bool base_execute(const Update& info);
    float child_seed();
    void start_emitters();
    void execute_emitters(const Update& info);
    void create_particle(size_t index);
    bool animate_particle(size_t index, const glm::mat4& inverse, double time);
    void destroy_particle(size_t index);
    void emitter_bounds();
    void start_debris();
    void execute_debris(const Update& info);
    void singleton_execute();
    void start_tube();
    void initialize_tube();
    void execute_tube(const Update& info);
    void create_profile(size_t index, const glm::mat4& origin);
    bool animate_profile(size_t index, size_t compact_index, const glm::mat4& inverse,
        math::Sphere& sphere);
    void include_box(const math::Aabb& box, bool minimum_radius = true);

    std::span<const EffectRecord> records_;
    const EffectRecord& record_;
    RandomFraction& random_;
    unsigned flags_;
    float age_ = 0, age_rate_ = 0, seed_ = 0;
    double last_ran_ = -1;
    size_t event_ = 0;
    glm::mat4 local_to_world_{1.0f};
    math::Obb bounds_;
    std::vector<std::unique_ptr<Instance>> children_;
    std::vector<Particle> particles_;
    size_t active_ = 0;
    float births_ = 0;
    Particle singleton_;
    LightSample light_;
    std::vector<Profile> profiles_;
    size_t head_ = 0, tail_ = 0;
    std::vector<glm::vec3> tube_vertices_;
    std::vector<glm::vec2> tube_uvs_;
    std::vector<glm::vec4> tube_colors_;
    float tube_u_bias_ = 0;
    std::optional<float> tube_u_size_;
};
glm::mat4 event_transform(const Event& event);
}
