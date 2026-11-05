#ifndef CAPABILITY_H
#define CAPABILITY_H
// UNIT-PROFILE-SEAM-1: first data-owned unit facet.
// Capabilities are ABILITIES ONLY. Structural truths (locomotion, location
// schema, sensors, UI, repair, AI role) are SEPARATE facets, never bits here.
// Slice 1 enumerates ONLY the capability it wires. Do not add speculative
// bits — the audit (Task 0) decides each concept's facet before it is added.
#include <cstdint>

enum Capability {
    CAP_JUMP = 0,
    CAP__COUNT
};

// Public gameplay intents. canPerform(UnitAction) requires the capability
// fact AND a working executor (see Mover).
enum class UnitAction { Jump = 0 };

// Bitmask is PRIVATE. Consumers use has()/set()/clear() only, so a future
// string-key registry + JSON values is an internal swap with no churn.
class CapabilitySet {
    uint32_t mask_ = 0;
  public:
    void set(Capability c, bool on) {
        const uint32_t bit = (static_cast<uint32_t>(1) << static_cast<uint32_t>(c));
        if (on) mask_ |= bit; else mask_ &= ~bit;
    }
    bool has(Capability c) const {
        return (mask_ & (static_cast<uint32_t>(1) << static_cast<uint32_t>(c))) != 0;
    }
    void clear() { mask_ = 0; }
};
#endif // CAPABILITY_H
