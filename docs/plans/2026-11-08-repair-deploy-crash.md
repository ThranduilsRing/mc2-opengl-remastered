# Repair deployment crash

Assignment: slot05, claude/repair-deploy-crash; requested base f269b312d.
Starting HEAD 8fc0c6c89 includes the two required Linux build fixes.

Original minidump and crash.txt copied (not moved) to
E:/mc2-archive-2026-10/proofs/repair-deploy-crash/.
Repair drop is already sim-owned: updateSimSystems -> updateVTol -> addVehicle
-> BringInReinforcement -> addMover. Main joins before GUI/presentation updates.
LG03 only changes sensor pool storage ownership, not scan/contact lifetimes.
freeMover clears outgoing sensor contacts but leaves incoming contacts until scans.
Mover::destroy nulls contactInfo only at pool destruction, not ordinary removal.
Dump correction: exception RCX=0, RIP=0x7ff77339daca, module base
0x7ff7732c0000. At RVA 0xddaca the shipped exe executes `mov rax,[rcx]`
(vtable load), BEFORE getFlag. The mover itself is null, not contactInfo.
Team contacts captured at R12+0x10 include duplicate 9 and an invalid 29807;
the failing scan index R13=8 points to that invalid handle. Team counters were
not captured. This is inconsistent contact storage, not proof of a thread race.

Root path repaired: newVehicle detached a previously teamed pooled slot ONLY
with MC2_PLAY_MISSION + TechBrain. Legacy campaign skipped cleanup; vehicle init
drops the old sensor but retains teamId, so setTeamId then removes the NEW sensor
at masterIndex=-1. removeSensor wrote before sensors[], aliasing the two contact
counters on Win64; compaction could replace them with pointer bits.
Cleanup is now unconditional. removeSensor validates membership before any
clear/compaction; meetsCriteria rejects a missing mover/contactInfo with one log.
No new runtime gates or layouts. LG03 only changed ownership of pool storage.

Added test_repair_deploy.cpp: verbatim production newVehicle/removeSensor slices
through the existing unit CMake slice facility. Fake engine services, real sim
worker. Tests legacy slot cleanup, valid compaction, index -1/stale-index rejection.
Next: finish focused unit run, negative control against original methods, commit;
box build/unit/contracts/determinism 01/03/10/24/saveload 01/smoke-lite and lint;
one Windows Ninja compile at final tip. Record receipts here or in proof folder.
No test receipts yet. No installs created. Never modify owner crash/install.
