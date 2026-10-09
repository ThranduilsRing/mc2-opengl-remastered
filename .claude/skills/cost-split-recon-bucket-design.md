---
name: cost-split-recon-bucket-design
description: Methodology for designing the bucket set for a "Slice 0 recon" inside a hot Tracy zone — picking which sub-task boundaries to instrument with CostSplit-style per-frame accumulators so subsequent slices can target the dominant residual. Use BEFORE writing recon instrumentation code; produces a list of buckets + counter + timer placement plan. Distinguishes per-call vs per-frame measurement, respects the 100ns Tracy-floor rule, and reuses the shipped Phase 1 Slice 0 infrastructure (gos_terrain_indirect cost-split + 600-frame summary line in smoke logs).
---

# Cost-Split Recon Bucket Design

Methodology for decomposing a hot Tracy zone into instrumentable sub-buckets BEFORE adding the instrumentation code. Use when you're starting a Slice 0 recon and need to decide what buckets to add.

## Important — two DIFFERENT instrumentation mechanisms

This skill is about **CostSplit buckets**, NOT Tracy zones. They look similar (both are RAII guards in C++) but have different mechanics, different audiences, and different output channels:

| | Tracy zones | CostSplit buckets |
|---|---|---|
| Macro | `ZoneScopedN("name")` (or `ZoneScopedNC`) | `CostSplitFooScope _csFoo;` |
| Per-call overhead | ~20-50 ns active | ~5 ns disabled (cached bool); ~50-100 ns active (chrono::steady_clock::now() × 2) |
| Aggregation | Per-call (each scope is one zone instance) | Per-frame (accumulated nanoseconds; rolled at frame end) |
| Output channel | Tracy profiler GUI (real-time TCP stream) | Smoke runner log; printed in `[TERRAIN_INDIRECT_PARITY v1] event=summary` line every 600 frames |
| Min observable cost | ~100 ns (the "100ns Tracy floor" rule) | Any (per-frame total can be µs even if per-call is ns) |
| Operator visibility | Tracy GUI shows it instantly while running | Operator must read smoke log after run; or session must report summary line back |
| Env gate | Compile-time `TRACY_ENABLE` macro (always on in this project) | Runtime `MC2_TERRAIN_COST_SPLIT=1` env var |
| Use case | Per-frame zones; "is this function getting hotter?" | Per-frame sub-task decomposition inside a hot loop; "where is the 11ms going?" |

**The skill below produces a CostSplit bucket plan.** If you want Tracy-zone-only instrumentation (no log-line summary, just visual profiler), this skill's methodology still helps you choose what to instrument, but use `ZoneScopedN` macros instead of CostSplit infrastructure and skip the summary-line extension.

Both mechanisms can coexist on the same code: a Tracy zone for visual profiling + a CostSplit bucket for log-line reporting. Most renderer-side hot zones in this project use BOTH (Tracy for the frame timeline; CostSplit for the operator-relayable summary numbers).

## Operator-visibility note

**CostSplit buckets are invisible to a Tracy GUI session.** They write to stderr as a summary line every 600 frames. When dispatching a session to do a recon, the dispatch prompt MUST require the session to report the summary line back after each smoke run, OR the operator runs the smoke themselves and `grep`s for the summary line. Without this, the operator (you) can't see the recon results in real-time — they only show up in the smoke log.

If you want simultaneous Tracy GUI visibility for the same sub-tasks, ALSO add `ZoneScopedN("subtask name")` macros at the same call sites. Tracy will then show the sub-task breakdown in its frame timeline AND the CostSplit summary line will appear in the smoke log.

The methodology was derived from the Phase 1 Slice 0 recon (commit `4fa7a9a`, 8 buckets inside `Terrain::geometry quadSetupTextures`) and the post-Phase-1 followups that closed the "missing 8ms" gap with VisibilityCheck + SetupTotal + CacheResident additions. See `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md (removed)` for the full applied example.

## When to use

**Mandatory** before any Slice 0 recon that:
- Adds new buckets inside an existing Tracy zone wider than ~1 ms mean
- Will inform subsequent slice prioritization (which sub-task to attack first)
- Lives inside the per-frame hot path

