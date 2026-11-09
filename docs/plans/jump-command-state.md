# Jump command lane state

Assignment: slot03, claude/jump-command, f269b312d; contact-pick-fixes cb52de4e5 fast-forwarded.
Root cause: doJump setWayPoint wrote coordinates but left numPoints=0. The tick ingress
copies only counted waypoints; TacticalOrder::validate rejects order_goal_missing.
Fix: addWayPoint records the destination and jump travel mode together.
Target validation extracted unchanged in meaning (gameplay XY, Z height, world-unit range).
Capability: loadout COMPONENT_FORM_JUMPJET sets numJumpJets=5; profile refresh grants CAP_JUMP;
executor supports mech walkers. Button and held J both activate the existing jump cursor.
Occupied landing cells are redistributed by calcJumpGoals; LOS changes the cursor only.
Done: fix, target/queue tests, scripted UI jump and opt-in execution receipt (PATH tag).
At 6fd27de7c: box build/unit/lint and headless mc2_01 determinism green; scripted
jump executed part 512 but witness used windowed render verdicts. Corrected witness
to use the existing strict headless verdict and a fixed tick budget.
Covering order_gate_coverage contract green on PC (--nightly required).
Next: final-tip box rows and Windows compile; receipts and remaining steps live in
E:/mc2-archive-2026-10/proofs/jump-command/STATE.md. No PC install or perf claim.
