# Temporal AA experiment: owner decision

Capture/build: `91340950252975bea50a272168e1fc9c591030a8` on
`claude/temporal-ab`. Report-only continuation; rendering defaults unchanged.
The W88 fixup retired all experimental runtime modes and their gates.
SMAA remains the current reference behavior.

Evidence: `E:/mc2-archive-2026-10/proofs/temporal-ab/index.html` and
`details-gallery.html`; `summary.md` contains per-map metrics, GPU status,
frame identities and cleanup. The SHA-named summary identifies the report tip.

| Mode | Ghosting judgment | Shimmer/detail judgment | RX 7900 XTX, 4K cost | Recommendation | Experiment default |
|---|---|---|---|---|---|
| A: SMAA 1x | No temporal history; no history trails expected | Reference; thin foliage/fences and distant edges remain unstable during rotation | NOISY day/night; failed mc2_24; unproven | Keep current behavior | OFF; reference selectable only in experiment |
| B: SMAA S2x / MSAA 2x | Untested | Untested | Unmeasured | Defer: multisample attachments/pipelines exceed cheap enablement | OFF / unavailable |
| C: simple TAA | No obvious long trail in inspected six-frame frozen-scene strips; moving-object/disocclusion ghosting unproven | Softer fine roof/fence detail; slow-pan MAD 27–36% below SMAA, but static-hold variance increases on all three maps | NOISY day/night; incomplete mc2_24; unproven | Do not ship this prototype; retain only for owner comparison | OFF |
| D: FSR1 EASU + RCAS, 67% | Spatial; no temporal history trails expected | Coarser/sharpened thin detail; slow-pan MAD 30–40% above SMAA | NOISY day/night; incomplete mc2_24; unproven | Consider only as a performance option after accepted cost evidence; not a demonstrated shimmer fix | OFF |
| E: AF16 + Toksvig-style roughness | Spatial; no temporal history trails expected | Close to reference in viewed crops; slow-pan MAD 12–29% above SMAA; no consistent demonstrated shimmer win | NOISY day/night; incomplete mc2_24; unproven | Preferred direction for spatial follow-up, not approved to ship from this result | OFF |

Keep SMAA and leave every experiment off. The evidence supports owner review,
not enabling a new option: TAA trades reduced moving-image differences for
softness and unresolved disocclusion behavior, while FSR1 needs a credible cost
benefit before accepting its detail loss. AF/roughness filtering warrants spatial
follow-up without a claim that this approximation solves shimmer. Eye adaptation
was not an arm and receives no recommendation to enable.

Three maps (`mc2_01` day, `mc2_10` night, `mc2_24`), four implemented arms:
301 consecutive orbit frames at projection 20/altitude 400, 360 degrees over
10 seconds nominal camera time; 121 low-horizon sweep frames; static hold and
slow pan; separate foliage/fence/building/roof/water views. Captures are 1080p;
only timing runs are 4K. Frames freeze simulation and foliage at seeded sim
frame 300, so moving actors, animated foliage and changing illumination are
outside the ghosting evidence. The low sweep is 4 seconds nominal. Six native
lossless consecutive crops accompany reduced overview animations.

The terrain/water wedges in the foliage control already occur in SMAA. They
are a **baseline confound**, excluded from AA judgment. Keep the original
control; inspect palm crowns and use `foliage-crowns-metrics.json` (ROI
1380,350–1620,440, source frames 3–8). Original center-ROI foliage measurements
include wedges and must not rank modes. Water views showing those artifacts
also cannot establish a water-quality improvement. No claim of a visible
specular highlight is made where the roof capture lacks one.

Numbers are linear-light luminance measurements. Slow-pan MAD is
uncompensated, includes real camera motion and is not a pure shimmer score.
Static holds with frozen content do not measure animated-foliage instability.
E inflates roughness using derivative normal variance, not stored Toksvig mip
moments. C clamps encoded final color and lacks depth-history/reactive rejection.
All cost rows require four quiet repetitions, matching timestamp windows,
explicit 7900 XTX receipts and an exclusive lease. NOISY means unproven, not
a performance estimate; raw host samples and GPU logs remain beside each arm.

The completed day/night timing sets were NOISY. The resumed `mc2_24/A_smaa`
attempt failed the harness verdict; its log is retained and its timestamps are
not accepted. The log contains three `SIM_OVERLAP` violations, starting with
`sim-thread-called-main-api site=ObjectTypeManager::get load type=0 count=1`.
No gate was weakened or overridden. Remaining mc2_24 timing sets are incomplete. Consequently this
report establishes no accepted 4K cost or performance ranking for any mode.
The crown-only six-frame pan MAD is 0.012529 (SMAA), 0.006358 (TAA),
0.013832 (FSR1), 0.013548 (AF/roughness). TAA's crown hold variance is
0.000018819 versus SMAA's 0.000000001789, consistent with visible jitter/softness
rather than a free stability improvement. These tiny frozen-scene holds do
not replace moving-foliage or disocclusion tests.

Validation inherited from the completed capture build: RelWithDebInfo engine
and viewer build, five selected shader/gate contract rows, four resource-graph
doctests (47 assertions), default-off day/night smoke, deterministic frame/seed
identity audit. Continuation reran metric tests (2 passed); documentation uses
`git diff --check`. No engine code or defaults changed in this continuation.
The completed experiment's motion attachments were removed in the W88 fixup.
This decision and archived evidence analysis remain available.
