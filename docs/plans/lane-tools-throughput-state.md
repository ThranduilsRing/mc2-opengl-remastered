# Lane tools throughput

Assignment: mc2-slot09, claude/lane-tools-throughput, base 8fc0c6c89.
Scope: GPU discovery/queue routing, retained installs and explicit crash-preserving
GC, targeted harness timing/zero-match rejection, deploy phase timing. No engine changes.

Implemented all four tooling changes, with focused regression coverage.
Targeted proof: five modules passed in focused-tests.log; the slot fix and its
three covering modules passed in slot-final-tests.log. Two GPU-routing Bash cases
passed in gpu-routing-tests.log. The live nvidia-smi probe found only GPU 0.
The cxx17 worktree ratchet and git diff --check passed.

Evidence directory: E:/mc2-archive-2026-10/proofs/lane-tools-throughput/.
Next: committed-tip gate_farm contract row, real box --gpu 0 -- build, final lint,
and owned scratch cleanup. Final receipts/status will be in that directory's STATE.md.
The unrelated full box-hygiene eviction suite was stopped after exceeding two
minutes; its changed GPU-routing cases were run separately. The initial cached
runner was also stopped during expensive whole-tree cache preparation.
No local installs or game processes created. No generated baselines changed.
