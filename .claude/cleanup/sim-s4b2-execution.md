# Authored route installation and execution

## Installed means a synchronous handoff

Part keys map editor IDs 0/1/2 to None/Traverse/Patrol; stance is 0..5, points are 1..8 finite in-map XYZ values (None has zero). Missing, duplicate, malformed or stray fields reject the whole order; never truncate. The selected alternate supplies content; the original Part supplies the pilot. Bind after commanders, never by position.
Mission::diagnoseAuthoredRoute owns the one rejection or informational suppression diagnostic. Suppress only when the shared OPORD consumer predicate admits the preserved pilot's Enhanced OPORD, including its gate and skip rules. Absent/None/rejected/suppressed content causes zero handoffs.
The unchanged legacy TacticalOrder goes through Mover::handleTacticalOrder and MechWarrior::setGeneralTacOrder. Verify every point/mode synchronously and retain original/selected Part IDs, pilot, kind, stance and complete route before ABL can overwrite the slot. Evidence reads this snapshot, not a later slot or retired receipt.
Installed (wire value 1, compatibility alias Applied) means slot installation, never movement completion. Installation flags a new order; the next warrior update can clear paths/attack state and call execute despite disabled route dispatch. Stock parity therefore requires zero handoffs, not inert installation.
The engine-linked seam uses real initialization, handler and setter; its adapter-disconnected control must fail the installation assertion despite an Installed receipt. Full contract runs include it; focused standalone/ASan runs do not.
The live witness writes only new scratch-mod copies of stock content. It requires the original pilot through alternate selection, complete installation, one diagnostic and zero installations for malformed/suppressed cases, and gate-off installation versus gate-on suppression. A disconnected loader must fail; movement is ungraded. Coordinates must round to the same finite float32; identities and modes remain exact.

## B2b route state and arbitration

Mission owns RouteExecution by durable Part incarnation and execution ID, independently of receipts. Retain original/selected IDs, pilot, kind, stance, points/modes, next cursor, owning order revision, state, suspension reason, retry budget/deadline, last transition tick and event sequence.
Ownership cannot be inferred from coordinates or legacy id, which assignment does not copy. Emit Started/Suspended/Resumed/Completed/Interrupted/Failed exactly once per transition, with tick and reason.
Stance remains mission-owned: no legacy field or established pilot setter exists. Define movement/combat policy for all six values before dispatch; never silently discard stance or change TacticalOrder's layout.

| Condition | Decision |
|---|---|
| Patrol endpoint | Wrap last to zero, including a one-point route; never reverse or compute -1. |
| Traverse endpoint | Complete after the last arrival; never read mode[numPoints]. |
| Cursor query/advance | Query preserves cursor; advance only on an accepted transition, keeping legacy and mission cursors consistent. |
| Path requests | Begin only after both PathFindMaps and the goal map exist; mech, vehicle and mover consumers share the cursor contract. |
| Unreachable segment | Preserve cursor during bounded deterministic retries, then Failed(Unreachable); never skip or retry forever. |
| Player order | Cancel permanently, matching general-order clearing. |
| Explicit ABL general replacement | Cancel as Superseded; never reinstall each tick. |
| Temporary combat/alarm | Suspend/resume only under the same owning revision; preserve cursor/mode/stance and revalidate the interrupted segment. Replacement while suspended cancels; clearing an alarm cannot resurrect it. |
| Death/removal/pilot incarnation change | Interrupted; permanently release ownership. |
| Terminal state | Traverse completion is terminal; patrol continues until cancellation, failure or death. |

## Save/restore and additional claims

Raw warrior saves copy TacticalOrder and queues; keep that layout unchanged and add a versioned mission execution section. Save only at a completed tick, with execution fields above and ingress tick/source/sequence/receipt state; never serialize pointers, watch IDs or live-session tokens.
Validate candidates atomically with one diagnostic. Restore durable identities into a fresh session after Part/pilot/commander reconstruction and path-map setup, before updates; reconcile slot/cursor/suspension once, without re-admission or replay. Pending, active, suspended and unreachable routes must resume identically to uninterrupted execution.
Mission::load currently calls neither resetOrderIngress nor bindOrderRecipients. B2b-2 must claim saveload.cpp and add reset/rebind before resumed updates.

| Claim | Exact additional paths |
|---|---|
| Movement | code/warrior_move.cpp, code/mech_move.cpp, code/gvehicl_move.cpp, code/mover_path.cpp |
| Arbitration | code/warrior.cpp, code/warrior_alarms.cpp, code/warrior.h |
| Legacy execution/layout | code/warrior_state.cpp, code/warrior_orders.cpp, code/tacordr.cpp, code/tacordr.h |
| Update | code/mission_update.cpp |
| Save | code/saveload.cpp, code/saveload_load.cpp, code/saveload_internal.h, code/save_archive.h, code/save_archive.cpp |

Renew code/mission.h, code/mission.cpp, code/mission_init.cpp, code/tactical_order_queue.h, code/tactical_order_queue.cpp, affected unit/seam tests and registrations, and tools/mission_order_ingress_witness.py with its test. Any other consumer or new file needs a named claim; save claims depend on release of lane 1's quickload ownership.

## Acceptance and open questions

Acceptance pins baseline 606b584b3c192e07f7207e9a6f48940bfc798eab and post-commit binaries by SHA256. Strict chains must match on mc2_03/10/24/16 and both camera arms on mc2_10/24, with identical content bytes, seed and resolved environment; no baseline rewriting.
Missing frame budget, fingerprint, smoke success or matching inputs fails acceptance, as does any strict mismatch. Preserve manifests/logs and binary hashes. Fingerprinted positive and loader-disconnected witness controls are separate from the offline adapter control; movement/arbitration acceptance belongs to b2b.
Open: MC2_BRAIN_MISSIONFIT_OPORD defaults OFF; should eligible Enhanced OPORD precedence become default-on? The gate itself is lane 2's. Stance application and save-hook implementation remain required before execution/restore.
