# Orchestrator handoff — modernization / tech-debt paydown (2026-10-09)

Owner intent: **pay down most/all tech debt before building features**. No time pressure; thorough + verified beats fast. Then build.

## Ground rules (owner-set, non-negotiable)
- Work ONLY in `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`. Local commits only — **never push**.
- **One agent edits C++/shaders/builds at a time.** Read-only scouts and Python/doc-only agents may run in parallel (they must not touch C++ or build).
- Model tiers: **Haiku** for mechanical/doc/memory edits (keep each <100k context: one file or small batch, exact file:line + replacement, "don't read beyond listed files"); **Sonnet** for code changes and design; **Opus** only for adversarial reviews.
- **Questions to the owner go inline, numbered, at the end of the reply. Never use popup (AskUserQuestion) questions.**
- **Look at captures yourself** before accepting any visual/render change (`tools/visual_verify/ab.py`; day AND night: `mc2_01_saved_205644`, `mc2_10_saved_204523`). MAD is NOT a pass signal.
- Adversarial audit (separate read-only agent) before deleting features/data; mechanical modernization doesn't need one.
- Path-existence / "does X still exist" checks: do them with a **script**, not an agent (Haiku agents produced ~65 false "(no longer exists)" tags from wrong cwd).
- Text edits to shaders/code: exact, line-anchored, re-read the region after editing (a text replace once spliced an `#include` into a comment and silently disabled every GL static prop).
- Commit BEFORE build+deploy (DEPLOY_FINGERPRINT gate). Captures: `MC2_CURSOR_NO_PARK=1`.

## State at handoff (verify with `git log`)
- C++23 default (`MC2_CXX_STANDARD=23`, MSVC 14.44). Full default build green (13 targets; text_tool EXCLUDE_FROM_ALL). Linux deferred.
- Enforced checks added this session: smoke shader-error gate; `cxx17_ratchet` (LG01–LG18, scans HEAD); `flag_registry_drift --strict`; gate-default agreement incl. registry-vs-reader; `gl_shader_variants` (231 variants, offline); `vk_pipeline_layout` (offline); `gpu_abi` (single-source GPU structs).
- Seam #2 slice 1 done: `GpuStaticPropInstance` 8 mirrors → 1 (`RenderCore/gpu_abi/`, `tools/gpu_abi_gen.py`).
- Logging facade P0/P1 DONE: `GameOS/gameos/utils/mc2log.{h,cpp}`; `mc2::log::init()` runs before the NUL redirect; `MC2_LOG=1` legacy kept; `MC2_LOG_DECORATE`; category `off` also silences that category's `MC2_EVENT`; out-of-line vformat, nothing formats when `MC2_LOG` unset. Pilot `gos_terrain_patch_stream.cpp` converted (tags shape-identical on 5 missions). Not yet: Tracy sink, `MC2_LOG_FILE`, `mc2_diag::writeEvent` bridge, legacy alias table. Pilot obj 505 KB (rest is CodeView). 638/638 unit tests.
- Terrain-type→material single-sourced (`mclib/terrain_material.h`; fixed type 20 = Rock in water stream).
- S1 loop-speed DONE (61f63be48): capture early-exit (one capture 157 s → 17 s; full GL/VK day+night ab.py = **64 s**), parallel check-contracts (270 s → 52–91 s; `-jN`/`--serial`), two slow rows 45 s/21 s → ~1 s, `run_smoke.py --lite` (mc2_10+mc2_01, 44 s; inner-loop default), full tier1 224 s. Remaining S1: Ninja dev build.
- Final verification at handoff (c6de9a23e+): contracts green, tier1 5/5 fingerprint OK, A/B day+night viewed by orchestrator — parity intact.
- Deploy gotcha: run `py -3 scripts/deploy_payload.py --target game` before smoke; `run_smoke --deploy 0.4` alone does not deploy. Fingerprint fails if anyone moves HEAD between build and smoke.
- `.claude/agents/` is gitignored (local-only agent definitions; updated this session).

## Plans to execute (all in `.claude/cleanup/`)
Primary roadmap: **opus-modernization-review.md** (S1–S5) + **opus-consistency-review.md** (D1–D11).
Supporting: cxx17-strangler-plan.md, seam2-gpu-structs-plan.md, logging-facade-plan.md (Amendments section overrides), seam-plans-adversarial.md, offline-checks-plan.md, giant-file-split-plan.md, clang-tidy-pareto.md, checks-and-lanes-plan.md, tierA-gates-plan.md, tierB-trace-gates.md, terrain-gates-plan.md, from-scratch-review.md, broken-targets.md, hires-heightmap-plan.md (visual, later).

