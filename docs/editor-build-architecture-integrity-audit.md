<!-- doc-status: current | reviewed:2026-10-11 -->
# Editor Build — Architecture Integrity Audit

**Audit ID:** `EDITOR-ARCHITECTURE-INTEGRITY-OPUS-1`
**Date:** 2026-06-02
**Worktree:** `A:/Games/mc2-trackv-ci-gate-restore` (post-merge equivalent of nifty `6489a671`)
**Method:** Read-only static audit, 3 Sonnet lanes + Haiku workers. Same style + severity rubric as the Track V audit. No code/CMake/shader changes.
**Targets audited:** `EditRel` (legacy mission editor, `editor/`, 71 cpp) and `mc2_asset_viewer` (`tools/asset_viewer/`, ImGui/SDL2/GL, Methuselas merge).

---

## 1. Executive verdict

**The editor build is SAFE to keep using. Zero P0 blockers.** But it is the **least-guarded corner of the tree** and carries real architectural debt the game side has already cleaned up.

Two themes dominate:

1. **The editor is outside every firewall and CI gate.** `scripts/check-include-firewall.sh` does **not** scope `editor/`, and neither editor target has any test/smoke/ASan coverage. So the EditorBridge layer (which exists and is well-designed) is silently bypassed in 3 places, and any RenderCore/GameOS API change that breaks the editor link goes undetected until a human builds it by hand.

2. **The legacy editor's 3D render path predates reverse-Z.** `EditorGameOS.cpp` sets up forward-Z depth (`GL_LEQUAL`, no `glClipControl`) but runs the engine's reverse-Z shaders — so editor 3D depth testing is wrong. Pre-existing, not introduced by Track V, but now clearly identified.

**`mc2_asset_viewer` is CLEAN** — genuinely standalone (SDL2+GLEW+ImGui only, no engine/game internals, no shared GL state), 2D-only in stage 1 so the depth question is N/A. The newer tool is the architecturally correct one; the legacy `EditRel` is where the debt lives.

**No P0. Six P1s**, all either pre-existing debt or missing-guardrail — none block current use, but they should be fixed before any serious editor feature work.

---

## 2. Severity table

| Sev | Lane | Finding | Evidence | Risk | Slice |
|-----|------|---------|----------|------|-------|
| P1 | A | `EditorData.cpp` includes GPU-pipeline internals directly (`gos_static_prop_batcher.h`, `gos_static_prop_registry.h`, `gpu_cull_substrate/compute/readback.h`) — bypasses EditorBridge | `editor/EditorData.cpp` | Couples editor to GPU internals; undetected (firewall script ignores `editor/`) | M |
| P1 | A | Raw GL in editor pick handler (`glGetIntegerv`/`glBindFramebuffer`/`glReadBuffer`/`glReadPixels`) outside EditorBridge | `editor/EditorInterface.cpp:3682-3720` | Firewall breach; OID-scan diagnostic. Gated by `s_pickDiagDone` (fires until first pick) — partial mitigation | S |
| P1 | A | Raw GL state driven in editor frame loop (`glViewport`/`glEnable`/`glDepthFunc`/`glClear`) outside gosRenderer state machine | `editor/EditorGameOS.cpp` | Can collide with gosRenderer state expectations; partial doc mitigation (comment warns vs `glUseProgram(0)`) | M |
| P1 | C | **Editor 3D depth convention wrong**: `GL_LEQUAL` forward-Z setup, but `glClipControl(ZERO_TO_ONE)` is never linked into the editor while it runs reverse-Z shaders → all editor 3D depth testing against default `[-1,1]` NDC | `editor/EditorGameOS.cpp:351`; `glClipControl` only in unlinked `gameosmain.cpp` | Editor 3D preview depth-sorts wrong. Pre-existing | S |
| P1 | B | **Neither editor target has CI/smoke/test/ASan coverage** — default-built on MSVC but never verified | `editor/CMakeLists.txt`, `tools/asset_viewer/CMakeLists.txt`; no smoke matrix entry | RenderCore/gameos/imgui API change silently breaks editor link until manual build | M |
| P1 | B | `find_package(SDL2_ttf REQUIRED)` but SDL2_ttf is **never in `target_link_libraries(EditRel)`** (only DLL-copied); vendored `sdl2_ttf-config.cmake` calls `cmake_minimum_required(3.0...3.5)` → policy warning that becomes a hard error in future CMake | `editor/CMakeLists.txt:78`; `…/cmake/sdl2_ttf-config.cmake:10` | REQUIRED hard-fails configure for a lib EditRel doesn't link; future-CMake break | S |
| P2 | B | ASan DLL copied next to `mc2.exe` under `MC2_ASAN=ON` but **not** next to `EditRel` | editor CMake post-build vs main target | ASan build of editor launches with `0xC0000139` (entry point not found) | XS |
| P2 | C | Dead upload error check `if (glTexture == 0)` after `glGenTextures` (always false); no `glGetError()` after `glTexImage2D` | asset_viewer texture upload | Silent VRAM-OOM marked `loaded=true` | XS |
| P2 | C | Unbounded asset_viewer texture cache `g_cache` — no eviction until `Shutdown()` | asset_viewer image cache | VRAM grows monotonically across a browse session (dev tool) | S |
| P3 | A | include-firewall contract scope does not include `editor/` (root cause enabling the A-lane bypasses) | `scripts/check-include-firewall.sh` | Editor firewall drift undetectable | S (extend scope) |

