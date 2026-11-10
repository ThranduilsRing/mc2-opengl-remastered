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

## Landing continuation 2026-11-09
Rebased on nifty b26fc9bfe (96b8d263f); pose-kernel-once cherry-picked as a distinct
second series (31c369671, 71f4dabab). Code proved at 71f4dabab content (box/Windows
runs used a temp commit adding only stage-137 pins: mc2_24 saveload baseline, battle.json,
battle-128.commands.txt, tools/sim_determinism.py; not committed here).
Box: determinism mc2_01/03/10/24 PASS, saveload mc2_01 + mc2_24 PASS, battle check
PASS (chain 0b6b0b698e47f0d5), smoke-lite 2/2 PASS (mc2_10 night timeout gone).
Red but identical on base b26fc9bfe (same-row run): unit (vfx_native_playback, 1 failed;
base 3921, tip 3924) and saveload mc2_10 (MechData digest 8962b8c28ded153f both).
Windows Ninja mc2 + viewer rc=0. Receipts: proofs/box/mc2-slot10/d6f44fe0, b26fc9bf.
