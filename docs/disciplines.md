<!-- doc-status: current | reviewed:2026-10-11 -->
# Disciplines (load-bearing)

Extracted from CLAUDE.md 2026-05-24. These are the procedural disciplines that
govern HOW work is done — when to spawn subagents, when to ask for adversarial
review, when META-FIX is required, when to grep-verify.

## Operating mode: single lane

All work happens in one checkout, `A:/Games/mc2-main`, on branch
`claude/nifty-mendeleev`. There are no lanes, lane leases, or per-lane worktrees.
`docs/archive/dev-velocity.md` describes the retired multi-lane system; do not follow it.

## Subagent bookend-review discipline (STANDING USER RULE, 2026-07-25)

**Every dispatched subagent is bookended by two mandatory reviews.** No subagent may
report work as done / good / fixed without the closing one.

*Justification (one line):* the 2026-07-25 adversarial audit found ~35 of ~50
session items produced code a default launch never executes, docs still
recommended fixes the user had rejected, and several "verified via smoke" claims
were void.

### 1. META REVIEW — before starting

Verify the state you are inheriting. Do **not** trust the brief or prior status docs.

- [ ] Read gate defaults from **READER SOURCE** — docs and status docs have
      repeatedly disagreed with source.
- [ ] Check what a **DEFAULT LAUNCH** actually executes, vs what is merely committed.
- [ ] Check whether prior "verified" evidence came from `-dirty` / unattributable builds.
- [ ] Check the working tree for orphaned/uncommitted work.
- [ ] Check whether a later commit already **superseded or regressed** the task.
- [ ] **Spot-check the brief's own premises.** Four consecutive subagent runs inherited a
      false premise ("vehicles are the odd one out on the legacy path") because
      nobody re-verified it. **Report corrections rather than silently building on
      a wrong foundation.**

### 2. ADVERSARIAL SELF-REVIEW — before reporting done

Try to **refute your own claim**. Answer explicitly:

- [ ] **If the user launched the game right now with default settings, would they
      still see this?** A fix behind a default-OFF gate the user never approved is
      **NOT delivered** — say so in those words.
- [ ] Gate is **registered** AND **smoke-allowlisted**. An unallowlisted gate makes
      both A/B arms run the same path; this has silently voided real verification
      more than once.
- [ ] Evidence came from a **CLEAN fingerprint build** (`MC2_BUILD_GIT_DIRTY 0`),
      md5-verified.
- [ ] The gate string is **actually present in the deployed binary**. One "fix"
      existed in no binary at all because its rebuild failed silently.
- [ ] Verified at **poses/conditions where the defect can actually manifest**
      (see the Standing Capture Rule in `docs/visual-verify.md`).
- [ ] **Nothing regressed a user approval.**
- [ ] State **residual risk** and **what you did NOT do**, plainly.

See also: `docs/visual-verify.md` — the Standing Capture Rule (full-frame wide
verdict shot, ≥3 locations incl. a control area, day AND night, across
mc2_01/mc2_03/mc2_10/mc2_24).

## Review discipline

When user asks for "review" / "second opinion": **adversarial, code-grounded by
default.** Read `.claude/skills/adversarial-plan-review.md`. High-stakes plans
(architectural endpoints, legacy retirement, SSBO schemas, perf gates >=30%)
get the full skill (grep every cited symbol; findings as CRITICAL/MAJOR/MINOR).
Dispatch prompt MUST include "use the adversarial-plan-review skill" verbatim.
Always dispatch without asking: `memory/feedback_always_dispatch_adversarial_review.md`.

## Meta-fix discipline

Before proposing/writing any fix: run the `greybeard` skill
(`.claude/skills/greybeard.md`). It forces an explicit `META-FIX` vs `PATCH
(justified)` ruling — the upstream change that retires the bug *class*, not
the local symptom patch. A patch with no named meta-fix and no debt
justification is not allowed. Documented history of additive slices netting
~0ms (`memory/feedback_offload_must_be_substitutive_not_additive.md`).
Dispatch prompts MUST include "run the greybeard skill" verbatim.

## MEASURED-BY discipline (numbers must be re-derivable)

When a commit message or status doc states a **number** for the first time, put
a trailer in that commit naming the tracked command that produces it:

```
MEASURED-BY: py -3 scripts/check-doc-links.py
```

Why: 24 of the 160 commits since 2026-07-29 are corrections or retractions of
numbers claimed in the same session. The one measurement that was never
disputed is `6d9360046` — it committed the *script*, a `--selftest` with
negative controls, and the exact CLI, and stated the number as a consequence of
running it. Commit the script in the **same** commit as the claim.

