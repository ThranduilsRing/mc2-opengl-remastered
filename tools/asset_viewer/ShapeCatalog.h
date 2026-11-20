// tools/asset_viewer/ShapeCatalog.h
// Mechs / Vehicles / Trees panels: what shapes exist in an install and how many triangles each costs.
// Pure data, no ImGui, no GPU. Sources: tgl.fst names (stock), data/tgl/*.glb with a .package.json (BT2018 mech GLBs),
// data/objects/*.csv (vehicle chassis list), and every models.json under data/model_overrides and mods/*/data/model_overrides.
#pragma once
#include "TglMeshLoader.h"
#include <string>
#include <vector>

enum class ShapeCategory { Mechs, Vehicles, Trees };

struct ShapeVariant {
    std::string role;   // "base", "LOD1", "shadow", "left arm", "LOD2" ...
    std::string path;   // tgl: name as stored in the FST; glb: path on disk
    bool glb = false;
};
struct ShapeEntry {
    std::string name;
    std::string origin;   // "stock", "BT2018 GLB", "mod <id>", "override" ...
    std::vector<ShapeVariant> variants;
};
struct ShapeStats {
    bool ok = false;
    std::string error;
    int tris = 0, verts = 0, parts = 0;      // verts = unique positions, parts = texture/material groups
    std::vector<std::string> textures;       // unique, non-empty texture basenames
};

// tglNames: TglMeshLoader::listTgl() output (may be empty when tgl.fst is absent; GLB/override sources still list).
std::vector<ShapeEntry> BuildShapeCatalog(ShapeCategory cat, const std::string& deployDir,
                                          const std::vector<std::string>& tglNames);
MeshData  LoadShapeVariant(const ShapeVariant& v);
ShapeStats MeasureMesh(const MeshData& md);
