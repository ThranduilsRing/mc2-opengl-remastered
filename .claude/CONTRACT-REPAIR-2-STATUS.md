# CONTRACT-REPAIR-2 — contract suite back to green

Lane: CONTRACT-REPAIR-2 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Start HEAD `1fcd373b` → HEAD moved to `f51cd63d` mid-session (see "Commit scoop" below).

## Verdict

**74 passed, 0 failed** — measured by me, `bash scripts/check-contracts.sh`, run 9 times
(1 transient, 8 consecutive clean; transient explained below, not a flaky check).

Entry state was **71 passed / 3 failed**, reproduced independently before touching anything.

No tier1 smoke was run and **no tier1 PASS is claimed**. Nothing here changes engine
behavior: the changes are a constexpr registry row, a checker table, two GLSL *comments*,
and regenerated SPIR-V sidecar hashes. No `DEPLOY_FINGERPRINT OK` exists for this lane
because no deploy was performed.

## The three failures

### 1. `env_registry` — unregistered `MC2_CEMENT_ATLAS_DUMP`

Exactly one var, added by CEMENT-ATLAS-CELL-2 as a diagnostic.

**Fixed by registering, not deleting or allowlisting.** Added a `kAuxEnvVars` row in
`RenderCore/RendererFeatureRegistry.h` as `EnvVarKind::Trace`, `defaultOn=false`,
documenting that the value is a **path prefix, not a bool** (`=<prefix>` writes
`<prefix>.rgba` + `<prefix>.csv`, emits `[CEMENT_ATLAS_DUMP v1]`; empty/unset = no dump)
and that it is diagnostic-only. The script's own header says registration is the correct
route and ALLOWLIST is "for legacy/trace vars only", so registration was the right lever.

Now: `PASS — all 1008 MC2_* env vars accounted for (591 registered, 780 allowlisted)`.

### 2. `sampler_bindings` — `ssaoTex` hard anchor tripped

Not drift. SSAO-BLUR-1 added `shaders/ssao_blur.frag`, which declares `ssaoTex` a
**second** time and binds it through a **second** receiver (`ssaoBlurProg_`) to a
**different** unit (1 — the blur pass puts `sceneDepthTex` on 0). That is legitimate
multiplexing, so the name fails all three anchor criteria (one decl file / one receiver /
one unit) and cannot be re-anchored as-is.

The check documents itself as SELF-DEMOTING and explicitly requires the author to remove
such an entry "rather than silently passing a now-unsafe anchor". Removed `ssaoTex` from
`HARD_ANCHORS` and recorded it in a new `DEMOTED_ANCHORS` note so a future reader can
distinguish a deliberate demotion from an anchor lost to attrition. `uSrc` and `sceneTex`
anchors are untouched and still enforced.

**Latent bug found and fixed in passing:** `ssao_blur.frag`'s unit comments were exactly
**inverted** vs the C++ — it claimed `ssaoTex` unit 0 / `sceneDepthTex` unit 1, while
`gos_postprocess.cpp:2915-2916` binds `ssaoTex`=1 / `sceneDepthTex`=0. Comments only;
`setInt` is authoritative, so there was no runtime effect — but the comment is what the
checker and the next reader trust. Corrected.

### 3. `spirv_artifacts` — `mech.vert` bake drift (4 variants)

**Confirmed the prior lane's finding exactly: zero `.spv` binaries changed.** I md5'd all
16 `.spv` before and after `build_variants.py`; the sets are byte-identical. Only 4
`mech.vert.*.json` sidecars (`source_sha256`) plus `spirv_package.json`
(`sidecars_sha256` / `source_set_sha256`) moved — 6 changed lines total.

**Root cause, and why it is benign:** commit `c982817e` (TERRAIN-DEPTH-CONTRACT-1,
**Jun 26**) added `#include <include/terrain_depth_bias.hglsl>` to `shaders/mech.vert` for
`OBJECT_DEPTH_BIAS`. Only the *fragment* shader consumes that constant, so the flattened
GLSL source text grew while glslang emitted identical code. The bake had been stale for a
month — consistent with the "graded month-old binaries" class the ratchet below addresses.

## ★ Ratchet confirmations

**`build64-h-<name>` search — STILL IN PLACE.** The file is `tools/run_contract_tests.py`
(not `scripts/`). It searches `build64-h-<name>` **first**, then the other build dirs, and
among existing candidates **newest wins**, returning a `shadowed` flag. The comment at
line 66 records the original bug verbatim.

