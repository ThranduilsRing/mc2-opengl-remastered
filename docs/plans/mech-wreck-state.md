# Mech wreck lane

Assignment: slot09, claude/mech-wreck, requested base f269b312d; inherited Linux fixes through 8fc0c6c89.
Diagnosis: damaged appearance uses living GPU geometry; imported actor unregister loses palette. Salvage-all bypasses destroyed status; full destruction lacks a cause. Existing mover serialization persists causeOfDeath.
Implemented: register/submit stock damaged shapes; preserve imported death mesh/palette; continue animation without replacement; explicit reactor cause and salvage exclusion including salvage-all. No recovery-craft files edited.
Next: verify self-destruct/ejection semantics, targeted doctests/contracts, box build/unit/smoke/determinism, final Windows compile/lint, PC scripted death captures; commit green work, cleanup.
Proof: not run yet.
