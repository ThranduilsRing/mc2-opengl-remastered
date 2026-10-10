<!-- doc-status: current | reviewed:2026-10-11 -->
# Advisor Landscape Audit - 2026-05-14

## Scope

Files audited (13 MC2-scoped advisors under `.claude/agents/`):

- `.claude/agents/amd-shader-reviewer.md`
- `.claude/agents/mc2-build-system-expert.md`
- `.claude/agents/mc2-cpu-gpu-offload-expert.md`
- `.claude/agents/mc2-gameos-expert.md`
- `.claude/agents/mc2-mech-import-expert.md`
- `.claude/agents/mc2-mech-skeletal-anim-expert.md`
- `.claude/agents/mc2-mech-update-geometry-expert.md`
- `.claude/agents/mc2-mission-data-expert.md`
- `.claude/agents/mc2-render-contract-synthesizer.md`
- `.claude/agents/mc2-render-expert.md`
- `.claude/agents/mc2-render-perf-expert.md`
- `.claude/agents/mc2-shader-expert.md`
- `.claude/agents/mc2-terrain-indirect-expert.md`

Files SKIPPED (out of scope, non-MC2-specific): none. The `.claude/agents/` directory contains zero GSD-framework agents at audit time; every file in the directory is an MC2 advisor.

Methodology: read each advisor's frontmatter (name, description), then body sections (role, load_first, core_knowledge, known_pitfalls, file_locations, work_protocol, limits, cross_references). Classified each advisor as domain or methodology. Mapped overlaps via three signals: (a) shared memory files referenced in load_first, (b) overlapping description triggers / keywords, (c) overlapping core_knowledge claims. Compared first-cut domain list from the audit prompt against what the advisors actually claim authority over.

## Inventory

| File | Frontmatter `name` | Lines | One-line scope | First-cut classification |
|------|-------------------|-------|----------------|--------------------------|
| amd-shader-reviewer.md | amd-shader-reviewer | 48 | Scan GLSL files for AMD RX 7900 XTX driver rule violations | domain:shaders (narrow review-only sub-domain) |
| mc2-build-system-expert.md | mc2-build-system-expert | 172 | CMake, vcpkg, FFmpeg delay-load, ASan, RelWithDebInfo, full-relink | domain:build-system |
| mc2-cpu-gpu-offload-expert.md | mc2-cpu-gpu-offload-expert | 152 | Methodology for staging any new CPU-to-GPU offload slice | methodology:cpu-gpu-offload |
| mc2-gameos-expert.md | mc2-gameos-expert | 173 | gos_* platform layer: input, audio, RendererEndFrame flush, file IO | domain:gameos-platform |
| mc2-mech-import-expert.md | mc2-mech-import-expert | 126 | Assimp / GLTF / FBX / OBJ mesh import; TG_TypeMultiShape population | domain:mech-import |
| mc2-mech-skeletal-anim-expert.md | mc2-mech-skeletal-anim-expert | 127 | Skeletal anim: P-conjugation, FK, IBM, gesture binding, GPU skinning | domain:mech-anim |
| mc2-mech-update-geometry-expert.md | mc2-mech-update-geometry-expert | 176 | Per-frame mech CPU pipeline: updateGeometry, TransformMultiShape variants, killswitch matrix | domain:mech-runtime |
| mc2-mission-data-expert.md | mc2-mission-data-expert | 179 | FST/.fit/.tga/.wav formats, asset loading, init order, atlas safety | domain:mission-data |
| mc2-render-contract-synthesizer.md | mc2-render-contract-synthesizer | 229 | Write-side synthesizer that updates docs/render-contract.md from notes corpus | methodology:doc-synthesis (meta, write-side) |
| mc2-render-expert.md | mc2-render-expert | 199 | Render pipeline: queue/flush, fast paths, cull, dual-queue, Track A/B/C strategy | domain:render-pipeline (broad) |
| mc2-render-perf-expert.md | mc2-render-perf-expert | 193 | CPU render-path perf: Tracy zone interpretation, slice ROI, dependency graph | ambiguous (between domain:render-perf and methodology:perf-orchestration) |
| mc2-shader-expert.md | mc2-shader-expert | 181 | GLSL 4.30, UBO/SSBO, sampler/depth state, uniform API, hot-reload | domain:shaders |
| mc2-terrain-indirect-expert.md | mc2-terrain-indirect-expert | 175 | Indirect terrain rendering: recipe SSBO, thin records, cement atlas, debug viz | domain:terrain (narrowed to indirect path) |

