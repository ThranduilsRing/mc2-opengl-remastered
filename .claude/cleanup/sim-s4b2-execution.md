# Authored route installation and execution

## Installed means a synchronous handoff

Part keys map editor IDs 0/1/2 to None/Traverse/Patrol; stance is 0..5, points are 1..8 finite in-map XYZ values (None has zero). Missing, duplicate, malformed or stray fields reject the whole order; never truncate. The selected alternate supplies content; the original Part supplies the pilot. Bind after commanders, never by position.
Mission::diagnoseAuthoredRoute owns the one rejection or informational suppression diagnostic. Suppress only when the shared OPORD consumer predicate admits the preserved pilot's Enhanced OPORD, including its gate and skip rules. Absent/None/rejected/suppressed content causes zero handoffs.
The unchanged legacy TacticalOrder goes through Mover::handleTacticalOrder and MechWarrior::setGeneralTacOrder. Verify every point/mode synchronously and retain original/selected Part IDs, pilot, kind, stance and complete route before ABL can overwrite the slot. Evidence reads this snapshot, not a later slot or retired receipt.
Installed (wire value 1, compatibility alias Applied) means slot installation, never movement completion. Installation flags a new order; the next warrior update can clear paths/attack state and call execute despite disabled route dispatch. Stock parity therefore requires zero handoffs, not inert installation.
The engine-linked seam uses real initialization, handler and setter; its adapter-disconnected control must fail the installation assertion despite an Installed receipt. Full contract runs include it; focused standalone/ASan runs do not.
The live witness writes only new scratch-mod copies of stock content. It requires the original pilot through alternate selection, complete installation, one diagnostic and zero installations for malformed/suppressed cases, and gate-off installation versus gate-on suppression. A disconnected loader must fail; movement is ungraded. Coordinates must round to the same finite float32; identities and modes remain exact.

## B2b route state and arbitration

Mission owns RouteExecution by Part incarnation and execution ID, independently of receipts. Destroy/reset clears routes, executions, rejections and mission-local identity counters; ObjectRef generation remains 1 under session isolation. Retain original/selected IDs, pilot, kind, stance, points/modes, next cursor, owning order revision, state, suspension reason, retry budget/deadline, last transition tick and event sequence.
Ownership cannot be inferred from coordinates or legacy id, which assignment does not copy. Emit Started/Suspended/Resumed/Completed/Interrupted/Failed exactly once per transition, with tick and reason.
Stance remains mission-owned: preserve all six values without changing TacticalOrder's layout; movement-fixture construction does not depend on defining combat policy.

The editor route is the unit's standing default order; ABL never cancels it. Player cancellation maps to Interrupted(PlayerOrder), death/removal/pilot change to Interrupted(Removed), and exhausted reachability to Failed(Unreachable).
Retry permits three retries within 30 simulation ticks of the first failure; PathAccepted and arrival reset both limits. Failed resume becomes Active+Retrying or Failed. Alarm also suspends Pending; clearing it waits in Pending if maps are unavailable. Poll Pending and map-unavailable requests for readiness and dispatch once available; PATH orders have no automatic execute retry.
Signals carry execution ID, owning revision, pilot incarnation and next event sequence; pilot mismatch interrupts as Removed. Stale, duplicate and terminal signals reject; retain only the last rejection per execution. Validate liveness before dereferences/effects and recheck terminal state afterward; poll a lost general slot without ABL cancellation. For owned paths, permit guarded returns before dispatch and calculation unless GameMap, terrain, PathManager, both PathFindMaps and the goal map are ready; missing PathFindMaps otherwise reach Fatal in mover_path.cpp:791.
Stance proposal: all six preserve authored travel mode; combat policies are evade/return fire/defend/engage/pursue/fight to death in editor order, pending owner confirmation.

