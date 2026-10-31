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
No green verification receipts or implementation changes yet.
