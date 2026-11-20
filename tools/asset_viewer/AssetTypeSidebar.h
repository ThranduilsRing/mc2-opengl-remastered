#pragma once
// Left sidebar: the modder-facing asset-type vocabulary.
// Every entry is implemented; nothing is deferred any more.
enum class AssetType { Textures, Materials, StaticProps, ModWorkbench, Archives, Mechs, Vehicles, Trees, TerrainMaterials, Vfx, ModPackage };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return active_; }
    void setActive(AssetType t) { active_ = t; }
private:
    AssetType active_ = AssetType::Textures;
};
