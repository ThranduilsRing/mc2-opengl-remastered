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
Base build 3b45512d5 (docs-only delta), fixed build 74a794055, both complete.
Fixed exe SHA256: 2102988583a70a28929862acc08c1fb9ccd8def0167f2cc57c2604114a7a0baf.
Private install also retains mc2-before.exe/.pdb for the base arm (same data).
Proof root: E:/mc2-archive-2026-10/proofs/shadow-crash-fix.
Green: targeted mc2 + viewer link; 10 shadow doctests/313 assertions (240 s,
tool slowness); four covering contract rows; all five ratchets with no changes
to baselines. SIMID 03/10/24 headless pairs with --duration 20 match committed
1000/800/1000-frame baseline chains. Receipts copied under proof root/receipts.
Explicit day (time_of_day 0.25) and night (0.75), base+turret, trigger120 /
settle30 / duration20: both before/after PNG pairs byte-identical. Day/night
base images viewed. Sync-layer capture: layer_enabled preset=sync, zero errors
and zero warnings; resources use existing shadow.bake.* debug-utils names.

Reproduced on base at 1760 MiB after five successful control captures (default,
explicit day/night, 1870 MiB and 1780 MiB). Before-pressure1760 log:4292 refuses
dirtyInstances (22,020,096 bytes; live=1,824,365,408, budget=1,845,493,760).
Crash 2026-10-31_082145_024_57420_393948125 is frame117, read 0x160, same NVIDIA
driver address as W91. Bundle copied under proof root/crashes. Base rate at the
identified repro: 1 crash / 1 run; fixed: 0 crashes / 20 consecutive runs.
The production-method fault-injection regression DOES reproduce the exact
fourth-allocation failure on base and proves atomic retry on the fixed method.
after-pressure1760-20: 20 manifests, all captures present, both bookmarks
byte-stable in all 20 runs (STABILITY PASS). All 20 logs exercise dirty buffer
refusal. after-pressure1760-sync: sync layer engaged, zero errors/warnings,
174 dirty allocation failures safely retried. No camera cut was needed: crash frame117 is
before the first trigger120 pose change; window resize was not exercised.
Farm job 76f9ec900f434be98a475cfced87adf1: 8x mc2_10/mc2_24, 20 s, fixed exe,
independent byte-pinned reference. COMPLETE/PASS: all sixteen exit codes zero,
fetched under farm/76f9ec900f434be98a475cfced87adf1. Remote work tree cleaned by fetch.
First farm job a3d785fddbc54f1ab1dd885a8383c3e0 ran no games (farm has no lane
git ref); fetched failure retained. Replacement uses a separate reference copy.
Initial SIMID 10/24 attempts refused to start while this lane's capture was live;
sequential retries passed. Initial scouts slow/timed out; direct ranges used.
Sibling audit: ensureTransients/ensureEncoder/ensureProducts also have partial
creation sentinels; incremental retirement has hardcoded +2 despite 3 frame
slots. Audit/fix these separately with focused proof; do not call them fixed yet.
Local atlas init publishes L.ok only on success, and local instance growth
returns before recording on allocation refusal. Caster wobble reconciliation
only rebuilds CPU live arrays; no Vulkan owner is released there.

All requested root-fix acceptance proofs above are green. Separate follow-ups:
sibling partial-init guards and 3-slot retirement findings above; resize not run
because first-load allocation refusal reproduced the exact W91 crash before any
camera cut. No budget increase, disabled bake, image repin or default-OFF gate.
Disk: sole install E:/mc2-installs/claude-shadow-crash-fix retained (with baseline
exe/PDB for review); no extra installs created. Removed 261,560,458 bytes of PDB
type-dump temp data from G:. All local children and farm job completed.
Tool slowness: targeted doctest build 240 s; scout audit timed out at 420 s.
Largest avoidable cost: wide PDB type dump (261 MB) and delayed search of the
pre-crash residency log. Next investigation should search allocation refusals
before dumping type records; use exact type indexes if a layout is needed.
