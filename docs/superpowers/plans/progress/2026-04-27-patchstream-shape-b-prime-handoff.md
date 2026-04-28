# Shape B' — PatchStream Canonical Bucket Key — Handoff Prompt

> **Hand this to a fresh session.** It is self-contained; the agent does not need to read the full M0b implementation history. Read the four reference docs cited in §1 to ground yourself, then answer the canonicalization-key questions in §3 before touching code.

---

## 1. Context — what M0b shipped and why this slice exists

PatchStream M0b landed on branch `claude/nifty-mendeleev` (worktree `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`). 11 commits total, tip `3a85c04`. Default-off behind `MC2_MODERN_TERRAIN_SURFACE`. Killswitch=0 has bit-identical legacy parity. Killswitch=1 is functionally correct but **fails spec §11 perf thresholds with -26% avg / -52% worst (mc2_10) FPS regression**.

Reference docs to read first:

- **Spec:** `docs/superpowers/specs/2026-04-27-patchstream-shape-b-design.md` (rev 5) — the architecture
- **Plan that landed M0b:** `docs/superpowers/plans/2026-04-27-patchstream-m0b-plan.md` (rev 3)
- **Perf-analysis closeout:** `docs/superpowers/explorations/2026-04-27-patchstream-m0b-perf-analysis.md` — read this carefully, it has the root cause analysis and the two fix options
- **Memory entry:** `~/.claude/projects/A--Games-mc2-opengl-src/memory/patchstream_m0b.md` — gotchas + invariants

Existing code:
- `GameOS/gameos/gos_terrain_patch_stream.{h,cpp}` — the patch stream class
- `GameOS/gameos/gos_terrain_bridge.{h}` + bridge defs in `gameos_graphics.cpp` — accessor layer
- `mclib/quad.cpp` — append callsites (4 mirror-write sites in SOLID-branch pz gates around lines 1636/1788/1955/2105)
- `mclib/txmmgr.cpp` — `Render.TerrainSolid` arm dispatches `flush()` with whole-frame legacy fallback

## 2. Root cause (one paragraph)

`appendTriangle(textureIndex, ...)` keys staging buckets by **raw `terrainHandle`** as it arrives at the quad.cpp callsite. The legacy path's `mcTextureManager->addVertices(handle, ..., flags)` aggregates many distinct raw handles into a small set of `MC_VertexArrayNode` slots — that's why the legacy `Render.TerrainSolid` loop iterates only 5–15 nodes per frame. M0b's modern bucket count, measured empirically across tier1 missions, is 24–206 per frame (4–15× the legacy count). Each modern bucket = one `glDrawArrays(GL_PATCHES, …)` + `gos_SetRenderState(gos_State_Texture, …)` flush. AMD's per-draw command-list overhead dominates the saved per-batch `glBufferData` upload (the M0b headline win), so the modern path runs slower than legacy when bucket count is high.

The fix: **make the modern bucket key match the legacy node identity**, so modern bucket count converges to legacy node count.

## 3. Questions you must answer before you write any code

These are the gating questions. The brainstorm + spec for B' must resolve them with file:line evidence; do not start implementation until they are answered.

### Q-B'-1: What is the canonical key the legacy path actually buckets by?

The apparent `mcTextureManager->addVertices(nodeId, data, flags)` key (search `mclib/txmmgr.h` around line 824) is `(nodeId, flags)` — each `nodeId` indexes `masterTextureNodes[nodeId]` with up to 5 `vertexData` slots distinguished by `flags`. But observed active node counts (5–15) imply there is either upstream handle normalization, inactive-node filtering, or post-addVertices aggregation that must be identified.

**Do not anchor on `(nodeId, flags)` alone.** The 4–15× discrepancy between distinct raw `terrainHandle` values arriving at the callsite and active legacy nodes per frame means something else is collapsing keys.

**Action:** read `addVertices` and `Render.TerrainSolid` carefully. Trace what `nextAvailableVertexNode` actually counts. Find where the 5–15 number comes from — is it a count of distinct nodes activated this frame, or filtered to non-empty `vertices`, or something else? The audit doc `docs/superpowers/explorations/2026-04-27-terrain-solid-state-binding-inventory.md` has partial answers; verify against actual code.

### Q-B'-2: Is `tex_resolve(textureIndex)` the right canonical key?

Spec §6.2 hints that `tex1` (the colormap GL texture handle) is what forces the draw boundary on AMD. `tex_resolve` is the engine's per-frame memoization of `terrainHandle → GL texture handle` (per `mclib/tex_resolve_table.h`). Multiple raw `terrainHandle` values could resolve to the same GL handle; conversely, the same GL handle bound twice is "the same draw" from AMD's perspective.

