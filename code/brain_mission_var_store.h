#pragma once
// TECHSCRIPT-DISPATCH-1D-M: MissionVarStore — mission-scope Var namespace.
//
// Gate: MC2_BRAIN_VAR_MISSION (default OFF).
//   Gate OFF → Var.Set/Get scope=Mission traces-only (1D behavior unchanged).
//   Gate ON  → Var.Set scope=Mission writes to this global store; Var.Get scope=Mission reads it.
//
// Design: guarded-single-writer (deterministic via WID-order-wins within a tick).
//   Same-tick write conflict: HIGHER wid wins (last-write-in-WID-order-wins).
//   This is threading-ready: even if warriors run in parallel, the higher-WID write always wins.
//   Conflict trace: [BRAIN_VAR_MISSION_CONFLICT] emitted when a lower-WID tries to overwrite
//   a same-tick entry already written by a higher-WID.
//
// Cap: kMissionVarStoreCap (64) entries.
// Layout: {key[32], value[32], lastWriterWID, lastWriteTick}. Linear scan (small cap, fast enough).
// Lifecycle: mission-ephemeral, NOT serialized. Reset via resetMissionVarStore() at mission load.
//   See brain_special_dispatch.cpp: resetMissionVarStore() — called from mission load hook.
//
// Scope isolation: MissionVarStore keys are NOT namespaced by WID (global per-mission).
// Per-unit keys (Unit scope) remain in VarStore (mech_brain_runtime.h), unchanged.

#pragma once
#include "mc2_czstring.h"
#include <cstdint>
#include <cstring>

struct MissionVarEntry {
    char     key[32];            // null-terminated, truncated to 31 chars
    char     value[32];          // raw token text (int/float/bool/string stored as-is)
    int      lastWriterWID;      // WID of last successful writer
    uint32_t lastWriteTick;      // brainTick of last successful write (for conflict detection)
};

struct MissionVarStore {
    static constexpr int kMissionVarStoreCap = 64;

    MissionVarEntry entries[kMissionVarStoreCap];
    int             count = 0;

    void reset() {
        count = 0;
        // Zero the entries for cleanliness (not strictly required, but avoids stale reads on edge cases).
        for (auto & entry : entries) {
            entry.key[0]   = '\0';
            entry.value[0] = '\0';
            entry.lastWriterWID  = 0;
            entry.lastWriteTick  = 0;
        }
    }

    // Linear-scan find by key. Returns pointer or nullptr.
    MissionVarEntry* find(mc2::czstring k) {
        for (int i = 0; i < count; ++i)
            if (std::strncmp(entries[i].key, k, 31) == 0)
                return &entries[i];
        return nullptr;
    }

    const MissionVarEntry* find(mc2::czstring k) const {
        for (int i = 0; i < count; ++i)
            if (std::strncmp(entries[i].key, k, 31) == 0)
                return &entries[i];
        return nullptr;
    }

    // Returns current value string for key, or "0" if not found.
    const char* get(mc2::czstring k) const {
        const MissionVarEntry* e = find(k);
        return e ? e->value : "0";
    }

    // Returns false if cap reached (soft-fail).
    // Conflict policy: within the same tick, HIGHER wid wins.
    // If writing from a lower WID than the current entry's lastWriterWID on the same tick,
    // that is a conflict — the higher-WID value is preserved and the caller should emit
    // [BRAIN_VAR_MISSION_CONFLICT]. Returns true (key exists, no cap issue) but the
    // caller is responsible for checking and skipping the actual write.
    // outConflict: set to true if caller should skip write + emit conflict trace.
    bool trySet(mc2::czstring k, mc2::czstring v, int writerWID, uint32_t tick, bool* outConflict) {
        *outConflict = false;
        MissionVarEntry* e = find(k);
        if (e) {
            // Same tick and lower WID → conflict, higher WID wins (existing value preserved).
            if (tick == e->lastWriteTick && writerWID < e->lastWriterWID) {
                *outConflict = true;
                return true; // cap not hit, but write suppressed
            }
            // Overwrite (same tick higher WID, or different tick = last-write-wins).
            std::strncpy(e->value, v, 31);
            e->value[31]       = '\0';
            e->lastWriterWID   = writerWID;
            e->lastWriteTick   = tick;
            return true;
        }
        // New entry.
        if (count >= kMissionVarStoreCap)
            return false; // cap hit
        MissionVarEntry& ne = entries[count++];
        std::strncpy(ne.key,   k, 31); ne.key[31]   = '\0';
        std::strncpy(ne.value, v, 31); ne.value[31] = '\0';
        ne.lastWriterWID = writerWID;
        ne.lastWriteTick = tick;
        return true;
    }
};

// Global mission-scope Var store (mission-ephemeral).
// Defined in brain_special_dispatch.cpp. Extern declaration here for harness linkage.
extern MissionVarStore g_missionVarStore;
