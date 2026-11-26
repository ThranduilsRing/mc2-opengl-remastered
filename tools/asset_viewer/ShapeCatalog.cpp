// tools/asset_viewer/ShapeCatalog.cpp
#include "ShapeCatalog.h"
#include "GlbMeshLoader.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static bool endsWith(const std::string& s, const char* suf) {
    size_t n = std::char_traits<char>::length(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
static bool startsWith(const std::string& s, const char* pre) { return s.rfind(pre, 0) == 0; }

namespace {

// Lowercase stem ("data/tgl/Pine1L1.tgl" -> "pine1l1") -> the name as stored in the FST.
using TglIndex = std::map<std::string, std::string>;

TglIndex indexTgl(const std::vector<std::string>& names) {
    TglIndex idx;
    for (const auto& n : names) {
        std::string s = lower(n);
        size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos) s = s.substr(slash + 1);
        if (endsWith(s, ".tgl")) s.resize(s.size() - 4);
        idx[s] = n;
    }
    return idx;
}

void addTgl(ShapeEntry& e, const TglIndex& idx, const std::string& stem, const char* role) {
    auto it = idx.find(stem);
    if (it != idx.end()) e.variants.push_back({role, it->second, false});
}

std::vector<fs::path> modDataRoots(const std::string& deployDir, const char* sub) {
    std::vector<fs::path> r;
    std::error_code ec;
    r.push_back(fs::path(deployDir) / "data" / sub);
    for (auto& de : fs::directory_iterator(fs::path(deployDir) / "mods", ec))
        if (!ec && de.is_directory()) r.push_back(de.path() / "data" / sub);
    return r;
}
std::string originOf(const fs::path& root) {
    // <install>/data/<sub> -> "install"; <install>/mods/<id>/data/<sub> -> "mod <id>"
    fs::path mod = root.parent_path().parent_path();
    return lower(mod.parent_path().filename().string()) == "mods" ? "mod " + mod.filename().string() : "install";
}

// BT2018 mech GLBs: data/tgl/<x>.glb with a <x>.package.json beside it (props like HangarGLB have none).
void addMechGlbs(std::vector<ShapeEntry>& out, const std::string& deployDir) {
    for (const auto& root : modDataRoots(deployDir, "tgl")) {
        std::error_code ec;
        std::set<std::string> files;
        for (auto& de : fs::directory_iterator(root, ec)) if (!ec) files.insert(lower(de.path().filename().string()));
        for (auto& de : fs::directory_iterator(root, ec)) {
            if (ec || !de.is_regular_file() || lower(de.path().extension().string()) != ".glb") continue;
            std::string stem = de.path().stem().string();
            if (!files.count(lower(stem) + ".package.json")) continue;
            ShapeEntry e;
            e.name = stem;
            e.origin = "BT2018 GLB (" + originOf(root) + ")";
            e.variants.push_back({"base", de.path().generic_string(), true});
            out.push_back(std::move(e));
        }
    }
}

// models.json overrides of one class ("vehicle" / "tree"): LOD0 = source, then lods[].source.
void addOverrides(std::vector<ShapeEntry>& out, const std::string& deployDir, const char* cls) {
    const std::string prefix = std::string(cls) + ":";
    for (const auto& root : modDataRoots(deployDir, "model_overrides")) {
        try {
        std::ifstream f(root / "models.json");
        if (!f) continue;
        json doc = json::parse(f, nullptr, false);
        if (doc.is_discarded() || !doc.contains("overrides") || !doc["overrides"].is_array()) continue;
        for (const auto& rec : doc["overrides"]) {
            if (!rec.is_object()) continue;
            std::string repl = rec.value("replaces", "");
            if (!startsWith(repl, prefix.c_str()) || !rec.contains("source") || !rec["source"].is_string()) continue;
            ShapeEntry e;
            e.name = repl.substr(prefix.size());
            e.origin = "override (" + originOf(root) + ")";
            e.variants.push_back({"LOD0", (root / rec["source"].get<std::string>()).generic_string(), true});
            if (rec.contains("lods") && rec["lods"].is_array())
                for (const auto& l : rec["lods"])
                    if (l.is_object() && l.contains("source") && l["source"].is_string())
                        e.variants.push_back({"LOD" + std::to_string(l.value("lod", 0)),
                                              (root / l["source"].get<std::string>()).generic_string(), true});
            out.push_back(std::move(e));
        }
        } catch (const std::exception&) { /* Ignore unreadable override files. */ }
    }
}

bool isMechStem(const TglIndex& idx, const std::string& s) {
    return idx.count(s + "leftarm") && idx.count(s + "rightarm");
}

}  // namespace

