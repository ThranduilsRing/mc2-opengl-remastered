#pragma once
// UNITQUERY-SETTARGETPRIORITY-1: brain symbol resolver (pure, engine-free).
//
// carver_v_enhanced passes SYMBOLIC args to brain verbs (e.g.
//   DO UnitQuery.SetTargetpriority 1 TARGET_PRIORITY_MOVER 0 200 \
//        CONTACT_CRITERIA_ENEMY + CONTACT_CRITERIA_VISUAL_OR_SENSOR + CONTACT_CRITERIA_NOT_DISABLED
// ). ABL compiled those symbols to ints before the engine saw them; our brain
// RawScan dispatch reads the RAW .fit text, so the symbols + '+'-joined flag
// expressions arrive as strings. This resolver turns them into the integers the
// engine setters (e.g. MechWarrior::setTargetPriority) expect.
//
// PURE: no engine headers, no globals, no I/O. Unit-tested in isolation
// (tools/brain_symbol_resolve_test). The symbol tables mirror the engine enums:
//   TARGET_PRIORITY_*  -> code/warrior.h   (TargetPriorityType, enum order)
//   CONTACT_CRITERIA_* -> code/dcontact.h  (bit flags)
// Keep the tables in sync with those headers.

#include "mc2_czstring.h"
#include <cstdint>
#include <optional>

// Resolve a single token to its integer value.
//   - bare decimal integer (incl. leading '-'):  "2" -> 2, "-1" -> -1, "0" -> 0
//   - TARGET_PRIORITY_* symbol:                   "TARGET_PRIORITY_MOVER"  -> 2
//   - CONTACT_CRITERIA_* symbol:                  "CONTACT_CRITERIA_ENEMY" -> 1
// Leading/trailing ASCII whitespace is ignored.
// Returns the value on success, or nullopt for an unknown/malformed token.
std::optional<long> brainResolveSymbolToken(mc2::czstring token);

// Resolve a '+'-joined expression to the sum of its terms. Each term is resolved
// by brainResolveSymbolToken (symbol or bare int). Whitespace around '+' and at
// the ends is ignored.
//   "CONTACT_CRITERIA_ENEMY + CONTACT_CRITERIA_VISUAL_OR_SENSOR + CONTACT_CRITERIA_NOT_DISABLED" -> 97
//   "-1" -> -1     "2" -> 2     "A+B" (no spaces) -> A summed with B
// Returns nullopt if the expression is empty, has an empty term (e.g. a trailing
// '+'), or any term fails to resolve.
std::optional<long> brainResolveIntExpr(mc2::czstring expr);