**Question:** is `tex_resolve(terrainHandle)` the value `gos_SetRenderState(gos_State_Texture, ...)` ultimately binds? The state-binding inventory audit suggests yes, but verify by tracing through `gos_SetRenderState → applyRenderStates → glBindTexture`.

**Question:** can two distinct `terrainHandle`s resolve to the same GL handle but require different sampler / wrap / material state? If yes, bucketing by GL handle alone would conflate them. If no (they all use the same terrain material with the same sampler params), bucketing by GL handle is correct.

### Q-B'-3: Does canonicalization need flags?

The legacy `mcTextureManager` keys by `(nodeId, flags)`. M0b uses `MC2_ISTERRAIN | MC2_DRAWSOLID` at every callsite — same flags everywhere. So in M0b's narrow scope (solid terrain only), flags are constant and the key reduces to nodeId-or-equivalent.

**Verify** the four append sites in `quad.cpp` (around lines 1636/1788/1955/2105) all pass the same flag combo. If yes, B' can ignore flags. If no, B' must include flags in the canonical key.

### Q-B'-4: Is the canonical key stable from append (in `Terrain::geometry`) through flush (in `Render.TerrainSolid`)?

Per the memory rule `mc2_texture_handle_is_live` ("MC2 texture handles mutate per-frame; store slot index, resolve at draw time, never cache handle"), naïvely caching `tex_resolve(handle)` could be unsafe if handles mutate mid-frame. But Shape A's `tex_resolve_table` is itself a per-frame memoization — once resolved within a frame, the value is stable for the rest of that frame.

**Question:** can append-time resolve be re-used at flush time within the same frame? The Shape A documentation (`docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md`) should answer this. If yes, append-time canonicalization is safe and simple. If no, B' needs a different approach (e.g., bucket by raw handle but coalesce groups at flush time after running all raw handles through `tex_resolve`).

### Q-B'-5: What if `tex_resolve` returns a sentinel (e.g., 0 or `0xffffffff`) for unloaded textures?

