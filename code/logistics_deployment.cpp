// Logistics deployment: campaign save mech records (write/read, legacy 10004 and 10005), load compatibility and the slot cap.
#include "logistics_deployment.h"
#include "inifile.h"
#include "objective_text.h"
#include "mc2str.h"
#include <array>
#include <vector>

namespace logistics_deployment {
void writeMech(FitIniFile& file, long index, const MechRecord& record)
{
    const auto block = "Inventory" + std::to_string(index);
    file.writeBlock(block.c_str());
    file.writeIdString("Chassis", record.chassis.c_str());
    file.writeIdString("Variant", record.variant.c_str());
    if (record.pilot)
        file.writeIdString("Pilot", record.pilot->c_str());
    file.writeIdBoolean("Designer", record.designer);
    file.writeIdLong("DeploymentSlot", record.slot);
}

std::optional<MechRecord> readMech(FitIniFile& file, long version)
{
    MechRecord record;
    record.chassis = objective_text::readPlain(file, "Chassis").value_or("");
    const auto variant = objective_text::readPlain(file, "Variant");
    if (!variant)
        return std::nullopt;
    record.variant = *variant;
    record.pilot = objective_text::readPlain(file, "Pilot");
    file.readIdBoolean("Designer", record.designer);
    if (version == recordVersion &&
        (file.readIdLong("DeploymentSlot", record.slot) != NO_ERR ||
         record.slot < 0 || record.slot > singlePlayerSlots))
        return std::nullopt;
    return record;
}

bool compatible(FitIniFile& file, long version, long inventoryCount)
{
    if (version != legacyVersion && version != recordVersion)
        return false;
    // 10004 retains its original permissive reconstruction, including pilots.
    if (version == legacyVersion)
        return true;
    if (inventoryCount < 0)
        return false;
    std::array<bool, singlePlayerSlots + 1> slots{};
    std::vector<std::string> pilots;
    for (long i = 0; i < inventoryCount; ++i) {
        const auto block = "Inventory" + std::to_string(i);
        if (file.seekBlock(block.c_str()) != NO_ERR)
            return false;
        const auto record = readMech(file, version);
        if (!record)
            return false;
        if (record->slot && slots[record->slot])
            return false;
        slots[record->slot] = record->slot != 0;
        if (record->pilot && !record->pilot->empty()) {
            for (const auto& pilot : pilots)
                if (mc2::str::iequals(pilot, *record->pilot))
                    return false;
            pilots.push_back(*record->pilot);
        }
    }
    return true;
}

int readCap(FitIniFile& file, int fallback)
{
    long value = fallback;
    if (file.readIdLong("MaxDeployedMechs", value) != NO_ERR)
        return fallback;
    return static_cast<int>(std::clamp(value, 0L, static_cast<long>(singlePlayerSlots)));
}
} // namespace logistics_deployment
