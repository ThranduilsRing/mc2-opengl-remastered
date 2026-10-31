# W90 fixups state

Assignment: mc2-lane10, claude/w90-fixups, base 123461d3cf98796d65f9ae21550d27b45cf4df94.
Budget: at most 150 calls. Plan: verify five findings, five separate commits,
one offline/build checkpoint, deploy one lane install, requested domain proofs.

Verified: all five cited gaps exist. P1-a now enforces requested ticks with
zero tolerance and records completion in result.json; truncated fixture added.
Next: P1-b finale/tick requirements; P2-c full SIMID records; P2-a home presence;
P2-b complete/disjoint mover lists; tests/contracts/ratchets/build/domain proofs.
