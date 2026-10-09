# MC2 Advisor Fleet - Domains and Routing

Canonical reference for the MC2 advisor subagent fleet. Use this file to find the right advisor for a question, decide where new knowledge belongs, and track pending fleet-maintenance decisions.

Last refreshed: 2026-05-14 (after the advisor-landscape audit).

## How to use this file

- **Routing a question?** See "Routing table" below.
- **Building a new advisor?** See "Conventions" + "Gaps" sections. Check that the domain doesn't already exist.
- **Updating an existing advisor?** See "Maintenance" section.
- **Auditing the fleet?** Most recent: [docs/advisor-landscape-audit-2026-05-14.md](../../docs/advisor-landscape-audit-2026-05-14.md).

---

## Conventions

### Filename pattern

- `mc2-<domain>-expert.md` for domain advisors (owns one coherent area of code)
- `mc2-<methodology>-expert.md` for methodology advisors (encodes practice across multiple domains)
- `mc2-<topic>-synthesizer.md` for write-side maintenance agents (the only advisors with Edit/Write tools)
- `amd-shader-reviewer.md` is the sole non-`mc2-`-prefixed advisor (pending reclassification to skill, see "Pending decisions")
- ALL-CAPS filenames in this directory (this file, future siblings) are fleet-level reference docs, not advisors

### Body structure (standard advisor)

All MC2 advisors except `amd-shader-reviewer` follow this template:

```
<role>           - one paragraph: what kinds of questions this advisor expects
<load_first>     - MEMORY.md + topic memory files + .planning/codebase docs to read before answering
<core_knowledge> - 5-15 load-bearing facts with file:line starting points
<known_pitfalls> - tacit-knowledge traps (the most load-bearing section; reason the advisor exists)
<file_locations> - starting points for grep; "grep symbol to find current line"
<work_protocol>  - Rule 0 (grep before line numbers) + numbered steps for invocation
<limits>         - what the advisor does NOT know; explicit defer-to list
<cross_references> - sibling advisors + relevant memory categories + planning docs
```

Preserve this template for any new advisor. Deviation creates audit friction.

### Classification axes

- **Domain advisor** - owns one coherent area of code. Answers most questions about that area authoritatively.
- **Methodology advisor** - encodes a practice that crosses multiple domains (e.g., "how to design any new CPU-to-GPU offload slice"). NOT bound to one area of code.
- **Skill-shaped** - procedural workflow guidance, no persistent expertise. Belongs in `.claude/skills/` rather than `.claude/agents/`.

### Source-of-truth hierarchy

1. Current code on disk (always wins)
2. `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` index + topic files (facts)
3. `.planning/codebase/*.md` (topology snapshots)
4. `docs/render-contract.md` and related canonical docs (contracts)
5. Advisor `<core_knowledge>` blocks (compiled views; stale by definition)

When advisor content disagrees with MEMORY.md or current code, code wins. Advisors are eventual-consistency.

---

## Routing table

Match the question shape against the trigger column. Use the cross-routes when the question straddles.

