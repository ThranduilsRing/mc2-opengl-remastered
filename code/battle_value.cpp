#include "battle_value.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mc2bv {
namespace {
void nonnegative(double value) {
    if (!std::isfinite(value) || value < 0)
        throw std::invalid_argument("BV requires finite nonnegative values");
}
void validate(const Profile& p) {
    if (p.version != 1) throw std::invalid_argument("unsupported BV profile");
    for (double v : {p.armor, p.structure, p.defensiveEquipment, p.defenseSpeed,
                     p.jump, p.offenseSpeed, p.unsustained, p.ammoPerShot, p.ammoCap})
        nonnegative(v);
    if (p.unsustained >= 1) throw std::invalid_argument("BV heat discount must be below one");
}
} // namespace

std::optional<Profile> readProfile(std::istream& input) {
    std::map<std::string, double> values;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find("//");
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream row(line);
        std::string type, key, equals, extra;
        if (!(row >> type)) continue;
        if (type == "FITini" || type == "[BattleValue]" || type == "FITend") continue;
        double value{};
        if ((type != "l" && type != "f") || !(row >> key >> equals >> value) ||
            equals != "=" || (row >> extra))
            return std::nullopt;
        const auto [entry, inserted] = values.emplace(key, value);
        if (!inserted) return std::nullopt;
    }
    const std::vector<std::string> keys{"BvProfile", "Armor", "Structure", "DefensiveEquipment",
        "DefenseSpeed", "Jump", "OffenseSpeed", "Unsustained", "AmmoPerShot", "AmmoCap"};
    if (values.size() != keys.size()) return std::nullopt;
    for (const auto& key : keys) if (!values.count(key)) return std::nullopt;
    if (values.at("BvProfile") != 1) return std::nullopt;
    Profile p{1, values.at("Armor"), values.at("Structure"), values.at("DefensiveEquipment"),
        values.at("DefenseSpeed"), values.at("Jump"), values.at("OffenseSpeed"),
        values.at("Unsustained"), values.at("AmmoPerShot"), values.at("AmmoCap")};
    try { validate(p); } catch (const std::invalid_argument&) { return std::nullopt; }
    return p;
}

int compute(const Layout& layout, const Variant& variant, const Components& components,
            const Profile& p) {
    validate(p);
    for (double v : {layout.armor, layout.structure, variant.speed, variant.jump,
                     variant.heatBudget, components.defensiveEquipment}) nonnegative(v);
    std::vector<Weapon> weapons;
    double offense = 0;
    for (const auto& w : components.weapons) {
        nonnegative(w.rating);
        nonnegative(w.heat);
        if (w.ammoShots) {
            nonnegative(*w.ammoShots);
            if (*w.ammoShots == 0) continue;
            offense += w.rating * std::min(p.ammoCap, *w.ammoShots * p.ammoPerShot);
        }
        offense += w.rating * p.unsustained;
        weapons.push_back(w);
    }
    std::sort(weapons.begin(), weapons.end(), [](const Weapon& a, const Weapon& b) {
        if (a.heat == 0 || b.heat == 0) return a.heat == 0 && b.heat != 0;
        return a.rating / a.heat > b.rating / b.heat;
    });
    double budget = variant.heatBudget;
    for (const auto& w : weapons) {
        const double fraction = w.heat == 0 ? 1 : std::min(1.0, budget / w.heat);
        offense += w.rating * (1 - p.unsustained) * fraction;
        budget = std::max(0.0, budget - w.heat * fraction);
    }
    const double defense = (layout.armor * p.armor + layout.structure * p.structure +
        components.defensiveEquipment * p.defensiveEquipment) *
        (1 + variant.speed * p.defenseSpeed + variant.jump * p.jump);
    const double result = defense + offense * (1 + variant.speed * p.offenseSpeed);
    if (!std::isfinite(result) || result >= std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    return static_cast<int>(std::floor(result + 0.5));
}
} // namespace mc2bv

// Offline report adapter: one numeric variant per line, no engine linkage.
#ifdef MC2_BV_REPORT_MAIN
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1]); // path-audit: loose-only; offline evaluator's explicit profile argument.
    const auto profile = mc2bv::readProfile(file);
    if (!profile) return 2;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        mc2bv::Layout l;
        mc2bv::Variant v;
        mc2bv::Components c;
        unsigned count{};
        if (!(in >> l.armor >> l.structure >> v.speed >> v.jump >> v.heatBudget >>
              c.defensiveEquipment >> count) || count > 1024) return 2;
        for (unsigned i = 0; i < count; ++i) {
            mc2bv::Weapon w;
            std::string kind;
            if (!(in >> w.rating >> w.heat >> kind)) return 2;
            if (kind == "ammo") {
                double shots{};
                if (!(in >> shots)) return 2;
                w.ammoShots = shots;
            } else if (kind != "energy") return 2;
            c.weapons.push_back(w);
        }
        std::string extra;
        if (in >> extra) return 2;
        try { std::cout << mc2bv::compute(l, v, c, *profile) << '\n'; }
        catch (const std::invalid_argument&) { return 2; }
    }
}
#endif
