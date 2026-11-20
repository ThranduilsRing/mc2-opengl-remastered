#include "AssetTypeSidebar.h"
#include "imgui.h"
void AssetTypeSidebar::draw()
{
    ImGui::TextDisabled("Asset types");
    if (ImGui::Selectable("Textures",     active_ == AssetType::Textures))     active_ = AssetType::Textures;
    if (ImGui::Selectable("Materials",    active_ == AssetType::Materials))    active_ = AssetType::Materials;
    if (ImGui::Selectable("Static Props", active_ == AssetType::StaticProps))  active_ = AssetType::StaticProps;
    if (ImGui::Selectable("Mechs",        active_ == AssetType::Mechs))        active_ = AssetType::Mechs;
    if (ImGui::Selectable("Vehicles",     active_ == AssetType::Vehicles))     active_ = AssetType::Vehicles;
    if (ImGui::Selectable("Trees",        active_ == AssetType::Trees))        active_ = AssetType::Trees;
    if (ImGui::Selectable("Terrain Materials", active_ == AssetType::TerrainMaterials)) active_ = AssetType::TerrainMaterials;
    if (ImGui::Selectable("VFX",          active_ == AssetType::Vfx))          active_ = AssetType::Vfx;
    ImGui::Spacing();
    ImGui::TextDisabled("Tools");
    if (ImGui::Selectable("Mod Workbench",active_ == AssetType::ModWorkbench)) active_ = AssetType::ModWorkbench;
    if (ImGui::Selectable("Mod Package",  active_ == AssetType::ModPackage))   active_ = AssetType::ModPackage;
    if (ImGui::Selectable("Archives",     active_ == AssetType::Archives))     active_ = AssetType::Archives;
}
