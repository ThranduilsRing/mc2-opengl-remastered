// EQUIPMENT-REGISTRY-FACADE-1 (equipment arc, engine slice E2) -- engine side.
//
// Populates a process-global EquipmentRegistry by walking the ALREADY-LOADED
// MasterComponent::masterList[0..numComponents), one registry entry per row.
// PURELY ADDITIVE: reads masterList; mutates nothing in it; touches no
// InventoryItem / save / firing / loadout path. Built + dumped only when the
// MC2_UNIT_PROFILE_DATA gate is ON, so gate-OFF is byte-identical.
//
// The pure registry + id/kind derivation live in equipment_registry.h (offline
// doctest-tested in mc2_tests). This TU is the ONLY place that reads the engine
// MasterComponent type.

#include "equipment_registry.h"
#include "unitprofile.h"           // mc2UnitProfileDataEnabled()
#include "cmponent.h"              // MasterComponent + masterList

#include <cstdio>

// Process-global registry (parallel index; queried by future slices, not by the
// runtime firing/loadout path). Empty until buildEquipmentRegistryFromMasterList
// runs under the gate.
static EquipmentRegistry g_equipmentRegistry;

const EquipmentRegistry& getEquipmentRegistry() {
    return g_equipmentRegistry;
}

// Walk masterList and (re)build the registry. Additive: reads only.
// Returns the number of entries added.
size_t buildEquipmentRegistryFromMasterList() {
    g_equipmentRegistry.clear();

    if (!MasterComponent::masterList)
        return 0;

    for (long i = 0; i < MasterComponent::numComponents; ++i) {
        MasterComponent& mc = MasterComponent::masterList[i];

        // A default-off / placeholder row is one the load loop left empty: the
        // "undefined" compbas rows reset masterID to -1 and never set a name
        // (cmponent.cpp initEXCEL). Everything else is an enabled component.
        long   ord     = mc.getMasterID();
        const char* nm = mc.getName();
        bool   enabled = (ord >= 0) && nm && nm[0] != '\0';

        // legacyMasterId is the ROW ORDINAL (the 8-bit addressable id), which is
        // the loop index i for populated rows. For placeholder rows getMasterID()
        // is -1; use the row index i as the ordinal alias so every slot is still
        // addressable, and mark it disabled.
        long legacyMasterId = i;

        EquipmentKind kind = componentFormToKind((int)mc.getForm());
        std::string qid    = coreQualifiedIdFor(nm, legacyMasterId);

        g_equipmentRegistry.add(qid, legacyMasterId, kind, enabled);
    }

    return g_equipmentRegistry.count();
}

// Gated build + verification dump. Called after loadMasterList. Gate-OFF: no-op
// (registry stays empty, nothing printed) -> byte-identical parity.
void buildAndDumpEquipmentRegistryIfEnabled() {
    if (!mc2UnitProfileDataEnabled())
        return;

    size_t n = buildEquipmentRegistryFromMasterList();
    const EquipmentRegistry& reg = g_equipmentRegistry;

    printf("[EQUIPMENT-REGISTRY-FACADE-1] populated: entries=%zu  withLegacyMasterId=%zu  enabled=%zu  defaultOff=%zu\n",
           n, reg.withLegacyMasterIdCount(), reg.enabledCount(), reg.disabledCount());

    // A few sample ordinal->core:id pairs (first enabled entries) so we can
    // eyeball that ids match the converter's core:<slug> output.
    int shown = 0;
    for (const auto& e : reg.entries()) {
        if (!e.enabled) continue;
        printf("[EQUIPMENT-REGISTRY-FACADE-1]   sample %ld -> %s\n",
               e.legacyMasterId, e.qualifiedId.c_str());
        if (++shown >= 8) break;
    }
    if (shown == 0)
        printf("[EQUIPMENT-REGISTRY-FACADE-1]   (no enabled entries -- masterList not loaded?)\n");
}
