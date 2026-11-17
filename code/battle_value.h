#pragma once

#include <istream>
#include <optional>
#include <vector>

// Informational MC2 rating only; no dependency on simulation or logistics state.
namespace mc2bv {
struct Profile {
    int version{};
    double armor{}, structure{}, defensiveEquipment{};
    double defenseSpeed{}, jump{}, offenseSpeed{};
    double unsustained{}, ammoPerShot{}, ammoCap{};
};

// Reject missing, duplicate, unknown, nonfinite or unsupported profile values.
std::optional<Profile> readProfile(std::istream& input);

struct Layout {
    double armor{}, structure{};
    // TODO(chassis-layout-model): per-location/rear armor terms in profile v2.
};
struct Variant {
    double speed{}, jump{}, heatBudget{}; // lab speed class and sink capacity
};
struct Weapon {
    double rating{}, heat{}; // compbas BR and lab heat columns
    // nullopt = energy weapon; otherwise allocated shots, zero = unusable.
    std::optional<double> ammoShots;
};
struct Components {
    std::vector<Weapon> weapons;
    double defensiveEquipment{};
};

// Inputs must be finite and nonnegative; invalid input throws invalid_argument.
// Profile v1 allocates the sustainable heat budget by BR/heat efficiency.
// Adding a weapon cannot reduce the value of weapons already installed.
int compute(const Layout&, const Variant&, const Components&, const Profile&);
} // namespace mc2bv
