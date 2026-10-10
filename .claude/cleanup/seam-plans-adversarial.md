<!-- doc-status: stale | reviewed:2026-10-11 -->
# Adversarial review: seam2 / logging / strangler (2026-10-09)

## Seam #2 (GPU structs)
BLOCKERS
1. Include path wrong. Plan says `#include "gpu_abi/X.gen.hglsl"`. Real forms: GL `#include <include/foo.hglsl>` (shaders/static_prop.vert:21); VK `#include "../include/foo.hglsl"` (vulkan/color_prop_tier1.frag:90). GL needs `<include/gpu_abi/..>`, VK `"../include/gpu_abi/.."`. shadow_static_prop.vert and VK vert files have no includes today: first use, must verify the engine resolver (shader_builder.cpp:262) and glslc both resolve nested subdir.
2. CMake DEPENDS glob `shaders/include/*.hglsl` (GameOS/gameos/CMakeLists.txt:365) is non-recursive: gpu_abi/ subdir NOT tracked, so editing generated file does not rebake SPIR-V. Plan cites this glob as sufficient: false. Add `gpu_abi/*.hglsl` glob (or put files flat in include/).
3. .claude/hooks/shader_deploy.py flattens: dst = shaders/include/<basename> (subdirs lost) -> gpu_abi/ file lands at wrong path, GL runtime include fails. deploy_payload.py:843 os.walk is fine. Fix hook or use flat include/ (recommended: `include/gpu_abi_static_prop.gen.hglsl`).
4. Mirror count is 8 not 7: RenderCore/StaticPropInstanceDesc.h:29-42 (alignas(16) 112B, comment "KEEP IN LOCKSTEP") omitted. Also bare `float x[4]`/`modelMatrix[16]` in mech GpuMechInstance etc.
5. .gitattributes has no eol rules (2 lines, LFS only): generator "LF" claim needs the attr added in slice 1, else --check diffs on autocrlf.
RISKS: generator unchecked (add golden self-test with known std140/430 vectors incl. vec3+float packing, float[N] in UBO = 16B stride, mat3 = 3xvec4, bool=4B in GLSL, nested struct align 16 in std140; cross-check with glslangValidator -V reflection or spirv-cross on one shader). Pilot hash: grep pilots.json before slice 1 (static_prop not seen as pilot; mech is -> slice 3 forces rebake). Row-vs-column mat4 comment is manual.
AMEND: flat include file; recursive glob; fix hook; add Desc to slice 1; add .gitattributes first.

## Logging
BLOCKERS
1. Macro/env naming: MC2_LOG is env var AND proposed macro; fine technically, but `MC2_LOG` spec parse must run before gameosmain.cpp:1145-1175 NUL redirect; static-init logging (before main, DLLs) hits uninitialised g_threshold: make thresholds constant-initialised (constinit) legacy-default.
2. LG12 count (2704) includes sprintf/snprintf/vsnprintf buffer formatting (R_PRINTF, cxx17_ratchet.py:116), not logging: cannot reach 0 via MC2_LOG. Split LG12 into log vs buffer-format.
3. MC2_EVENT vs raw printf ordering: stdout (unbuffered) vs stderr streams; facade "one fwrite per line + sink mutex" must still hit the same FILE*; if buffered via thread_local then fwrite, ordering with remaining printf calls OK only if same FILE*. Smoke parsers read merged stream.
RISKS: std::format throws on bad runtime format (format_string compile-checks only literals; SPEW/user-format sites runtime -> use vformat+try/catch); '{' '}' in existing literals (JSON-ish text) need escaping, codemod must escape; %s nullptr; %ld (long 32-bit on Win) vs int64; wide strings; hot path: macros fine but `allowRate` per-site cost and format in per-draw loops - keep Trace compiled out. gosASSERT/ErrorHandler unchanged. Parsers at risk: every regex listed in plan section 2 plus oracle scripts setting *_TRACE env (alias table).
AMEND: golden harness before P2; constinit defaults; fold only print-only gates.

## Strangler
BLOCKERS: Roadmap section 6 stale: CMakeLists.txt:12 already MC2_CXX_STANDARD 23, so P3/P4 "bump" phases are done; plan must re-sequence to adoption only. Baseline.json is a single shared file: every parallel branch/agent tightening it conflicts; make baseline per-dir files or tighten only on main.
RISKS: --check scans working tree (dirty, other agents' uncommitted work counts, can block your commit); git diff HEAD only for sampling (cxx17_ratchet.py:333). Moving code across dirs (code->mclib) raises one dir's count. Heuristic regex gameable (comment/string blanking, splitting lines). Per-dir granularity OK; add per-file cap for new files (new file must have 0).
AMEND: check vs merge-base on committed files in CI; local advisory only.

## Sequencing
C++23 flip: done. Better: seam #2 slice 1 (shader-only, small) -> logging P0/P1 (no call-site churn) -> mechanical clang-tidy by directory in quiet windows, AFTER logging pilot/ seams in flight, rebaseline once per batch -> nulls/overflow -> compile speed. Never run clang-tidy repo-wide while seam/logging branches open.