## Ordered queue
1. **S1 finish** — Ninja dev build (kills 3 stale-binary traps), logging de-bloat verified (obj ≤ ~460 KB), target ~7 min per verified change. Fix `CLAUDE.md` C++17 → C++23 line if still present.
2. **D1/D2 — DONE** (check-contracts rows `vk_ubo_layout`, `glsl_port_diff`, `vk_shaders_compile`; coverage checker now scans `tools/check_*.py` + `tools/glsl_port_diff.py`). `check_ubo_layout` multi-declarator parse fixed, 13/13 pairs clean. `glsl_port_diff --all` has per-port rename maps; the 2 REAL VK gaps (mech `MC2_SUN_AXIS_FIX` in `calc_light`, water shore meander in `waterBaseColor`) are pinned in `tools/glsl_port_diff.allowlist` (hash of the exact diff + WHY; fails if the entry goes stale or the diff moves). `visualDisplaceFar` unregistered: GL `terrain_lod_chunk.vert` has no PORT markers (add them, then re-add the two registry rows).
3. **S2** — typed gate registry + logging P2 in the same diffs (1504 `getenv` in 204 files; one env-flag accessor — ~45 local helpers disagree on "=false"/empty; one registry parser instead of 10). Delete ≥1 text-lint contract row per typed seam.
4. **D3/D4 + seam #2 slices 2–4** — VK terrain shader de-copy (skirt byte-copy; unused `terrain_frame_vk.glsl`); mech instance, ObjLightsMirror, VK UBO/push constants into gpu_abi.
5. **S3** — narrowing: fix ~10 `mclib/mlr` headers (~2000 of 3178 C4267), then warnings-as-errors per dir; `_MSVC_STL_HARDENING` in RelWithDebInfo; `/std:c++23preview` + `/Zc:__cplusplus` + `/utf-8`; measure `/Ob2`.
6. **clang-tidy safe set** per directory in quiet windows (nullptr/override/using/…); ratchet split into drive-to-zero vs freeze-only; track half-migrated files; retire-by deadlines.
7. **D5–D11** — shared VK helper (load_spv/vlog ×13), GL-gate→VK coverage check, retire VK probe fixture, duplicated-lines ratchet, typed `(-x,z,y)` axis conversion (~17 hand copies).
8. **S4** — VK RAII in one island, offline mission-load test, giant-file splits (`gosRenderer` to header first).
   **File-size program (owner priority — agent context efficiency + incremental builds):** split owned `.cpp`/`.h` by responsibility to a soft target of ~800–1500 lines (diagnostics → `*_diag.cpp`), per `giant-file-split-plan.md` (gameos_graphics 11.8k, gos_static_prop_batcher 11.2k, gos_postprocess 7.7k, vulkan_backend_skeleton 7.6k, txmmgr 6k, …). Add a **file-size ratchet** (count of owned files > 1500 lines and max lines per dir may only go down) to check-contracts. Offset extra header parsing with **PCH**, not modules: C++23 `import std;`/modules are NOT adopted yet because clangd/clang-tidy can't read MSVC BMIs (would break our clangd + clang-tidy tooling); re-evaluate when clangd supports it, optionally pilot `import std;` in an isolated tool target. Can start early (in parallel planning with S2) since smaller files make every later seam cheaper.
9. Gate cleanups (terrain gates, tier A/B) per their plans.
10. **S5** (medium-term perf) — Tracy baseline, 2 frames in flight, threaded update. Perf changes only with Tracy deltas.

Parked visual work (after debt paydown): VK mech shadows (vk-mech-shadows-plan.md), mc2_10 hi-res recook sweep, road ground color (brownish vs grey), parity items H2/H4/M1/M6/M7 (consistency-code.md), dead shaders gos_grass.*/overlay_alpha_clear.frag (audit first).

## Session exit bar
Ratchet baseline tightened (counts only down), check-contracts all green, tier1 smoke 5/5 (or smoke-lite inner loop + full tier1 at milestones), A/B viewed, memory handoff updated (`MEMORY.md` RESUME line).