| Question shape (keywords / triggers) | Owning advisor | Cross-route to |
|---|---|---|
| Render queue/flush, fast-path bring-up, dual-queue, MLR exception, TG_Shape, `renderLists`, "why does my draw produce no output" | mc2-render-expert | terrain-indirect (cement/runway), perf-expert (Tracy/slice ROI) |
| Tracy bucket interpretation, slice ROI sizing, dependency graph between in-flight slices, `applyRenderStates` invalidation, GameLogic / Camera.UpdateRenderers / wolfman | mc2-render-perf-expert | render-expert (queue internals), cpu-gpu-offload (methodology) |
| GLSL syntax/compile, UBO/SSBO layout, std430, sampler/depth state, uniform API, hot-reload, BGRA swizzle | mc2-shader-expert | amd-shader-reviewer (AMD-specific rules), render-expert (pipeline shape) |
| AMD RX 7900 XTX driver rule violations (attribute 0, sampler2DArray, gl_FragDepth, feedback loops, transpose) | amd-shader-reviewer | shader-expert (general GLSL) |
| Mechanical recipe execution: recon sweeps, witness smoke runs, menu-oracle checks, log triage, free-ordinal / FST lookups, "is gate X on" | mc2-haiku-scout (model=haiku, CHEAP — prefer for execute-a-recipe tasks) | any domain expert (when the scout reports BLOCKED/OPEN) |
| CMake, vcpkg, FFmpeg delay-load, ASan, RelWithDebInfo, full-relink, `-DLINUX_BUILD`, `/DELAYLOAD:`, link libraries | mc2-build-system-expert | (rarely cross-routes) |
| FST/.fit/.tga/.wav formats, asset loading, S_strlwr/elfHash, init order, ForceGroupIcon, options.cfg drift, atlas safety | mc2-mission-data-expert | gameos-expert (platform IO), render-expert (texture handles) |
| `gos_*` API, gos_GetKey input semantics, gameos_sound, `gos_RendererEndFrame` flush trigger, platform file IO, AUDIO_U8 | mc2-gameos-expert | mission-data (engine-side normalization), render-expert (renderLists flush) |
| Indirect terrain pipeline, recipe SSBO, thin records, cement atlas, `MC2_TERRAIN_INDIRECT*`, `gpu_driven_terrain_solid.comp`, `tessDebug.x` | mc2-terrain-indirect-expert | render-expert (broader pipeline), shader-expert (GLSL side) |
| Mesh import (Assimp/GLTF/FBX/OBJ), TG_TypeMultiShape population, BT2018, hierarchy passes, texture binding at load | mc2-mech-import-expert | mech-anim (skeletal output), mech-runtime (per-frame consumer) |
| Skeletal animation math, P-conjugation, FK, IBM, gesture binding, GPU skinning math | mc2-mech-skeletal-anim-expert | mech-import (rig loading), mech-runtime (orchestration) |
| Per-frame mech update, `updateGeometry`, `TransformMultiShape` variants, killswitch matrix, mover-pick, PerPolySelect | mc2-mech-update-geometry-expert | mech-anim (per-bone math), cpu-gpu-offload (D-gpu-pose precedent) |
| How to design a NEW CPU-to-GPU offload slice (Stage 0-6 recipe, recon, parity gate, soak, default-on flip) | mc2-cpu-gpu-offload-expert (methodology) | render-expert / terrain-indirect / mech-runtime for specific pipeline questions |
| Refresh docs/render-contract.md from accumulated notes corpus (manual invocation, write-side) | mc2-render-contract-synthesizer (methodology) | render-expert for read-side rendering questions |
| ABL scripting, mission objectives, behavior trees, unit logic | NO ADVISOR YET (GAP - high priority) | main agent fallback |
| Post-processing, FBOs, bloom, FXAA, shadow projection setup | NO ADVISOR (thin coverage in gameos-expert; medium-priority gap) | gameos-expert |
| Networking / multiplayer transport | NO ADVISOR NEEDED | (no transport exists; only ws2_32 for Tracy) |

When a query straddles two advisors with equal claim (e.g., "why is substrate slow but only when wolfman is zoomed out?"), the user or main agent picks; both advisors should defer to each other per the cross-routes above. If you find yourself routing the same query type to both repeatedly, the routing table needs a sharpening edit (see "Pending decisions").

---

## Domain advisors

Each owns one coherent area of code. Mature unless noted.

- **mc2-render-expert** (199 lines) - render pipeline shape: queue/flush, fast paths, cull, dual-queue retirement debt, Track A/B/C strategy, MLR exception. Authoritative for "how does the pipeline submit" and "why does my draw look wrong."
- **mc2-render-perf-expert** (193 lines, HYBRID) - CPU render-path perf: Tracy zone interpretation, slice ROI, in-flight slice dependency graph, applyRenderStates contract. Pending split into methodology advisor + snapshot doc (see "Pending decisions").
- **mc2-shader-expert** (181 lines) - GLSL 4.30, UBO/SSBO, sampler/depth state, uniform API, hot-reload, BGRA convention.
- **mc2-build-system-expert** (172 lines) - CMake, vcpkg, FFmpeg delay-load, ASan, RelWithDebInfo, full-relink semantics.
- **mc2-mission-data-expert** (179 lines) - FST/.fit/.tga/.wav, asset loading, init order, atlas safety, stock-must-be-playable rule. Absorbs save-game and audio-asset-format.
- **mc2-gameos-expert** (173 lines) - gos_* API, input semantics, audio playback, `gos_RendererEndFrame` flush, file IO at platform layer. Absorbs audio playback questions.
- **mc2-terrain-indirect-expert** (175 lines) - indirect terrain pipeline only: cement atlas, thin records, compute dispatch, `MC2_TERRAIN_INDIRECT*` env vars. Non-indirect terrain routes to render-expert (medium-priority gap, see below).
- **mc2-mech-import-expert** (126 lines) - mesh import, TG_TypeMultiShape population, BT2018, hierarchy passes.
- **mc2-mech-skeletal-anim-expert** (127 lines) - per-bone math, FK, IBM, GPU skinning. Boundary with mech-runtime is the `submitActor` function.
- **mc2-mech-update-geometry-expert** (176 lines) - per-frame mech orchestration: updateGeometry, TransformMultiShape, killswitches, mover-pick.

