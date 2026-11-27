// BT2018-FIRST-CLASS P3: engine half of the package chassis registry (code/package_chassis.h).
// Enumerates data/defs/chassis/**/chassisdef.fit from mounted mods that declare the
// "package_chassis" capability, parses the registry blocks, sorts by qualified id and
// allocates runtime object-type numbers after the legacy Object2.pak packets. With no such
// mod mounted the registry is empty and every caller keeps its stock path.

#include "mc2_czstring.h"
#include "package_chassis.h"
#include "inifile.h"
#include "file.h"
#include "qualified_id.h"
#include "mc2fmt.h"
#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <filesystem>

namespace {

std::vector<mc2pkg::PackageChassis> g_chassis;
uint64_t g_generation = 0;

std::string readStr(FitIniFile& f, std::string_view key) {
    char buf[256] = {0};
    if (f.readIdString(key, buf, sizeof(buf) - 1) != NO_ERR) return {};
    return buf;
}

long readLong(FitIniFile& f, std::string_view key, long fallback) {
    long v = fallback;
    if (f.readIdLong(key, v) != NO_ERR) v = fallback;
    return v;
}

float readFloat(FitIniFile& f, std::string_view key, float fallback) {
    float v = fallback;
    if (f.readIdFloat(key, v) != NO_ERR) v = fallback;
    return v;
}

void finding(std::string_view path, std::string_view what) {
    mc2::fmt::print_out("[PACKAGE_CHASSIS] {}: {} -- chassis skipped\n", path, what);
}

std::optional<mc2pkg::PackageChassis> parse(std::string_view path) {
    mc2pkg::PackageChassis out;
    FitIniFile f;
    if (f.open(std::string(path).c_str()) != NO_ERR) { finding(path, "cannot open"); return std::nullopt; }
    if (f.seekBlock("Chassis") != NO_ERR) { finding(path, "no [Chassis] block"); return std::nullopt; }
    out.fitPath = path;
    if (f.seekBlock("ObjectClass") == NO_ERR)
        out.objectType = readLong(f, "ObjectTypeNum", 2);
    if (out.objectType != 2 && out.objectType != 3) { finding(path, "unsupported package object class"); return std::nullopt; }
    f.seekBlock("Chassis");
    out.chassisId = mc2id::canonicalizeQualifiedId(readStr(f, "ChassisId"));
    out.stem = readStr(f, "Stem");
    out.balanceProfile = readStr(f, "BalanceProfile");
    out.hudIcon = readStr(f, "HudIcon");
    out.scale = readFloat(f, "Scale", 1.0f);
    out.tonnage = readFloat(f, "Tonnage", 0.0f);
    out.emptyMass = readFloat(f, "EmptyMass", 0.0f);
    out.payload = readFloat(f, "Payload", 0.0f);
    out.layoutRev = static_cast<int>(readLong(f, "LayoutRev", 1));
    bool purch = false;
    if (f.readIdBoolean("Vehicle", out.vehicle) != NO_ERR) out.vehicle = out.objectType == 3;
    if (f.readIdBoolean("Purchasable", purch) == NO_ERR) out.purchasable = purch;
    const long regionCount = readLong(f, "RegionCount", 0);
    const long hardpointCount = readLong(f, "HardpointCount", 0);
    if (regionCount < 0 || regionCount > 36 || hardpointCount < 0 || hardpointCount > 256) {
        finding(path, "invalid region or hardpoint count"); return std::nullopt;
    }
    if (out.chassisId.empty() || !mc2id::isModAuthorable(out.chassisId)) { finding(path, "ChassisId must be a non-core qualified id"); return std::nullopt; }
    if (out.stem.empty()) { finding(path, "no Stem"); return std::nullopt; }
    if (out.payload <= 0.0f) { finding(path, "no Payload"); return std::nullopt; }

    char block[32];
    for (long i = 0; i < regionCount; ++i) {
        mc2::fmt::to_buf(block, "Region{:d}", mc2::fmt::as_i32(i));
        if (f.seekBlock(block) != NO_ERR) { finding(path, "missing [RegionN] block"); return std::nullopt; }
        mc2pkg::Region r;
        r.location = readStr(f, "Location");
        r.x = readLong(f, "X", -1); r.y = readLong(f, "Y", -1);
        r.w = readLong(f, "W", 0);  r.h = readLong(f, "H", 0);
        if (r.location.empty() || r.x < 0 || r.y < 0 || r.w < 1 || r.h < 1 || r.w > 6 || r.h > 6 || r.x > 6 - r.w || r.y > 6 - r.h) {
            finding(path, "region outside the 6x6 mech-lab grid");
            return std::nullopt;
        }
        out.regions.push_back(std::move(r));
    }
    for (long i = 0; i < hardpointCount; ++i) {
        mc2::fmt::to_buf(block, "Hardpoint{:d}", mc2::fmt::as_i32(i));
        if (f.seekBlock(block) != NO_ERR) { finding(path, "missing [HardpointN] block"); return std::nullopt; }
        mc2pkg::Hardpoint h;
        h.id = readStr(f, "Id");
        h.location = readStr(f, "Location");
        h.mount = mc2pkg::mountFromName(readStr(f, "Mount"));
        h.socket = readStr(f, "Socket");
        h.key = mc2pkg::hardpointKey(h.id);
        if (h.id.empty() || h.mount == mc2pkg::Mount::None || h.socket.empty()) { finding(path, "hardpoint needs Id, Mount and Socket"); return std::nullopt; }
        out.hardpoints.push_back(std::move(h));
    }
    if (f.seekBlock("MountTags") == NO_ERR) {
        std::stringstream ss(readStr(f, "AntiPersonnel"));
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) out.antiPersonnelOrdinals.push_back(std::atoi(tok.c_str()));
    }
    return out;
}

} // namespace

