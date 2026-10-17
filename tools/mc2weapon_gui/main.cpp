// mc2weapon_gui — Dear ImGui (SDL2 + Vulkan) weapon editor for MC2.
//
// Foolproof front-end over the same CSV model as tools/mc2weapon/mc2weapon.py:
// enum fields are dropdowns (can't typo a Type/Range/FX), numbers are validated
// live, and Save is disabled while anything is invalid. Save writes a loose
// mods/<id>/data/objects/compbas.csv overlay (+ mod.json) that the game resolves
// first via MC2_ACTIVE_MOD — no .pak repack. No engine/game code linked.
//
// Build: -DENABLE_MC2WEAPON_GUI=ON (guarded in top CMakeLists).
// --headless-smoke [--frames N]: load + render N frames (or core-only fallback
// if no display) then exit 0 — autonomous build/run verification.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "weapon_csv.h"

#include <SDL.h>
#include "tool_vk_shell.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Directory containing this exe (for defaults relative to the install, not cwd).
std::string exeDir() {
#ifdef _WIN32
    char buf[2048];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return ".";
    std::string p(buf, n);
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
#else
    return ".";
#endif
}

const char* kDefaultCompbasCandidates[] = {
    "data/objects/compbas.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/compbas.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/compbas.csv",
};
const char* kDefaultEffectsCandidates[] = {
    "data/objects/effects.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/effects.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/effects.csv",
};

// missile-type enum (compbas "Missile type" column) — label shown, value stored.
const char* kMTypeLabels[5] = {"Energy (0)", "Ballistic (1)", "LRM", "ST (streak)", "SRM"};
const char* kMTypeValues[5] = {"0", "1", "LRM", "ST", "SRM"};
// fields bitfield (compbas "Fields" column).
const char* kFieldLabels[4] = {"streak", "inferno", "LBX", "Artillery"};
const int   kFieldBits[4]   = {1, 2, 4, 8};

void helpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// One scanned mod: whether it defines weapons (has compbas), its count, and its
// mod.json "type" (campaign / dependency / ...) for the ones that don't.
struct ModInfo {
    std::string name;
    bool        hasWeapons = false;
    int         weapons = 0;
    std::string type;
};

// Crude mod.json "type" extractor (no JSON dep): find "type":"<x>".
std::string readModType(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t p = s.find("\"type\"");
    if (p == std::string::npos) return "";
    p = s.find(':', p);
    if (p == std::string::npos) return "";
    size_t q1 = s.find('"', p);
    if (q1 == std::string::npos) return "";
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return s.substr(q1 + 1, q2 - q1 - 1);
}

struct App {
    mc2w::Compbas cb;
    std::vector<mc2w::FxEntry> fx;
    char filter[64] = "";
    bool showAll = false;
    int selectedRow = -1;          // data-row index in cb.rows
    // edit buffers for the selected weapon
    char eName[128] = "";
    int eType = 0;                 // index into kWeaponTypes
    char eDamage[32] = "", eHeat[32] = "", eRecycle[32] = "", eTons[32] = "";
    char eSlots[32] = "";
    int eRange = 1;                // index into kRanges
    int eFx = 0;                   // index into fx palette
    int eMType = 0;                // index into kMTypeValues
    bool eFld[4] = {false, false, false, false};  // streak/inferno/lbx/artillery
    char eIconX[8] = "", eIconY[8] = "", eAmmo[16] = "";
    char modId[64] = "my-weapons";
    char modRoot[256] = "mods";          // deploy target (where Save writes mods/<id>)
    char newId[16] = "";
    // source / mod loading
    std::string basePath;                // original auto-detected compbas (Stock)
    char loadPath[256] = "";             // explicit compbas path to load
    char modsDir[256] = "mods";          // dir scanned for loadable mods
    std::vector<ModInfo> foundMods;      // all mods under modsDir (weapon + others)
    std::string status;
};

bool fileExists(const char* p) { FILE* f = std::fopen(p, "rb"); if (f) { std::fclose(f); return true; } return false; }
const char* pickPath(const char* const* cands, int n, const char* override_) {
    if (override_ && *override_) return override_;
    for (int i = 0; i < n; ++i) if (fileExists(cands[i])) return cands[i];
    return cands[0];
}

void setBuf(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}