**Recommended** when:
- You have a hot zone but don't know where the cost concentration lives
- You want to bisect a complex function into work-phase categories
- Pre-Phase-D-style "what's actually left in this zone after the obvious optimization shipped"

**Skip** when:
- The zone is already well-instrumented (just read the existing buckets)
- The hot path runs <100ns per call AND has no clear iteration structure (per the 100ns Tracy floor rule, instrumenting will be measurement-of-instrumentation)

## What this skill produces

A document listing:
1. The parent Tracy zone being decomposed
2. Sub-buckets to add, each named + classified by work-phase category
3. Per-bucket choice of "RAII timer scope" vs "per-call counter" (vs both)
4. Caller-side or callee-side instrumentation decision
5. Sanity check: estimated overhead of the instrumentation itself (must be <10% of zone cost to be useful)

Hand this output to the implementation step (or a sonnet executor) to actually write the instrumentation code mirroring the Phase 1 Slice 0 commit pattern.

## Process (five steps)

### Step 1 — Identify the parent Tracy zone

The hot zone from the flame graph. Grep `ZoneScopedN("<zone name>")` to find the source site.

Output: file:line of the zone open + close braces. Note the function call inside (it's almost always a single function call wrapped by the zone).

### Step 2 — Read the function body cold

Walk the body open-brace to close-brace once. Identify each distinct work block as a candidate sub-task.

Look for:
- Loops (each loop is at minimum 2 buckets: iteration overhead + per-element work)
- `if/else` branches (each branch is a potential bucket if cost is materially different)
- Function call chains (each downstream function is a potential bucket boundary)
- Cache lookups vs cache hits (the lookup overhead is a distinct work shape)
- GL submission calls (always a separate output bucket from prep work)

### Step 3 — Classify each block by work-phase category

Every hot zone decomposes into a subset of these five categories:

| Category | Shape | Examples | Bucket type |
|---|---|---|---|
| **Setup/reset** | Once per zone entry; small | `clearArrays`, frame-counter reset, per-frame counter init | Single RAII timer scope wrapping the setup |
| **Iteration overhead** | The for-loop control + cull/skip checks | walking quadList just to check visibility | Single RAII timer wrapping the whole outer loop |
| **Per-element work** | Inside the loop body, scales with N | per-quad classification, per-shape light dedup | Two patterns: (a) RAII timer wrapping the whole loop body + counter for N; OR (b) RAII timer inside the loop body if cost-per-iteration is >>100ns |
| **Lookups/caches** | Per-element but separable | texture handle resolve, recipe cache lookup | RAII timer wrapping the lookup + counter for call volume |
| **Output/flush** | End-of-zone GL submit | `glMultiDrawArraysIndirect`, batch list flush | Single RAII timer wrapping the GL call(s) |

The classification gates the next step (timer vs counter choice).

### Step 4 — Choose timer vs counter

**Tracy 100ns floor rule** (repo CLAUDE.md): never time work that's <100ns per call inside a hot loop — the instrumentation IS the cost.

| Per-call cost | Tool |
|---|---|
| ≥1 µs per call | RAII timer per call (Tracy zone, but more typically chrono via CostSplitScope) |
| 100ns – 1 µs per call | RAII timer wrapping the whole loop; counter for N |
| <100ns per call (e.g., singleton hashmap dedup) | Counter ONLY; the loop's wrapping timer already captures cumulative cost |

For setup/output buckets (category 1, 5): always single RAII timer scope per frame — no per-call concern.

For iteration buckets (category 2): always single RAII timer wrapping the for-loop start to end.

### Step 5 — Caller-side or callee-side instrumentation

**Caller-side** (add `CostSplitFooScope _csFoo;` around the call to `bar()`):
- When `bar()` is called from one site
- When you want to measure "time this caller spent in this call"
- Phase 1 Slice 0's WaterVertProj + Lighting + RecipeCache buckets are all caller-side

**Callee-side** (add timer/counter inside `bar()` itself):
- When `bar()` is called from many sites and you want aggregate cost
- When you can't safely add an RAII scope at the caller (template metaprogramming, virtual dispatch chain, etc.)
- The Phase 1 SetupTotal bucket is callee-side (wraps the entire `setupTextures` body)
- The `addLightDataStructure` counter is callee-side (one increment per call, regardless of caller)

## Sanity check — bucket overhead

Before shipping the recon, estimate total instrumentation overhead:

```
total_overhead_per_frame =
    sum over buckets of:
        (overhead_per_RAII_entry_exit + overhead_per_counter_bump) × calls_per_frame
```

Typical numbers on x64:
- `std::chrono::steady_clock::now()` × 2 (enter + exit) = ~50–100 ns per scope
- atomic counter increment: ~5 ns
- branch on `IsCostSplitEnabled()` cached bool: ~2 ns

If a bucket has 14K calls/frame × 100ns = 1.4 ms of pure instrumentation. With 8 buckets all per-call inside the same loop = 11 ms of instrumentation overhead. This is what bit Phase 1 Slice 0 — production `quadSetupTextures` was 1.87 ms, but with COST_SPLIT=1 active it ballooned to 6.7 ms because of accumulated chrono overhead.

**Rule of thumb**: total instrumentation overhead should be <10% of the zone's mean cost. If it exceeds, use lower-overhead measurement:
- RDTSC (`__rdtsc()` intrinsic, ~5–10 ns vs 50–100 ns)
- Sampling (instrument every Nth call, extrapolate)
- Per-frame totals only (one outer timer + per-bucket counters; multiply counter × known-per-call-cost from a microbench)

The Phase D dispatch prompt at `docs/superpowers/plans/progress/2026-05-11-phase-d-quadsetuptextures-residual-prompt.md (removed)` documents this trade-off explicitly.

## Existing infrastructure to reuse

Don't re-invent. Phase 1 Slice 0 shipped (commit `4fa7a9a`) all of these:

| Component | File | What to reuse |
|---|---|---|
| Env-gate reader | `GameOS/gameos/gos_terrain_indirect.cpp` `IsCostSplitEnabled()` | Boot-cached bool; gates all timers |
| Per-frame accumulator + total + RollFrame | `gos_terrain_indirect.cpp:180-214` | Pattern for adding new buckets |
| CostSplit*Scope RAII struct | `mclib/quad.cpp:68-209` | Pattern for caller-side timers |
| 600-frame summary line | `gos_terrain_indirect.cpp:270-339` (`ParityFrameTick`) | Print format: `event=summary frames=N ... <bucket>_ns_per_frame=X ...` |
| Smoke runner env allowlist | `scripts/run_smoke.py:256` (`MC2_TERRAIN_COST_SPLIT`) | Already propagates; new buckets get the env-on path for free |

Adding a new bucket pair (timer + total + getter + summary column) is ~15-20 lines of code per bucket, all mirroring the existing pattern.

## Worked example — `mcTextureManager->update()` + `Terrain::render` recon

A user wanted to find the cost concentration inside two hot zones:
- `GameCamera::render textureManager` (1.35 ms)
- `GameCamera::render terrain` (2.63 ms)

### Applying the methodology

**Step 1 — Parent Tracy zones:**
- `gamecam.cpp:244` `ZoneScopedN("GameCamera::render textureManager")` wrapping `mcTextureManager->renderLists()` call
- `gamecam.cpp:200` `ZoneScopedN("GameCamera::render terrain")` wrapping `land->render()` call

**Step 2 — Reading the bodies:**

For `mcTextureManager->update()` (NOTE: called from `mission.cpp:526`, NOT from `gamecam.cpp` directly — the textureManager Tracy zone wraps `renderLists()` flush but the per-frame state-building work happens in `update()` earlier). Body has:
- `clearArrays()` — per-frame reset
- For-loop iterating registered shapes/lights → calls `addLightDataStructure`
- Texture handle resolution cache work
- (Other registry maintenance)

For `Terrain::render` (terrain.cpp): walks `quadList`, calls `TerrainQuad::draw` per quad, eventually hands off triangle lists to mcTextureManager for batched submission.

**Step 3 — Classify each block:**

| Block | Category | Notes |
|---|---|---|
| `clearArrays` | 1 (setup/reset) | Per-frame, small |
| `addLightDataStructure` loop | 3 (per-element work) | ~25K calls/frame at hashmap dedup 200-300ns each |
| Handle resolve cache | 4 (lookup/cache) | Per-shape; varies |
| `renderLists` flush | 5 (output/flush) | The actual GL submit |
| `Terrain::render` quadList walk | 2 (iteration overhead) | ~14K iterations |
| `Terrain::render` indirect cmd build | 3 (per-element work) | SSBO writes |
| `Terrain::render` MDI dispatch | 5 (output/flush) | `glMultiDrawArraysIndirect` |

**Step 4 — Timer vs counter:**

- `mctex_clearArrays`: single RAII timer (~1 call/frame, setup phase) ✓
- `mctex_add_light_data`: timer wrapping the for-loop + counter for call volume (per-call work is 200-300ns, AT the 100ns floor → don't time each call, time the loop)
- `mctex_handle_resolve`: timer wrapping the lookup loop + counter
- `mctex_renderLists_flush`: single RAII timer wrapping the GL submit
- `terrain_per_quad_walk`: single RAII timer wrapping the outer for-loop
- `terrain_indirect_cmd_build`: timer wrapping the cmd-build pass (one call per active block, not per quad)
- `terrain_mdi_dispatch`: single RAII timer wrapping the dispatch call

**Step 5 — Caller-side or callee-side:**

For `mctex_*`: callee-side (instrument inside `mcTextureManager->update()` and `->renderLists()`).
For `terrain_*`: callee-side (instrument inside `Terrain::render`).
For the addLightDataStructure call volume counter: callee-side inside the function body (one counter, many call sites).

**Step 5b — Sanity check:**

- mctex_clearArrays: 1 RAII scope/frame × 100ns = 100 ns overhead → fine
- mctex_add_light_data: 1 RAII scope/frame + 25K counter bumps × 5ns = ~125 µs overhead → ~9% of 1.35 ms zone → OK
- mctex_handle_resolve: similar
- mctex_renderLists_flush: 1 RAII/frame × 100ns → fine
- terrain_per_quad_walk: 1 RAII/frame × 100ns → fine
- terrain_indirect_cmd_build: 1 RAII/frame × 100ns → fine
- terrain_mdi_dispatch: 1 RAII/frame × 100ns → fine

Total overhead ≈ 200 µs/frame ≈ 5% of combined zone cost (1.35 + 2.63 = 4 ms). Acceptable.

### Output handed to executor

```
Buckets to add inside mcTextureManager (callee-side):
- mctex_clearArrays            (RAII timer, no counter)
- mctex_add_light_data         (RAII timer wrapping loop + counter for N calls)
- mctex_handle_resolve         (RAII timer wrapping loop + counter)
- mctex_renderLists_flush      (RAII timer)

Buckets to add inside Terrain::render (callee-side):
- terrain_per_quad_walk        (RAII timer wrapping outer for-loop)
- terrain_indirect_cmd_build   (RAII timer wrapping cmd-build pass)
- terrain_mdi_dispatch         (RAII timer)

Existing infrastructure to reuse: gos_terrain_indirect cost-split pattern at commit 4fa7a9a.
Env gate: MC2_TERRAIN_COST_SPLIT=1 (already in smoke runner allowlist).
Summary line: extend [TERRAIN_INDIRECT_PARITY v1] event=summary at gos_terrain_indirect.cpp:270-339 with 7 new columns.
```

Hand this to a sonnet executor; they wire the buckets and run a 90s mc2_10 smoke.

## Common pitfalls

1. **Instrumenting per-element work inside a 14K-iteration loop.** Bites every newcomer. Use category 3 patterns (timer wraps loop, counter tracks N).
2. **Marking sub-buckets as M (matches) without grep-verifying location.** The line numbers shift between commits. Always re-grep at write-time.
3. **Forgetting the env-allowlist propagation.** New env vars MUST be added to `scripts/run_smoke.py` (around the existing `MC2_TERRAIN_COST_SPLIT` block) or the smoke runner won't propagate them through `subprocess.Popen`'s env arg.
4. **Skipping the sanity check.** Phase 1 Slice 0 ran fine but the instrumented-state numbers were ~3.5× inflated vs production due to per-call chrono overhead. The Phase D prompt explicitly documents this and mandates lower-overhead re-measurement.
5. **Caller-side timer where callee-side counter would suffice.** If you only need "how many calls" not "how long each call took," a counter is 10x cheaper and gives the same info.
6. **Per-bucket Tracy `ZoneScopedN` inside hot loops.** Tracy zones have ~20-50ns overhead each; 14K × that × N buckets quickly busts the 100ns floor. Per-frame `CostSplitScope` is the right tool for sub-second-zone decomposition; per-quad Tracy zones are forbidden.

## Output format

Produce a markdown table or list:

```markdown
# <ZoneName> Slice 0 Recon — Bucket Plan

Parent zone: `Tracy zone name` at `file:line`

| Bucket name | Category | Timer / counter | Caller/callee | Tracy zone too? | Notes |
|---|---|---|---|---|---|
| <name> | 1-5 | RAII / counter / both | C/E | Y/N | <implementation hint> |

Existing infrastructure: gos_terrain_indirect cost-split (commit `4fa7a9a`).
New env vars: <list any>. Add to scripts/run_smoke.py allowlist.
Sanity check: estimated overhead ~<X> µs/frame = <Y>% of zone mean.
Summary line columns: <list new column names>.
Operator visibility: <reporting plan for relaying summary-line results back>.
```

The "Tracy zone too?" column captures the decision per-bucket on whether to add a `ZoneScopedN` alongside the CostSplit guard. Default: **Y** for per-frame buckets (cheap, operator sees instantly in Tracy GUI); **N** for per-element counters (Tracy floor violation) or when the sub-task is tiny.

Hand this output to the next session (often a sonnet executor) to implement the instrumentation. The implementation step is mechanical; the design step is where this skill saves time.

## Keeping the operator in the loop

When a session does recon instrumentation, the operator (orchestrator session OR human) typically can't see the results unless they're explicitly relayed. Three patterns to keep visibility:

1. **Session reports summary line after each smoke.** Dispatch prompt requires: "After running each smoke, `grep '[TERRAIN_INDIRECT_PARITY v1] event=summary' <log>` and report the LATEST line back to the operator before deciding next steps."
2. **Operator runs the smokes themselves.** Session implements instrumentation; operator deploys + smokes + reads log. Slower iteration but operator stays in control of the data.
3. **Dual Tracy + CostSplit instrumentation.** Add Tracy zones alongside CostSplit buckets so the operator sees real-time data in Tracy GUI even while the session iterates. Most expensive (double-instrument) but maximum visibility.

For high-stakes recons (where the bucket breakdown determines slice prioritization), pattern 1 or 3 is mandatory. For mechanical follow-up recons (where the breakdown is just a sanity check), pattern 2 works.

## Integration with other skills

**Called by**: a session at the start of any Slice 0 recon. Usually the session has already identified the hot zone from Tracy data and just needs to plan the decomposition.

**Pairs with**: `superpowers:writing-plans` (the bucket plan goes into the recon's design doc Stage 0 section).

**Followed by**: implementation step (often a sonnet executor) that wires the buckets + runs the smoke + reports the breakdown.

**Reviewed by**: `adversarial-plan-review` ensures the bucket plan doesn't have fictional file:line citations or missed sub-tasks. The Phase 1 Slice 0 recon went through this — the SetupTotal + CacheResident + VisibilityCheck buckets were added in follow-up commits after the initial 3 buckets were found insufficient to close the Tracy-zone-vs-bucket-sum gap.

## Origin

Derived from Phase 1 Slice 0 recon work (2026-05-10) targeting `quadSetupTextures`. Eight buckets shipped at commit `4fa7a9a` decomposed the 11.87 ms zone into lighting (5,177 µs), water_vert_proj (911 µs), visibility_check (890 µs), recipe_cache (656 µs), cache_resident (269 µs), detail_overlay (201 µs), setup_total (wrapping), and residual (3,767 µs distributed dispatch overhead). The breakdown drove Phase 1's architectural reframing from threading to GPU compute and identified lighting as the dominant target. Post-shipping, the Phase 1 plan v2 + Phase D prompt both reference this slice's residual decomposition for downstream slice prioritization.
