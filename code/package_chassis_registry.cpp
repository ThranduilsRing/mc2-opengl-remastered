// BT2018-FIRST-CLASS P3: engine half of the package chassis registry (code/package_chassis.h).
// Enumerates data/defs/chassis/**/chassisdef.fit from mounted mods that declare the
// "package_chassis" capability, parses the registry blocks, sorts by qualified id and
// allocates runtime object-type numbers after the legacy Object2.pak packets. With no such
// mod mounted the registry is empty and every caller keeps its stock path.

#include "package_chassis.h"
#include "inifile.h"
#include "file.h"
#include "qualified_id.h"
#include "mc2fmt.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace {

std::vector<mc2pkg::PackageChassis> g_chassis;
bool g_built = false;

void collectChassisDef(const char* absPath, void* user) {
    const char* base = absPath;
    for (const char* p = absPath; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (_stricmp(base, "chassisdef.fit") == 0)
        static_cast<std::vector<std::string>*>(user)->push_back(absPath);
}

std::string readStr(FitIniFile& f, const char* key) {
    char buf[256] = {0};
    if (f.readIdString(key, buf, sizeof(buf) - 1) != NO_ERR) return {};
    return buf;
}

long readLong(FitIniFile& f, const char* key, long fallback) {
    long v = fallback;
    if (f.readIdLong(key, v) != NO_ERR) v = fallback;
    return v;
}

float readFloat(FitIniFile& f, const char* key, float fallback) {
    float v = fallback;
    if (f.readIdFloat(key, v) != NO_ERR) v = fallback;
    return v;
}

void finding(const std::string& path, const char* what) {
    mc2::fmt::print_out("[PACKAGE_CHASSIS] {}: {} -- chassis skipped\n", path, what);
}

bool parse(const std::string& path, mc2pkg::PackageChassis& out) {
    FitIniFile f;
    if (f.open(path.c_str()) != NO_ERR) { finding(path, "cannot open"); return false; }
    if (f.seekBlock("Chassis") != NO_ERR) { finding(path, "no [Chassis] block"); return false; }
    out.fitPath = path;
    out.chassisId = mc2id::canonicalizeQualifiedId(readStr(f, "ChassisId"));
    out.stem = readStr(f, "Stem");
    out.balanceProfile = readStr(f, "BalanceProfile");
    out.hudIcon = readStr(f, "HudIcon");
    out.scale = readFloat(f, "Scale", 1.0f);
    out.tonnage = readFloat(f, "Tonnage", 0.0f);
    out.emptyMass = readFloat(f, "EmptyMass", 0.0f);
    out.payload = readFloat(f, "Payload", 0.0f);
    bool purch = false;
    if (f.readIdBoolean("Purchasable", purch) == NO_ERR) out.purchasable = purch;
    const long regionCount = readLong(f, "RegionCount", 0);
    const long hardpointCount = readLong(f, "HardpointCount", 0);
    if (out.chassisId.empty() || !mc2id::isModAuthorable(out.chassisId)) { finding(path, "ChassisId must be a non-core qualified id"); return false; }
    if (out.stem.empty()) { finding(path, "no Stem"); return false; }
    if (out.payload <= 0.0f) { finding(path, "no Payload"); return false; }

    char block[32];
    for (long i = 0; i < regionCount; ++i) {
        mc2::fmt::to_buf(block, "Region{:d}", mc2::fmt::as_i32(i));
        if (f.seekBlock(block) != NO_ERR) { finding(path, "missing [RegionN] block"); return false; }
        mc2pkg::Region r;
        r.location = readStr(f, "Location");
        r.x = readLong(f, "X", -1); r.y = readLong(f, "Y", -1);
        r.w = readLong(f, "W", 0);  r.h = readLong(f, "H", 0);
        if (r.location.empty() || r.x < 0 || r.y < 0 || r.w < 1 || r.h < 1 || r.x + r.w > 6 || r.y + r.h > 6) {
            finding(path, "region outside the 6x6 mech-lab grid");
            return false;
        }
        out.regions.push_back(std::move(r));
    }
    for (long i = 0; i < hardpointCount; ++i) {
        mc2::fmt::to_buf(block, "Hardpoint{:d}", mc2::fmt::as_i32(i));
        if (f.seekBlock(block) != NO_ERR) { finding(path, "missing [HardpointN] block"); return false; }
        mc2pkg::Hardpoint h;
        h.id = readStr(f, "Id");
        h.location = readStr(f, "Location");
        h.mount = mc2pkg::mountFromName(readStr(f, "Mount"));
        h.socket = readStr(f, "Socket");
        h.key = mc2pkg::hardpointKey(h.id);
        if (h.id.empty() || h.mount == mc2pkg::Mount::None || h.socket.empty()) { finding(path, "hardpoint needs Id, Mount and Socket"); return false; }
        out.hardpoints.push_back(std::move(h));
    }
    if (f.seekBlock("MountTags") == NO_ERR) {
        std::stringstream ss(readStr(f, "AntiPersonnel"));
        std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) out.antiPersonnelOrdinals.push_back(std::atoi(tok.c_str()));
    }
    return true;
}

} // namespace

void packageChassisEnsureRegistry(long firstFreeTypeNum, long limitExclusive) {
    if (g_built) return;
    g_built = true;
    if (!ModCapabilityMounted("package_chassis")) return;

    std::vector<std::string> paths;
    EnumerateModFitFiles("data/defs/chassis/", collectChassisDef, &paths);
    std::vector<mc2pkg::PackageChassis> found;
    for (const std::string& p : paths) {
        // Only a package that declares the capability may register chassis.
        std::string key = p;
        std::replace(key.begin(), key.end(), '\\', '/');
        for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const size_t at = key.find("data/defs/chassis/");
        if (at == std::string::npos || !ModOwnerHasCapability(key.c_str() + at, "package_chassis")) {
            finding(p, "owning mod does not declare the package_chassis capability");
            continue;
        }
        mc2pkg::PackageChassis c;
        if (!parse(p, c)) continue;
        if (const char* owner = LookupModOwner(key.c_str() + at)) c.modId = owner;
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

const mc2pkg::PackageChassis* packageChassisByStem(const char* stem) {
    if (!stem) return nullptr;
    for (const auto& c : g_chassis)
        if (_stricmp(c.stem.c_str(), stem) == 0) return &c;
    return nullptr;
}

const mc2pkg::PackageChassis* packageChassisById(const char* chassisId) {
    if (!chassisId) return nullptr;
    const std::string want = mc2id::canonicalizeQualifiedId(chassisId);
    for (const auto& c : g_chassis)
        if (c.chassisId == want) return &c;
    return nullptr;
}
