#pragma once

// Main-thread-only diagnostic mirror. Never feeds simulation state or SIMID.
#include <array>
#include <cstdint>
#include "../../Gates/gate.h"
#include <sstream>
#include <string>

namespace mc2::observation {
inline bool enabled() {
    static const bool value = mc2::gate::get<"MC2_DEBUG_STATE_DUMP">();
    return value;
}
struct Zone { const char* name = nullptr; uint64_t ns = 0; };
struct Sample {
    uint64_t frame = 0;
    long movers = 0, pilots = 0, brains = 0;
    bool censusValid = false;
    std::array<Zone, 16> zones{};
    unsigned overflow = 0;
};
inline Sample current;
inline void beginFrame() {
    const auto next = current.frame + 1;
    current = {};
    current.frame = next;
}
inline void recordZone(unsigned index, const char* name, uint64_t ns) {
    if (index >= current.zones.size()) { ++current.overflow; return; }
    current.zones[index].name = name;
    current.zones[index].ns += ns;
}
inline std::string json() {
    std::ostringstream out;
    out << "{\"version\":1,\"mission_frame\":" << current.frame
        << ",\"census_valid\":" << (current.censusValid ? "true" : "false")
        << ",\"movers\":" << current.movers << ",\"pilots\":" << current.pilots
        << ",\"brains\":" << current.brains << ",\"zone_overflow\":" << current.overflow
        << ",\"zone_ns\":{";
    bool first = true;
    for (const auto& zone : current.zones) {
        if (!zone.name) continue;
        if (!first) out << ',';
        first = false;
        out << '"' << zone.name << "\":" << zone.ns;
    }
    out << "}}";
    return out.str();
}
} // namespace mc2::observation
