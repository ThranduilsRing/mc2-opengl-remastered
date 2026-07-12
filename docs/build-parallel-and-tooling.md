# Parallel build, smoke, and deploy-dir/worktree map

Ground-truthed 2026-06-30 via `ls -d A:/Games/mc2-opengl/mc2-win64-*` and
`git worktree list`. Do not edit sections marked "ground-truth" from memory —
re-run the commands and update the table.

---

## 1. Deploy directories

Six install roots exist under `A:/Games/mc2-opengl/`. All carry `mc2.exe` +
`Mission Editor.exe` except `v0.3` (no editor). Identical content except the
`mods/` folder.

| Directory | mc2.exe | Mission Editor.exe | mods/ contents | Safe as spare smoke target? |
|---|---|---|---|---|
| `mc2-win64-v0.4` | yes | yes | desertfoxformcosetup, magic-ballistic-weapons, magic-gamesys-tweaks, mcosetup628(1) | **yes** — primary canonical target |
| `mc2-win64-v0.4c` | yes | yes | (empty) | **yes** — spare game smoke |
| `mc2-win64-0.4c` | yes | yes | (empty) | **yes** — spare game smoke |
| `mc2-win64-v0.4d-rc1` | yes | yes | (empty) | **yes** — spare game smoke |
| `mc2-win64-abl-validate` | yes | yes | darkrain, modern-tree-pack-v1 | **conditionally** — mods load at startup; use only when testing ABL/mod paths, not for clean baseline smokes |
| `mc2-win64-v0.3` | yes | no | (empty) | **no** — version-pinned legacy baseline; do not overwrite |

`deploy_payload.py` enforces an allowlist (`DEPLOY_ALLOWLIST`) that covers
`v0.4`, `v0.4c`, `0.4c`, `abl-validate`, and `v0.3`. Deploying to a
non-listed path requires `--allow-new-target`. Never use that flag for a
per-lane throwaway — full installs are ~5 GB each.

**Interchangeable game smoke targets** (clean mods baseline): `v0.4`, `v0.4c`,
`0.4c`, `v0.4d-rc1`. Any of these can receive a deploy and run `run_smoke.py`
independently. `abl-validate` is safe but mod-loaded. `v0.3` must not be
overwritten.

---

## 2. Worktrees — isolation, classification, and parallel build

### How worktrees give isolated build64

Each git worktree is a distinct checkout directory with its own `build64/`
subdirectory. CMake writes object files, link outputs, and `mc2.exe` into that
`build64/` only. Two worktrees building concurrently never share build
artifacts.

Confirmed: `nifty-mendeleev/build64/`, `mc2-nifty-land/build64/`, and
`mc2-compute-lane/build64/` all exist independently.

### The cardinal rule

**NEVER build inside another active lane's worktree.** Each lane owns its
worktree. Checking out a different commit inside someone else's worktree
destroys their in-progress work and corrupts their build64 (stale objects from
the wrong tree).

### Worktree classification (ground-truth, 52 total)

| Class | Count | Examples | Rule |
|---|---|---|---|
| **Canonical nifty** | 1 | `.claude/worktrees/nifty-mendeleev` | Your primary worktree for nifty-mendeleev lane work |
| **Active named lanes** | ~45 | `mc2-abl-arg-guard-*`, `mc2-compute-lane`, `mc2-techscript-*`, `.claude/worktrees/editor-parity`, etc. | Do NOT build in these — they are concurrent work |
| **Agent/throwaway** | 2 | `.claude/worktrees/agent-a74909f76d924756c`, `.claude/worktrees/agent-a99771ed90491ccfb` | Short-lived; safe to reuse once the owning agent session ends |
| **Detached** | 1 | `mc2-nifty-land` (detached HEAD `94b9479d`) | Available as a spare build worktree for nifty's commit |

The two agent worktrees (`agent-a74909...`, `agent-a99771...`) are the primary
pool of spare build slots. `mc2-nifty-land` (detached HEAD) is also available.

### Parallel build recipe — two pipelines without collision

Each concurrent pipeline needs a **different worktree AND a different deploy
dir**. The pipeline:

```
1. Pick a SPARE worktree (agent-* or detached).
2. cd into it; git checkout --detach <nifty-commit-sha>  (detached — no branch conflict)
3. cmake configure if build64/ is empty or CMakeCache references the wrong commit.
4. cmake --build build64 --config RelWithDebInfo --target mc2
5. py -3 scripts/deploy_payload.py <SPARE-DEPLOY-DIR> --source-root . --build-dir build64 --exe-name mc2.exe
6. py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
   (run_smoke auto-leases the deploy dir; or pass --exe <SPARE-DEPLOY-DIR>/mc2.exe)
```

Example — two pipelines simultaneously:

| Pipeline | Worktree | Deploy dir |
|---|---|---|
| A | `.claude/worktrees/agent-a74909f76d924756c/` | `mc2-win64-v0.4c/` |
| B | `mc2-nifty-land/` | `mc2-win64-0.4c/` |