int findFxIndex(const std::vector<mc2w::FxEntry>& fx, const std::string& fxidStr) {
    long id = std::strtol(fxidStr.c_str(), nullptr, 10);
    for (size_t i = 0; i < fx.size(); ++i) if (fx[i].id == (int)id) return (int)i;
    return 0;
}
int comboIndex(const char* const* opts, int n, const std::string& v) {
    std::string lv;
    for (char c : v) lv += (char)tolower((unsigned char)c);
    for (int i = 0; i < n; ++i) {
        std::string o;
        for (const char* p = opts[i]; *p; ++p) o += (char)tolower((unsigned char)*p);
        if (o == lv) return i;
    }
    return -1;
}

void loadSelectionToBuffers(App& a) {
    int r = a.selectedRow;
    const auto& I = a.cb.idx;
    setBuf(a.eName, sizeof a.eName, a.cb.cell(r, I.name));
    setBuf(a.eDamage, sizeof a.eDamage, a.cb.cell(r, I.damage));
    setBuf(a.eHeat, sizeof a.eHeat, a.cb.cell(r, I.heat));
    setBuf(a.eRecycle, sizeof a.eRecycle, a.cb.cell(r, I.recycle));
    setBuf(a.eTons, sizeof a.eTons, a.cb.cell(r, I.tons));
    setBuf(a.eSlots, sizeof a.eSlots, a.cb.cell(r, I.slots));
    setBuf(a.eAmmo, sizeof a.eAmmo, a.cb.cell(r, I.ammoMasterId));
    setBuf(a.eIconX, sizeof a.eIconX, a.cb.cell(r, I.iconX));
    setBuf(a.eIconY, sizeof a.eIconY, a.cb.cell(r, I.iconY));
    a.eMType = comboIndex(kMTypeValues, 5, a.cb.cell(r, I.missileType));
    if (a.eMType < 0) a.eMType = 0;
    {
        long fv = std::strtol(a.cb.cell(r, I.fields).c_str(), nullptr, 10);
        for (int i = 0; i < 4; ++i) a.eFld[i] = (fv & kFieldBits[i]) != 0;
    }
    int ti = comboIndex(mc2w::kWeaponTypes, 3, a.cb.cell(r, I.type));
    a.eType = ti < 0 ? 0 : ti;
    int ri = comboIndex(mc2w::kRanges, 3, a.cb.cell(r, I.range));
    a.eRange = ri < 0 ? 1 : ri;
    a.eFx = findFxIndex(a.fx, a.cb.cell(r, I.fxid));
}

// apply edit buffers back into the selected row
void buffersToRow(App& a) {
    int r = a.selectedRow;
    const auto& I = a.cb.idx;
    a.cb.setCell(r, I.name, a.eName);
    a.cb.setCell(r, I.type, mc2w::kWeaponTypes[a.eType]);
    a.cb.setCell(r, I.damage, a.eDamage);
    a.cb.setCell(r, I.heat, a.eHeat);
    a.cb.setCell(r, I.recycle, a.eRecycle);
    a.cb.setCell(r, I.tons, a.eTons);
    a.cb.setCell(r, I.slots, a.eSlots);
    a.cb.setCell(r, I.range, mc2w::kRanges[a.eRange]);
    a.cb.setCell(r, I.missileType, kMTypeValues[a.eMType]);
    {
        int fv = 0;
        for (int i = 0; i < 4; ++i) if (a.eFld[i]) fv |= kFieldBits[i];
        char fb[8];
        std::snprintf(fb, sizeof fb, "%d", fv);
        a.cb.setCell(r, I.fields, fb);
    }
    a.cb.setCell(r, I.ammoMasterId, a.eAmmo);
    a.cb.setCell(r, I.iconX, a.eIconX);
    a.cb.setCell(r, I.iconY, a.eIconY);
    if (!a.fx.empty()) {
        char fxbuf[16];
        std::snprintf(fxbuf, sizeof fxbuf, "%d", a.fx[a.eFx].id);
        a.cb.setCell(r, I.fxid, fxbuf);
    }
}

// validated InputText: red border + returns whether valid
bool field(const char* label, char* buf, size_t cap, const char* kind,
           const std::vector<mc2w::FxEntry>& fx, int width = 120) {
    std::string err = mc2w::validateCell(kind, buf, fx);
    bool ok = err.empty();
    if (!ok) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
    ImGui::SetNextItemWidth((float)width);
    ImGui::InputText(label, buf, cap);
    if (!ok) {
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", err.c_str());
    }
    return ok;
}

