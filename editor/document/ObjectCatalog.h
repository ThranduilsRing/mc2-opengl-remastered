#pragma once

#include "FileResolver.h"

namespace editor::document {

enum class ObjectFamily { mech, vehicle, building, turret, tree, bridge, gate, light, marker };
enum class CatalogAppearance { mech, vehicle, building, tree };
std::string_view familyName(ObjectFamily family);
struct CatalogVariant { unsigned index{}; std::string name; };
struct CatalogSource {
    std::string key, layer;
    std::filesystem::path source;
};
struct CatalogEntry {
    std::uint32_t id{};
    std::string stem, displayName, appearance, profile, special, icon;
    ObjectFamily family{};
    CatalogAppearance appearanceType{};
    std::int64_t runtimeClass{}, group{}, nameId{}, forestGroup{};
    bool capturable{}, alignable{}, onTacMap{}, hover{}, blocksLineOfFire{};
    std::uint32_t tacMapValue{};
    double scale = 1;
    std::uint64_t impassability{};
    CatalogSource enumeration, definition;
    std::optional<CatalogSource> variantFile;
    std::vector<CatalogVariant> variants;
};

// Catalog facts exclude team, pilot and brain defaults, which belong to placement policy.
class ObjectCatalog {
public:
    using ResourceName = std::function<std::optional<std::string>(std::int64_t)>;
    static ObjectCatalog load(const FileResolver& resolver, Diagnostic log, ResourceName names = {});
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    std::size_t rejections() const { return rejections_; }
private:
    std::vector<CatalogEntry> entries_;
    std::size_t rejections_{};
};

// The owned string index resolves clean FIT aliases before legacy numeric names.
ObjectCatalog::ResourceName resourceNames(const FileResolver& resolver, Diagnostic log = {});

} // namespace editor::document