They share no build artifacts and target different `mc2.exe` files. Both can
run smoke concurrently without the crash_silent self-collision (two `run_smoke`
instances against the same exe = one kills the other's process).

---

## 3. The serialization constraint and when it applies

**Within a single worktree** all builds serialize: they share `build64/` and
CMake's lock file. Issuing two `cmake --build` commands against the same
`build64/` from different shells is undefined behavior (partial link, torn
output).

**Within a single deploy dir** all smokes must serialize: `run_smoke.py`
launches `mc2.exe` from the dir's path. Two smoke instances against the same
`mc2.exe` will kill each other's process and produce false `crash_silent`
verdicts. The deploy-lease system (`scripts/smoke_lib/deploy_lease.py`) manages
this automatically when `--exe` is omitted — it picks the least-recently-used
free folder from the preferred list. When running manually you must pick a free
dir yourself.

**Summary**: parallelism requires a distinct (worktree, deploy-dir) pair per
concurrent pipeline. Sharing either axis forces serialization.

---

## 4. New verification tooling

### `scripts/verify_executor_slice.py`

Canonical OFF/ON/dryrun gauntlet for frame-graph executor slices. Runs three
smoke passes in sequence: gate OFF (baseline parity), gate ON (executor active),
and dryrun mode. Also checks for stale-deploy (the exe's build timestamp must
match a recent `deploy_payload.py` run). Use after any executor or
frame-graph-adjacent commit before declaring it passing. Avoids the false-pass
pattern where a stale `.obj` or a silently-dropped env var made the gate-ON
pass look identical to gate-OFF.

### `run_smoke.py --require-gate` and the ENV-DROP warning

`--require-gate MC2_FOO=1` asserts that the named env var is actually present
in the child process environment (not dropped by `subprocess.Popen`'s explicit
env dict). If the var is not in the smoke allowlist, `run_smoke.py` emits an
`[ENV-DROP] WARNING` at startup and the gate will never activate inside the
game process — a default-OFF gate will silently pass as if it were ON. Check
the allowlist comment block in `run_smoke.py` (~line 1018) whenever adding a
new `MC2_*` gate, and add the var there. Use `--require-gate` to make the
smoke fail loudly rather than silently pass.

### MCP `verify_citations` (`mc2-repo-intel` server) — NOT LANDED

**Status: planned, never implemented.** No `verify_citations` exists in
`scripts/mcp/repo_intel_server.py` and there is no citation_verifier module
under `tools/repo_intel/`. The intent — check every `file:line` citation in
a recon or planning doc against the live codebase and flag any that have
drifted (line number off, symbol moved, function deleted) — is still valid;
stale citations led to deleting the wrong code range at least twice this arc
(MLR range; terrain draw-site). Until it lands, use `slice-preflight`
(`tools/repo_intel/repo_query.py`) for symbol-level drift and re-verify cited
line ranges by hand before any recon-derived deletion.

### `get_executor_health()` (render-state MCP)

Returns per-frame frame-graph executor metrics in one call:
`executor_owned_wrappers`, `executor_validated_top_level_passes`,
`executor_apply_state_passes`, `executor_scheduled_passes`,
`executor_validation_failures`, `executor_skipped_deferred_passes`. Use after
deploying an executor slice with the engine running (`MC2_DEBUG_STATE_DUMP=1`)
to confirm `validation_failures=0` and `owned_wrappers` matches the expected
island count. Faster than parsing raw diagnostic JSONL manually.

---

## 5. Standing rules learned from the frame-graph arc

**(a) Verify the actual dump from a rebuilt-and-deployed exe — never trust
agent claims alone.** Stale `.obj` files (incremental build missed a header
change) and silently-dropped env gates both produce exe output that looks
correct to an agent reviewing its own prior output. After any build+deploy,
check the dump or smoke artifact directly. `verify_executor_slice.py`
automates this for executor slices.

**(b) A default-OFF env gate is only verified if the smoke harness passes the
var to the child process.** `subprocess.Popen` with an explicit `env=` dict
drops all vars not in that dict. Every new `MC2_*` gate must be added to
`run_smoke.py`'s allowlist block (search `# allowlist` in the file) or it will
never activate inside the smoke process. Use `--require-gate` to turn the
silent drop into a hard failure.

**(c) Ground-truth recon file:line and symbol citations before deleting or
coding.** Three separate deletions this arc were avoided or corrected because
reading the actual code before acting caught a recon mischaracterization: the
MLR loop range (`:3062-3114` was the shared solid loop, not MLR-only), the
terrain draw site ("no caller" was wrong), and the indirect-patch callsite.
Run `verify_citations` on any recon doc before deriving a fix slice, then
cross-check with `slice-preflight`.

**(d) After relocating an observe-note, update the pass-order model
(`kFramePassOrder`) to match, or the dry-run reports false out-of-order
results.** The frame-graph dry-run compares observed pass order against
`kFramePassOrder`. Moving a `noteRenderPass()` call to a different call site
changes the runtime order; if `kFramePassOrder` is not updated in lockstep the
dry-run will flag a spurious OOO divergence. This was hit during
SHADOW-OBSERVE-2-REVISE-1 (static vs dynamic shadow mix-up). The fix model
and the observe point must be co-committed.
