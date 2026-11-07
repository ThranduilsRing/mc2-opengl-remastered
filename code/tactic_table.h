// tactic_table.h - the one explicit-value table for attack tactics (LG14 part 2).
// Generates the legacy TacticType enum (tacordr.h), the sim Tactic enum class (tactical_order_queue.h), the domain check, the
// bounded names, the brain-name aliases and the legacy<->sim adapters (mission.cpp), plus the order-params encoding
// (TACORDER_PARAM_TACTIC_* flags). Pure and dependency free so the unit tests include it without the engine.
// Values are saved (TacticalOrder queue, brain section weights[12], selectedTactic) and read by ABL: they never change.
// save_layout_guard.h pins the same values INDEPENDENTLY; do not generate those pins from this table.
#pragma once

#include <cstdint>

// X(LegacyEnumerator, SimEnumerator, Value, "Name", "Code2", LegacyParamBit, ExtParamIndex)
//  Name: the brain/FIT canonical name. Code2: two-character pilot debug code.
//  LegacyParamBit: bit of move.h TACORDER_PARAM_TACTIC_* (0 = none). The legacy flag bits are NOT in enum order:
//    FLANK_RIGHT is bit 22 and FLANK_LEFT bit 23, while value 1 is LEFT and 2 is RIGHT.
//  ExtParamIndex: 1-based slot in the extended-tactic field (params bits 28..30) for tactics with no legacy flag bit.
#define MC2_TACTIC_TABLE(X) \
	X(TACTIC_NONE,             None,             0,  "None",             "  ",  0, 0) \
	X(TACTIC_FLANK_LEFT,       FlankLeft,        1,  "FlankLeft",        "LT", 23, 0) \
	X(TACTIC_FLANK_RIGHT,      FlankRight,       2,  "FlankRight",       "RT", 22, 0) \
	X(TACTIC_FLANK_REAR,       FlankRear,        3,  "FlankRear",        "RR", 24, 0) \
	X(TACTIC_STOP_AND_FIRE,    StopAndFire,      4,  "StopAndFire",      "SF", 25, 0) \
	X(TACTIC_TURRET,           Turret,           5,  "Turret",           "TT", 26, 0) \
	X(TACTIC_JOUST,            Joust,            6,  "Joust",            "JS", 27, 0) \
	X(TACTIC_INDIRECT_FIRE,    IndirectFire,     7,  "IndirectFire",     "IF",  0, 1) \
	X(TACTIC_HULL_DOWN,        HullDown,         8,  "HullDown",         "HD",  0, 2) \
	X(TACTIC_FIGHTING_WITHDRAW, FightingWithdraw, 9, "FightingWithdraw", "FW",  0, 3) \
	X(TACTIC_PURSUE,           Pursue,           10, "Pursue",           "PU",  0, 4) \
	X(TACTIC_HIT_AND_RUN,      HitAndRun,        11, "HitAndRun",        "HR",  0, 5)

// Brain Tactics block aliases (brain_archetype.h): X("Alias", Value). Canonical names above also resolve.
#define MC2_TACTIC_ALIASES(X) \
	X("Flank", 2) X("Rear", 3) X("Standard", 4) X("Suppress", 7) X("Rearguard", 9)

