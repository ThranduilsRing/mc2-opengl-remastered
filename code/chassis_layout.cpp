#include "chassis_layout.h"
#include "inifile.h"

namespace mc2pkg {
namespace {
std::string text(FitIniFile& f, const std::string& key) {
    char value[512]{};
    return f.readIdString(key, value, sizeof(value) - 1) == NO_ERR ? value : "";
}
bool number(FitIniFile& f, const std::string& key, int& out, int low, int high) {
    long value = 0;
    if (f.readIdLong(key, value) != NO_ERR || value < low || value > high) return false;
    out = static_cast<int>(value); return true;
}
unsigned maskOf(const std::string& text) {
    unsigned mask = 0;
    std::size_t begin = 0;
    while (begin < text.size()) {
        auto end = text.find('|', begin);
        if (end == std::string::npos) end = text.size();
        const auto tag = text.substr(begin, end - begin);
        unsigned bit = tag == "Energy" ? Energy : tag == "Ballistic" ? Ballistic :
            tag == "Missile" ? Missile : tag == "AntiPersonnel" ? AntiPersonnel : tag == "AMS" ? AMS : 0u;
        if (!bit || (mask & bit)) return 0;
        mask |= bit; begin = end + 1;
    }
    return !text.empty() && text.back() != '|' ? mask : 0;
}
}
std::optional<ChassisLayout> readChassisLayout(FitIniFile& f, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<ChassisLayout> {
        if (error) *error = why; return std::nullopt;
    };
    ChassisLayout out;
    int version = 0;
    float payload = 0;
    if (f.seekBlock("Layout") != NO_ERR) return fail("missing Layout");
    out.chassisId = text(f, "ChassisId"); out.source = text(f, "Source");
    if (out.chassisId.empty() || out.chassisId.find(':') == std::string::npos || out.source.empty() ||
        !number(f, "Version", version, 1, 1)) return fail("invalid Layout identity/version/source");
    if (f.readIdFloat("Payload", payload) != NO_ERR || !std::isfinite(payload) || payload < 0)
        return fail("Payload required, finite and nonnegative");
    out.payload = payload;
    if (!number(f, "ArmorPool", out.armorPool, 0, 11 * 255)) return fail("invalid ArmorPool");
    for (int loc = 0; loc < 8; ++loc) {
        const std::string block = std::string("Location.") + locationNames[loc];
        if (f.seekBlock(block.c_str()) != NO_ERR) return fail("missing " + block);
        auto& l = out.locations[loc];
        int count = 0;
        if (!number(f, "Slots", l.slots, 0, 4096) || !number(f, "Structure", l.structure, 0, 255) ||
            !number(f, "ArmorMax", l.armorMax, 0, 255) || !number(f, "ArmorRearMax", l.armorRearMax, 0, 255) ||
            !number(f, "HardpointCount", count, 0, 12)) return fail("invalid " + block);
        if ((loc == 0 || loc >= 4) && l.armorRearMax) return fail("rear armor only on torsos");
        for (int n = 0; n < count; ++n) {
            LocationMount h;
            h.mask = maskOf(text(f, "Hardpoint" + std::to_string(n)));
            if (!h.mask) return fail("unknown or empty hardpoint mask in " + block);
            h.key = hardpointKey(std::string(locationNames[loc]) + "." + std::to_string(n));
            h.socket = text(f, "Socket" + std::to_string(n));
            l.hardpoints.push_back(std::move(h));
        }
    }
    if (error) error->clear();
    return out;
}
std::optional<ChassisLayout> loadChassisLayout(std::string_view path, std::string* error) {
    FitIniFile f;
    if (f.open(std::string(path).c_str()) != NO_ERR) {
        if (error) *error = "cannot open layout";
        return std::nullopt;
    }
    return readChassisLayout(f, error); // FitIniFile uses the normal mounted VFS
}

bool readLocationVariant(FitIniFile& f, LocationVariant& out) {
    LocationVariant result;
    int count = 0;
    if (!number(f, "ComponentCount", count, 0, 54)) return false;
    long version = 1;
    f.readIdLong("LayoutVersion", version);
    if (version != 1 && version != 2) return false;
    result.version = static_cast<int>(version);
    for (int i = 0; i < count; ++i) {
        VariantItem item;
        const auto key = "Component" + std::to_string(i);
        if (!number(f, key, item.id, 0, 65535) ||
            !number(f, key + "x", item.x, -32768, 32767) ||
            !number(f, key + "y", item.y, -32768, 32767) ||
            !number(f, key + "Location", item.legacyLocation, 0, 2)) return false;
        if (version == 2) {
            if (!number(f, key + "Loc", item.location, -1, 7) ||
                !number(f, key + "Slot", item.slot, -1, 11)) return false;
            // FIT's ul writer narrows through as_i32. Decimal text preserves
            // all 32 hash bits identically on Windows and Linux.
            const auto hp = text(f, key + "Hp");
            if (hp.empty() || hp.size() > 10) return false;
            std::uint64_t value = 0;
            for (char digit : hp) {
                if (digit < '0' || digit > '9') return false;
                value = value * 10 + static_cast<unsigned>(digit - '0');
            }
            if (value > 0xffffffffu) return false;
            item.hardpoint = static_cast<unsigned>(value);
        }
        result.items.push_back(item);
    }
    if (version == 2) {
        for (int i = 0; i < 11; ++i)
            if (!number(f, "Armor" + std::to_string(i), result.armor[i], 0, 255)) return false;
        long grandfathered = 0, edited = 0;
        f.readIdLong("LayoutGrandfathered", grandfathered);
        f.readIdLong("LayoutEdited", edited);
        result.grandfathered = grandfathered != 0; result.edited = edited != 0;
    }
    out = std::move(result); return true;
}
bool writeLocationVariant(FitIniFile& f, const LocationVariant& record) {
    if (record.items.size() > 54) return false;
    bool ok = true;
    auto write = [&](const std::string& key, long value) { if (f.writeIdLong(key, value) <= 0) ok = false; };
    write("LayoutVersion", 2);
    write("ComponentCount", static_cast<long>(record.items.size()));
    write("LayoutGrandfathered", record.grandfathered); write("LayoutEdited", record.edited);
    for (std::size_t i = 0; i < record.items.size(); ++i) {
        const auto& item = record.items[i]; const auto key = "Component" + std::to_string(i);
        write(key, item.id); write(key + "x", item.x); write(key + "y", item.y);
        write(key + "Location", item.legacyLocation); write(key + "Loc", item.location); write(key + "Slot", item.slot);
        if (f.writeIdString(key + "Hp", std::to_string(item.hardpoint).c_str()) <= 0) ok = false;
    }
    for (int i = 0; i < 11; ++i) write("Armor" + std::to_string(i), record.armor[i]);
    return ok;
}
} // namespace mc2pkg