## Domain map (refined)

The first-cut domain list from the audit prompt was render-pipeline / terrain / culling / mechs / shaders / build-system / mission-data / gameos-platform / abl-scripting / audio / save-game / networking. The actual advisor fleet supports a different decomposition. Mechs split into three distinct domains (import / animation / runtime-orchestration); terrain has one indirect-path-specific advisor but no general terrain advisor; culling does not surface as its own domain (it is co-owned by render-expert and render-perf-expert via cull_gates_are_load_bearing.md). Audio collapses inside gameos-platform. Save-game collapses inside mission-data.

| Domain | Status | Existing advisors | Notes |
|---|---|---|---|
| render-pipeline (general) | mature, single canonical | mc2-render-expert | Broad scope: queue/flush, fast paths, cull, dual-queue, Track A/B/C strategy. Authoritative for pipeline shape questions. |
| render-perf (CPU side, slice ROI, sequencing) | overlap with render-pipeline | mc2-render-perf-expert | Overlapping core_knowledge: cull cascade, substrate state, parity discipline, gpu_direct_renderer_bringup_checklist. Adds Tracy bucket map + dependency graph + slice ROI math. See OVERLAP-1 below. |
| terrain (general) | partial - only indirect path covered | mc2-terrain-indirect-expert | No general-terrain advisor; non-indirect terrain (legacy CPU walk, tessellation pre-indirect, water) routes to mc2-render-expert. |
| culling | distributed, not isolated | (co-owned) | inView / canBeSeen / objBlockInfo / Track C compute cull straddle render-expert + render-perf-expert + cpu-gpu-offload-expert. Reasonable as-is; cull is too cross-cutting to bound. |
| shaders (general) | mature | mc2-shader-expert | GLSL syntax, uniform API, sampler/depth state, hot-reload. |
| shaders (AMD review) | narrow, single-purpose | amd-shader-reviewer | 48-line review-only sub-agent. Does only AMD driver rule checking on specified files. Sibling to shader-expert, not a duplicate. |
| build-system | mature, single canonical | mc2-build-system-expert | CMake, vcpkg, FFmpeg, ASan, RelWithDebInfo, full-relink. |
| mission-data / asset-format | mature, single canonical | mc2-mission-data-expert | FST/.fit/.tga/.wav, init order, atlas safety, stock-must-be-playable rule. Absorbs save-game and audio-asset-format questions. |
| gameos-platform | mature, single canonical | mc2-gameos-expert | gos_GetKey, gameos_sound, gos_RendererEndFrame flush trigger, file IO at platform layer. Absorbs audio-playback questions. |
| mech-import (load-time / Assimp) | mature | mc2-mech-import-expert | TG_TypeMultiShape population, BT2018, hierarchy FOURTH PASS, texture binding at load. |
| mech-anim (skeletal animation math) | mature | mc2-mech-skeletal-anim-expert | P-conjugation, FK, IBM, gesture binding, GPU skinning math. |
| mech-runtime (per-frame mech update) | mature | mc2-mech-update-geometry-expert | updateGeometry, TransformMultiShape variants, killswitch matrix, mover-pick discipline. |
| cpu-gpu-offload methodology | mature | mc2-cpu-gpu-offload-expert | The clearest methodology advisor: how to design ANY new offload slice. References cost-split-recon-bucket-design skill + adversarial-plan-review skill. |
| render-contract synthesis (write-side) | mature, single-purpose | mc2-render-contract-synthesizer | The only write-side advisor (has Edit/Write tools). Manually invoked. Refreshes docs/render-contract.md from notes corpus. Sibling to render-expert. |
| abl-scripting | GAP | (none) | First-cut list named this. Multiple advisors explicitly defer ABL to main agent. See gaps below. |
| networking / multiplayer | not needed | (none) | Build-system and gameos advisors both explicitly note there is no MP transport; only Tracy uses ws2_32. No advisor needed. |
| post-processing / FBOs / bloom | thin coverage | mc2-gameos-expert (partial) | gameos-expert mentions gos_postprocess.cpp owns FBOs/bloom/shadows but does not deep-cover. mc2-render-expert defers FBO details to gameos. Borderline gap. |
| audio playback / sound effects | covered as sub-domain | mc2-gameos-expert | AUDIO_U8 / S8 / gameos_sound.cpp covered there. Sufficient for current state. |
| save game / mission load sequence | covered as sub-domain | mc2-mission-data-expert | Sufficient. |