---

## Methodology advisors (cross-cutting)

Each encodes a practice that applies across multiple domains.

- **mc2-cpu-gpu-offload-expert** (152 lines) - the clearest methodology advisor. Encodes the Stage 0-6 staging shape for any new CPU-to-GPU offload slice: recon (cost-split), design + adversarial review, scaffold, parity gate, soak, default-on flip, demote-not-delete. Plus the 3-slot tryConsume ring pattern and readback sync-stall rule. Explicitly disclaims authority over how shipped pipelines work; defers to domain advisors for those.
- **mc2-render-contract-synthesizer** (229 lines) - the only write-side advisor in the fleet. Reads observation notes from `docs/observations/` + current code + existing render contract, produces an updated `docs/render-contract.md` plus a dated changelog. Manually invoked. Sibling to render-expert (read-side); the two never overlap.
- **mc2-render-perf-expert** (HYBRID, pending split) - partially methodological (cull cascade safety contract, applyRenderStates invalidation, sub-100ns Tracy floor, agent-isolation cleanup pattern). Also carries domain-specific bucket map + dependency graph that mutates session-to-session. Pending split: extract methodology to `mc2-render-perf-methodology-expert`, move snapshot content to a deliberately-refreshed doc.

---

## Skill candidates (not in the advisor fleet)

Patterns the audit flagged as workflow-skill-shaped. None built yet; flagged for future.

- **mc2-parity-soak-check** - "is this slice ready for default-on flip?" recurring check
- **mc2-tier1-smoke-triage** - parse smoke artifacts, find the failure mode
- **mc2-render-doc-update** - workflow wrapper around the synthesizer-and-notes loop

Plus: **amd-shader-reviewer is currently in `.claude/agents/` but reads as skill-shaped** (48 lines, no advisor body sections, narrow procedural review task). Pending move to `.claude/skills/` (see "Pending decisions").

---

## Gaps

Domains without an advisor, ranked by audit priority. Use the meta-prompt at `.claude/prompts/distill-session-into-advisor-agent.md` to harvest an advisor from a session that did real work in one of these domains.

- **HIGH priority: ABL scripting.** Multiple advisors explicitly defer ABL to main agent. Mission scripting, behavior trees, unit logic, mission objectives, scripting-side debugging. Tier-2 coverage of `code/abl/` plus mission.cpp driver wiring.
- **MEDIUM priority: terrain (non-indirect).** Water rendering, terrain tessellation pre-indirect, patchstream pipeline, `makeLists` semantics, vertexProjectLoop, quadSetupTextures. Decision: widen mc2-terrain-indirect-expert into mc2-terrain-expert, OR add a sibling. Audit recommends widening.
- **MEDIUM priority: post-processing / FBOs / bloom / shadow projection.** `gos_postprocess.cpp` is thinly covered in gameos-expert. Borderline whether a dedicated advisor or a sub-section in gameos-expert is right.
- **LOW priority: GPU compute infrastructure (sibling to terrain-indirect).** `gpu_cull_compute.cpp`, `gpu_cull_readback.cpp`, substrate-coalesce armed work. Arguably covered well enough by render-cluster + cpu-gpu-offload methodology today.
- **LOW priority: Tracy / RGP / RenderDoc tooling.** No advisor owns these tools-as-tools; currently distributed across render-perf and cpu-gpu-offload.

