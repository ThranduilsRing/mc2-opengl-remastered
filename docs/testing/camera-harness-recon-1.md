<!-- doc-status: stale | reviewed:2026-10-11 -->
# CAMERA HARNESS TARGET RECON-1

**Status:** RECON ONLY (read-only, no code changes). Verdict: **GREEN on a
pure-math subset; DEFER the Camera-member projection math.**
**Recommended target:** `CAMERA-FRUSTUM-HARNESS-1`.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Recurring camera bug classes (commit evidence)
1. **Pick / cursor-target** (dominant): near-plane straddle `d56d0377`, PICK-CACHE-UAF
   `d14d45e5`, hover-cache validate `5841f71c`/`cd52608b`/`40807a21`, right-click snap
   `3bffb04d`/`be5c3009`. Open residuals: rect-size sanity clamp; `MC2_PICK_BEHIND_LEGACY`.
2. **Aspect / pillarbox**: FORCE-43 `454ef74c`/`c6cc6f98` + box-relative mouse remap.
3. **Frustum cull plane extraction**: documented D3D↔GL split-brain (mirrored L/R planes
   → on-screen LOD chunks culled). Real on-screen regression class.
4. Eye/altitude clamp `ed90e823`/`32bbc69f` (small, not edge-shaped).

## Verdicts
| Candidate | Verdict | Why |
|---|---|---|
| Pick/unproject round-trip, `projectForScreenXY`, aspect/`cameraToClip` | **DEFER** | Camera member fns reading live instance matrices (`worldToClipGL`, viewMul/Add). Already adjudicated: `tests/unit/test_projection.cpp` is **SCOPE-STOPPED** (camera.h in ~30 TUs, class-layout risk). No new evidence overturns it. |
| `gos_Compute43Box` pillarbox rect + box-relative mouse remap | **GREEN** | `gameos_graphics.cpp:8116` — pure int/double math, only env coupling is one `getenv`. No GL/Camera. Link the symbol, or lift to `gos_aspect_box.h` if the TU drags GL link. |
| `extractFrustumPlanes` | DEFER | reads `worldToClipGL()` (Camera-coupled). |
| `quadAabbInFrustum(planes, mn, mx)` (camera.cpp:790) + `s_pointInScreenTri` (camera.cpp:834) | **GREEN** | **pure functions of their parameters**, zero Camera/GL state. Gribb-Hartmann sign convention documented camera.cpp:744-755. Feed synthetic planes → exercises real code (lift to shared header; no fake-green). |

## Recommendation: `CAMERA-FRUSTUM-HARNESS-1`
Hits the highest-value recurring class (frustum split-brain — a real on-screen cull
regression) with pure parameter-driven math + near-zero include surface (IBL tier).
Pair with `gos_Compute43Box` rect tests. Proposed tests:
- Frustum/AABB (synthetic planes → real `quadAabbInFrustum`/`s_pointInScreenTri`):
  inside→admit; behind one plane→reject (p-vertex); straddle→admit (conservative);
  zero-extent AABB→no NaN; **mirrored L/R plane** guard (locks the split-brain fix);
  screen-tri inside/edge/outside/degenerate.
- Pillarbox (`gos_Compute43Box`): 1920×1080→`obw=1440,ox=240`; 1024×768→no-op (false);
  800×900→letterbox; w=0/h=0→inactive (no div0); box-relative remap round-trip identity.

## Cost / caveat
Both subsets are pure but currently sit as a stateless member (`quadAabbInFrustum`) and
in a GL-heavy TU (`gos_Compute43Box`). Small behavior-preserving extraction to shared
headers is the no-fake-green path (tier1-gated production touch, like prior extractions).
`s_pointInScreenTri` is already a free static.

## Live-bug note
No NEW live bug (recent pick fixes verify clean at HEAD). Two **documented open residuals**
(not regressions) are the harness's natural future home: (1) rect-size sanity clamp — a
`behind:0` object still targetable from ~500px via oversized-but-valid rect (objmgr
`projectPickCandidateRect`, d56d0377); (2) hard "reject if any corner behind" boundary.
Both pure-given-projected-corners; if a real fix lands, extracting the `bcsp[8]→rect/reject`
decision becomes the test target (mirrors how objmgr's deferral was lifted).