## Methodology advisors (cross-cutting)

| Advisor | What practice it encodes | Domains it crosses |
|---|---|---|
| mc2-cpu-gpu-offload-expert | The Stage-0-through-Stage-6 staging shape for any new offload slice: recon (cost-split), design + adversarial review, scaffold, parity gate, soak, default-on flip, demote-not-delete. Also: the 3-slot tryConsume ring pattern, the readback sync-stall avoidance rule, adversarial-review iteration discipline. Explicitly disclaims authority over how shipped subsystems work internally. | render-pipeline, terrain, mechs, shaders (any compute slice). The clearest methodology advisor in the fleet. |
| mc2-render-contract-synthesizer | The write-side merge/changelog discipline for refreshing a long-lived doc from accumulated observation notes. Re-grep every cited symbol, FLAG contradictions rather than silently resolving, produce a separate changelog file. Manually invoked. | render-pipeline (sole target doc is render-contract). Could in principle be generalized into a doc-synthesis pattern but isn't. |
| mc2-render-perf-expert | Partially methodological: the cull-cascade safety contract, applyRenderStates state-cache invalidation contract, the agent-isolation cleanup pattern, slice-sequencing dependency-graph reasoning. But also carries a specific bucket map and dependency graph for the current in-flight slice landscape - which is domain knowledge, not methodology. Hybrid. | render-pipeline (heavy), mechs (Track D), terrain (substrate). See ambiguity section. |

## Overlaps detected

### OVERLAP-1: mc2-render-expert vs mc2-render-perf-expert (HIGH)

Evidence of overlap:

- **Shared load_first memory files (~7 of 8 in each advisor's list):** Both load `cull_gates_are_load_bearing.md`, `gpu_direct_renderer_bringup_checklist.md`, `pause_unpause_diagnostic_for_static_render_bugs.md`, `track_c_compute_cull.md`, `track_c_substrate_regression.md`, `substrate_coalesce_sync_point_lesson.md`, plus the renderwater / track-a / track-b memories. perf-expert adds `perf_profiling_results.md`, `update_skip_touch_regression.md`, `m2_thin_record_cpu_reduction_results.md`. render-expert adds `render_functions_are_enqueuers_not_submitters.md`, `mc_texture_manager_dual_queue_legacy_retirement_debt.md`, `render_order_post_renderlists_hook.md`, `black_tree_bug_investigation_state.md`, `render_state_change_cost_hierarchy.md`. About 60 percent of the load_first set is identical.
- **Overlapping description keywords:** both trigger on "compute cull", "substrate", "Track A/B/C", "fast path", "cull". Distinguishing keyword in perf is "Tracy zones / GameLogic / Camera.UpdateRenderers / wolfman / frame budget"; in render-expert it is "queue/flush / renderLists / TG_Shape / dual queue".
- **Overlapping core_knowledge:** both state the substrate sync-stall lesson, both state the cull cascade is load-bearing, both state Track A/B/C history. render-perf-expert additionally describes the dependency graph (substrate blocks lifecycle; Track D obsoletes per-mech lifecycle; PR2 is independent) and the bucket map (terrain drawPass ~1.7 ms, etc.). render-expert additionally describes the dual-queue architecture, MLR exception, and the gpu_direct fast-path bring-up.

Severity: **HIGH**. Both advisors will fire on the same question shapes ("why is substrate slow?", "what does cull gate?", "what's next after Track C?"). Routing between them is unclear from descriptions: a user asking "why is my fast-path producing no output but only when wolfman is zoomed out?" hits both triggers. The 60 percent load_first overlap means both will load the same memory cluster and produce parallel partially-correct answers.

Proposed resolution: **KEEP-BOTH-CLARIFY-CROSS-REFS** (NOT merge). The distinction is real: render-expert is the bring-up / architecture / shape advisor, render-perf-expert is the slice-ROI / dependency-graph / Tracy-interpretation advisor. The fix is to sharpen the descriptions and add explicit "defer to perf-expert for" / "defer to render-expert for" routing lines in both cross_references blocks. Specifically: render-expert should defer Tracy bucket questions and slice sequencing to perf-expert; perf-expert should defer queue/flush internals and fast-path bring-up traps to render-expert. The current perf-expert cross_references lists render-expert as a sibling but does NOT yet have a hard routing rule; render-expert does not cross-reference perf-expert at all (perf-expert was written more recently; render-expert is stale on this front).

Load-bearing content to preserve in render-perf-expert if not merged: the bucket map, the dependency graph between in-flight slices (substrate -> lifecycle, Track D independence, PR2c MINE state), the applyRenderStates invalidation contract, the agent-isolation cleanup pattern, and the `Camera.UpdateRenderers` wrapper-self-time lesson. None of this is in render-expert.

### OVERLAP-2: mc2-render-expert vs mc2-terrain-indirect-expert (LOW)

Evidence:

- **Shared load_first:** both load `gpu_direct_renderer_bringup_checklist.md`, `cull_gates_are_load_bearing.md`, `track_c_compute_cull.md`, `track_c_substrate_regression.md`, `substrate_coalesce_sync_point_lesson.md`, `cpp_glsl_ubo_struct_lockstep.md`. terrain-indirect adds the cement / quadList-camera-windowed / render-state-cost memories.
- **Overlapping description keywords:** "substrate" triggers both. "MC2_GPU_CULL_SUBSTRATE" appears in both. The render-expert description mentions "terrain rendering" as a covered area generally.

Severity: **LOW**. Terrain-indirect is clearly scoped to one specific pipeline (gos_terrain_indirect.cpp, cement atlas, thin records, `MC2_TERRAIN_INDIRECT*` env vars). It explicitly defers broader-render-pipeline questions to render-expert and treats the cross-reference correctly. Acceptable as-is.

Proposed resolution: **KEEP-BOTH-CLARIFY-CROSS-REFS**. The cross-reference is already mostly clean; the only sharpening needed is that render-expert should mention terrain-indirect-expert in its limits/cross_references (currently does not).

### OVERLAP-3: mc2-render-expert vs mc2-cpu-gpu-offload-expert (LOW)

Evidence:

- **Shared load_first:** both load `gpu_direct_renderer_bringup_checklist.md`, `cull_gates_are_load_bearing.md`, `track_c_substrate_regression.md`, `substrate_coalesce_sync_point_lesson.md`, `track_b_widen_static_prop_registry.md`.
- **Overlapping description keywords:** "GPU-direct", "Track A/B/C", "compute cull" appear in both.

Severity: **LOW**. cpu-gpu-offload-expert explicitly disclaims authority over how a shipped pipeline works internally and defers to render-expert / terrain-indirect-expert / shader-expert. Its limits block is the most rigorous in the fleet on this front. The overlap is in shared lesson memory (sync-stall, bring-up checklist), which is the right behavior - those lessons are cross-cutting.

Proposed resolution: **KEEP-BOTH-CLARIFY-CROSS-REFS**. No action needed; routing is already correct.

### OVERLAP-4: mc2-mech-update-geometry-expert vs mc2-mech-skeletal-anim-expert (LOW)

Evidence:

- **Overlapping description:** both mention submitActor; both touch `gos_mech_batcher.cpp`. update-geometry covers what submitActor reads (`listOfShapes[i].shapeToWorld`); skeletal-anim covers what submitActor's `skel_compute_fk` does inside it.
- **Overlapping core_knowledge:** both reference `MC2_GPU_MECH_SKIN` killswitch behavior.

Severity: **LOW**. Boundary is sharp and acknowledged in both advisors' cross_references blocks: anim owns per-bone math (FK, IBM, P-conjugation, GPU skinning); update-geometry owns per-mech orchestration (which killswitch flag does what, how the per-leaf state contract gates PerPolySelect / Render). The submitActor function is the natural interface between them.

Proposed resolution: **KEEP-BOTH-CLARIFY-CROSS-REFS**. The cross-references are already clean; this is a textbook case of well-scoped sibling advisors.

### OVERLAP-5: mc2-mech-update-geometry-expert vs mc2-cpu-gpu-offload-expert (LOW)

Evidence:

- update-geometry explicitly references the cpu-gpu-offload-expert as the right home for "should we GPU-compute mech bones" questions and cites the D-gpu-pose abandonment as a precedent that fits the methodology framework.

Severity: **LOW**. The boundary is explicit in update-geometry's cross_references. No action.

### OVERLAP-6: mc2-shader-expert vs amd-shader-reviewer (LOW)

Evidence:

- Both deal with GLSL shaders. shader-expert covers GLSL syntax + uniform API + UBO/SSBO. amd-shader-reviewer covers a narrow scan-and-report task against six specific AMD driver rules (attribute 0, sampler2DArray, gl_FragDepth, feedback loops, transpose consistency, no #version).
- shader-expert's `limits` and `cross_references` blocks acknowledge amd-shader-reviewer's existence and route AMD-specific perf analysis there.

Severity: **LOW**. amd-shader-reviewer is purpose-built for a specific repetitive review task (pre-build shader scan). It is not a general advisor and does not compete with shader-expert.

Proposed resolution: **KEEP-BOTH**. The role split is correct; amd-shader-reviewer reads as a workflow-skill-shaped reviewer rather than a domain advisor, but it lives in `.claude/agents/` rather than `.claude/skills/` because it benefits from the persona / role framing.

### OVERLAP-7: mc2-render-expert vs mc2-render-contract-synthesizer (LOW)

Evidence:

- Both touch the render contract concept. synthesizer's first cross-reference is render-expert, and it explicitly says "the two never overlap" because synthesizer is write-side, render-expert is read-side.

Severity: **LOW**. Boundary is sharply defined in synthesizer's frontmatter ("Manually-invoked synthesizer ... NOT triggered automatically by render questions - use mc2-render-expert for those").

Proposed resolution: **KEEP-BOTH**. Acceptable as-is.

### OVERLAP-8: mc2-gameos-expert vs mc2-mission-data-expert on file IO (LOW)

Evidence:

- gameos-expert covers "GameOS file IO sits below the engine's File::open" and explicitly defers path-normalization invariants to mission-data-expert.
- mission-data-expert owns `mclib/file.cpp`, `S_strlwr`, `elfHash`, FST normalization.

Severity: **LOW**. The line is drawn cleanly: gameos owns the platform primitives, mission-data owns the engine-side normalization. The PAUSE/UNPAUSE diagnostic at `mcTextureManager->update()` is shared territory but both advisors handle this correctly (gameos covers the flush trigger and texture eviction; mission-data covers texture handle lifecycle).

Proposed resolution: **KEEP-BOTH**.

## Gaps

Domains in the refined list with no advisor, ranked by recommended priority:

- **High priority: abl-scripting.** Multiple advisors (build-system, gameos, mission-data, mech-update-geometry) explicitly defer ABL questions to the main agent. ABL is the gameplay-scripting language that drives missions and unit behaviors; questions about it land regularly during mission work. Today these route to the main agent's ad-hoc reasoning, which has no memory cluster or file-locations starting points. An mc2-abl-expert would absorb mission scripting, behavior trees, unit logic, mission objective definitions, scripting-side debugging. Tier-2 coverage of `code/abl/` (if that path is accurate) plus the existing `code/mission.cpp` mission-driver wiring.

- **Medium priority: terrain (non-indirect).** mc2-terrain-indirect-expert is sharp but covers only the indirect path. Questions about water rendering (renderWater fast path, Stage 2/3 shipped), terrain tessellation (TerrainQuad::draw before indirect arms), patchstream pipeline, makeLists semantics, vertexProjectLoop, quadSetupTextures (the GPU compute Phase 1 endpoint per cpu-gpu-offload-expert) land between render-expert and terrain-indirect-expert with no clean home. Either (a) widen terrain-indirect-expert into a general mc2-terrain-expert, or (b) create a sibling. Option (a) is cleaner; the indirect advisor's known_pitfalls already touches the legacy/world-space overlay coexistence, so it has the right context.

- **Medium priority: post-processing / FBOs / bloom / shadow projection.** gameos-expert mentions `gos_postprocess.cpp` owns this but does not deep-cover. render-expert defers FBO setup to gameos. shadow.hglsl content is documented in shader-expert but the FBO management around it is unowned. A dedicated mc2-postprocess-expert would absorb: gos_postprocess.cpp, bloom, FXAA, shadow projection setup, FBO lifecycle, post-process effects chain. Borderline whether this justifies its own advisor or should be a sub-section in gameos-expert.

- **Low priority: GPU compute infrastructure (sibling to terrain-indirect).** cpu-gpu-offload-expert covers methodology; terrain-indirect covers one shipped compute pipeline; `GameOS/gameos/gpu_cull_compute.cpp` + `gpu_cull_readback.cpp` are the shipped compute-cull substrate referenced in many memory files but not deeply owned by any one advisor. render-expert and render-perf-expert both touch the surface. A dedicated mc2-gpu-cull-expert (or mc2-compute-shader-expert) would absorb the gpu_cull_* files plus the substrate-coalesce armed work. Borderline; arguably covered well enough today by the render cluster + cpu-gpu-offload-expert methodology.

- **Low priority: ImGui / debug overlays / Tracy.** The Tracy 100ns floor rule is discussed in many advisors but no one owns Tracy-as-a-tool. Tracy GUI usage, Tracy zone authoring conventions, the cost-split recon skill, RGP / RenderDoc workflows could be a single advisor or several. Currently distributed across render-perf-expert and cpu-gpu-offload-expert; arguably they have it covered.

## Methodology vs domain ambiguity

- **mc2-render-perf-expert.** Borderline. It encodes methodology (cull cascade safety, applyRenderStates invalidation contract, agent-isolation cleanup, citation drift discipline, sub-100ns floor) but also carries a heavy load of domain-specific bucket map and dependency graph that mutates session-to-session. Trade-off: keeping it as a hybrid means it has the most actionable answers for "what should we work on next" questions, but it also makes its core_knowledge a moving target that the user must keep up-to-date by hand. Alternative: split into mc2-perf-methodology (the contracts and discipline) + a lighter mc2-render-roadmap that holds just the bucket map and dependency graph, refreshed deliberately when slices land. User decision: is the hybrid more useful than the split?

- **amd-shader-reviewer.** Borderline between domain advisor and workflow skill. The file is short (48 lines), has no `<role>` / `<load_first>` / `<core_knowledge>` / `<known_pitfalls>` block structure, and reads more like a workflow checklist than a domain expert. It is functionally a skill that has been packaged as an agent. User decision: leave as-is (works fine), or move to `.claude/skills/amd-shader-review.md` with the rest of the workflow skills? Both are defensible.

## Skill candidates (flagged, not designed)

Patterns noticed during the audit that look like they want to be workflow skills rather than advisor agents:

- **mc2-parity-soak-check** - the recurring "is the slice ready for default-on flip" question. References `track_b_widen_static_prop_registry.md` 7-day soak precedent. Currently lives across cpu-gpu-offload-expert and render-perf-expert as prose.
- **mc2-tier1-smoke-triage** - the smoke runner produces artifacts; pattern of "look at the per-mission log, find the failure mode" repeats. Currently lives in CLAUDE.md and run_smoke.py.
- **mc2-render-doc-update** - the procedural counterpart to mc2-render-contract-synthesizer. The synthesizer is an agent; the "dump session render observations" prompt at `.claude/prompts/dump-render-observations.md` is the producer. The producer-to-synthesizer loop could be a skill.
- **mc2-killswitch-audit** - the 10-flag mech killswitch matrix and the broader env-var inventory could use a regular audit-and-document skill, given the post-flip semantics churn.

## Recommended merge plan (priority order)

This audit finds zero advisors that should be merged-and-deleted. The fleet is reasonably well-scoped. The action set is sharpening boundaries, not deletion. In priority order:

1. **Sharpen mc2-render-expert <-> mc2-render-perf-expert routing.** First priority because this is the only HIGH-severity overlap and the only place where a user query genuinely could hit either advisor with no clear winner.
   - Why first: HIGH-severity overlap; both advisors will load the same memory cluster on the same query and produce parallel partially-correct answers. Cheapest fix in the fleet.
   - What changes in the survivor (both survive): render-expert gets a cross_reference line and a `<limits>` line that explicitly defers "Tracy bucket interpretation, slice ROI sizing, dependency graph between in-flight slices, applyRenderStates invalidation contract" to render-perf-expert. render-perf-expert (already cross-references render-expert) gets a `<limits>` line that explicitly defers "queue/flush ordering, MLR exception, dual-queue debt, GPU-direct fast-path bring-up traps" to render-expert.
   - What changes in the deprecated advisor: neither is deprecated. Both survive.

2. **Add mc2-terrain-indirect-expert to mc2-render-expert's cross_references.** Currently render-expert does not list it. A user asking about cement/runway/decal rendering routes through render-expert's general fast-path traps before reaching the terrain-indirect advisor's deep knowledge.
   - Why second: LOW severity but very cheap fix; one-line edit.
   - What changes in render-expert: add terrain-indirect-expert to `<cross_references>` and to the `<limits>` "defer to" list. Add `MC2_TERRAIN_INDIRECT*` to the description triggers as a "see terrain-indirect-expert" hint.
   - What changes in terrain-indirect: no change (its cross-reference is already clean).

3. **Decide on amd-shader-reviewer's classification (agent vs skill).** User decision needed. If kept as agent, no change. If reclassified as skill, move file to `.claude/skills/amd-shader-review.md` (with the explicit no-leading-`mc2-` filename per the existing skill naming convention `/mc2-build` `/mc2-deploy` etc., or with `/mc2-amd-shader-review`).
   - Why third: ambiguity is benign today but creates classification confusion when more shader-review-style agents land.

4. **Fill the abl-scripting gap.** Highest-priority gap. Bootstrap an mc2-abl-expert that absorbs mission scripting, behavior trees, mission objectives, and the engine's ABL VM bindings.
   - Why fourth: this is a creation task not a merge task, and creates real new coverage. Lowest priority in the merge plan because it requires actually building the advisor (the meta-prompt at `.claude/prompts/distill-session-into-advisor-agent.md` is the harvesting tool). The other items are touch-up edits.

## Risks and caveats

- **mc2-render-perf-expert's bucket map is a moving target.** Lines like "Terrain::render drawPass ~1.7 ms (post-PR2c MINE retire)" carry a snapshot date (2026-05-08). Without regular refresh, this advisor's most useful content becomes a liability (it will route the user toward stale conclusions). Same problem as the render contract: the synthesizer pattern exists for render-contract but not for this advisor's perf-numbers section. User may want to either factor perf-numbers out into a dedicated memory file the advisor reads, or accept that this advisor needs deliberate maintenance after each load-bearing slice ship.

- **The user's first-cut domain list disagrees with the actual fleet on two axes:**
  - The first-cut list named "mechs" as one domain; the fleet has three separate mech advisors (import, anim, runtime-orchestration). This is the right call - the campaign artifacts confirm three distinct knowledge clusters with sharp boundaries. Recommend updating the user's mental model to match.
  - The first-cut list named "culling" as a possible domain; the fleet treats cull as cross-cutting (co-owned by render-expert and render-perf-expert via the shared memory cluster). This is also the right call - cull touches every actor type and every pipeline; isolating it into its own advisor would create more overlap, not less.

- **Healthy advisors that need no action:** mc2-build-system-expert, mc2-gameos-expert, mc2-mech-import-expert, mc2-mech-skeletal-anim-expert, mc2-mech-update-geometry-expert, mc2-mission-data-expert, mc2-render-contract-synthesizer, mc2-shader-expert, mc2-terrain-indirect-expert, mc2-cpu-gpu-offload-expert. Each is well-scoped, has clean cross-references, has substantial load-bearing tacit knowledge in its known_pitfalls block, and routes adjacent-domain questions correctly.

- **One structural pattern worth keeping in mind.** Every advisor except amd-shader-reviewer follows the same body template (`<role>` / `<load_first>` / `<core_knowledge>` / `<known_pitfalls>` / `<file_locations>` / `<work_protocol>` / `<limits>` / `<cross_references>`). This is a strength - it makes audit and routing tractable. Preserve it for any new advisor. amd-shader-reviewer's deviation from the template is consistent with its narrower task and is part of why it reads as skill-shaped.

- **No advisor currently owns the `docs/render-contract.md` document on the read side.** The synthesizer writes it; render-expert and render-perf-expert reference it as a "pending durable artifact" per CLAUDE.md. Once it lands, render-expert should add a load_first entry for it. The synthesizer's invocation cycle is the user's responsibility (manual trigger).
