# startup-load state

Assignment: slot02, claude/startup-load; actual starting tip 117480ac2 includes
load-waste and Linux link fixes after the assigned f269b312d base.

Plan (<=150 calls): inspect existing startup/mission timers and video/input
paths; baseline PC launch; smallest startup/load/input fixes; covering offline
checks, box build/unit, final Windows compile and PC smoke; local commit only.

Implemented: anchor startup at main entry; STARTUP first-present/first-audio
summary, first decoded video frame marker, archive/mod and sound catalog marks.
Sound bites now decode on first audible use (including rain), retaining resources
until shutdown. Music streaming from load-waste is unchanged. Loading pumps SDL
OS events at phase marks and object loops, using the central OS-cursor policy
until mission-ready; no game-input dispatch into the half-built mission.

Inspection: MainMenu requests msft.bik (present in the isolated install), and the
campaign intro BIKs are present. FFmpeg opens OS paths although its resolver
advertises an FST fallback. Missing loose/FST-backed playback needs separate
confirmation; no speculative decoder change. Added terrain_objects_ready to
separate object creation from appearance/texture prewarm in the existing timers.

Green: baseline mc2 Ninja compile rc=0 at 117480ac2; cxx17 worktree ratchet.
The warm executable and viewer were baa609474399: provenance checks refused
each stale artifact. The standard payload baseline build finished, but edits
made during that build dirtied the viewer fingerprint. Deployment refused it;
the before launch DID NOT RUN. No provenance bypass was applied. This was a
mistake: keep every payload source unchanged until the before deployment ends.
Diff-check reports CR-at-EOL on the six added soundsys lines; the indexed file
and worktree are both CRLF. No whitespace-check policy override was applied.

Next: commit the bounded patch and run final standard Ninja build, covering input
contracts/unit cases, box build/unit/lint, PC launch and smoke-lite. No new pure
algorithm or deferral queue was introduced. Mission largest-phase optimization,
and intro root cause, remain unproven until logs exist. Archive receipts under
E:/mc2-archive-2026-10/proofs/startup-load; scratch logs G:/mc2-tmp/startup-*.log.
One isolated install: E:/mc2-installs/pc-lane-02. Retain per-slot install only
with its proof; remove scratch and preserve crash evidence at cleanup.

Budget hand-back scope: the bounded patch, compile and covering offline checks.
Before timing, precise intro root cause, mission top-three phase attribution and
the largest-phase fix remain unfinished. Final receipts and exact proved-at SHA
are recorded outside the tree in proofs/startup-load/state.md so recording
results does not invalidate the compiled commit identity. No timing win claimed.