void packageChassisEnsureRegistry(long firstFreeTypeNum, long limitExclusive) {
    if (g_generation == mc2::vfs::generation()) return;
    const auto entries = mc2::vfs::enumerate("data/defs/chassis/", ".fit", true);
    g_generation = mc2::vfs::generation();
    std::vector<mc2pkg::PackageChassis> found;
    for (const auto& entry : entries) {
        if (!entry.key.ends_with("/chassisdef.fit")) continue;
        const std::string& p = entry.key;
        // Only a package that declares the capability may register chassis.
        // The host manifest already selected capability-authorized definitions.
        if (entry.mountName != "match-cache" && !ModOwnerHasCapability(p.c_str(), "package_chassis")) {
            finding(p, "owning mod does not declare the package_chassis capability");
            continue;
        }
        auto parsed = parse(p);
        if (!parsed) continue;
        auto& c = *parsed;
        if (auto owner = LookupModOwner(p.c_str())) c.modId = owner;
        bool duplicate = false;
        for (const auto& existing : found)
            if (existing.chassisId == c.chassisId || _stricmp(existing.stem.c_str(), c.stem.c_str()) == 0) duplicate = true;
        if (duplicate) { finding(p, "duplicate chassis id or stem (first wins)"); continue; }
        found.push_back(std::move(c));
    }
    std::sort(found.begin(), found.end(),
              [](const mc2pkg::PackageChassis& a, const mc2pkg::PackageChassis& b) { return a.chassisId < b.chassisId; });
    const size_t n = mc2pkg::allocateTypeNums(found, firstFreeTypeNum, limitExclusive);
    for (const auto& c : found)
        mc2::fmt::print_out("[PACKAGE_CHASSIS] registered {} stem={} objType={:d} regions={:d} hardpoints={:d} payload={:.1f}\n",
                            c.chassisId, c.stem, mc2::fmt::as_i32(c.objTypeNum), mc2::fmt::as_size(c.regions.size()),
                            mc2::fmt::as_size(c.hardpoints.size()), c.payload);
    if (n < found.size())
        mc2::fmt::print_out("[PACKAGE_CHASSIS] {:d} chassis got no object-type number (table full)\n",
                            mc2::fmt::as_size(found.size() - n));
    g_chassis = std::move(found);
}

const std::vector<mc2pkg::PackageChassis>& packageChassisAll() { return g_chassis; }

const mc2pkg::PackageChassis* packageChassisByTypeNum(long objTypeNum) {
    if (objTypeNum < 0) return nullptr;
    for (const auto& c : g_chassis)
        if (c.objTypeNum == objTypeNum) return &c;
    return nullptr;
}

const mc2pkg::PackageChassis* packageChassisByStem(std::string_view stem) {
    if (stem.empty()) return nullptr;
    for (const auto& c : g_chassis)
        if (c.stem.size() == stem.size() && _strnicmp(c.stem.data(), stem.data(), stem.size()) == 0) return &c;
    return nullptr;
}

const mc2pkg::PackageChassis* packageChassisById(std::string_view chassisId) {
    if (chassisId.empty()) return nullptr;
    const std::string want = mc2id::canonicalizeQualifiedId(chassisId);
    for (const auto& c : g_chassis)
        if (c.chassisId == want) return &c;
    return nullptr;
}
