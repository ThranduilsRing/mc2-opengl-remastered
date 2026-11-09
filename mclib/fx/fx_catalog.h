#pragma once

#include "fx_curve.h"
#include "fx_mesh_data.h"

namespace mc2::fx {
struct Event {
    float time = 0;
    uint32_t flags = 0, effect_id = 0;
    std::array<float, 12> local_to_parent{}; // wire affine block, not a host matrix layout
};
struct GeometryPiece {
    Vec3 position{}, sphere_center{};
    float sphere_radius = 0;
    MeshShape shape;
};
struct EffectRecord {
    uint32_t class_id = first_effect_class;
    std::string name;
    std::vector<Event> events;
    // Includes inherited curves, in wire order; names are the authored member names.
    std::vector<Curve> curves;
    MlrVersion state_version;
    Material material;
    uint32_t population_limit = 0, profile_type = 0, vertex_count = 0, child_effect = 0;
    std::array<bool, 4> alignment{};
    bool animated = false, inside_out = false, two_sided = false;
    std::string light_map;
    MlrVersion geometry_version;
    Vec3 center_of_force{};
    std::vector<GeometryPiece> geometry;
    float radius = 0;
    const Curve* find_curve(std::string_view curve_name) const;
};

// Owns decoded values only. All published access is const, and no legacy types,
// pointers, registries, texture pools, file providers or source bytes survive here.
class Catalog {
public:
    unsigned version() const { return version_; }
    const std::vector<EffectRecord>& effects() const { return effects_; }
    const EffectRecord* find(std::string_view name) const;
    std::vector<uint8_t> rebuild() const;
private:
    Catalog(unsigned version, std::vector<EffectRecord> effects);
    unsigned version_;
    std::vector<EffectRecord> effects_;
    friend std::optional<Catalog> read_catalog(std::span<const uint8_t>, std::string&);
};
std::optional<Catalog> read_catalog(std::span<const uint8_t> bytes, std::string& error);
}