Checked by `check-contract-coverage.py` arm (f): if HEAD carries the trailer,
the script it names must exist in HEAD. The check is deliberately opt-in — it
cannot tell that a message states a number (SHAs and line numbers look like
numbers too), so it does not guess. It guarantees that a trailer, once
written, is true. A reproduction command nobody else can run is worse than no
trailer.

Not a replacement for `.claude/MEASUREMENT-LEDGER.md`, which records six
already-settled disputes. Keep that as a record; do not grow it — new numbers
go in the trailer, at the moment of the claim.

## Slice-preflight discipline (anti-rediscovery / stale-base gate)

Before any **recon-derived fix slice** (a fix whose justification came from a
recon doc / earlier session / shared queue), run:

```powershell
py -3 tools\repo_intel\repo_query.py slice-preflight ^
  --base <branch-base-ref> --slice <SLICE_NAME> ^
  --paths <target files> --symbols <key symbols>
```

- `verdict=STOP` (a target symbol changed on `base..nifty`) → **do not write code**;
  re-recon against current HEAD first. The bug may already be fixed (this gate
  encodes the lesson from the duplicated watchID / fit-parse / icon-divisor
  re-recons — another session had already landed the fix).
- `verdict=WARN` (slice name already in log / stale base / dirty overlap) → review
  before proceeding.
- **Mandatory** for shared-recon-queue work; advisory (still useful) for ordinary
  implementation already based on fresh same-session recon.
- MCP form `repo.slice_preflight(...)` once the `mc2-repo-intel` server is reloaded;
  until then use the CLI. Do not block adoption on MCP availability.

## repo-intel MCP commands (token-cut / faster-findings)

Added 2026-06-27 (`7b0448fa`). All additive; MCP tools go live after a server
restart, CLI works immediately. Aimed at faster, cheaper repo queries.

- **`preflight` / `dirty` now digest by default** — preflight no longer dumps the
  full ~239-file dirty list (mostly `tests/visual/baselines/*.png`); it returns
  `class_counts` + only the elevated (non-safe-class) files. MCP `dirty(full=True)`
  or CLI `repo_query.py dirty` (no flag = full) restores the complete list.
- **`repo.diag_tag(tag, log=, last_n=, log_path=)`** (`mc2-render-state`) — greps the
  latest editor/game stderr for a printf diag tag prefix (e.g. `EDITOR_STATIC_PRIME`,
  `BLDG_CMD_DIAG`) and returns only matching lines + any `SUMMARY` line. The
  run→verify loop without tailing the whole log. `log="editor"` reads
  `editor-stderr.log` (override dir via `MC2_EDITOR_DEPLOY_DIR`).
- **`repo.symbol(..., in_ref="HEAD", def_context=N)`** — `in_ref` reports whether the
  symbol exists in a ref (new-vs-already-landed, same question slice-preflight asks);
  `def_context` attaches body lines to the first definition; `caller_files` always
  lists unique calling files. One call instead of grep + git-grep + Read.
- **`repo.grep(..., mode="files"|"count")`** — snippet-free reduced output for cheap
  exploratory sweeps (`content` is the default, unchanged).

## Documentation discipline

Every cited symbol grep-verified AT WRITE-TIME. Applies at every stage.
Carve-out: intentions ("we will create X") need no grep.
Full: `memory/brainstorm_code_grounding_lesson.md`.

## Advisor invocation discipline

For any substantive question whose domain has a dedicated advisor per
`.claude/agents/DOMAINS.md`, **spawn the advisor first.** The advisor reads
MEMORY.md + topic files + current code with fresh context; main-agent compiled
knowledge is stale by definition.

**Applies to:** pipeline questions (rendering, shaders, mech, terrain-indirect,
GameOS), methodology (CPU→GPU offload, render-contract refresh, perf-slice
sizing, cull-cascade safety), asset-format (mission data, FST/.fit/.tga/.wav,
file IO, init order), build-system (CMake, vcpkg, FFmpeg delay-load,
full-relink).

**Does NOT apply to:** trivial lookups, follow-up clarification on something
this-session, or when user says "answer directly".

Why: advisors carry tacit knowledge in `<known_pitfalls>` that doesn't live in
MEMORY.md, and grep-verify file:line per their Rule 0. Answering from
main-agent context bypasses both protections.

## Debug instrumentation rule (for reworks)

Any rework touching object lifecycle, cull/visibility gates, render path,
resource lifetime, or cross-system control flow lands env-gated `[SUBSYSTEM]`
lifecycle prints in the same commit. Stays gated off; demote-don't-delete
after fix. Full rule: `memory/debug_instrumentation_rule.md`.

## Render-slice observability rule (state-dump block)

Any **gated render slice** ships with a state-dump observability block in the
same commit (`debug_state_dump.cpp` → `"shadow"`-style object, queryable via
the `mc2-render-state` MCP). It exposes the feature's gate + key resolved
runtime values.

