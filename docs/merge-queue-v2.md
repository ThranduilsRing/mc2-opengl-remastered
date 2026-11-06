# Merge queue v2 (design note, not built)

v1 (tools/lane_merge.py --stage/--promote/--bisect, scripts/merge_gate.py durable verdict) stages frozen tips on
claude/stage-W, gates the candidate, and fast-forwards nifty only on a complete PASS. v2 picks up what v1 left out:

- astra-review-r7 changes 4 and 5 (out of v1 scope).
- Informational inventories as gate artifacts: vk-ownership-ledger.md, gated-layout-macros.md and
  binding-slot-occupancy.json are still committed because their contract rows (vk_ownership_ledger --check,
  gated_layout_macros, binding_slots) compare against the committed copy. Moving them out means turning those rows
  into "generator runs clean" checks and publishing the output under the verdict's artifacts; do it as one change so
  coverage never drops.
- Automatic bisect: v1 prints the next --stage command; v2 can drive stage -> gate -> bisect without the orchestrator.
- Speculative pipelining: gate the second half of a red batch on a parallel candidate while the first half retests.
