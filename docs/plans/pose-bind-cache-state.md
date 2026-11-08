# pose-bind-cache state

Assignment: slot03, claude/pose-bind-cache; base 8fc0c6c89 (Linux fixes).

Implemented: destination-owned last-clip track map; fresh source identity on load;
ClearAnimation/detach/destroy/arm-loss invalidation; retained sim torso/hit/cockpit
and anchor indices. Imported world-anchor resolution remains ahead of indexed
stock fallbacks. PoseView carries sim ownership to avoid reading worker state.
New offline doctests cover first case-insensitive match, missing nodes, reuse
without comparisons, separate destination ordering, invalidation and reload.

Next: review diff, compile final tip, targeted contracts/doctests, box smoke and
headless SIMID/saveload. Record receipts and cleanup here before hand-back.
No performance measurement: weekly orchestrator batch owns savings evidence.
