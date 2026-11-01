# Cull parity lane state (claude/cull-parity, base e5cf6ba9c)

Done (uncommitted until proofs green):
- RenderCore/hzb_prop_bounds.h: added `mc2hzb::hzb_hidden(ProjectedProp, baseW, baseH, fetch)` (CPU mirror of hzb_cull.comp mip test); `project_prop` already mirrored the projection.
- tests/unit/test_hzb_cull_reference.cpp (+ tests/unit/CMakeLists.txt): 13 doctest cases.
- GameOS/gameos/vulkan_post_hzb_native.cpp: counter-only `[CULL_PARITY v1]` check, one frame in 64, 4096 strided samples, mips copied on check frames only, Tracy zone `HZB.ParityCheck`; level images gain TRANSFER_SRC usage.
- scripts/smoke_lib/logparse.py: red marker `cull_parity_false_cull`, presence rule `cull_parity_missing` (mc2_01, mc2_10).

Next: unit run, ninja build, check-contracts, pytest, deploy to E:/mc2-installs/claude-cull-parity (copy, --allow-new-target --ninja), smoke-lite --exe, mc2_24 profile, validation run on mc2_10, commit, report.