namespace tactic_table {

inline constexpr int kCount =
#define MC2_TACTIC_COUNT(L, S, V, N, C, B, E) +1
	0 MC2_TACTIC_TABLE(MC2_TACTIC_COUNT);
#undef MC2_TACTIC_COUNT
inline constexpr int kMinValue = 0;
inline constexpr int kMaxValue = kCount - 1;

// Order-params layout. Legacy flags: bits 22..27 (== TACORDER_TACTIC_MASK); extended tactics: bits 28..30.
inline constexpr unsigned long kExtShift = 28;
inline constexpr unsigned long kExtMask = 0x7ul << kExtShift;

constexpr bool in_domain(int v) { return v >= kMinValue && v <= kMaxValue; }

constexpr bool str_eq(const char* a, const char* b) {
	while (*a && *a == *b) { ++a; ++b; }
	return *a == *b;
}

// Bounded: out-of-domain values get a placeholder, never an out-of-bounds read.
constexpr const char* name(int v) {
	switch (v) {
#define MC2_TACTIC_NAME(L, S, V, N, C, B, E) case V: return N;
		MC2_TACTIC_TABLE(MC2_TACTIC_NAME)
#undef MC2_TACTIC_NAME
	}
	return "Unknown";
}

constexpr const char* code2(int v) {
	switch (v) {
#define MC2_TACTIC_CODE(L, S, V, N, C, B, E) case V: return C;
		MC2_TACTIC_TABLE(MC2_TACTIC_CODE)
#undef MC2_TACTIC_CODE
	}
	return "??";
}

// Canonical name or brain alias -> value, -1 if unknown. Case sensitive, exact.
constexpr int find_name(const char* n) {
	if (!n || !n[0]) return -1;
#define MC2_TACTIC_FIND(L, S, V, N, C, B, E) if (str_eq(n, N)) return V;
	MC2_TACTIC_TABLE(MC2_TACTIC_FIND)
#undef MC2_TACTIC_FIND
#define MC2_TACTIC_FIND_ALIAS(A, V) if (str_eq(n, A)) return V;
	MC2_TACTIC_ALIASES(MC2_TACTIC_FIND_ALIAS)
#undef MC2_TACTIC_FIND_ALIAS
	return -1;
}

inline constexpr unsigned long kLegacyParamMask =
#define MC2_TACTIC_LMASK(L, S, V, N, C, B, E) | (B ? (1ul << (B ? B : 0)) : 0ul)
	0ul MC2_TACTIC_TABLE(MC2_TACTIC_LMASK);
#undef MC2_TACTIC_LMASK

// Tactic value -> order-params bits. Explicit per tactic: no ordinal shift arithmetic.
constexpr unsigned long to_params(int v) {
	switch (v) {
#define MC2_TACTIC_TO_PARAMS(L, S, V, N, C, B, E) \
	case V: return (B ? (1ul << (B ? B : 0)) : 0ul) | (E ? (static_cast<unsigned long>(E) << kExtShift) : 0ul);
		MC2_TACTIC_TABLE(MC2_TACTIC_TO_PARAMS)
#undef MC2_TACTIC_TO_PARAMS
	}
	return 0ul;
}

// Order-params bits -> tactic value. Exactly one legacy flag selects that tactic (as the original switch did); any other
// legacy combination is NONE. With no legacy flag the extended field selects tactics 7..11.
constexpr int from_params(unsigned long params) {
	const unsigned long legacy = params & kLegacyParamMask;
	if (legacy) {
#define MC2_TACTIC_FROM_LEGACY(L, S, V, N, C, B, E) if (B && legacy == (1ul << (B ? B : 0))) return V;
		MC2_TACTIC_TABLE(MC2_TACTIC_FROM_LEGACY)
#undef MC2_TACTIC_FROM_LEGACY
		return 0;
	}
	const unsigned long ext = (params & kExtMask) >> kExtShift;
	if (ext) {
#define MC2_TACTIC_FROM_EXT(L, S, V, N, C, B, E) if (E && ext == static_cast<unsigned long>(E)) return V;
		MC2_TACTIC_TABLE(MC2_TACTIC_FROM_EXT)
#undef MC2_TACTIC_FROM_EXT
	}
	return 0;
}

// Legacy <-> sim adapters. Explicit per-row mapping (no numeric casts); an unknown enumerator maps to None.
// Sim and Legacy are the generated enums (Sim = mc2::sim::Tactic, Legacy = ::TacticType).
template <class Legacy, class Sim>
constexpr Legacy sim_to_legacy(Sim s) {
	switch (s) {
#define MC2_TACTIC_S2L(L, S, V, N, C, B, E) case Sim::S: return Legacy::L;
		MC2_TACTIC_TABLE(MC2_TACTIC_S2L)
#undef MC2_TACTIC_S2L
	}
	return Legacy::TACTIC_NONE;
}

template <class Sim, class Legacy>
constexpr Sim legacy_to_sim(Legacy l) {
	switch (l) {
#define MC2_TACTIC_L2S(L, S, V, N, C, B, E) case Legacy::L: return Sim::S;
		MC2_TACTIC_TABLE(MC2_TACTIC_L2S)
#undef MC2_TACTIC_L2S
	}
	return Sim::None;
}

} // namespace tactic_table

// Enum generators. Use inside the one header that declares each enum.
#define MC2_TACTIC_LEGACY_ENUMERATOR(L, S, V, N, C, B, E) L = V,
#define MC2_TACTIC_SIM_ENUMERATOR(L, S, V, N, C, B, E) S = V,