namespace fs = std::filesystem;

// Load a different compbas (a mod's, or stock) as the working set.
bool reloadCompbas(App& a, const std::string& path) {
    mc2w::Compbas nb;
    std::string err;
    if (!nb.load(path, err)) { a.status = "load failed: " + err; return false; }
    a.cb = nb;
    a.selectedRow = -1;
    a.status = "loaded " + path + "  (" + std::to_string(nb.rows.size()) + " components)";
    return true;
}

// Scan modsDir for ALL mod subfolders; flag which define weapons (a compbas.csv)
// and, for the rest, record their mod.json type so the user sees WHY they're not
// editable here (campaign / dependency mods reuse base or a dependency's weapons).
void scanMods(App& a) {
    a.foundMods.clear();
    std::error_code ec;
    if (!fs::is_directory(a.modsDir, ec)) { a.status = std::string("no such mods dir: ") + a.modsDir; return; }
    for (const auto& e : fs::directory_iterator(a.modsDir, ec)) {
        if (!e.is_directory()) continue;
        ModInfo mi;
        mi.name = e.path().filename().string();
        if (!mi.name.empty() && mi.name[0] == '.') continue;  // .import_reports etc.
        std::error_code ec2;
        fs::path cb = e.path() / "data" / "objects" / "compbas.csv";
        if (fs::exists(cb, ec2)) {
            mc2w::Compbas tmp;
            std::string err;
            if (tmp.load(cb.string(), err)) {
                mi.hasWeapons = true;
                for (int i = 0; i < (int)tmp.rows.size(); ++i)
                    if (mc2w::isWeaponType(tmp.cell(i, tmp.idx.type))) ++mi.weapons;
            }
        }
        mi.type = readModType((e.path() / "mod.json").string());
        a.foundMods.push_back(mi);
    }
    std::sort(a.foundMods.begin(), a.foundMods.end(),
              [](const ModInfo& x, const ModInfo& y) {
                  if (x.hasWeapons != y.hasWeapons) return x.hasWeapons > y.hasWeapons;
                  return x.name < y.name;
              });
    int wc = 0;
    for (const auto& m : a.foundMods) if (m.hasWeapons) ++wc;
    a.status = "found " + std::to_string(a.foundMods.size()) + " mod(s), " +
               std::to_string(wc) + " with weapons, in " + a.modsDir;
}

