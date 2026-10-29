// S3 clock slice: V2 packet 3 is a value record; pre-V2 saves retain the native reader.
#pragma once

#include "combat_retirement.h"
#include "save_sections.h"

#include <cstring>

namespace mc2::save {

struct CombatClockValue {
    std::uint64_t ticks = 0;
    double frac = 0;

    void serialize(Archive& archive) {
        archive(ticks, frac);
        if (archive.good() && !combat::SavedCombatClock{ticks, frac}.valid())
            archive.reject("combat_clock");
    }
};

// Preserve the packet cursor on both paths, including malformed records. The fixed
// 16-byte LE value representation matches the Windows legacy clock bytes exactly.
template<class Packet>
std::optional<combat::SavedCombatClock> readSavedCombatClock(Packet& file) {
    const auto resume = file.getCurrentPacket();
    char stamp[sizeof(saveStampV2)]{};
    const bool v2 = file.seekPacket(0) == 0 && file.getPacketSize() == sizeof(stamp) &&
        file.readPacket(0, reinterpret_cast<unsigned char*>(stamp)) == sizeof(stamp) &&
        std::memcmp(stamp, saveStampV2, sizeof(stamp)) == 0;
    file.seekPacket(resume);
    if (!v2) return combat::readCombatClock(file);
    std::array<std::uint8_t, 16> bytes{};
    CombatClockValue clock;
    const bool valid = file.getNumPackets() > combat::combatClockPacket &&
        file.seekPacket(combat::combatClockPacket) == 0 && file.getPacketSize() == bytes.size() &&
        file.readPacket(combat::combatClockPacket, bytes.data()) == bytes.size() &&
        decodeRecord(bytes, clock);
    file.seekPacket(resume);
    return valid ? std::optional(combat::SavedCombatClock{clock.ticks, clock.frac}) : std::nullopt;
}

} // namespace mc2::save