std::vector<ShapeEntry> BuildShapeCatalog(ShapeCategory cat, const std::string& deployDir,
                                          const std::vector<std::string>& tglNames) {
    std::vector<ShapeEntry> out;
    const TglIndex idx = indexTgl(tglNames);

    if (cat == ShapeCategory::Mechs) {
        for (const auto& kv : idx) {
            const std::string& s = kv.first;
            if (!isMechStem(idx, s)) continue;
            ShapeEntry e;
            e.name = s;
            e.origin = "stock";
            addTgl(e, idx, s, "base");
            addTgl(e, idx, s + "l1", "LOD1");
            addTgl(e, idx, s + "x", "shadow");
            addTgl(e, idx, s + "leftarm", "left arm");
            addTgl(e, idx, s + "rightarm", "right arm");
            out.push_back(std::move(e));
        }
        addMechGlbs(out, deployDir);
    } else if (cat == ShapeCategory::Vehicles) {
        // Vehicle chassis = data/objects/<name>.csv whose <name>.tgl exists and that has no arm shapes.
        std::error_code ec;
        for (auto& de : fs::directory_iterator(fs::path(deployDir) / "data" / "objects", ec)) {
            if (ec || lower(de.path().extension().string()) != ".csv") continue;
            std::string s = lower(de.path().stem().string());
            if (!idx.count(s) || isMechStem(idx, s)) continue;
            ShapeEntry e;
            e.name = s;
            e.origin = "stock";
            addTgl(e, idx, s, "base");
            addTgl(e, idx, s + "l1", "LOD1");
            addTgl(e, idx, s + "x", "shadow");
            out.push_back(std::move(e));
        }
        addOverrides(out, deployDir, "vehicle");
    } else {
        static const char* kPrefix[] = {"pine", "birch", "oak", "palm", "cactus", "jungletree", "treeclump", "dummytree"};
        auto isTree = [&](const std::string& s) {
            for (const char* p : kPrefix) if (startsWith(s, p)) return true;
            return false;
        };
        for (const auto& kv : idx) {
            const std::string& s = kv.first;
            if (!isTree(s)) continue;
            // "<base>l1" / "<base>x" belong to the base entry's LOD chain.
            bool chain = false;
            for (const char* suf : {"l1", "x"})
                if (endsWith(s, suf) && idx.count(s.substr(0, s.size() - std::char_traits<char>::length(suf)))) chain = true;
            if (chain) continue;
            ShapeEntry e;
            e.name = s;
            e.origin = "stock";
            addTgl(e, idx, s, "LOD0");
            addTgl(e, idx, s + "l1", "LOD1");
            addTgl(e, idx, s + "x", "shadow");
            out.push_back(std::move(e));
        }
        addOverrides(out, deployDir, "tree");
    }
    std::stable_sort(out.begin(), out.end(), [](const ShapeEntry& a, const ShapeEntry& b) {
        return lower(a.name) < lower(b.name);
    });
    return out;
}

MeshData LoadShapeVariant(const ShapeVariant& v) {
    return v.glb ? GlbMeshLoader::load(v.path) : TglMeshLoader::loadMesh(v.path);
}

ShapeStats MeasureMesh(const MeshData& md) {
    ShapeStats st;
    st.ok = md.ok;
    st.error = md.error;
    if (!md.ok) return st;
    std::set<std::array<float, 3>> pos;
    std::set<std::string> tex;
    for (const auto& sm : md.submeshes) {
        st.tris += (int)(sm.idx.size() / 3);
        for (const auto& v : sm.verts) pos.insert({v.px, v.py, v.pz});
        if (!sm.textureName.empty()) tex.insert(sm.textureName);
    }
    st.parts = (int)md.submeshes.size();
    st.verts = (int)pos.size();
    st.textures.assign(tex.begin(), tex.end());
    return st;
}