void drawList(App& a) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 780), ImGuiCond_FirstUseEver);
    ImGui::Begin("Weapons");

    if (ImGui::CollapsingHeader("Source / Mods")) {
        // load any compbas by path (a mod's, or stock); Stock = original base.
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##loadpath", a.loadPath, sizeof a.loadPath);
        ImGui::SameLine();
        if (ImGui::Button("Load##cb")) reloadCompbas(a, a.loadPath);
        ImGui::SameLine();
        if (ImGui::Button("Stock")) {
            if (reloadCompbas(a, a.basePath)) { setBuf(a.modId, sizeof a.modId, "my-weapons"); }
        }
        // scan a mods dir, click a mod to load + target it for Save
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("mods dir", a.modsDir, sizeof a.modsDir);
        ImGui::SameLine();
        if (ImGui::Button("Scan")) scanMods(a);
        if (!a.foundMods.empty()) {
            ImGui::TextDisabled("click a weapon mod to load + deploy back to it:");
            ImGui::BeginChild("mods", ImVec2(0, 120), true);
            for (const auto& m : a.foundMods) {
                if (m.hasWeapons) {
                    char lbl[200];
                    std::snprintf(lbl, sizeof lbl, "%s  (%d weapons)##mod", m.name.c_str(), m.weapons);
                    if (ImGui::Selectable(lbl)) {
                        std::string p = std::string(a.modsDir) + "/" + m.name + "/data/objects/compbas.csv";
                        if (reloadCompbas(a, p)) {
                            setBuf(a.modId, sizeof a.modId, m.name.c_str());
                            setBuf(a.modRoot, sizeof a.modRoot, a.modsDir);  // Save -> same mod
                        }
                    }
                } else {
                    // campaign / dependency mods define no weapons of their own
                    ImGui::TextDisabled("%s  (%s - no weapons)", m.name.c_str(),
                                        m.type.empty() ? "no compbas" : m.type.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::TextDisabled("dimmed = campaign/dependency mod; its weapons come from base or a dependency (e.g. Omnitech).");
        }
        ImGui::Separator();
    }

    ImGui::InputText("filter", a.filter, sizeof a.filter);
    ImGui::SameLine();
    ImGui::Checkbox("all components", &a.showAll);
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, 0), false);
    std::string flt;
    for (char* p = a.filter; *p; ++p) flt += (char)tolower((unsigned char)*p);
    for (int i = 0; i < (int)a.cb.rows.size(); ++i) {
        std::string type = a.cb.cell(i, a.cb.idx.type);
        if (!a.showAll && !mc2w::isWeaponType(type)) continue;
        std::string name = a.cb.cell(i, a.cb.idx.name);
        std::string mid = a.cb.cell(i, a.cb.idx.masterID);
        if (!flt.empty()) {
            std::string nl;
            for (char c : name) nl += (char)tolower((unsigned char)c);
            if (nl.find(flt) == std::string::npos) continue;
        }
        char label[160];
        std::snprintf(label, sizeof label, "[%s] %s##%d", mid.c_str(), name.c_str(), i);
        if (ImGui::Selectable(label, a.selectedRow == i)) {
            a.selectedRow = i;
            loadSelectionToBuffers(a);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void drawEditor(App& a) {
    ImGui::SetNextWindowPos(ImVec2(400, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(860, 780), ImGuiCond_FirstUseEver);
    ImGui::Begin("Editor");

    // create-new section
    ImGui::TextUnformatted("New weapon (unused masterID):");
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("masterID##new", a.newId, sizeof a.newId);
    ImGui::SameLine();
    if (ImGui::Button("Create")) {
        int r = a.cb.findByMasterId(a.newId);
        if (r < 0) a.status = std::string("masterID ") + a.newId + " not in compbas";
        else {
            a.selectedRow = r;
            loadSelectionToBuffers(a);
            // seed as a weapon if currently not one
            if (!mc2w::isWeaponType(a.cb.cell(r, a.cb.idx.type))) {
                a.eType = 0;
                setBuf(a.eName, sizeof a.eName, "New Weapon");
                setBuf(a.eDamage, sizeof a.eDamage, "1");
                setBuf(a.eHeat, sizeof a.eHeat, "1");
                setBuf(a.eRecycle, sizeof a.eRecycle, "3");
                setBuf(a.eTons, sizeof a.eTons, "1");
                setBuf(a.eSlots, sizeof a.eSlots, "1");
                a.eRange = 1;
                a.eMType = 0;
                for (int i = 0; i < 4; ++i) a.eFld[i] = false;
                setBuf(a.eIconX, sizeof a.eIconX, "1");
                setBuf(a.eIconY, sizeof a.eIconY, "1");
            }
            a.status = std::string("editing masterID ") + a.newId;
        }
    }
    ImGui::Separator();

    if (a.selectedRow < 0) {
        ImGui::TextUnformatted("Select a weapon from the list, or Create one.");
        ImGui::End();
        return;
    }

    ImGui::Text("masterID %s", a.cb.cell(a.selectedRow, a.cb.idx.masterID).c_str());

    bool ok = true;
    ok &= field("name", a.eName, sizeof a.eName, "str", a.fx, 200);

    ImGui::SetNextItemWidth(160);
    ImGui::Combo("type", &a.eType, mc2w::kWeaponTypes, 3);

    ok &= field("damage", a.eDamage, sizeof a.eDamage, "ufloat", a.fx);
    ok &= field("heat", a.eHeat, sizeof a.eHeat, "ufloat", a.fx);
    ok &= field("recycle (s)", a.eRecycle, sizeof a.eRecycle, "ufloat", a.fx);
    ok &= field("tons", a.eTons, sizeof a.eTons, "ufloat", a.fx);
    ok &= field("crit slots", a.eSlots, sizeof a.eSlots, "uint", a.fx);
    ImGui::SameLine();
    helpMarker("BattleTech critical-slot count (damage transfer). NOT the mech-bay grid size -- that's 'bay grid' below.");

    ImGui::SetNextItemWidth(120);
    ImGui::Combo("range", &a.eRange, mc2w::kRanges, 3);

    // FX picker
    if (!a.fx.empty()) {
        std::string preview;
        {
            char b[96];
            std::snprintf(b, sizeof b, "%d: %s", a.fx[a.eFx].id, a.fx[a.eFx].name.c_str());
            preview = b;
        }
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("FX", preview.c_str())) {
            for (int i = 0; i < (int)a.fx.size(); ++i) {
                char b[96];
                std::snprintf(b, sizeof b, "%d: %s", a.fx[i].id, a.fx[i].name.c_str());
                if (ImGui::Selectable(b, a.eFx == i)) a.eFx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("hit=%s miss=%s", a.fx[a.eFx].hit.c_str(), a.fx[a.eFx].miss.c_str());
    }

    // mech-bay grid footprint (Icon X = columns, Icon Y = rows)
    ImGui::TextUnformatted("bay grid"); ImGui::SameLine();
    ImGui::SetNextItemWidth(46); ImGui::InputText("##ix", a.eIconX, sizeof a.eIconX);
    ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine();
    ImGui::SetNextItemWidth(46); ImGui::InputText("cols x rows##iy", a.eIconY, sizeof a.eIconY);
    {
        bool gridOk = mc2w::validateCell("uint", a.eIconX, a.fx).empty() &&
                      mc2w::validateCell("uint", a.eIconY, a.fx).empty();
        if (!gridOk) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "positive ints");
            ok = false;
        }
    }
    ImGui::SameLine();
    helpMarker("How big the weapon's icon shows in the mech-bay grid (Icon X = columns, Icon Y = rows). e.g. PPC 1x3, Gauss 2x4.");

    // missile type enum (self-documenting dropdown — fixes the false 'SRM not an integer')
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("missileType", &a.eMType, kMTypeLabels, 5);
    ImGui::SameLine();
    helpMarker("Sim sub-class: 0=Energy, 1=Ballistic; LRM / ST (streak) / SRM for missiles. Drives ammo + flight behaviour.");

    // fields bitfield as checkboxes
    ImGui::TextUnformatted("fields"); ImGui::SameLine();
    helpMarker("Special-behaviour flags (bitfield): streak=1, inferno=2, LBX=4, Artillery=8. Most weapons have none.");
    for (int i = 0; i < 4; ++i) { ImGui::SameLine(); ImGui::Checkbox(kFieldLabels[i], &a.eFld[i]); }

    ok &= field("ammoMasterId", a.eAmmo, sizeof a.eAmmo, "int", a.fx, 60);

    ImGui::Separator();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("mod id", a.modId, sizeof a.modId);
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("deploy mods dir", a.modRoot, sizeof a.modRoot);
    ImGui::SameLine();
    helpMarker("Folder Save writes into: <deploy mods dir>/<mod id>/data/objects/compbas.csv (+ mod.json). "
               "Point it at a game install's mods\\ folder to deploy straight there; launch with MC2_ACTIVE_MOD=<mod id>.");
    ImGui::TextDisabled("-> %s/%s/data/objects/compbas.csv", a.modRoot, a.modId);

    if (!ok) ImGui::BeginDisabled();
    if (ImGui::Button("Save overlay")) {
        buffersToRow(a);
        std::string outPath, err;
        if (mc2w::writeOverlay(a.modRoot, a.modId, a.cb, outPath, err))
            a.status = "saved " + outPath + "  (run MC2_ACTIVE_MOD=" + a.modId + ")";
        else
            a.status = "save FAILED: " + err;
    }
    if (!ok) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "fix invalid fields to save");
    }

    if (!a.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", a.status.c_str());
    }
    ImGui::End();
}

int coreOnly(App& a, const char* cbp, const char* efp) {
    std::printf("[mode] core-only (no display)\n");
    std::printf("loaded %zu components from %s\n", a.cb.rows.size(), cbp);
    int weapons = 0;
    for (int i = 0; i < (int)a.cb.rows.size(); ++i)
        if (mc2w::isWeaponType(a.cb.cell(i, a.cb.idx.type))) ++weapons;
    std::printf("  %d weapons; %zu FX entries from %s\n", weapons, a.fx.size(), efp);
    return 0;
}

}  // namespace

