#pragma once
// BrainExecutionPolicy (architecture lane B4, gamelogic rewrite Phase 5).
//
// One per-mission decision, made at mission load, replaces the ladder of MC2_BRAIN_* feature gates
// (TASKQ / RUNTIME / RUNTIME_APPLY / MISSIONFIT_OPORD / DISPATCH* / INTENT_QUEUE / COMMIT_PHASE / PATROL /
// ENGAGE / FSM / FLOW / ALIAS / VARIANTOF / SCOPE_GLOBAL / VAR_MISSION / SNAPSHOT / INLINE_EMPTY_SKIP).
//
//   Legacy    stock ABL brains only. The brain layer is inert; behaviour is byte-identical to a build
//             without the TechBrain code. Chosen for every mission that authors no brain data. The
//             Omnitech / Carver ABL extensions are native ABL functions registered unconditionally
//             (ablmc2.cpp), so ABL-extended content runs on this path too; it needs no policy of its own.
//   TechBrain the mission or its mod authors brain data: an inline Brain{} block in mission.fit
//             (OPORD / archetype), a <m>_specials.fit TechScript, or a <m>_ai.fit mode table. TechBrain
//             overrides ABL for the units it describes; every other unit keeps its ABL brain.
//
// MC2_BRAIN_POLICY=legacy forces Legacy (kill-switch); =techbrain forces TechBrain (test fixtures);
// unset or "auto" uses the saved AI rule: Stock keeps ABL; Smarter enables authored TechBrain
// policies and detected-target selection for units without authored brain data.
#include <cstdint>
#include <string_view>

namespace mc2::brainpolicy {

enum class Policy : std::uint8_t { Legacy = 0, TechBrain = 1 };

extern Policy g_policy;                 // written only by decide(), on the mission-load thread

inline Policy current() { return g_policy; }
inline bool   techBrain() { return g_policy == Policy::TechBrain; }
// Bumped by every decide(); readers that cache a policy-derived value across ticks compare it to re-derive per mission.
extern std::uint32_t g_generation;
inline std::uint32_t generation() { return g_generation; }
const char*   name(Policy p);

// Decide the policy for the mission being loaded. Call once per Mission::init after the mission.fit
// is resolvable (mod overlay active). missionFitPath is the resolved mission .fit path, missionDir the
// "data/missions/" prefix, missionName the bare mission name. Logs one [BRAIN_POLICY] line.
void decide(std::string_view missionFitPath, std::string_view missionDir, std::string_view missionName);

}  // namespace mc2::brainpolicy