**Harness counts 16 vs 12 — left alone, as instructed.** Two different scopes (16 run
includes 4 PY_HARNESSES; 12 build is C++ only). Not "fixed".

## ★ ENV_DROP_RISK coverage — the finding

Measured by me (lexical, `getenv`/`envFlag*`/`envInt*`/`envFloat*` call sites vs the
`run_smoke.py` allowlist, registry `defaultOn` field as the default source):

| metric | measured |
|---|---|
| `MC2_*` read by engine code | 1036 |
| ENV_DROP_RISK (read, not allowlisted) | **571** |
| registry entries `defaultOn=true` | 40 |
| default-ON **and** ENV_DROP_RISK | **14** |

These differ from the cited audit (620 / 69) — same order of magnitude, same conclusion;
the methodology and the point in time differ, and my regex covers a fixed idiom set, so
treat mine as a lower bound. I did **not** attempt to register the backlog.

**Does the refusal cover default-ON gates? Yes, in the direction that matters — but for a
different reason than the code claims, and that is worth flagging.**

The refusal (`run_smoke.py:2829`) fires on `k in os.environ and k not in cfg.env_extra`,
i.e. only when a var is **explicitly set**. For a default-ON gate that is the correct
trigger: leaving it unset is not a drop (the engine's own default applies), while the
dangerous case — setting `MC2_TERRAIN_LOD_CHUNK=0` to run the OFF arm of an A/B — *is*
set, *is* absent from the allowlist, and *does* exit 2. Both named levers
(`MC2_GPU_MECHS`, `MC2_TERRAIN_LOD_CHUNK`) are confirmed **not** allowlisted, so both
would refuse rather than silently run the wrong arm. The class is closed for A/B use.

**However, the stated premise no longer holds.** The comment says the vars are "silently
dropped by the Popen env-replacement", but both launch paths —
`scripts/smoke_lib/runner.py:83` (the shared launcher behind every capture harness) and
`run_smoke.py:347` — build their env as `os.environ.copy()` and then `update()`. They
**inherit** the parent environment; nothing is stripped. So the refusal is now a
conservative guard against a drop that these paths no longer perform, and its practical
effect is to **block** legitimate A/B runs (exit 2 on a var that would in fact have
reached `mc2.exe`) unless `--allow-env-drop` is passed.

That is fail-closed and therefore safe — it will never grade a configuration that did not
execute, which is the point of the ratchet — but it is over-strict, and anyone running an
A/B on an unallowlisted gate will hit a hard stop that the message explains as a drop that
did not occur. **Recommend a follow-up lane** to either re-point the check at the actual
env construction or reword it; I did not touch `run_smoke.py` because it is claimed by
lane CEMENT-ATLAS-CELL-2 and the instruction was not to risk the green suite.

## Commit scoop (process finding — flagged, not actioned)

HEAD moved `1fcd373b` → `c7e0499c` → `f51cd63d` while this lane was working. My
uncommitted `MC2_TRACE_CEMENT_ATLAS_DUMP` registry edit was **swept into sibling commit
`c7e0499c`** (CEMENT-INTRACELL-1 ARM C) — `git log -S` confirms that commit introduced my
exact text. The fix is landed and the contract passes, but it is attributed to another
lane's commit. This is the commit-scoop class the lane registry exists to prevent; my
`register_lane` call warned only about the `scripts/` overlap, not `RenderCore/`.

**This also explains the one non-green run.** The single `73 passed, 1 failed` was the
first suite run after my edits, concurrent with the sibling lane's commit landing — the
working tree was mid-change. Eight consecutive `74 passed, 0 failed` runs followed. It was
a concurrent-lane artifact, not a flaky check; I re-ran specifically to avoid reporting a
lucky green.

## Files changed by this lane

- `scripts/check-sampler-bindings.py` — `ssaoTex` anchor demoted + `DEMOTED_ANCHORS` note
- `shaders/ssao_blur.frag` — inverted unit comments corrected
- `shaders/spv/mech.vert.{default,objectid,viewuniforms,objectid_viewuniforms}.json` — re-baked
- `shaders/spv/spirv_package.json` — rollup hashes
- `RenderCore/RendererFeatureRegistry.h` — registry row (landed via `c7e0499c`, see scoop)

Not touched: `docs/render-backend-seams/gated-layout-macros.md` (dirty before this lane
started), `scripts/run_smoke.py` (claimed by CEMENT-ATLAS-CELL-2).
