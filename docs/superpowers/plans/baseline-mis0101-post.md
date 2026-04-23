# Post-instrumentation perf baseline (mis0101)

**Status:** PENDING — user game-launch capture required.

Methodology per plan Task 27 (`docs/superpowers/plans/2026-04-23-stability-tier1-instrumentation.md`):

1. Close `mc2.exe`.
2. Launch with all three gates **OFF**:
   - unset `MC2_TGL_POOL_TRACE`
   - unset `MC2_DESTROY_TRACE`
   - set `MC2_GL_ERROR_DRAIN_SILENT=1`
3. Load `mis0101`, camera at spawn, 30-second Tracy capture.
4. Save trace to `docs/superpowers/plans/baseline-mis0101-post.tracy`.
5. Compute deltas vs `baseline-mis0101-pre.md` from Task 0.
6. Thresholds: median within 2%, P99 within 5%, P99.9 within 10%.

**Deltas (TBD):**
- median: pending
- P99: pending
- P99.9: pending