Networking / multiplayer is explicitly NOT a gap - no transport code exists; no advisor needed until that changes.

---

## Cross-cutting concerns (NOT domains)

Concerns that span multiple advisors and should NOT be isolated into their own domain. Future sessions wanting to harvest one of these will hit the "wait, who owns this?" question and the answer is "nobody, it's cross-cutting."

- **Culling** - `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive`. Co-owned by render-expert and render-perf-expert via the `cull_gates_are_load_bearing.md` shared memory. Isolating into its own advisor would create more overlap, not less.
- **Texture handle lifecycle** - mutate-per-frame discipline. Co-owned by mission-data (loading side) and render-expert (consumption side).
- **`mcTextureManager->update()` cache eviction** - shared between gameos (flush trigger) and render-expert (queue consumer) and mission-data (handle resolution). The PAUSE/UNPAUSE diagnostic is the load-bearing tool.
- **Adversarial code-grounded review** - codified in `.claude/skills/adversarial-plan-review.md`. Cross-cuts every domain; not an advisor.

---

## Executed decisions (resolved 2026-05-14, executed 2026-05-14)

1. **mc2-render-perf-expert split: DONE.** Methodology content kept in `.claude/agents/mc2-render-perf-expert.md` (rewritten to methodology-only); snapshot content moved to `docs/render-perf-snapshot.md`. Methodology advisor's `<load_first>` reads the snapshot every invocation. Snapshot doc has a "Last refreshed" header for deliberate refresh discipline.
2. **amd-shader-reviewer reclassification: DONE.** Converted from agent to skill. File at `.claude/skills/mc2-amd-shader-review.md` (matches the `mc2-*` skill naming convention). Old `.claude/agents/amd-shader-reviewer.md (removed)` removed. `mc2-shader-expert`'s `<cross_references>` now points at the skill.
3. **mc2-render-expert <-> mc2-render-perf-expert routing sharpening: DONE.** Render-expert has explicit `<cross_references>` entries and a `DEFER` section in `<limits>` for Tracy/perf questions. Render-perf-expert has matching entries pointing the other way.
4. **mc2-render-expert <-> mc2-terrain-indirect-expert cross-reference: DONE.** Render-expert now lists terrain-indirect-expert in both `<cross_references>` and the `<limits>` DEFER section.

## Pending decisions (none currently)

None at this time. Audit follow-ups from 2026-05-14 are fully executed.

---

## Maintenance

The fleet is fully built; staying current matters more than building more.

### When to refresh an advisor

- A memory topic file listed in the advisor's `<load_first>` block has been added, deleted, or substantively revised
- The advisor's cited `file:line` references have drifted (a `Rule 0` grep miss during invocation is the symptom)
- A new slice has landed that changes the advisor's `<core_knowledge>` claims (e.g., legacy queue retirement when it ships will obsolete the dual-queue debt section in render-expert)
- The advisor's snapshot content (perf bucket map, dependency graph) is dated more than a milestone old

### How to refresh

Two patterns:

- **Inline edit.** Small drift, one or two facts. Read advisor, Edit, done.
- **Notes -> synthesizer cycle (preferred for substantive refreshes).** Sessions doing work in the advisor's domain paste the dump prompt at `.claude/prompts/dump-render-observations.md` (or its generalized successor) to produce dated notes in `docs/observations/`. When notes accumulate, manually invoke the synthesizer for that domain. Currently only `mc2-render-contract-synthesizer` exists; an equivalent for the advisor fleet itself is on the future-build list.

### Audit cadence

Run an advisor-landscape audit (like the 2026-05-14 one) when:
- The fleet has grown by 3+ advisors since the last audit
- Overlaps are noticed empirically (same query routing to two advisors with no clear winner)
- A new advisor is being designed and you want to confirm it doesn't duplicate

The audit recipe lives in this session's user-message corpus; can be re-prompted into a fresh subagent.

---

## Audit history

- **2026-05-14:** Initial audit. 13 advisors, 1 HIGH overlap (render-expert <-> render-perf), 7 LOW overlaps, 0 mergers recommended. Findings: [docs/advisor-landscape-audit-2026-05-14.md](../../docs/advisor-landscape-audit-2026-05-14.md). Decisions logged in "Pending decisions" above.
