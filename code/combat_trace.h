#pragma once

#include "diagnostic_trace.h"
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace mc2 {

// Each pooled combat object owns its trace identity so reuse cannot share sequences.
struct CombatTrace {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    bool live = false;
    long type = -1;
    long effect = -1;

    void begin(long objectType, long effectId)
    {
        type = objectType;
        effect = effectId;
        ++generation;
        sequence = 0;
        live = true;
    }

    template<class Vector, class Flight, class Target, class Shot>
    void write(const char* kind, long slot, long tick, const char* event, const char* reason,
               const Vector& position, Flight getFlight, Target target, const Shot* shot)
    {
        if (!live || !mc2_diag::tagEnabled("VFX_COMBAT")) return;
        // Birth omits position: pooled objects have not acquired their new coordinates yet.
        const bool birth = std::strcmp(event, "birth") == 0;
        const auto* flight = birth ? nullptr : getFlight();
        const auto snapshot = birth ? std::string{} : std::format(
            ",\"position_bits\":[{},{},{}]", std::bit_cast<std::uint32_t>(position.x),
            std::bit_cast<std::uint32_t>(position.y), std::bit_cast<std::uint32_t>(position.z));
        const auto extra = birth || !flight ? std::string{} : std::format(
            ",\"flight_bits\":[{},{},{}]", std::bit_cast<std::uint32_t>(flight->x),
            std::bit_cast<std::uint32_t>(flight->y), std::bit_cast<std::uint32_t>(flight->z));
        std::string damage;
        if (target && shot) {
            const auto targetPosition = target->getPosition();
            damage = std::format(
                ",\"target\":{},\"target_wid\":{},\"damage_bits\":{},\"location\":{},"
                "\"master\":{},\"attacker\":{},\"entry_bits\":{},\"target_position_bits\":[{},{},{}]",
                target->getHandle(), target->getWatchID(false), std::bit_cast<std::uint32_t>(shot->damage),
                shot->hitLocation, shot->masterId, shot->attackerWID, std::bit_cast<std::uint32_t>(shot->entryAngle),
                std::bit_cast<std::uint32_t>(targetPosition.x), std::bit_cast<std::uint32_t>(targetPosition.y),
                std::bit_cast<std::uint32_t>(targetPosition.z));
        }
        // Complete JSON keeps sequence and field order auditable; fixed buffers could truncate it.
        const auto data = std::format(
            "{{\"kind\":\"{}\",\"slot\":{},\"generation\":{},\"seq\":{},\"tick\":{},"
            "\"event\":\"{}\",\"reason\":\"{}\",\"effect_id\":{},\"type\":{}{}{}{}}}",
            kind, slot, generation, sequence++, tick, event, reason, effect, type, snapshot, extra, damage);
        mc2_diag::writeEvent("VFX_COMBAT", 1, 0, data.c_str());
    }
};

} // namespace mc2
