#include "mc2_czstring.h"
#include "mech_brain_runtime.h"
#include "brain_execution_policy.h"
#include <cstdio>

uint32_t getBrainTickIndex();


// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global definition.
// Mission-ephemeral: reset via resetMissionVarStore() at each mission load.
// Not serialized.
MissionVarStore g_missionVarStore;

// Gate helper for MC2_BRAIN_VAR_MISSION (1D-M).
// Requires MC2_BRAIN_DISPATCH_VAR=1 to be meaningful, but has its own separate gate
// so it can be turned on independently. Warns if turned on without DISPATCH_VAR.
bool s_missionVarGate() { return mc2::brainpolicy::techBrain(); }   // BrainExecutionPolicy (was a gate)

// resetMissionVarStore — clears g_missionVarStore.
// Call at mission load before warriors execute their first tick.
// Safe to call at any time (no-op if store is already empty).
void resetMissionVarStore() {
    g_missionVarStore.reset();
    std::fprintf(stderr, "[BRAIN_VAR_MISSION_RESET] g_missionVarStore cleared\n");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: parseVarVerb
//
// Parses "Var.Set \"<key>\" <value> [scope=Mission]" or
//         "Var.Get \"<key>\" [scope=Mission]"
// into outKey, outValue, outScope.
// outValue is only meaningful for Var.Set; for Var.Get it is set to "".
// outScope: 0=Unit (VarScope::Unit), 1=Mission (VarScope::Mission).
// Returns true on success; false on malformed input (empty key; missing value for Set).
//
// FORBIDDEN-CALL CONTRACT: calls ONLY std::strchr/strstr/strncpy/snprintf. No orders.
bool parseVarVerb(mc2::czstring verbStr, char outKey[32], char outValue[32], uint8_t* outScope) {
    outKey[0] = '\0';
    outValue[0] = '\0';
    *outScope = 0;  // default Unit

    // Determine whether this is Var.Set or Var.Get.
    bool isSet = (std::strncmp(verbStr, "Var.Set", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    bool isGet = (std::strncmp(verbStr, "Var.Get", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    if (!isSet && !isGet)
        return false;

    // Skip verb token + whitespace.
    const char* p = verbStr + 7;
    while (*p == ' ' || *p == '\t') ++p;

    // Parse key token — supports two forms:
    //   "key"   — double-quoted (canonical spec form; used in non-FIT contexts)
    //   [key]   — bracket-quoted (FIT-safe form; FIT outer-quotes can't embed '"')
    char closeDelim = '\0';
    if (*p == '"') {
        closeDelim = '"';
    } else if (*p == '[') {
        closeDelim = ']';
    } else {
        return false;  // unrecognized key delimiter
    }
    ++p;  // skip opening delimiter
    const char* keyStart = p;
    const char* keyEnd = std::strchr(p, closeDelim);
    if (!keyEnd)
        return false;
    size_t keyLen = static_cast<size_t>((keyEnd - keyStart));
    if (keyLen == 0)
        return false;
    keyLen = std::min<size_t>(keyLen, 31);
    std::strncpy(outKey, keyStart, keyLen);
    outKey[keyLen] = '\0';

    p = keyEnd + 1;  // skip closing delimiter

    // Skip whitespace between key and value/scope.
    while (*p == ' ' || *p == '\t') ++p;

    if (isSet) {
        // Parse value token (whitespace-delimited; may be quoted or unquoted).
        if (*p == '\0')
            return false;  // missing value for Set

        if (*p == '"') {
            // Quoted value.
            ++p;
            const char* vStart = p;
            const char* vEnd = std::strchr(p, '"');
            if (!vEnd) return false;
            size_t vLen = static_cast<size_t>((vEnd - vStart));
            vLen = std::min<size_t>(vLen, 31);
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
            p = vEnd + 1;
        } else {
            // Unquoted value — read until whitespace.
            const char* vStart = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            size_t vLen = static_cast<size_t>((p - vStart));
            vLen = std::min<size_t>(vLen, 31);
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
        }

        // Skip whitespace after value.
        while (*p == ' ' || *p == '\t') ++p;
    }

    // Detect optional "scope=Mission" trailing token.
    if (std::strncmp(p, "scope=Mission", 13) == 0)
        *outScope = 1;  // VarScope::Mission

    return true;
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarSet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->set + fprintf/fflush. No order functions.
// Verified by inspection: no warrior pointer, no TacticalOrder, no slot writes.
void handleVarSet(mc2::czstring key, mc2::czstring value, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real write to g_missionVarStore.
        // Gate OFF → trace-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=%s value=%s wid=%d (write deferred -- TechBrain policy off)\n",
                         key, value, wid);
            std::fflush(stderr);
            return;
        }
        // Guarded-single-writer: WID-order-wins within same tick.
        // brainTick via getBrainTickIndex() — available in dispatch.cpp context.
        uint32_t tick = getBrainTickIndex();
        bool conflict = false;
        bool ok = g_missionVarStore.trySet(key, value, wid, tick, &conflict);
        if (conflict) {
            // A higher-WID already wrote this key this tick — lower WID suppressed.
            // Retrieve the winning value and prevWID for the conflict trace.
            const MissionVarEntry* e = g_missionVarStore.find(key);
            int prevWid = e ? e->lastWriterWID : -1;
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_CONFLICT] key=%s wid=%d prevWid=%d tick=%u (higher-WID wins; this write suppressed)\n",
                         key, wid, prevWid, tick);
            std::fflush(stderr);
            return;
        }
        if (!ok) {
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET_FULL] key=%s wid=%d (cap=%d reached -- skipped)\n",
                         key, wid, MissionVarStore::kMissionVarStoreCap);
            std::fflush(stderr);
            return;
        }
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET] key=%s val=%s wid=%d tick=%u\n", key, value, wid, tick);
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
    if (!store) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] WARN: no varStore for wid=%d key=%s (store null)\n", wid, key);
        std::fflush(stderr);
        return;
    }
    bool ok = store->set(key, value, VarScope::Unit);
    if (ok) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] key=%s value=%s scope=Unit wid=%d\n", key, value, wid);
    } else {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_FULL] wid=%d key=%s (cap=%d reached -- skipped)\n",
                     wid, key, VarStore::kVarStoreCap);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarGet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->get + fprintf/fflush. No order functions.
void handleVarGet(mc2::czstring key, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real read from g_missionVarStore.
        // Gate OFF → trace-only default-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE] key=%s wid=%d (default -- TechBrain policy off)\n",
                         key, wid);
            std::fflush(stderr);
            return;
        }
        const char* val = g_missionVarStore.get(key);
        bool isHit = (g_missionVarStore.find(key) != nullptr);
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_GET] key=%s val=%s wid=%d (%s)\n",
                     key, val, wid, isHit ? "hit" : "default");
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
    const char* val = (store) ? store->get(key, VarScope::Unit) : "0";
    bool isHit = store && (store->find(key, VarScope::Unit) != nullptr);
    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET] key=%s value=%s scope=Unit wid=%d (%s)\n",
                 key, val, wid, isHit ? "hit" : "default");
    std::fflush(stderr);
}

