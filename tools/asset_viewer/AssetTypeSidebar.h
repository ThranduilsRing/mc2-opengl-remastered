#pragma once
// Left sidebar: the modder-facing asset-type vocabulary.
// Entries not yet implemented are listed greyed out under "Deferred".
enum class AssetType { Textures, Materials, StaticProps, ModWorkbench, Archives, Mechs };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return active_; }
    void setActive(AssetType t) { active_ = t; }
private:
    AssetType active_ = AssetType::Textures;
};