---

## 3. Cross-cutting invariants (editor)

| Invariant | Status | Note |
|-----------|--------|------|
| EditorBridge firewall (editor → engine one-way) | ⚠ BYPASSED | Bridge exists, narrow, well-designed (`EditorRenderBridge.h`/.cpp, tight allowlist) — but bypassed by EditorData / EditorInterface / EditorGameOS (Lane A) |
| asset_viewer standalone isolation | ✅ CLEAN | No engine/game internals, no shared GL state, own SDL2+GLEW context |
| Reverse-Z depth convention | ❌ NOT HELD (editor) | Editor 3D uses forward-Z setup vs reverse-Z shaders (Lane C); viewer is 2D so N/A |
| CI / build guardrails | ❌ NONE | Neither target tested/smoked/ASan-covered (Lane B) |
| Build-by-default & rot-resistance | ⚠ DEFAULT-BUILT, UNVERIFIED | Both default-built (MSVC); legacy `Viewer/` correctly disabled + documented |

---

## 4. Lane reports

### Lane A — Editor firewall — CONCERNS (P0:0 P1:3)
**Checked:** EditRel + asset_viewer reach into engine/game internals; EditorBridge narrowness; shared globals; object-id/pick ownership; resource duplication.
**Verdict:** EditorBridge (`EditorRenderBridge.h`/.cpp) exists, is narrow and one-way with a tight allowlist — but `editor/` code bypasses it in 3 spots (EditorData GPU-internal includes; EditorInterface raw-GL pick handler; EditorGameOS raw-GL frame state). `mc2_asset_viewer` is fully standalone/clean. The include-firewall contract does not watch `editor/`, so these are invisible to CI.
**Follow-up:** route the 3 bypasses through EditorBridge; extend `check-include-firewall.sh` scope to `editor/`. Full: `docs/audit-lanes-editor/lane-A-editor-firewall.md`.