// Exercise the C++ load -> edit -> writeOverlay -> reload round-trip (no SDL).
int selftest(App& a, const char* cbp) {
    int r = -1;
    for (int i = 0; i < (int)a.cb.rows.size(); ++i)
        if (mc2w::isWeaponType(a.cb.cell(i, a.cb.idx.type))) { r = i; break; }
    if (r < 0) { std::printf("SELFTEST FAIL: no weapon found\n"); return 1; }
    a.selectedRow = r;
    loadSelectionToBuffers(a);
    std::string mid = a.cb.cell(r, a.cb.idx.masterID);
    setBuf(a.eDamage, sizeof a.eDamage, "42");
    buffersToRow(a);
    std::string outPath, err;
    if (!mc2w::writeOverlay("build64/out/tools/mc2weapon_gui/selftest", "st", a.cb, outPath, err)) {
        std::printf("SELFTEST FAIL: write: %s\n", err.c_str()); return 1;
    }
    mc2w::Compbas re;
    if (!re.load(outPath, err)) { std::printf("SELFTEST FAIL: reload: %s\n", err.c_str()); return 1; }
    int rr = re.findByMasterId(mid);
    std::string got = re.cell(rr, re.idx.damage);
    bool ok = (rr >= 0 && got == "42") && (int)re.rows.size() == (int)a.cb.rows.size();
    std::printf("SELFTEST %s: masterID %s damage='%s' (rows %zu==%zu), overlay=%s\n",
                ok ? "PASS" : "FAIL", mid.c_str(), got.c_str(),
                re.rows.size(), a.cb.rows.size(), outPath.c_str());
    (void)cbp;
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    bool headless = false, doselftest = false;
    int frames = 30;
    const char* cbOverride = nullptr;
    const char* efOverride = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--headless-smoke") headless = true;
        else if (s == "--selftest") doselftest = true;
        else if (s == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (s == "--compbas" && i + 1 < argc) cbOverride = argv[++i];
        else if (s == "--effects" && i + 1 < argc) efOverride = argv[++i];
    }
    const char* cbp = pickPath(kDefaultCompbasCandidates, 3, cbOverride);
    const char* efp = pickPath(kDefaultEffectsCandidates, 3, efOverride);

    App app;
    std::string err;
    if (!app.cb.load(cbp, err)) { std::fprintf(stderr, "load compbas: %s\n", err.c_str()); return 2; }
    if (!mc2w::loadEffects(efp, app.fx, err)) { std::fprintf(stderr, "load effects: %s\n", err.c_str()); return 2; }
    app.basePath = cbp;
    setBuf(app.loadPath, sizeof app.loadPath, cbp);
    // Default the mods dir + deploy target to the INSTALL's mods folder, derived
    // from the exe location (<install>/tools/mc2weapon/exe -> <install>/mods), so
    // it's correct regardless of the working directory. Bundled with releases at
    // <release>/tools/mc2weapon/, so this resolves to <release>/mods.
    {
        std::string ms = (std::filesystem::path(exeDir()) / ".." / ".." / "mods")
                             .lexically_normal().string();
        setBuf(app.modsDir, sizeof app.modsDir, ms.c_str());
        setBuf(app.modRoot, sizeof app.modRoot, ms.c_str());
        std::printf("mc2weapon_gui: default mods dir = %s\n", app.modsDir);
    }
    std::printf("mc2weapon_gui: %zu components, %zu FX from %s / %s\n",
                app.cb.rows.size(), app.fx.size(), cbp, efp);

    if (doselftest) return selftest(app, cbp);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        if (headless) return coreOnly(app, cbp, efp);
        return 3;
    }
    Uint32 winFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (headless || toolvk::captureRequested()) winFlags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow("mc2weapon - weapon editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, winFlags);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit();
                   if (headless) return coreOnly(app, cbp, efp); return 3; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    toolvk::Shell vk;
    if (!toolvk::init(vk, window, "mc2weapon_gui")) {
        std::fprintf(stderr, "Vulkan shell init failed\n");
        toolvk::shutdown(vk); ImGui::DestroyContext(); SDL_DestroyWindow(window); SDL_Quit();
        if (headless) return coreOnly(app, cbp, efp); return 3; }

    bool running = true;
    int frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) running = false;
        }
        if (!toolvk::begin(vk)) { SDL_Delay(10); continue; }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        drawList(app);
        drawEditor(app);

        ImGui::Render();
        if (toolvk::end(vk)) running = false;

        ++frame;
        if (headless && frame >= frames) running = false;
    }
    if (headless) std::printf("rendered %d frames, exiting cleanly\n", frame);

    toolvk::shutdown(vk);
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
