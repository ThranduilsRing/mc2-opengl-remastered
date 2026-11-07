# rest legacy-idiom sweep (branch claude/cxx23-rest-sweep, base 2d042f852)

Result: ratchet --check --worktree: 43 fewer legacy sites than baseline (mclib 36 files' worth, GuiRuntime 4, GameAdapters 3).
Full ninja build (-k 0) exit 0 at the final code state.

## Changed
- LG09: functional casts -> static_cast (int/float/unsigned/unsigned long long); (MemoryPtr) -> reinterpret_cast.
- LG10: one-sided clamps -> std::min/std::max, operand order preserving tie/NaN behavior of the original compare; both operands same type (mech3d_type uses std::min<long>).

## Skipped
- RenderCore/bcdec.h (31 sites, vendored): ratchet EXCLUDE_PARTS is a set of directory names only (3rdparty, Tracy, ...), not files; no file-level exclusion exists, so it is listed and left. Fix would be moving bcdec.h under 3rdparty/ or adding a file-level exclusion.
- mclib/stuff/*container* files: none had sites. mclib/stuff/style.hpp Clamp macro left (macro, other users).
- LG18 (all): pointer reinterpret_casts and unions (ablexec.h, ablsymt.h, ablxstd_stack, alloc_probe, mouse, soundsys, utilities, tube, mlr_i_tmesh): no value copy, not bit_cast candidates.
- LG08 (all): map-iterator it->second / mit->second (assimp_importer, content_ir, mech_skel_import, mech3d_render/update): no structured-binding gain.
- LG09 false positive: mlr/mlrsorter.hpp member-function-pointer type; texture_assets.cpp:79 (MemoryPtr)&header left only if not matched (done: line 57 only).
- LG10: argmin/argmax with extra assignments (arm_node_remap, camera_project, projectz_trace, ImpactDecals, terrain_subpass_contract, GuiRuntime_internal), debug-only tube.cpp, vk_object_light_gather probe, GuiRuntime_draw (already std::max).
