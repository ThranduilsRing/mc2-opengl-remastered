# T1 review fixes

Assignment: A:/Games/mc2-cxxt1, claude/w122-cxx-t1, base ef86f849e0b2.

- P1: same-mech edit re-entry preserves the editable copy; switching mechs
  cancels the previous edit before taking the next variant snapshot.
- Next: commit P1; fix the pilot rank comparison in its own commit; remove
  introduced static unique_ptr owners (vehicle pilot and mech-icon template).
- Proof pending at final tip: ratchet, box build/unit/smoke-lite/saveload, lint.
- Check campaign cross-load feasibility without adding infrastructure.
- Finish by archiving receipts and cleaning the box lane and own scratch.