Criteria (gap-driven, NOT blanket): a feature earns a block when it has a
default-on/off gate **AND** a runtime effect you iterate on visually
(shadows, water, vegetation, terrain LOD, PBR). Skip always-on, cosmetic-only,
or trivially-confirmed features — a dead field is engine maintenance cost for
zero query value.

Why: cheap at authoring time (the values are already in hand), expensive to
bolt on later. The 2026-06-18 CSM work proved the cost — "is CSM actually on?"
required md5-ing exes and reading `launcher_env.json` across deploy folders
because no live state exposed it. Pattern reference: `9d0556c0`
(`"shadow"` block). Aligns with the *measure-before-infer* /
*forced-constant-verify-over-code-theory* discipline.

## Smoke sessions are USER-DRIVEN

**The user can see and control every smoke session.** `run_smoke.py` launches
mc2.exe in a real game window the user is watching live. Smoke feedback like
"I saw the triangle" is **first-hand visual observation.**

Anti-patterns: DO NOT ask user to "re-run manually to reproduce" or "confirm
with X env var"; they're already the visual observer. When user says "still
doing it" they mean the most recent smoke — find the latest artifact dir and
analyze its `ring_trace.log`.

After every smoke the user reports a bug in: read
`tests/smoke/artifacts/<latest>/{mission}.ring_trace.log` for that run's probe
events.

## Subagent dispatch discipline (lean intake)

Plan-writers and implementer subagents must have **lean intake** prompts —
one authoritative source file, inline everything else, no "read this as a
reference" lists. See `memory/dispatch_prompt_intake_budget.md` for the
pattern + the gsd-executor analysis-paralysis guard that should be encoded
in every long-running dispatch.

Key prompt-construction rules:
- ONE authoritative file per dispatch (typically the spec or plan)
- Inline all decisions / format / context from sidecars
- Eliminate background docs (recon, prior reviews) — their findings are
  already baked into the authoritative source
- Encode `analysis-paralysis-guard` verbatim in long-running dispatches:
  "5+ consecutive Read/Grep/Glob without Edit/Write/Bash = STOP"

## Agent-efficiency tools (deterministic, replace in-context re-derivation)

Four tools subagents should NAME instead of rebuilding (AGENT-EFFICIENCY-TOOLS-1):

- **`tools/menu_oracle.py`** — visual menu-visibility oracle:
  `py -3 tools/menu_oracle.py --exe <mc2.exe> [--env MC2_NO_LAUNCHER=1] [--wait 18] [--threshold 10] [--shot out.png]`
  → JSON `{mean, nonblack_frac, verdict}`; exit 0 VISIBLE / 1 BLACK / 2 INCONCLUSIVE.
  Caveat: sparse text-on-black menus can read mean <1 while visible — threshold is the caller's judgment.
  **USER-PRESENT ONLY (NOFOCUS-FINISH-1).** It reads the DESKTOP COMPOSITE (`PIL.ImageGrab` of the window's client rect), so it cannot see an engine window parked offscreen at (-32000,-32000), which is now the default for every agent launch. `nofocus.apply_env` therefore raises `DesktopOccupancyRefused` unless `MC2_HARNESS_ALLOW_FOREGROUND=1`. That abort is deliberate: the alternative is a confident verdict on the user's wallpaper. The registry of composite-capture harnesses is `check-background-launch.COMPOSITE_CAPTURE_FILES`; the agent-runnable equivalents read the engine's own framebuffer (`scripts/run_visual_capture.py`, `scripts/mech_shot.py`, `scripts/mechbay_preview_harness.py`).
- **`tools/witness_smoke.py`** — deploy+smoke+log-grep verdict in one call:
  `py -3 tools/witness_smoke.py --install <dir> [--deploy] --mission mc2_01 --expect "<regex>" [--forbid "<regex>"]`
  → smoke table + per-expect FOUND/MISSING; exit 0 only if smoke passed + all expects + no forbids.
- **`tools/build_quiet.py`** — VS CMake build with spew swallowed:
  `py -3 tools/build_quiet.py [--target mc2] [--config RelWithDebInfo]`
  → errors verbatim, warning COUNT, final `vcxproj ->` line, elapsed; exit = cmake exit.
- **`tools/list_free_ordinals.py`** — free (undefined) compbas ordinals for modding:
  `py -3 tools/list_free_ordinals.py --compbas <compbas.csv | install-dir>`
  (install dir → auto-extracts from misc.fst via `tools/mc2x_import/fst.py`, whose
  LZW decoder is now asm-faithful to `mclib/lzdecomp.cpp` and sniffs zlib-per-entry).