| Condition | Decision |
|---|---|
| Patrol endpoint | Wrap last to zero, including a one-point route; never reverse or compute -1. |
| Traverse endpoint | Complete after the last arrival; never read mode[numPoints]. |
| Cursor query/advance | Owned paths replace the legacy advance body; never run both. Query (`inc=false`, including rethinkPath) preserves cursor; advance only on an accepted transition. |
| Path requests | Begin only after both PathFindMaps and the goal map exist; mech, vehicle and mover consumers share the cursor contract. |
| Unreachable segment | Preserve cursor during bounded deterministic retries, then Failed(Unreachable); never skip or retry forever. |
| Player order | Cancel permanently, matching general-order clearing. |
| Explicit ABL general replacement | Preserve the standing default route; ABL never cancels it. |
| Temporary combat/alarm | Suspend/resume the standing default route; preserve cursor/mode/stance and revalidate the interrupted segment. Player cancellation or removal while suspended remains terminal. |
| Death/removal/pilot incarnation change | Interrupted; permanently release ownership. |
| Terminal state | Traverse completion is terminal; patrol continues until cancellation, failure or death. |

## Save/restore and additional claims

Raw warrior saves copy TacticalOrder and queues; keep that layout unchanged and add a versioned mission execution section. Save only at a completed tick, with execution fields above and ingress tick/source/sequence/receipt state; never serialize pointers, watch IDs or live-session tokens.
Validate candidates atomically with one diagnostic. Restore durable identities into a fresh session after Part/pilot/commander reconstruction and path-map setup, before updates; reconcile slot/cursor/suspension once, without re-admission or replay. Pending, active, suspended and unreachable routes must resume identically to uninterrupted execution.
Mission::load clears executions through destroy(); the existing seam calls destroy(false), but a full load/restore fixture is still missing. B2b-2 must claim saveload.cpp and add restore/rebind before resumed updates.

| Claim | Exact additional paths |
|---|---|
| Movement | code/warrior_move.cpp, code/mech_move.cpp, code/gvehicl_move.cpp, code/mover_path.cpp |
| Arbitration | code/warrior.cpp, code/warrior_alarms.cpp, code/warrior.h |
| Legacy execution/layout | code/warrior_state.cpp, code/warrior_orders.cpp, code/tacordr.cpp, code/tacordr.h |
| Update | code/mission_update.cpp |
| Save | code/saveload.cpp, code/saveload_load.cpp, code/saveload_internal.h, code/save_archive.h, code/save_archive.cpp |

Renew code/mission.h, code/mission.cpp, code/mission_init.cpp, code/tactical_order_queue.h, code/tactical_order_queue.cpp, affected unit/seam tests and registrations, and tools/mission_order_ingress_witness.py with its test. Any other consumer or new file needs a named claim; save claims depend on release of lane 1's quickload ownership.

## Acceptance and open questions

Fixture baseline is 94ced1e6e (claude/sim-s4b2b). BattleMech waypoint characterizations and a dispatch-blocker case exist: execute's PATH cases are commented out, and status returns success so order*Path exits before enqueue. The synthetic queued movement fixture remains blocked; map-unavailable coverage must assert fixture readiness, not claim legacy deferral/recovery. Wiring acceptance must pin reviewed baseline/post-commit binaries by SHA256 and strict chains on mc2_03/10/24/16 and both camera arms on mc2_10/24, with identical content bytes, seed and resolved environment.
Missing frame budget, fingerprint, smoke success or matching inputs fails acceptance, as does any strict mismatch. Preserve manifests/logs and binary hashes. Fingerprinted positive and loader-disconnected witness controls are separate from the offline adapter control; movement/arbitration acceptance belongs to b2b.
MC2_BRAIN_MISSIONFIT_OPORD stays default OFF by owner ruling. Next movement slice needs the PATH dispatch/status blocker resolved, scoped synthetic GameMap/terrain/maps/object resolution/PathManager, then real queued complex paths and omitted-service control; add GroundVehicle and owned segment INIT with both buffer snapshots (the calculated buffer can be path[1]) before execution/restore acceptance.