### Lane B — Editor build/CMake/coverage — CONCERNS (P0:0 P1:4 P2:3 P3:3)
**Checked:** default-build status, the `:78` find_package warning, dep sanity, legacy Viewer disablement, test/smoke/ASan coverage, flag consistency, editor env gates, SDL2/GLEW fallback.
**Verdict:** No configure-fatal break today, but 4 latent P1s. Both `EditRel` and `mc2_asset_viewer` build by default (MSVC) yet have **zero** verification coverage. `editor/CMakeLists.txt:78` `find_package(SDL2_ttf REQUIRED)` is both a future-CMake policy break (vendored config's `cmake_minimum_required` upper bound) and a logic smell (REQUIRED but never linked). ASan DLL not copied for `EditRel`. Legacy `Viewer/` hard-disabled and documented (root:535).
**Follow-up:** add a minimal editor build-smoke to CI; drop SDL2_ttf to non-REQUIRED or actually link it; copy ASan DLL for EditRel. Full: `docs/audit-lanes-editor/lane-B-editor-build.md`.

### Lane C — Editor GL/render correctness — CONCERNS (P0:0 P1:1 P2:2)
**Checked:** raw-GL safety, depth convention, FBO feedback, texture/KTX/BC7 reuse, ImGui lifecycle, GL-object leaks per reload, shared state, RenderCore reuse.
**Verdict:** `mc2_asset_viewer` GL is safe; stage-1 viewer is **2D-only** (no depth, no clipControl) so the reverse-Z question is N/A and internally consistent; standalone process, no shared GL state. The **legacy editor** has a real depth-convention defect: `EditorGameOS.cpp:498` sets `GL_LEQUAL` forward-Z but never calls `glClipControl(ZERO_TO_ONE)` while running reverse-Z shaders → editor 3D depth testing is wrong (pre-existing). Minor viewer issues: dead texture-upload error check, unbounded texture cache.
**Follow-up:** add `glClipControl`+`glClearDepth(0)`+`GL_GEQUAL` to editor `InitGameOS`; real `glGetError()` upload check; LRU cap on viewer cache. Full: `docs/audit-lanes-editor/lane-C-editor-gl-render.md`.

---

## 5. Top fix queue

| # | Fix | Lane | Scope |
|---|-----|------|-------|
| 1 | Copy ASan DLL for `EditRel` post-build (unblocks ASan-ing the editor) | B | XS |
| 2 | `editor/CMakeLists.txt:78` — make SDL2_ttf non-REQUIRED or actually link it; kill the future-CMake policy warning | B | S |
| 3 | Add minimal editor build-link smoke to CI (compile+link both targets) — stops silent rot | B | M |
| 4 | Fix editor 3D depth: `glClipControl(ZERO_TO_ONE)` + `glClearDepth(0)` + `GL_GEQUAL` in editor `InitGameOS` | C | S |
| 5 | Route the 3 editor firewall bypasses (EditorData includes, EditorInterface pick GL, EditorGameOS frame GL) through EditorBridge | A | M |
| 6 | Extend `check-include-firewall.sh` scope to `editor/` so bypasses are caught | A | S |
| 7 | asset_viewer: real `glGetError()` upload check + LRU texture-cache cap | C | S |

---

## 6. Stop-doing list

- **Do NOT invest in `EditRel` 3D-preview features** before its depth convention is fixed — you'd be building on a wrong depth test.
- **Do NOT treat `mc2_asset_viewer` as needing firewall work** — it's correctly standalone; keep it that way (don't let it start including engine internals as it grows into stage 2).
- **Do NOT "fix" the legacy `Viewer/` target** — correctly disabled + documented; leave dead.
- Keep editor severity weighted for a **dev tool** (lower blast radius than the shipping game) — none of these are ship-blockers.

---

## 7. Next prompt (highest-priority fix)

> **`EDITOR-CI-COVERAGE-1` (scope: M, CMake/CI only — no editor behavior change)**
>
> The editor build (`EditRel` + `mc2_asset_viewer`) has zero verification coverage and one configure-fragility. Make it rot-resistant.
>
> 1. Copy `clang_rt.asan_dynamic-x86_64.dll` next to `EditRel` output under `MC2_ASAN=ON` (mirror the `mc2` target's post-build copy).
> 2. In `editor/CMakeLists.txt:78`, change `find_package(SDL2_ttf REQUIRED)` to non-REQUIRED (or add SDL2_ttf to `target_link_libraries(EditRel)` if it is genuinely needed) and silence the vendored-config `cmake_minimum_required(...3.5)` policy warning (e.g. pin policy or document).
> 3. Add a CI/smoke step that **compiles and links** both editor targets in RelWithDebInfo (link-only gate — no runtime). Wire it next to the existing contract/test gates so a RenderCore/gameos/imgui API change that breaks the editor link fails fast.
> 4. Verify: configure emits no policy warning; both targets build clean; ASan editor build launches (no `0xC0000139`).
>
> Constraints: CMake/CI only, no editor source behavior change. Fresh branch off nifty. Do NOT touch the shared nifty worktree except as a normal feature branch.

---

## Audit process record

- **Targets:** `EditRel` (legacy editor), `mc2_asset_viewer` (Methuselas tool). Legacy `Viewer/` confirmed disabled+documented.
- **Lane verdicts:** A CONCERNS · B CONCERNS · C CONCERNS. No P0. 6× P1 (firewall ×3, build/coverage ×3 incl. SDL2_ttf + depth), several P2/P3.
- **Cleanest target:** `mc2_asset_viewer` (standalone, no firewall debt). **Debt concentration:** `EditRel` (no firewall, wrong depth, no coverage).
- **Changed files (this audit):** `docs/editor-build-architecture-integrity-audit.md` + `docs/audit-lanes-editor/lane-{A,B,C}.md`. No production code/CMake/shaders.
- **Recommended next action:** `EDITOR-CI-COVERAGE-1` (§7) — make the editor build rot-resistant before further editor work.