Several distinct raw handles could all resolve to the same sentinel "missing" handle. Bucketing them together is functionally correct (they'd all draw with the missing-texture binding) but might mask data issues. Verify the sentinel handling and decide: skip these triangles, or bucket them under one "missing-tex" key, or fall through to legacy?

### Required diagnostic — bucket-census table BEFORE code

Before implementation, produce a bucket-census table comparing the candidate keys empirically. For at least tier1 + Wolfman, capture per frame (or aggregate over a 30s run):

| Mission | raw `textureIndex` count | unique `tex_resolve(handle)` count | legacy active `MC_VertexArrayNode` count (current frame) | candidate canonical bucket count | sentinel/missing count |
|---|---:|---:|---:|---:|---:|

Mechanism: add temporary instrumentation (env-gated, removed before merge) that logs each bucket-key candidate alongside the existing `event=draw_count`. The chosen key must **empirically converge toward the legacy 5–15 range**, or the spec must explain why it still expects a perf win despite a higher count.

This turns B' from "we think canonicalization fixes it" into "we proved candidate key matches legacy draw count before writing implementation code." It is cheap and very valuable. If the table shows the candidate key still produces 50+ buckets per frame, the perf assumption is wrong and the brainstorm must reconsider before spending implementer time.

### Implementation constraint — frame-local only

If the canonical key is a resolved GL/GOS handle (per Q-B'-2), it is **frame-local only**. Do not store it across frames, in savegames, in mission caches, in `MC_VertexArrayNode` persistent state, or in any data structure that survives `clearArrays()`. This preserves the `mc2_texture_handle_is_live` memory rule. The patch-stream's per-frame staging vectors are the right place to hold canonicalized keys; nothing longer-lived is.

## 4. Scope — what to do, what NOT to do

### IN SCOPE

A single surgical fix: **change PatchStream's bucket key from raw `textureIndex` to a canonical key**, where "canonical" is whatever Q-B'-1..5 resolve to.

Two implementation options to evaluate (perf-analysis doc §"Fix path forward"):

- **Option A — flush()-time canonicalization** (safer, default choice): re-bucket `s_drawBuckets[]` by canonical key after the staging-consolidation phase, just before issuing draws. Possibly requires a sort step. Resolves all raw handles as close to draw time as possible — strictly within the same frame as the draw, so handle liveness is unambiguous.
- **Option B — append-time canonicalization**: resolve `textureIndex` → canonical key inside `appendTriangle` and bucket by that from the start. Simpler bucket-management code, but only valid if Q-B'-4 (Shape A frame-lifetime proof) is solid.

**Prefer Option A unless the Shape A frame-lifetime proof is conclusive.** Resolving as close to draw time as possible keeps the modern path's coupling to handle-mutability minimal. Option B is a perf optimization on top of Option A's safety baseline; only adopt it if the brainstorm establishes append-time resolve is genuinely safe AND the bucket-census shows append-time resolve is required to hit the perf target (i.e., flush-time canonicalization isn't enough on its own).

### OUT OF SCOPE — do NOT touch

- `sampler2DArray` colormap collapse (Shape B-array, gated on Canary B)
- Shadow.StaticAccum migration off legacy ring
- Grass pass migration off `terrain_extra_vb_`
- Water / decal / overlay rendering
- ProjectZ rework
- Static-shadow ring removal / `addVertices` removal
- Default-on promotion (still gated on B' passing perf + manual visual gates)
- `glGetAttribLocation` per-draw stall fix
- `matNormal0-4` per-node rebind hoist
- Unit-9 collision (matNormal4 ↔ shadowMap)

If you find yourself wanting to touch any of those, **stop**. They are tracked as separate lanes (see memory entry's "Follow-on lanes" section).

## 5. Manual gates — already cleared

Manual visual A/B was performed by the user on mc2_01 at killswitch=1 (2026-04-27 post-M0b). Result: **PASS**. Static shadows render correctly, "the other things work". Grass was confirmed deprecated and a don't-care, so the consolidated `terrain_extra_vb_` upload path doesn't need defending visually.

This means M0b is functionally + visually validated. B' is the remaining perf gate; once it lands and passes §11 thresholds, M0b+B' is mergeable.

## 6. Workflow expectations

Use the superpowers workflow:

1. **Brainstorm** (`/superpowers:brainstorming`) — answer the §3 questions, weigh Option A vs Option B, identify any new risks
2. **Spec** (`/superpowers:writing-plans` after brainstorm produces requirements) — concrete spec document with the canonicalization design and the §3 answers
3. **Plan** (continuing in writing-plans) — TDD-style task list referencing exact file:line anchors
4. **Execute** (`/superpowers:subagent-driven-development`) — one subagent per task, two-stage review (spec then code quality), commit per task
5. **Verify** — re-run tier1 perf table at killswitch=0 (parity) + killswitch=1 (target: at or above killswitch=0); compare bucket-count distributions; check tier2 if tier1 is clean
6. **Manual gates** (above) — RAlt+F3, RAlt+5 with the user
7. **Commit + update memory** — overwrite `~/.claude/projects/A--Games-mc2-opengl-src/memory/patchstream_m0b.md` with B' status. **Do NOT flip default-on as part of B'.** If B' passes perf, record whether default-on is a future candidate after a bake-in period — that decision is its own slice, not a B' deliverable.

## 7. Recording the status — what to put in memory after B'

If B' brings perf within spec §11 thresholds:

```
M0b + B' status:
  Functional: PASS
  Legacy parity default-off: PASS
  Overflow fallback: PASS
  AMD compatibility: PASS
  Performance: PASS (after B' canonicalization)
  Visual A/B (manual): PASS

Disposition:
  killswitch default may flip to 1 after one full release cycle of
  default-off bake-in.
```

If B' does NOT bring perf within thresholds:

```
M0b + B' status:
  Functional: PASS
  Performance: STILL FAIL (residual <X> regression on <missions>)

Cause:
  <whatever B' didn't catch — likely something beyond bucket count>

Disposition:
  keep default-off.
  next slice <name> investigates <residual cause>.
```

The memory entry is the persistence point for future sessions. Be specific about what B' did and didn't change.

## 8. Bottom line

M0b is not a failed modernization attempt. It is an **infrastructure pass that exposed the exact missing equivalence layer** between raw quad.cpp callsite handles and post-mcTextureManager canonical handles. B' is one surgical fix: make PatchStream bucket like legacy. If that brings draw count back to the 5–15 range, persistent-mapped VBO uploads finally have a chance to win, and the spec §11 thresholds get a real test.

Estimated effort: **half-day** for the implementation if Q-B'-4 (append-time resolve safety) answers cleanly. Add a half-day for the visual gates and tier1+tier2 perf re-runs.

Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
Branch: `claude/nifty-mendeleev`
Build: ALWAYS `RelWithDebInfo` + `--target mc2`
Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.2/`
Smoke runner: env propagation already fixed in commit `9cf3d4f`; can use `MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs` directly.
