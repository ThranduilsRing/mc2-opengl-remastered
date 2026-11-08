# Jump command lane state

Assignment: slot03, claude/jump-command, f269b312d; contact-pick-fixes cb52de4e5 fast-forwarded.
Root cause: doJump setWayPoint wrote coordinates but left numPoints=0. The tick ingress
copies only counted waypoints; TacticalOrder::validate rejects order_goal_missing.
Fix: addWayPoint records the destination and jump travel mode together.
Target validation extracted unchanged in meaning (gameplay XY, Z height, world-unit range).
Capability: loadout COMPONENT_FORM_JUMPJET sets numJumpJets=5; profile refresh grants CAP_JUMP;
executor supports mech walkers. Button and held J both activate the existing jump cursor.
Occupied landing cells are redistributed by calcJumpGoals; LOS changes the cursor only.
Done: implementation and focused target/queue tests. Proof pending.
Next: add scripted jump through existing UI channel, log execution, commit, box build/unit/
smoke-lite/headless mc2_01 determinism, covering contracts, final PC compile and lint.
