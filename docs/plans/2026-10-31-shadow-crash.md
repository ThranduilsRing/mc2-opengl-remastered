# Shadow dirty-batch crash (2026-10-31)

Assignment: lane10 / claude/shadow-crash-fix / base 123461d3cf98796d65f9ae21550d27b45cf4df94.
Budget: <=150 tool calls. No pushes. One private install, no parallel coding agents.

Completed: read canonical/lane rules; clean branch/base verified. Read both W91
crash reports under E:/mc2-archive-2026-10/proofs/merge91/capture-crashes.
Both are mc2_24, frames 119/117, dirty-batch recording, wild reads 0x160/0x10.
Review's transient lifetime hypothesis is unconfirmed: releaseTransients does
not destroy maskImage/topR8; invalidate already clears incActive.

Next: recover exact base/turret and day capture commands; seed private install,
deploy base executable and reproduce up to ten runs. Trace object and command
buffer lifetimes with validation. Fix evidenced cause and add fast regression.
Then targeted build/contracts/harness, 20 repro repeats, farm mc2_10/24 x8,
sync validation, SIMID 03/10/24, viewed day/night A/B and five ratchets.
Root cause confirmed: W91 amd-day/mc2_24_capture.log:4286 and
verified-kit-night/mc2_24_capture.log:4290 refuse dirtyInstances (22,020,096
bytes) at the 2 GiB residency cap, immediately followed by buffer-init failure.
ensureIncremental publishes incParams before the remaining allocations. Next
frame sees incParams and skips initialization, leaving incQueries null.
NVIDIA minidump exception RDX=0/R8=4/R9=2; driver instruction at module offset
0xe0d19c reads [RDX+0x160] at entry to query reset. AMD has the same null pool.

Fix: allocate four incremental buffers in local owners, upload and create the
query pool, then publish members together. Failure frees temporary allocations
and leaves the dirty queue available for retry; no gates or budget increases.
Regression compiles actual production method bodies with fault-injected Vulkan/
VMA owners, six failure points and 90 recreation cycles at each point.
RED base: G:/mc2-tmp/shadow-init-before.log (assert, exit 3221226505).
GREEN: pytest 1 passed (1.10 s), four covering contracts pass (4.42 s).

Private install seeded and deployed at E:/mc2-installs/claude-shadow-crash-fix.
Base build 3b45512d5 (docs-only delta) complete. Base capture currently running
through run_visual_capture, output proof/shadow-crash-fix/before-base.
Next: finish base capture; commit/build/deploy fixed code; run all requested
proofs above. Initial and audit scout calls timed out/slow; inspect direct ranges.
Sibling audit: ensureTransients/ensureEncoder/ensureProducts also have partial
creation sentinels; incremental retirement has hardcoded +2 despite 3 frame
slots. Audit/fix these separately with focused proof; do not call them fixed yet.
