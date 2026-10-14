<!-- doc-status: current | reviewed:2026-10-14 -->
# Gate amnesty wave 3 — audit only

Pinned worktree: `A:/Games/mc2-lane4`, branch `claude/gate-amnesty-3`, HEAD `ff726a83bb92fe73f78952690f6d1fba6a8da5f6`. No engine, gate, generated registry, script, test or data deletion/edit was performed. The requested branch already existed; no git index/ref write was needed.

## Decision boundary and arithmetic

The current registry has **980 total runtime gates and 100 Feature rows** (`docs/flags.auto.yaml:6`; Feature selected by `registry.kind`, not the Bool `kind`). Feature types/defaults: 56 Bool off, 31 Bool on, 5 String, 5 Float, 2 Enum and 1 Int. The target is **fewer than 30 Feature rows**, requiring **at least 71 removals (100 to 29)**. It is not a target of 29 total runtime gates.

**Cut applied:** inventory all 100; fully adjudicate the 12-gate lighting/light-upload family below; the other **88 rows are PROVISIONAL**, even where the prior wave supplies a KEEP or DELETE rationale. No provisional row counts as a verified DELETE. This fulfills the binding reduced audit scope; it does not claim 100 final rulings.

| Metric | Count |
|---|---:|
| Current Feature rows | 100 |
| Fully adjudicated lighting family | 12 |
| DELETE candidates after fresh source/consumer review | 11 |
| KEEP-LIVE in that family | 1 |
| DELETE without W2/VETO hold | 5 |
| DELETE carrying W2 delivery-consumer hold | 3 |
| DELETE carrying VETO alternative | 3 |
| DELETE with no lane-1 reader/consumer edits (still lane 2) | 3 |
| Completed deletions possible without any lane-1/2 file edit | **0** |
| Feature count if all 11 reviewed candidates are approved/removed | 89 |
| Additional removals still needed after those 11 | **60** |
| Additional removals still needed after the 5 unflagged candidates | **66** |

Sub-30 feasibility remains **unproven**: 88 rows still need arm/owner adjudication, and protected/live gates cannot be counted toward removals. Even the optimistic, conditional reviewed batches stop at 89. No achievable sub-30 schedule is promised.

**Protected/live shortfall scenario:** 29 current gates had KEEP-LIVE/KEEP-RULING in wave 1. Their union with current owner/arc holds and this family's live gate is **34 retained/held gates**. If all these holds survive re-adjudication, even deleting every other Feature would stop at 34, **5 above 29**; at least that many holds would need a new owner ruling or a proven settings/consolidation migration. This is a conservative conditional floor, not 34 freshly final KEEP verdicts; all 88 unreviewed rows remain provisional.

## Rules and owner dependencies

Apply wave 1 DELETE only when the alternative arm has no live game/editor, harness matrix, capture, smoke parser, launcher setting, script or data user. Retain the live arm as plain code. A name-only hit, expected log transcript, frozen seed row or TRACKED_FLAGS record is not an arm user; zero lexical getters is not deletion proof. A static harness exercising an algorithm is distinguished from a harness actually setting the engine gate.

FOLD applies to a Tri umbrella member only after reading its inheritance/veto semantics. No remaining Feature row is Tri, so this audit claims **0 FOLD Feature removals**. The 2026-10-09 expendable VK =0 ruling is specific to direct-chain debug overrides; it does not authorize deleting the VK umbrella, interop, ImGui or LOD-fade arms. Current Vulkan facility requirements also do not imply those presentation/content gates are dead.

Preserve KEEP-RULING (night ambient floor, cliff tessellation, GPU particles and parity substrate, step-4 section-2 readers), KEEP-LIVE settings, W2 and every VETO alternative. The original 2026-10-09 rulings are at `gate-deletion-audit.md:119-125`; wave-1 rules at `gate-amnesty-wave1.md:6-15`. Owner endpoint/editor/cinematics/malformed-content/static-owner rules remain in force from ORCHESTRATOR-HANDOFF and the newest 20 HANDOFF-LOG entries read this session.

**Brain family hold:** BRAIN_DISPATCH, BRAIN_RUNTIME and BRAIN_RUNTIME_APPLY are provisional HOLD-L1, along with their non-Feature apply/call/var/FSM/taskq/force-mode companions. Current lane-1 brain/ABL/patrol work owns the decision; do not dismantle it because its shipping defaults are off. Bind to `Gates/gate_table.cpp:95`, `code/brain_special_dispatch.cpp:157`, `code/mission_init_load.cpp:338,473`, `code/warrior.cpp:44,48,1438` and the 2026-10-13/14 Hybrid owner rulings. No brain deletion batch is proposed.

Owners used here: **L1** = code/, mclib/, tests/unit/, tests/analyze/, tools/save* and tools/campaign_save_roundtrip.py. **L2** = GameOS/, Gates/, RenderCore/, shaders/, tools/visual_verify/, tools/mc2_launcher/, scripts/run_visual_capture.py, scripts/smoke_lib/capture_backend.py, scripts/smoke_lib/render_resolution.py and tests/smoke/test_capture_backend.py. **Sol/unowned** = remaining files, including GuiRuntime/, RenderWorld/, editor/, other scripts/tools. This follows the current brief, not the broader historical wave-1 lane scope.

**Every deletion has mandatory L2 edits:** `Gates/gate_table.cpp`, compiler-emitted `Gates/gate_table.json`, generated `Gates/gate_index.gen.h`, and generated `RenderCore/RendererFeatureRegistry.h`; Sol/unowned `docs/flags.auto.yaml` follows too (`tools/gate_gen.py:4-13`). The manual positional RendererFeature enum must be renumbered in the same L2 change where applicable, never tombstoned. Generic enum loops/COUNT tests need revalidation; name/index fixtures may need L1 edits. Frozen `tools/gate_table.seed.json` is provenance, not a runtime setter. No generated artifacts were regenerated here.

## Fresh scan method and limits

The read-only collector unions `git ls-files -z` with `rg --files --hidden` and normalizes Windows separators. It scanned **4381 text files**, including untracked visible source/config files, with per-file SHA-256 evidence. Exclusions: .git/, .scratch/, build* trees, 3rdparty/, graphify output and .claude/l2cap capture artifacts. Binary files are excluded after NUL detection. This audit covers repository text setters/configuration; external user environments and packed FST/mod-install payloads were not exhaustively decoded, so it makes no claim of absence there.

It scans executable C/C++ with comments blanked, retaining newlines, for get/isSet/isNonEmpty/str/is/state/hasParent/armed/effective/regionTier1Armed, multiline templates and rule/token arguments; **165 typed call sites**, **0 direct raw getenv-family call sites** for the 100 Feature names. All source mentions, featureId aliases, positional enum uses, shader macro/conditional references, scripts, tests, data and root launchers are also indexed. Raw-looking matches in terrain/launcher comments are not executable readers.

`regionTier1Armed` and `armed<...>` have no current executable occurrence in the scanned sources; searches for nonliteral gate template arguments also found none. They were searched explicitly rather than assumed obsolete. featureId aliases are registry identifiers, not automatically environment aliases. Row aliases and enum uses are listed in the inventory; all unclassified users outside the selected family remain provisional.

**Indirect readers:** `Gates/gate.cpp:198` resolves every row by runtime name; `:360` envText is a general raw-env escape; `:362` boolState is a dynamic reader. Positional Feature rows also reach `RenderCore/RendererFeatureRegistry.h:1717-1726` [L2] and `GuiRuntime/EditorInspector_sections.cpp:33-69` [Sol/unowned] by table iteration; the latter is a read-only display, not a setter. `tests/unit/test_rendercore.cpp:330-338` [L1] validates the enum/table generically. These common consumers are dependencies/revalidation work even without a per-gate lexical hit. Auxiliary Feature rows with order -1 are not falsely assigned to the positional display loop.

**Conditional builds:** the selected family readers are in the ordinary GameOS/mclib source lists (`GameOS/gameos/CMakeLists.txt:14,17,77,80`; `mclib/CMakeLists.txt:40,43,99,131,145`). Their getter declarations are not in platform/VK conditional arms. Adjacent MC2_VULKAN_ISLAND blocks forward the same light data; GL paths still count as live while VK parity remains open. Shared uniforms, shader branches, pure-row VERIFY and independently gated LIGHTBAKE are explicitly retained in the family rulings. Other gates carry conditional/name evidence below and await full build-arm adjudication.

For every DELETE, fresh evidence includes its live/dead branch, setter values (or no family setter), downstream helper/constant/uniform readers, registry/enum consumers, all root launcher/config hits and current prior/owner flags. The consumer appendix deliberately includes non-setter hits so reviewers can distinguish schema/default pins from users. Outside the selected family, value expressions are reported without treating their role as a final verdict.

## Complete Feature inventory (100 rows)

One row per current Feature. Type/rule/default are the typed table values; alias is registry.featureId; `P` means positional registry display plus the common indirect readers above, `A` means auxiliary. Reader locations include owners. E001–E100 link to every scripts/tests/tools/data/root-launcher occurrence and its source value/context below. The last column is a conservative source/consumer dependency classification; **L2 is always required**, and L1 consumer presence outside the fully reviewed family is provisional edit scope.

| Gate | Kind/rule/default; alias; registry | Every typed reader [owner] | Setter/consumer evidence | Verdict and preserved flag | Complete deletion dependency |
|---|---|---|---|---|---|
| `MC2_ABL_COMPILE_FATAL` | Bool/Lead0Kill/true; MC2_ABL_COMPILE_FATAL; A | `mclib/ablrtn.cpp:816` [L1] | [E001](#e001); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_ASSIMP_MECH_IMPORT` | Bool/Presence/false; MC2_FEATURE_ASSIMP_MECH_IMPORT; A | `mclib/mech3d_type.cpp:99` [L1] | [E002](#e002); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: scripts/mech_shot.py sets it (BT2018 mech import arc) | L2 + L1 + Sol/unowned |
| `MC2_BRAIN_DISPATCH` | Bool/AtoiNZ/false; MC2_FEATURE_BRAIN_DISPATCH; A | `code/brain_special_dispatch.cpp:157,2650` [L1]<br>`code/mission_init_load.cpp:473` [L1]<br>`code/warrior.cpp:1438` [L1] | [E003](#e003); registry/enum refs below | PROVISIONAL; HOLD-L1: current special-dispatch/ABL work and explicit Legacy/Enhanced/Hybrid ownership; Gates/gate_table.cpp:95 and code/brain_special_dispatch.cpp:157. Audit entire companion family with lane 1 before a verdict. | L2 + L1 + Sol/unowned |
| `MC2_BRAIN_RUNTIME` | Bool/AtoiNZ/false; MC2_FEATURE_BRAIN_RUNTIME; A | `code/mission_init_load.cpp:338` [L1]<br>`code/warrior.cpp:44` [L1] | [E004](#e004); registry/enum refs below | PROVISIONAL; HOLD-L1: current brain runtime/patrol/hybrid work; owner 2026-10-13 18:33 and 2026-10-14 07:21. No deletion scheduled. | L2 + L1 + Sol/unowned |
| `MC2_BRAIN_RUNTIME_APPLY` | Bool/AtoiNZ/false; MC2_FEATURE_BRAIN_RUNTIME_APPLY; A | `code/warrior.cpp:48` [L1] | [E005](#e005); registry/enum refs below | PROVISIONAL; HOLD-L1: application/arbitration companion of BRAIN_RUNTIME; same owner hold. | L2 + L1 |
| `MC2_BURNIN_NO_JPG` | Bool/Presence/false; MC2_FEATURE_BURNIN_NO_JPG; A | `mclib/terrtxm2_internal.h:72` [L1] | [E006](#e006); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_CALLED_SHOT_FIX` | Bool/Lead0P/false; MC2_FEATURE_CALLED_SHOT_FIX; A | `code/gvehicl_weapons.cpp:664` [L1]<br>`code/mech_weapons.cpp:308` [L1]<br>`code/mover_attack.cpp:193` [L1] | [E007](#e007); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_COLORMAP_CPU_RETIRE` | Bool/Lead0Kill/true; MC2_FEATURE_COLORMAP_CPU_RETIRE; P | `GameOS/gameos/gos_terrain_indirect_cement.cpp:992` [L2] | [E008](#e008); registry/enum refs below | PROVISIONAL; prior DELETE: default-on CPU colormap free; =0 has no user | L2 + Sol/unowned |
| `MC2_COLORMAP_KTX2` | Bool/Lead0Kill/true; MC2_FEATURE_COLORMAP_KTX2; P | `mclib/terrtxm2_internal.h:166` [L1] | [E009](#e009); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_CONE_POOL_POLICY` | Bool/Lead0Kill/true; MC2_FEATURE_CONE_POOL_POLICY; A | `mclib/cone_glow.cpp:171` [L1] | [E010](#e010); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_DEBUG_RENDERER` | Bool/Lead0/false; MC2_FEATURE_DEBUG_RENDERER; P | `GameOS/gameos/debug_renderer.cpp:33` [L2]<br>`GuiRuntime/EditorInspector_sections.cpp:850` [Sol/unowned] | [E011](#e011); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/viewmode.json tactical entries; GuiRuntime inspector toggle | L2 + Sol/unowned |
| `MC2_DETERMINISTIC_RNG` | Bool/Lead0/false; MC2_DETERMINISTIC_RNG; A | `GameOS/gameos/gameos.cpp:296` [L2]<br>`code/mechcmd2.cpp:372` [L1]<br>`code/mission_init.cpp:80` [L1]<br>`mclib/routines.cpp:90` [L1]<br>`tools/vfx_oracle/lifecycle_platform.cpp:16` [Sol/unowned]<br>`tools/vfx_oracle/lifecycle_test.cpp:55` [Sol/unowned] | [E012](#e012); registry/enum refs below | PROVISIONAL; KEEP-RULING: step-4 reader sites and required sim determinism; current oracle tools also set =1. | L2 + L1 + Sol/unowned |
| `MC2_DIRECT_FIRE_STRAIGHT` | Bool/Exact1/false; MC2_DIRECT_FIRE_STRAIGHT; A | `code/weaponbolt_update.cpp:20` [L1] | [E013](#e013); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_DYNAMIC_DECALS` | Bool/Lead0P/false; MC2_FEATURE_DYNAMIC_DECALS; A | `GuiRuntime/GraphicsOptionsWindow_terrain.cpp:402` [Sol/unowned]<br>`mclib/dynamic_decal_ring.cpp:43` [L1] | [E014](#e014); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_FAR_BACKDROP_LIT` | Bool/Lead0/false; MC2_FAR_BACKDROP_LIT; P | `GameOS/gameos/gos_static_prop_batcher_state.cpp:213` [L2] | [E015](#e015); registry/enum refs below | PROVISIONAL; prior DELETE: default-off far-LOD night shade; no setter; VETO-NIGHT: ALT promote (VK gap #1 night) | L2 |
| `MC2_FRAMEGRAPH_EXECUTOR` | Bool/Lead1/false; MC2_FEATURE_FRAMEGRAPH_EXECUTOR; A | `GameOS/gameos/gos_postprocess_executor.cpp:80` [L2]<br>`mclib/render_contract.cpp:1393` [L1] | [E016](#e016); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: scripts/verify_executor_slice.py sets it; run_smoke --executor flags; MCP render-state | L2 + L1 + Sol/unowned |
| `MC2_FX_COST_SPLIT` | Bool/Exact1/false; MC2_FX_COST_SPLIT; A | `mclib/fx_trace/fx_cost_split.cpp:33` [L1] | [E017](#e017); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_FX_DEFS` | Bool/Presence/false; MC2_FX_DEFS; A | `code/mechcmd2.cpp:1054` [L1] | [E018](#e018); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_FX_FORCE_SPAWN` | Enum/Std/""; MC2_FX_FORCE_SPAWN; A | `code/warrior.cpp:2083,2089,2090,2091,2092,2093` [L1] | [E019](#e019); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_GLSTATEGUARD_TERRAIN` | Bool/DefOnAtoiNZ/true; MC2_GLSTATEGUARD_TERRAIN; A | `GameOS/gameos/gos_terrain_lod_chunk_gates.cpp:12` [L2] | [E020](#e020); registry/enum refs below | PROVISIONAL; prior DELETE: default-on RAII state guard kill-switch; =0 has no user | L2 |
| `MC2_GPUBUF_LIGHT_GROWONCE` | Bool/Presence/false; MC2_GPUBUF_LIGHT_GROWONCE; A | `GameOS/gameos/gameos_graphics_light_ssbo.cpp:23` [L2] | [E021](#e021); registry/enum refs below | DELETE; VETO-ARCH | L2 |
| `MC2_GPUBUF_RING` | Bool/Presence/false; MC2_GPUBUF_RING; A | `GameOS/gameos/gameos_graphics_draw.cpp:21` [L2] | [E022](#e022); registry/enum refs below | PROVISIONAL; prior DELETE: default-off HUD ring buffer; no setter; VETO-ARCH: GPU-BUFFER-WRAPPER tier 0; ALT promote | L2 |
| `MC2_GPU_PARTICLES` | Bool/Lead1/false; MC2_GPU_PARTICLES; A | `mclib/gosfx/effect.cpp:44` [L1]<br>`mclib/particles/batcher.cpp:78` [L1] | [E023](#e023); registry/enum refs below | PROVISIONAL; KEEP-RULING: MLR remains live until GPU-particle parity; no deletion of its replacement substrate. | L2 + L1 + Sol/unowned |
| `MC2_HUD_IMGUI_TABS` | Bool/Lead0/false; MC2_FEATURE_HUD_IMGUI_TABS; A | `code/controlgui_hudtabs.cpp:11` [L1] | [E024](#e024); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_HUD_READABILITY_V1` | Bool/Lead0/false; MC2_FEATURE_HUD_READABILITY_V1; A | `code/controlgui_vehicle.cpp:149` [L1]<br>`gui/afont.cpp:32` [Sol/unowned] | [E025](#e025); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_HUD_TABS_TEST` | Bool/Lead0/false; MC2_FEATURE_HUD_TABS_TEST; A | `code/controlgui_hudtabs.cpp:63` [L1] | [E026](#e026); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_HZB_BUILD` | Bool/Lead0/false; MC2_FEATURE_HZB_BUILD; P | `GameOS/gameos/gos_postprocess.cpp:284` [L2] | [E027](#e027); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/hzb.json, strict runner, run_golden_parity.py | L2 + Sol/unowned |
| `MC2_HZB_PROBE` | Bool/Lead0/false; MC2_FEATURE_HZB_PROBE; P | `GameOS/gameos/gos_postprocess.cpp:556` [L2] | [E028](#e028); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/hzb.json sets 1 | L2 + Sol/unowned |
| `MC2_ICON_FAILLOUD` | Bool/Lead0/false; MC2_FEATURE_ICON_FAILLOUD; A | `code/mechicon.cpp:45` [L1] | [E029](#e029); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_IMGUI` | Bool/Lead0Kill/true; MC2_FEATURE_IMGUI; P | `GuiRuntime/GuiRuntime.cpp:40` [Sol/unowned] | [E030](#e030); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + Sol/unowned |
| `MC2_IMGUI_INSPECTOR` | Bool/Lead0Kill/true; MC2_FEATURE_IMGUI_INSPECTOR; P | `GuiRuntime/EditorInspector.cpp:29` [Sol/unowned] | [E031](#e031); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + Sol/unowned |
| `MC2_LEGACY_LIT_MC2_FRAME` | Bool/Lead0Kill/true; MC2_LEGACY_LIT_MC2_FRAME; P | `mclib/txmmgr_render_phases.cpp:112` [L1] | [E032](#e032); registry/enum refs below | DELETE; W2 | L2 + L1 + Sol/unowned |
| `MC2_LETTERBOX_OPAQUE` | Bool/Lead0Kill/true; MC2_FEATURE_LETTERBOX_OPAQUE; A | `code/gamecam.cpp:506` [L1] | [E033](#e033); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_LIGHTBAKE_REBAKE_ON_READY` | Bool/Lead0Kill/true; MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY; A | `mclib/bdactor_lightbake.cpp:209` [L1] | [E034](#e034); registry/enum refs below | DELETE | L2 + L1 |
| `MC2_LIGHT_GRID` | Bool/Lead0Kill/true; MC2_FEATURE_LIGHT_GRID; A | `GameOS/gameos/gos_terrain_lighting.cpp:260` [L2] | [E035](#e035); registry/enum refs below | KEEP-LIVE | L2 + Sol/unowned |
| `MC2_LIGHT_POOL_COMPRESS` | Bool/Lead0Kill/true; MC2_FEATURE_LIGHT_POOL_COMPRESS; A | `GameOS/gameos/gameos_graphics_overlay.cpp:397` [L2]<br>`GameOS/gameos/gos_terrain_lighting.cpp:655` [L2]<br>`GameOS/gameos/gos_terrain_lod_chunk_gates.cpp:51` [L2] | [E036](#e036); registry/enum refs below | DELETE | L2 |
| `MC2_LIGHT_POS_COLUMN_FIX` | Bool/Lead0Kill/true; MC2_LIGHT_POS_COLUMN_FIX; P | `mclib/txmmgr_lightrow.cpp:51` [L1] | [E037](#e037); registry/enum refs below | DELETE | L2 + L1 |
| `MC2_LIGHT_PREFIX_GPU_COPY` | Bool/Lead0P/false; MC2_LIGHT_PREFIX_GPU_COPY; A | `GameOS/gameos/gameos_graphics_light_ssbo.cpp:139` [L2] | [E038](#e038); registry/enum refs below | DELETE; VETO-PERF | L2 |
| `MC2_LIGHT_REALISM` | Bool/Lead0Kill/true; MC2_FEATURE_LIGHT_REALISM; A | `GameOS/gameos/gos_terrain_lighting.cpp:1206` [L2]<br>`mclib/cone_glow.cpp:352` [L1] | [E039](#e039); registry/enum refs below | DELETE | L2 + L1 + Sol/unowned |
| `MC2_LIGHT_ROW_PURE` | Bool/Exact1/false; MC2_LIGHT_ROW_PURE; P | `mclib/txmmgr_lightrow.cpp:74` [L1] | [E040](#e040); registry/enum refs below | DELETE; VETO-ARCH | L2 + L1 |
| `MC2_MATERIAL_GPU` | Bool/Lead0Kill/true; MC2_FEATURE_MATERIAL_GPU; P | `GameOS/gameos/debug_state_dump.cpp:475` [L2]<br>`GameOS/gameos/gameosmain_init.cpp:224` [L2]<br>`GameOS/gameos/gos_materials.cpp:48` [L2]<br>`GameOS/gameos/gos_mech_batcher.cpp:452` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_state.cpp:186` [L2] | [E041](#e041); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/staticprop.json sets 0 | L2 + Sol/unowned |
| `MC2_MATERIAL_GPU_SAMPLE` | Bool/Lead0Kill/true; MC2_FEATURE_MATERIAL_GPU_SAMPLE; P | `GameOS/gameos/debug_state_dump.cpp:476` [L2]<br>`GameOS/gameos/gameosmain_init.cpp:225` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_state.cpp:192` [L2] | [E042](#e042); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/staticprop.json sets 0 | L2 + Sol/unowned |
| `MC2_MATERIAL_KTX` | Bool/Lead0P/false; MC2_FEATURE_MATERIAL_KTX; P | `GameOS/gameos/gos_static_prop_batcher_state.cpp:606` [L2] | [E043](#e043); registry/enum refs below | PROVISIONAL; prior DELETE: default-off static-prop KTX2 arrays; no setter; no static-prop .ktx2 arrays in data/ (3 ktx2 files: hdr + 2 pbr maps); VETO-FEATURE: KTX loader path for static props (RenderCore/KtxLoader stays for COLORMAP_KTX2/TEXMGR) | L2 |
| `MC2_MOVE_RECON` | Bool/Lead0/false; MC2_FEATURE_MOVE_RECON; A | `mclib/move_recon.cpp:283` [L1] | [E044](#e044); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_NIGHT_FOG_DIM` | Bool/Exact1/false; MC2_NIGHT_FOG_DIM; P | `GameOS/gameos/gos_postprocess.cpp:514` [L2] | [E045](#e045); registry/enum refs below | PROVISIONAL; prior DELETE: default-off night horizon dim; no setter; VETO-NIGHT: ALT promote (mc2_10 white horizon band, VK night gap) | L2 + Sol/unowned |
| `MC2_OBJECT_ID_BUFFER` | Bool/DefOnLead0/true; MC2_FEATURE_OBJECT_ID_BUFFER; P | `RenderWorld/RenderWorld.cpp:70` [Sol/unowned] | [E046](#e046); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/viewmode.json sets 1 and 0; shader variant macro | L2 + Sol/unowned |
| `MC2_OVERLAY_CEMENT_ADOPT` | Bool/Lead0Kill/true; MC2_FEATURE_OVERLAY_CEMENT_ADOPT; P | `GameOS/gameos/gameos_graphics_overlay.cpp:328,432` [L2]<br>`GameOS/gameos/gos_terrain_indirect_cement.cpp:236` [L2] | [E047](#e047); registry/enum refs below | PROVISIONAL; prior DELETE: default-on; =0 has no user; W2: delivery ledger consumer | L2 + Sol/unowned |
| `MC2_OVERLAY_TILE_HIRES` | String/Std/""; MC2_OVERLAY_TILE_HIRES; A | `mclib/terrtxm.cpp:70` [L1] | [E048](#e048); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_PAVED_OVERLAY_CLASS` | Int/Prefix/0; MC2_FEATURE_PAVED_OVERLAY_CLASS; A | `GameOS/gameos/gos_terrain_indirect_cement.cpp:288` [L2] | [E049](#e049); registry/enum refs below | PROVISIONAL; prior DELETE: retracted CONCRETE-DELIVER-2 class, default 0 (OFF); no setter; W2: delivery ledger consumer | L2 + Sol/unowned |
| `MC2_PROJECTILE_SPEED_MULT` | Float/Prefix/1; MC2_PROJECTILE_SPEED_MULT; A | `code/weaponbolt_update.cpp:25` [L1] | [E050](#e050); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_RENDER_BACKEND` | String/Std/""; MC2_FEATURE_RENDER_BACKEND; A | `GameOS/gameos/render_backend_select.cpp:33` [L2] | [E051](#e051); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: backend selector; strict runner, field check, capture tools | L2 + Sol/unowned |
| `MC2_RENDER_BACKEND_IFACE` | Bool/Lead1/false; MC2_FEATURE_RENDER_BACKEND_IFACE; A | `GameOS/gameos/gos_postprocess.cpp:932` [L2]<br>`GameOS/gameos/gos_postprocess_effects.cpp:785` [L2] | [E052](#e052); registry/enum refs below | PROVISIONAL; prior DELETE: default-off backend-iface routing; no setter (golden_scene.py only records); VETO-ARCH: RENDER-BACKEND-SEAMS arc seam; ALT promote | L2 |
| `MC2_RENDER_PASS_ORDER` | Bool/Lead1/false; MC2_FEATURE_RENDER_PASS_ORDER; A | `mclib/render_contract.cpp:902` [L1] | [E053](#e053); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_SETUPTEXTURES_LEGACY_FORCE` | Bool/Presence/false; MC2_FEATURE_SETUPTEXTURES_LEGACY_FORCE; P | `mclib/quad_setup.cpp:309` [L1]<br>`mclib/terrtxm2_internal.h:157` [L1] | [E054](#e054); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_SHADER_SPIRV` | Bool/Lead1/false; MC2_SHADER_SPIRV; A | `GameOS/gameos/utils/shader_builder.cpp:425` [L2] | [E055](#e055); registry/enum refs below | PROVISIONAL; prior DELETE: default-off GL SPIR-V consumer pilot; no setter; VETO-ARCH: SPIRV-CONSUMER-PILOT (shaders/spv GL consumer); VK uses spv natively | L2 + Sol/unowned |
| `MC2_SHADOW_CASTER_CULL_CACHE` | Bool/Lead0P/false; MC2_SHADOW_CASTER_CULL_CACHE; A | `mclib/txmmgr_render_passes.cpp:162` [L1] | [E056](#e056); registry/enum refs below | PROVISIONAL; prior DELETE: default-off perf cache; no setter; VETO-PERF: S5 perf item; ALT measure+promote | L2 + L1 |
| `MC2_SHADOW_DYNAMIC_PROP_CASTERS` | Enum/Std/"1"; MC2_FEATURE_SHADOW_DYNAMIC_PROP_CASTERS; A | `GameOS/gameos/gos_static_prop_batcher_shadow.cpp:667` [L2]<br>`mclib/txmmgr_render_passes.cpp:103` [L1] | [E057](#e057); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/shadow.json sets 0 | L2 + L1 + Sol/unowned |
| `MC2_SHADOW_ENABLE` | Bool/Lead0Kill/true; MC2_FEATURE_SHADOW_MAPS; P | `GameOS/gameos/gos_mech_batcher.cpp:927` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_shadow.cpp:174` [L2]<br>`GameOS/gameos/vulkan_post_screenfx_native.cpp:727` [L2] | [E058](#e058); registry/enum refs below | PROVISIONAL; HOLD: parked G0 owner decision; VK direct CSM/default-ON and parity precede consolidation. | L2 + Sol/unowned |
| `MC2_SHADOW_GUARD_CLAMPED_SUN` | Bool/Lead0Kill/true; MC2_FEATURE_SHADOW_GUARD_CLAMPED_SUN; A | `GameOS/gameos/gos_postprocess_gates.cpp:321` [L2] | [E059](#e059); registry/enum refs below | PROVISIONAL; prior DELETE: default-on; =0 has no user | L2 |
| `MC2_SMART_LOAD` | Bool/Lead0Kill/true; MC2_FEATURE_SMART_LOAD; A | `code/logisticsdata_init.cpp:15` [L1] | [E060](#e060); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 |
| `MC2_SNAPSHOT_MECH_EXTRACT` | Bool/Lead0/false; MC2_FEATURE_SNAPSHOT_MECH_EXTRACT; P | `GameOS/gameos/debug_state_dump.cpp:588` [L2]<br>`GameOS/gameos/gos_mech_batcher_flush.cpp:1173` [L2]<br>`GameOS/gameos/render_snapshot.cpp:116` [L2] | [E061](#e061); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: observe-only extraction feeding the debug_state mech inventory sidecar (capture_baseline MECH-BASELINE-0, MCP get_render_state); VETO-ASK: no script sets it; ALT promote to always-on observe | L2 |
| `MC2_SSAO` | Bool/Lead0/false; MC2_FEATURE_SSAO; P | `GameOS/gameos/gos_postprocess.cpp:431` [L2] | [E062](#e062); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/ssao.json, strict runner, run_golden_parity.py | L2 + L1 + Sol/unowned |
| `MC2_STATICPROP_BC7` | Bool/Lead0P/false; MC2_FEATURE_STATICPROP_BC7; P | `GameOS/gameos/gos_static_prop_batcher_state.cpp:622` [L2] | [E063](#e063); registry/enum refs below | PROVISIONAL; prior DELETE: requires MATERIAL_KTX; no setter; VETO-FEATURE | L2 |
| `MC2_STATICPROP_MATERIAL_PBR_SLOTS` | Bool/Lead0/false; MC2_FEATURE_STATICPROP_MATERIAL_PBR_SLOTS; A | `GameOS/gameos/gos_static_prop_batcher_state.cpp:323` [L2] | [E064](#e064); registry/enum refs below | PROVISIONAL; HOLD: owner 2026-10-13 says PBR OFF; absence of a setter is not permission to promote.; VETO-FEATURE: PBR arc substrate | L2 + Sol/unowned |
| `MC2_STATIC_LIGHT_UPLOAD_SPLIT` | Bool/Lead0Kill/true; MC2_FEATURE_STATIC_LIGHT_UPLOAD_SPLIT; A | `mclib/txmmgr_lights.cpp:345` [L1] | [E065](#e065); registry/enum refs below | DELETE | L2 + L1 |
| `MC2_TACTICAL_ARC_OVERLAY` | Bool/Lead0/false; MC2_FEATURE_TACTICAL_ARC_OVERLAY; A | `code/gamecam.cpp:423` [L1] | [E066](#e066); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_AMBIENT_FLOOR` | Float/Prefix/0.349999994; MC2_FEATURE_TERRAIN_AMBIENT_FLOOR; A | `GameOS/gameos/gos_terrain_lod_chunk_upload.cpp:637,640` [L2]<br>`tests/unit/test_gate.cpp:835,836,837,840,841,844` [L1] | [E067](#e067); registry/enum refs below | PROVISIONAL; prior DELETE: day ambient floor default 0.35 (the NIGHT floor stays by ruling and reads max(dayGate, floor)); no setter; W2: consumer-heavy (delivery ledger + test fixture) | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT` | Float/Prefix/0; MC2_FEATURE_TERRAIN_AMBIENT_FLOOR_NIGHT; A | `GameOS/gameos/gos_terrain_lod_chunk_upload.cpp:644,647` [L2] | [E068](#e068); registry/enum refs below | PROVISIONAL; KEEP-RULING: 2026-10-09 night-parity lever stays. | L2 |
| `MC2_TERRAIN_CLIFF_DISPLACE` | Bool/Lead0/false; MC2_FEATURE_TERRAIN_CLIFF_DISPLACE; A | `GameOS/gameos/gos_terrain_lod_chunk.cpp:21` [L2] | [E069](#e069); registry/enum refs below | PROVISIONAL; KEEP-RULING: 2026-10-09 cliff hardware tessellation and MAXTESS/shaders stay. | L2 |
| `MC2_TERRAIN_CONTROLMAP` | Bool/Lead0/false; MC2_TERRAIN_CONTROLMAP; A | `mclib/terrain_init.cpp:270` [L1] | [E070](#e070); registry/enum refs below | PROVISIONAL; prior DELETE: default-off authored control-map sidecar; no setter; 24 cooked control_map.png sidecars are deployed by the beauty install gate and never read; VETO-CONTENT: 24 cooked sidecars exist (dead content while OFF); ALT promote default-ON after captures | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_CONTROLMAP_FILE` | String/Std/""; MC2_TERRAIN_CONTROLMAP_FILE; A | `mclib/terrain_init.cpp:275,277` [L1] | [E071](#e071); registry/enum refs below | PROVISIONAL; prior DELETE: path override of CONTROLMAP | L2 + L1 |
| `MC2_TERRAIN_LOD_CHUNK` | Bool/Lead0Kill/true; MC2_FEATURE_TERRAIN_LOD_CHUNK; A | `mclib/terrain.cpp:64` [L1] | [E072](#e072); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: launcher row with offVal 0; run_smoke / check-env-drive-guard A/B arm | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_MATERIAL_TEXTURES` | Bool/Lead0/false; MC2_FEATURE_TERRAIN_MATERIAL_TEXTURES; A | `GameOS/gameos/gos_terrain_lod_chunk_gates.cpp:74` [L2]<br>`GameOS/gameos/terrain_material_lib.cpp:186,221` [L2] | [E073](#e073); registry/enum refs below | PROVISIONAL; prior DELETE: default-off 6-layer BC7 albedo array; no setter (deploy_payload.py comment names the loader); VETO-FEATURE: TERRAIN-MATERIAL-TEXTURES-1 (terrain_material_lib.cpp + data/terrain_materials.json); VK gap #9 macro | L2 + Sol/unowned |
| `MC2_TERRAIN_OVERLAY_V2` | Bool/Lead0/false; MC2_TERRAIN_OVERLAY_V2; A | `mclib/terrain_init.cpp:367` [L1] | [E074](#e074); registry/enum refs below | PROVISIONAL; prior DELETE: default-off overlay_v2 sidecar compositing; no setter; 1 cooked overlay_v2.png exists; VETO-CONTENT: ALT promote (runway/road parked item) | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS` | Bool/Lead0Kill/true; MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS; A | `GameOS/gameos/gos_terrain_indirect_mine_decal.cpp:664` [L2] | [E075](#e075); registry/enum refs below | PROVISIONAL; prior DELETE: default-on suppression that is inert while OVERLAY_V2 is off; =0 has no user; goes with OVERLAY_V2 | L2 |
| `MC2_TERRAIN_OVERLAY_V2_FILE` | String/Std/""; MC2_TERRAIN_OVERLAY_V2_FILE; A | `mclib/terrain_init.cpp:371,373` [L1] | [E076](#e076); registry/enum refs below | PROVISIONAL; prior DELETE: path override of OVERLAY_V2 | L2 + L1 |
| `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` | Bool/Lead0Kill/true; MC2_FEATURE_TERRAIN_SHADOW_GUARD_CLAMPED_SUN; A | `GameOS/gameos/gameos_graphics.cpp:101` [L2] | [E077](#e077); registry/enum refs below | PROVISIONAL; prior DELETE: default-on terrain twin; =0 has no user | L2 |
| `MC2_TERRAIN_TESS` | Bool/Lead0Kill/true; MC2_FEATURE_TERRAIN_TESSELLATION; P | `GameOS/gameos/gameos_graphics.cpp:318` [L2] | [E078](#e078); registry/enum refs below | PROVISIONAL; HOLD: do not conflate general terrain tessellation with the specifically protected cliff arm; old KEEP-LIVE must be rechecked.; W2: consumer-heavy, positional enum 3 | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE` | Bool/Lead0Kill/true; MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE; A | `GameOS/gameos/gos_terrain_lod_chunk_draw.cpp:171` [L2]<br>`GameOS/gameos/gos_terrain_lod_chunk_upload.cpp:146` [L2]<br>`mclib/terrain_init.cpp:919` [L1] | [E079](#e079); registry/enum refs below | PROVISIONAL; prior DELETE: default-on object grounding fade (3 readers); =0 has no user | L2 + L1 + Sol/unowned |
| `MC2_TERRAIN_VISUAL_HEIGHT` | Bool/Lead0/false; MC2_FEATURE_TERRAIN_VISUAL_HEIGHT; A | `mclib/terrain_init.cpp:708` [L1] | [E080](#e080); registry/enum refs below | PROVISIONAL; prior DELETE: load-only gate ORed with MC2_TERRAIN_VISUAL_DISPLACE (displace implies load); the standalone =1 has no user; VETO-CONTENT: check MC2_TERRAIN_VISUAL_DISPLACE default before deleting the OR term | L2 + L1 |
| `MC2_UNIT_PROFILE_DATA` | Bool/Lead0/false; MC2_FEATURE_UNIT_PROFILE_DATA; A | `GameOS/gameos/debug_state_dump.cpp:712,748,780` [L2]<br>`code/unitprofile.cpp:9` [L1] | [E081](#e081); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: launcher SetEnvironmentVariableA; tools/unit_converter readers | L2 + L1 + Sol/unowned |
| `MC2_VEHICLE_AMBIENT_FLOOR` | Bool/Lead0Kill/true; MC2_VEHICLE_AMBIENT_FLOOR; P | `mclib/txmmgr_render_phases.cpp:168` [L1] | [E082](#e082); registry/enum refs below | DELETE; W2 | L2 + L1 + Sol/unowned |
| `MC2_VEHICLE_LIT_PATH` | Bool/Lead0Kill/true; MC2_VEHICLE_LIT_PATH; P | `mclib/gvactor_render.cpp:38` [L1] | [E083](#e083); registry/enum refs below | DELETE; W2 | L2 + L1 + Sol/unowned |
| `MC2_VFX_AGE_SAMPLE` | Bool/Lead1/false; MC2_FEATURE_VFX_AGE_SAMPLE; P | `mclib/particles/spawn.cpp:36` [L1] | [E084](#e084); registry/enum refs below | PROVISIONAL; KEEP-RULING: GPU-particle parity substrate; ownership/oracle arc still live.; VETO-VFX: no setter today; delete here only if item 13 does not want the substrate | L2 + L1 |
| `MC2_VFX_GPU_SIM_CARDCLOUD` | Bool/Lead1/false; MC2_FEATURE_VFX_GPU_SIM_CARDCLOUD; P | `mclib/particles/batcher.cpp:132` [L1] | [E085](#e085); registry/enum refs below | PROVISIONAL; KEEP-RULING: GPU-particle parity substrate; ownership/oracle arc still live.; VETO-VFX: no setter today; delete here only if item 13 does not want the substrate | L2 + L1 + Sol/unowned |
| `MC2_VFX_GPU_SIM_COMPARE` | Bool/Lead1/false; MC2_FEATURE_VFX_GPU_SIM_COMPARE; P | `mclib/particles/batcher.cpp:142` [L1] | [E086](#e086); registry/enum refs below | PROVISIONAL; KEEP-RULING: GPU-particle parity substrate; ownership/oracle arc still live.; VETO-VFX: no setter today; delete here only if item 13 does not want the substrate | L2 + L1 + Sol/unowned |
| `MC2_VFX_ORACLE_RENDER` | Bool/Lead1/false; MC2_FEATURE_VFX_ORACLE_RENDER; P | `mclib/particles/batcher.cpp:114` [L1] | [E087](#e087); registry/enum refs below | PROVISIONAL; KEEP-RULING: current VFX oracle/parity substrate; default-off does not establish obsolescence. | L2 + L1 + Sol/unowned |
| `MC2_VFX_ORACLE_TUBE` | Bool/Lead1/false; MC2_VFX_ORACLE_TUBE; A | `mclib/particles/batcher.cpp:166` [L1] | [E088](#e088); registry/enum refs below | PROVISIONAL; KEEP-RULING: current VFX oracle/parity substrate; default-off does not establish obsolescence. | L2 + L1 + Sol/unowned |
| `MC2_VIEWMODE_FRAMEWORK` | Bool/Lead0/false; MC2_FEATURE_VIEWMODE_FRAMEWORK; P | `GameOS/gameos/gos_postprocess.cpp:588` [L2] | [E089](#e089); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: tests/smoke/matrices/viewmode.json, strict runner --view-mode | L2 + Sol/unowned |
| `MC2_VIEW_UNIFORMS` | Bool/Lead0Kill/true; MC2_FEATURE_VIEW_UNIFORMS; P | `GameOS/gameos/debug_state_dump.cpp:474` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_program.cpp:60` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_state.cpp:331` [L2]<br>`code/gamecam.cpp:259` [L1]<br>`editor/EditorCamera.h:237` [Sol/unowned] | [E090](#e090); registry/enum refs below | PROVISIONAL; prior DELETE: default-on UBO; staticprop matrix sets =1 (the default); =0 has no user (5 readers incl. code/gamecam.cpp, editor/EditorCamera.h reader lines); W2: consumer-heavy | L2 + L1 + Sol/unowned |
| `MC2_VK_CSM_ARRAY_INTEROP` | Bool/Lead1/false; MC2_FEATURE_VK_CSM_ARRAY_INTEROP; A | `GameOS/gameos/vulkan_shadow_island_live.cpp:146` [L2] | [E091](#e091); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: strict runner STRICT_ENV sets =1 (H5 producer arm) | L2 |
| `MC2_VK_CSM_FULLMAP_INTEROP` | Bool/Lead1/false; MC2_FEATURE_VK_CSM_FULLMAP_INTEROP; A | `GameOS/gameos/vulkan_shadow_island_live.cpp:138` [L2] | [E092](#e092); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: strict runner STRICT_ENV sets =1 | L2 |
| `MC2_VK_IMGUI` | Bool/Lead1/false; MC2_FEATURE_VK_IMGUI; A | `GuiRuntime/GuiRuntime.cpp:48` [Sol/unowned] | [E093](#e093); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + Sol/unowned |
| `MC2_VK_SHADOW_INTEROP` | Bool/Lead1/false; MC2_FEATURE_VK_SHADOW_INTEROP; A | `GameOS/gameos/vulkan_shadow_island_live.cpp:130` [L2] | [E094](#e094); registry/enum refs below | PROVISIONAL; prior KEEP-LIVE: strict runner =1, tracy_capture_compare.py =0, test_check_vk_nointerop_env.py | L2 + Sol/unowned |
| `MC2_VK_SHARED_GBUFFER` | Bool/Exact1/false; MC2_FEATURE_VK_SHARED_GBUFFER; A | `mclib/vk_umbrella_gate.cpp:9` [L1] | [E095](#e095); registry/enum refs below | PROVISIONAL; arm/owner review pending | L2 + L1 + Sol/unowned |
| `MC2_VK_TERRAIN_LODFADE` | Bool/Exact1/false; MC2_FEATURE_VK_TERRAIN_LODFADE; A | `GameOS/gameos/gos_terrain_lod_chunk_feed.cpp:359` [L2] | [E096](#e096); registry/enum refs below | PROVISIONAL; prior DELETE: Exact1 opt-in VK terrain LOD fade; no setter (bookmark note only); VETO-VK: parity feature (GL lodfade twin); ALT fold into umbrella default-ON | L2 + Sol/unowned |
| `MC2_WATER_SHINE` | String/Std/""; MC2_FEATURE_WATER_SHINE; A | `GameOS/gameos/gameos_graphics_water.cpp:158` [L2] | [E097](#e097); registry/enum refs below | PROVISIONAL; prior DELETE: opt-in water shine; no setter; VETO-SETTINGS: ALT settings entry | L2 + Sol/unowned |
| `MC2_WATER_SHORE_MASK` | Bool/Lead0Kill/true; MC2_FEATURE_WATER_SHORE_MASK; A | `GameOS/gameos/gos_terrain_lod_chunk_upload.cpp:425` [L2] | [E098](#e098); registry/enum refs below | PROVISIONAL; prior DELETE: default-on sidecar mask (no-op without the sidecar); =0 has no user; W2: delivery ledger consumer | L2 + Sol/unowned |
| `MC2_WATER_SHORE_MEANDER` | Float/Prefix/0; MC2_FEATURE_WATER_SHORE_MEANDER; A | `GameOS/gameos/gos_terrain_water_stream.cpp:131` [L2] | [E099](#e099); registry/enum refs below | PROVISIONAL; prior DELETE: fBm shore meander default 0 (off); no setter; the pinned VK gap (glsl_port_diff allowlist, vk_not_ported) closes with it; VETO-FEATURE: SHORELINE-GEOMETRY-1 code (gos_terrain_water_stream.cpp ShoreMeanderWu + w.frag) | L2 + Sol/unowned |
| `MC2_WATER_SHORE_MEANDER_FREQ` | Float/Prefix/0.00260416674; MC2_TUNE_WATER_SHORE_MEANDER_FREQ; A | `GameOS/gameos/gos_terrain_water_stream.cpp:139` [L2] | [E100](#e100); registry/enum refs below | PROVISIONAL; prior DELETE: knob of MEANDER | L2 + Sol/unowned |

## Fully adjudicated lighting/light-upload family (12)

Each DELETE remains a candidate for orchestrator review, not authorization for Sol to edit either lane. W2 and VETO rows count as wave-1 candidates but are held out of the unflagged batch. The listed downstream files show why removal is larger than deleting the getter and table row.

| Gate | Verdict / flag | Retained and candidate-dead arm | Additional branch/consumer dependencies |
|---|---|---|---|
| `MC2_GPUBUF_LIGHT_GROWONCE` | DELETE / VETO-ARCH | On-arm: persistent single-store growth and prefix-residency upload. Preserve the default orphan path. NVIDIA hazard and AMD alternative remain an orchestrator choice. | GameOS/gameos/gameos_graphics_light_ssbo.cpp:287,355; GameOS/gameos/gameos_graphics_material_internal.h:575; no setter. No shader or CMake arm depends on this name. |
| `MC2_LEGACY_LIT_MC2_FRAME` | DELETE / W2 | Off-arm: upload legacy-frame uniform 0. Keep corrected MC2 coordinate frame and uniform 1; the delivery manifest tests the fix, not an off-arm user. | mclib/txmmgr_render_phases.cpp:114; scripts/delivery-fixes.json:37,41; shaders/gos_tex_vertex_lighted.vert:61,74,218. Keep shader ABI unless consumer/variant review proves it removable. |
| `MC2_LIGHTBAKE_REBAKE_ON_READY` | DELETE / - | Off-arm: bypass the mission-ready rebake. Keep the one-time invalidation, readiness condition, frame delay and log unchanged. | mclib/bdactor_lightbake.cpp:212; no setter or name-based external consumer. |
| `MC2_LIGHT_GRID` | KEEP-LIVE / - | Both arms have a user: default grid and the capture selfcheck that explicitly forces the linear full-pool path with =0. | tools/tracy_capture_compare.py:873 passes MC2_LIGHT_GRID=0 with a pool cap; GameOS/gameos/gos_terrain_lighting.cpp:260; retain this performance control. |
| `MC2_LIGHT_POOL_COMPRESS` | DELETE / - | Off-arm: upload compress=0. Keep compress=1 at all three producers and keep the uniforms and pool ABI. | GameOS/gameos/gameos_graphics_overlay.cpp:398; GameOS/gameos/gos_terrain_lighting.cpp:668; GameOS/gameos/gos_terrain_lod_chunk_upload.cpp:693; GameOS/gameos/gos_terrain_lod_chunk_draw.cpp:342; internal.h:207; shaders/terrain_lod_chunk.frag:251. No setter. |
| `MC2_LIGHT_POS_COLUMN_FIX` | DELETE / - | Off-arm: zero translation column. Keep translation copy, Query.posColumnFix=true, diagnostics and pure-producer policy. | mclib/txmmgr_lightrow.cpp:130,276,485; mclib/object_light_row.h:174; mclib/tgl.h:346; shaders/include/lighting.hglsl:202; positional enum LightPosColumnFix. No setter. |
| `MC2_LIGHT_PREFIX_GPU_COPY` | DELETE / VETO-PERF | On-arm: prefix stash/copy path. Keep default full orphan upload and any grow-once branch retained by its independent veto. Do not remove shared upload helpers or prefix data. | GameOS/gameos/gameos_graphics_light_ssbo.cpp:385; VRAM stash is the candidate dead resource; destructor cleanup must follow its ownership. No setter. |
| `MC2_LIGHT_REALISM` | DELETE / - | Off-arm: legacy pool/cone shape. Keep user-approved variant B; six numeric tunables remain independent rows and are outside this Feature removal. | GameOS/gameos/gos_terrain_lighting.cpp:1206 and its reshape branch; mclib/cone_glow.cpp:546,780; mclib/cone_glow.h:121; mclib/bdactor_update.cpp:382; mclib/genactor_frame.cpp:377; scripts/build_review_pack.py:299,348,349 are exported-id comments, not setters. |
| `MC2_LIGHT_ROW_PURE` | DELETE / VETO-ARCH | On-arm: replace legacy gathered row with the pure result. Keep the default legacy row. Retain the separately gated VERIFY comparison, BuildObjectLightRow and policy substrate; =1 in a source comment is a suggested proof, not an executing setter. | mclib/txmmgr_lightrow.cpp:320,339,358; mclib/bdactor_update.cpp:561; mclib/txmmgr.h:1441; positional enum LightRowPure. Pure algorithm tests are not an engine adapter =1 user. Orchestrator alternative is prove+promote. |
| `MC2_STATIC_LIGHT_UPLOAD_SPLIT` | DELETE / - | Off-arm: force whole-buffer upload despite a valid bake. Make split depend only on mc2LightBakeEnabled; keep its false branch, since LIGHTBAKE remains independently selectable. | mclib/txmmgr_lights.cpp:343; mclib/txmmgr_internal.h:115; mclib/txmmgr_render_phases.cpp:324; GameOS/gameos/gameos_graphics_light_ssbo.cpp:135; mclib/bdactor_static.cpp:652. No setter. |
| `MC2_VEHICLE_AMBIENT_FLOOR` | DELETE / W2 | Off-arm: uniform 0 disables floor. Keep uniform 1 and corrected vehicle lighting; delivery/checker assertions are consumer migration work. | mclib/txmmgr_render_phases.cpp:170; scripts/delivery-fixes.json:52,56; scripts/check-lighting-divergence.py:182,348; tools/split/allow/txmmgr.json:18; shaders/gos_tex_vertex_lighted.vert:134; positional enum VehicleAmbientFloor. No setter. |
| `MC2_VEHICLE_LIT_PATH` | DELETE / W2 | Off-arm: omit CacheGpuLightDataAllLeaves. Keep all-leaf gather; preserve the gameplay/render invocation order. | mclib/gvactor_render.cpp:128; scripts/delivery-fixes.json:67,69,71,82 (two fix rows, one gate); positional enum VehicleLitPath. No setter; delivery exe_needle must be replaced by durable fix evidence. |

## Conditional removal batches and remaining audit queue

All batches contain at most 25 Feature gates. These are removal proposals for the **11 reviewed candidates only**. They do not reach sub-30, and W2/VETO batches cannot be treated as approved work. No Sol-only batch exists.

| Batch | Feature gates | Count | Files/owners touched in addition to mandatory generation | Resulting Feature count if approved |
|---|---|---:|---|---:|
| GA-LIGHT-1 | LIGHTBAKE_REBAKE_ON_READY, LIGHT_POOL_COMPRESS, LIGHT_POS_COLUMN_FIX, LIGHT_REALISM, STATIC_LIGHT_UPLOAD_SPLIT (all MC2_) | 5 | **Lane 1:** bdactor_lightbake, txmmgr_lightrow/lights/render_phases/internal, object_light_row, cone_glow and relevant headers/comments. **Lane 2:** Gates/gate_table.cpp plus JSON/index; RenderCore registry and LightPosColumnFix enum renumber; lighting/overlay/chunk constant feeders, shader comments. Sol/unowned review-pack docs. | 95 |
| GA-LIGHT-W2 (held) | LEGACY_LIT_MC2_FRAME, VEHICLE_AMBIENT_FLOOR, VEHICLE_LIT_PATH | 3 | **Lane 1:** txmmgr_render_phases, gvactor_render and any durable fix-evidence tests. **Lane 2:** Gates/gate_table.cpp plus JSON/index; RenderCore three enum entries/renumber; shader comments/branch review. **Sol:** scripts/delivery-fixes.json, check-lighting-divergence.py, tools/split/allow/txmmgr.json. Delivery manifest migration first. | 92 |
| GA-LIGHT-VETO (held) | GPUBUF_LIGHT_GROWONCE, LIGHT_PREFIX_GPU_COPY, LIGHT_ROW_PURE | 3 | **Lane 1:** txmmgr_lightrow and pure-row policy/header comments; retain VERIFY/algorithm tests. **Lane 2:** Gates/gate_table.cpp plus JSON/index; RenderCore LightRowPure enum/renumber; light_ssbo stash/grow resources and internal comments. VETO-ARCH/VETO-PERF alternatives must be ruled on first. | 89 |

Within these proposals, the three reader-edit candidates that avoid lane 1 are GPUBUF_LIGHT_GROWONCE, LIGHT_PREFIX_GPU_COPY and LIGHT_POOL_COMPRESS. All three still require lane 2 and generation. No gate in this audit has a complete deletion limited to Sol/unowned files.

The remaining **88 gates** are assigned below to **audit-only** queues, each at most 25. They are not deletion commitments. Only after these owner/family reviews can batches for any of the missing 60 removals be proposed. Queue grouping is deterministic by source owner/name, not a substitute for semantic families.

| Audit queue | Count | Owner dependency | Provisional gates (all MC2_) |
|---|---:|---|---|
| Q1 | 25 | Lane 1 semantic owner + lane 2 Gates/registry; Sol consumers where listed; every future deletion edits Gates/gate_table.cpp [L2] | ABL_COMPILE_FATAL, ASSIMP_MECH_IMPORT, BRAIN_DISPATCH, BRAIN_RUNTIME, BRAIN_RUNTIME_APPLY, BURNIN_NO_JPG, CALLED_SHOT_FIX, COLORMAP_KTX2, CONE_POOL_POLICY, DETERMINISTIC_RNG, DIRECT_FIRE_STRAIGHT, DYNAMIC_DECALS, FRAMEGRAPH_EXECUTOR, FX_COST_SPLIT, FX_DEFS, FX_FORCE_SPAWN, GPU_PARTICLES, HUD_IMGUI_TABS, HUD_READABILITY_V1, HUD_TABS_TEST, ICON_FAILLOUD, LETTERBOX_OPAQUE, MOVE_RECON, OVERLAY_TILE_HIRES, PROJECTILE_SPEED_MULT |
| Q2 | 22 | Lane 1 semantic owner + lane 2 Gates/registry; Sol consumers where listed; every future deletion edits Gates/gate_table.cpp [L2] | RENDER_PASS_ORDER, SETUPTEXTURES_LEGACY_FORCE, SHADOW_CASTER_CULL_CACHE, SHADOW_DYNAMIC_PROP_CASTERS, SMART_LOAD, TACTICAL_ARC_OVERLAY, TERRAIN_AMBIENT_FLOOR, TERRAIN_CONTROLMAP, TERRAIN_CONTROLMAP_FILE, TERRAIN_LOD_CHUNK, TERRAIN_OVERLAY_V2, TERRAIN_OVERLAY_V2_FILE, TERRAIN_VISUAL_DISPLACE_OBJFADE, TERRAIN_VISUAL_HEIGHT, UNIT_PROFILE_DATA, VFX_AGE_SAMPLE, VFX_GPU_SIM_CARDCLOUD, VFX_GPU_SIM_COMPARE, VFX_ORACLE_RENDER, VFX_ORACLE_TUBE, VIEW_UNIFORMS, VK_SHARED_GBUFFER |
| Q3 | 25 | Lane 2 Gates/registry/render owners + Sol/unowned reader owners where listed; every future deletion edits Gates/gate_table.cpp [L2] | COLORMAP_CPU_RETIRE, DEBUG_RENDERER, FAR_BACKDROP_LIT, GLSTATEGUARD_TERRAIN, GPUBUF_RING, HZB_BUILD, HZB_PROBE, IMGUI, IMGUI_INSPECTOR, MATERIAL_GPU, MATERIAL_GPU_SAMPLE, MATERIAL_KTX, NIGHT_FOG_DIM, OBJECT_ID_BUFFER, OVERLAY_CEMENT_ADOPT, PAVED_OVERLAY_CLASS, RENDER_BACKEND, RENDER_BACKEND_IFACE, SHADER_SPIRV, SHADOW_ENABLE, SHADOW_GUARD_CLAMPED_SUN, SNAPSHOT_MECH_EXTRACT, SSAO, STATICPROP_BC7, STATICPROP_MATERIAL_PBR_SLOTS |
| Q4 | 16 | Lane 2 Gates/registry/render owners + Sol/unowned reader owners where listed; every future deletion edits Gates/gate_table.cpp [L2] | TERRAIN_AMBIENT_FLOOR_NIGHT, TERRAIN_CLIFF_DISPLACE, TERRAIN_MATERIAL_TEXTURES, TERRAIN_OVERLAY_V2_DECAL_SUPPRESS, TERRAIN_SHADOW_GUARD_CLAMPED_SUN, TERRAIN_TESS, VIEWMODE_FRAMEWORK, VK_CSM_ARRAY_INTEROP, VK_CSM_FULLMAP_INTEROP, VK_IMGUI, VK_SHADOW_INTEROP, VK_TERRAIN_LODFADE, WATER_SHINE, WATER_SHORE_MASK, WATER_SHORE_MEANDER, WATER_SHORE_MEANDER_FREQ |

## Consumer, setter-value, alias and enum evidence

Every E entry lists all external name/featureId hits found by the fresh scan, including non-setter fixtures/checks. Context includes up to two following lines for variable/fixture setters; exact source file:line remains authoritative. Values that are variables or expressions are intentionally preserved rather than guessed. Shared runners also forward inherited environments and generic --env/--set KEY=VALUE; that capability exists for every gate and is not evidence of a specific alternative-arm user. Root launch scripts and data text are included; no selected-family data/root-launcher setter was found. Frozen seed records for all rows and prior prose are common provenance and are not repeated.

<a id="e001"></a>
### E001 MC2_ABL_COMPILE_FATAL

Registry/alias declarations: `Gates/gate_index.gen.h:19` [L2]<br>`Gates/gate_table.cpp:26` [L2]<br>`Gates/gate_table.json:8` [L2]<br>`RenderCore/RendererFeatureRegistry.h:505,506` [L2]<br>`docs/flags.auto.yaml:33,41` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/log_tag_baseline.json:73` [L1] | historical/expected-output record (not setter) | `"MC2_ABL_COMPILE_FATAL"` |
| `tests/unit/printf_corpus.jsonl:597` [L1] | historical/expected-output record (not setter) | `{"files": ["mclib/ablrtn.cpp"], "new": "[ABL_COMPILE_FATAL] file={} syntaxErrors={:d} -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", "old": "[ABL_COMPILE_FATAL] file=%s syntaxErrors=%ld -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}, {"cls": "", "conv": "d", "length": "l", "wrap": "as_i32"}]}` |
| `tests/unit/printf_corpus_gen.inc:10180` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[ABL_COMPILE_FATAL] file=%s syntaxErrors=%ld -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:10181` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[ABL_COMPILE_FATAL] file={} syntaxErrors={:d} -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", mc2::fmt::safe(v0), mc2::fmt::as_i32(v1));` |
| `tests/unit/printf_corpus_gen.inc:10184` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[ABL_COMPILE_FATAL] file=%s syntaxErrors=%ld -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:10185` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[ABL_COMPILE_FATAL] file={} syntaxErrors={:d} -- refusing to load a module that failed to compile (partial bytecode corrupts the ABL VM later; docs/asan-follow-ups.md #3). Fix the script, or set MC2_ABL_SKIP_ERRORED_MODULES=1 (load mission, never execute errored brains) or MC2_ABL_COMPILE_FATAL=0 (legacy execute-anyway).", mc2::fmt::safe(v0), mc2::fmt::as_i32(v1));` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/ablrtn.cpp:819` [L1].

<a id="e002"></a>
### E002 MC2_ASSIMP_MECH_IMPORT

Registry/alias declarations: `Gates/gate_index.gen.h:36` [L2]<br>`Gates/gate_table.cpp:52,53` [L2]<br>`Gates/gate_table.json:25` [L2]<br>`RenderCore/RendererFeatureRegistry.h:533,534` [L2]<br>`docs/flags.auto.yaml:165,172` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `docs/modding/examples/mechanism-map.md:50` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `&#124; Dead/doc-drift env var &#124; — &#124; 'MC2_ASSIMP_MECH_IMPORT' &#124; — &#124; **DEAD** — never read by any code in this checkout; only appears in stale docs/tooling metadata. Use 'ENABLE_ASSIMP_IMPORTER'/'MC2_ASSIMP_TRACE' above instead &#124;  ## Buildings, props, trees` |
| `scripts/mcp/repo_intel_server.py:357` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `(e.g. MC2_ASSIMP_MECH_IMPORT). A gate's default STATE is IRRELEVANT to deletability. This tool classifies by CODE-PATH EVIDENCE ONLY. ` |
| `scripts/mech_shot.py:15` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `GLB as data/tgl/MadCat.glb + base tga) and MC2_ASSIMP_MECH_IMPORT is set here. """ import argparse` |
| `scripts/mech_shot.py:60` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_ASSIMP_MECH_IMPORT": "1",` |
| `tools/repo_intel/dead_gate_scan.py:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `SHIPPED FEATURES (e.g. MC2_ASSIMP_MECH_IMPORT). So a gate's DEFAULT STATE is IRRELEVANT to deletability. This tool classifies by CODE-PATH EVIDENCE ONLY. ` |

<a id="e003"></a>
### E003 MC2_BRAIN_DISPATCH

Registry/alias declarations: `Gates/gate_index.gen.h:67` [L2]<br>`Gates/gate_table.cpp:94,95,96,97` [L2]<br>`Gates/gate_table.json:56,57,58` [L2]<br>`RenderCore/RendererFeatureRegistry.h:603,604` [L2]<br>`docs/flags.auto.yaml:399,407,413,421` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/run_brain_apply_ab.ps1:37` [Sol/unowned] | value-bearing consumer; provisional role outside family | `$env:MC2_BRAIN_DISPATCH        = "1"` |
| `tests/fixtures/brain_runtime/manifest.json:11` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:31` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:52` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:72` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:91` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:110` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:134` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:153` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:172` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:191` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:206` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:220` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:236` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:249` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:286` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:307` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:329` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:350` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:371` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1"` |
| `tests/fixtures/brain_runtime/manifest.json:390` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:409` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:427` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:468` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:488` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:513` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:539` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:569` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:583` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:608` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:636` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:660` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:678` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:707` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/manifest.json:734` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_BRAIN_DISPATCH=1",` |
| `tests/fixtures/brain_runtime/mc2_01_patrol_specials.fit:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `; Gate: MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=1 MC2_BRAIN_PATROL=1` |
| `tests/fixtures/brain_runtime/mc2_17_attack_live_specials.fit:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `; Discovery: ran mc2_17 30s with MC2_BRAIN_DISPATCH=1 MC2_BRAIN_RUNTIME=1, observed` |
| `tests/unit/log_tag_baseline.json:457` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_DISPATCH",` |
| `tests/unit/log_tag_baseline.json:469` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_DISPATCH"` |
| `tests/unit/log_tag_baseline.json:595` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_DISPATCH"` |
| `tests/unit/log_tag_baseline.json:670` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_DISPATCH"` |
| `tests/unit/log_tag_baseline.json:785` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_DISPATCH"` |
| `tests/unit/printf_corpus.jsonl:645` [L1] | historical/expected-output record (not setter) | `{"files": ["code/warrior.cpp"], "new": "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 \u2014 apply is INERT\\n", "old": "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 \u2014 apply is INERT\\n", "specs": []}` |
| `tests/unit/printf_corpus.jsonl:646` [L1] | historical/expected-output record (not setter) | `{"files": ["code/mission_init_load.cpp"], "new": "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission={} special_fit={}\\n", "old": "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special_fit=%s\\n", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}, {"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}]}` |
| `tests/unit/printf_corpus.jsonl:658` [L1] | historical/expected-output record (not setter) | `{"files": ["code/mission_init_load.cpp"], "new": "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 \u2014 inert\\n", "old": "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 \u2014 inert\\n", "specs": []}` |
| `tests/unit/printf_corpus.jsonl:667` [L1] | historical/expected-output record (not setter) | `{"files": ["code/mission_init_load.cpp"], "new": "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\\n", "old": "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\\n", "specs": []}` |
| `tests/unit/printf_corpus_gen.inc:11017` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11018` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11021` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11022` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11028` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0645: [BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special [maybe-null]")` |
| `tests/unit/printf_corpus_gen.inc:11034` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special_fit=%s\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:11035` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission={} special_fit={}\n", mc2::fmt::safe(v0), mc2::fmt::safe(v1));` |
| `tests/unit/printf_corpus_gen.inc:11038` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special_fit=%s\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:11039` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission={} special_fit={}\n", mc2::fmt::safe(v0), mc2::fmt::safe(v1));` |
| `tests/unit/printf_corpus_gen.inc:11234` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11235` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11238` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11239` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11383` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11384` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11387` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");` |
| `tests/unit/printf_corpus_gen.inc:11388` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");` |
| `tools/brain_dispatch_harness/README-v2.md:52` [Sol/unowned] | value-bearing consumer; provisional role outside family | `$env:MC2_BRAIN_DISPATCH="1"; $env:MC2_BRAIN_DISPATCH_CALL="1"` |
| `tools/brain_dispatch_harness/harness_main.cpp:610` [Sol/unowned] | setter; expression/value shown | `(void)_putenv("MC2_BRAIN_DISPATCH=1"); (void)_putenv("MC2_BRAIN_DISPATCH_CALL=1"); (void)_putenv("MC2_BRAIN_DISPATCH_VAR=1");` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/brain_special_dispatch.cpp:158,2651` [L1]<br>`code/mech_brain_runtime.h:135` [L1]<br>`code/mission_init_load.cpp:491,495,506` [L1]<br>`code/warrior.cpp:1439` [L1].

<a id="e004"></a>
### E004 MC2_BRAIN_RUNTIME

Registry/alias declarations: `Gates/gate_index.gen.h:84` [L2]<br>`Gates/gate_table.cpp:114,115` [L2]<br>`Gates/gate_table.json:73` [L2]<br>`RenderCore/RendererFeatureRegistry.h:617,618` [L2]<br>`docs/flags.auto.yaml:530,538` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/fixtures/brain_runtime/mc2_17_attack_live_specials.fit:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `; Discovery: ran mc2_17 30s with MC2_BRAIN_DISPATCH=1 MC2_BRAIN_RUNTIME=1, observed` |
| `tests/unit/log_tag_baseline.json:1427` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_RUNTIME",` |
| `tests/unit/log_tag_baseline.json:1437` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_RUNTIME",` |
| `tests/unit/log_tag_baseline.json:1448` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_RUNTIME"` |
| `tests/unit/printf_corpus.jsonl:697` [L1] | historical/expected-output record (not setter) | `{"files": ["code/warrior.cpp"], "new": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 \u2014 runtime staying inert\\n", "old": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 \u2014 runtime staying inert\\n", "specs": []}` |
| `tests/unit/printf_corpus.jsonl:698` [L1] | historical/expected-output record (not setter) | `{"files": ["code/warrior.cpp"], "new": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 \u2014 apply staying inert\\n", "old": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 \u2014 apply staying inert\\n", "specs": []}` |
| `tests/unit/printf_corpus.jsonl:700` [L1] | historical/expected-output record (not setter) | `{"files": ["code/mission_init_load.cpp"], "new": "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission={} seeking {}\\n", "old": "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %s\\n", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}, {"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}]}` |
| `tests/unit/printf_corpus_gen.inc:11925` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0696: [BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtim")` |
| `tests/unit/printf_corpus_gen.inc:11929` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtime staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11930` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtime staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11933` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtime staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11934` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtime staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11940` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0697: [BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 ")` |
| `tests/unit/printf_corpus_gen.inc:11944` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11945` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11948` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11949` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11973` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0699: [BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %sn [maybe-null]")` |
| `tests/unit/printf_corpus_gen.inc:11979` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %s\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:11980` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission={} seeking {}\n", mc2::fmt::safe(v0), mc2::fmt::safe(v1));` |
| `tests/unit/printf_corpus_gen.inc:11983` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %s\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:11984` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission={} seeking {}\n", mc2::fmt::safe(v0), mc2::fmt::safe(v1));` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/mission_init_load.cpp:342` [L1]<br>`code/warrior.cpp:50,60,448` [L1]<br>`code/warrior.h:51,870` [L1].

<a id="e005"></a>
### E005 MC2_BRAIN_RUNTIME_APPLY

Registry/alias declarations: `Gates/gate_index.gen.h:85` [L2]<br>`Gates/gate_table.cpp:116,117` [L2]<br>`Gates/gate_table.json:74` [L2]<br>`RenderCore/RendererFeatureRegistry.h:624,625` [L2]<br>`docs/flags.auto.yaml:539,547` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/log_tag_baseline.json:1447` [L1] | historical/expected-output record (not setter) | `"MC2_BRAIN_RUNTIME_APPLY",` |
| `tests/unit/printf_corpus.jsonl:698` [L1] | historical/expected-output record (not setter) | `{"files": ["code/warrior.cpp"], "new": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 \u2014 apply staying inert\\n", "old": "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 \u2014 apply staying inert\\n", "specs": []}` |
| `tests/unit/printf_corpus_gen.inc:11940` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0697: [BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 ")` |
| `tests/unit/printf_corpus_gen.inc:11944` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11945` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11948` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
| `tests/unit/printf_corpus_gen.inc:11949` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/warrior.cpp:38,50` [L1].

<a id="e006"></a>
### E006 MC2_BURNIN_NO_JPG

Registry/alias declarations: `Gates/gate_index.gen.h:102` [L2]<br>`Gates/gate_table.cpp:134,135` [L2]<br>`Gates/gate_table.json:91` [L2]<br>`RenderCore/RendererFeatureRegistry.h:631,632` [L2]<br>`docs/flags.auto.yaml:669,676` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_gate.cpp:1072` [L1] | consumer/fixture reference; provisional role outside family | `const gate::Row& burnin = g1b::rowNamed("MC2_BURNIN_NO_JPG"); CHECK(burnin.rule == Rule::Presence); CHECK_FALSE(rowValue(burnin, nullptr));` |

<a id="e007"></a>
### E007 MC2_CALLED_SHOT_FIX

Registry/alias declarations: `Gates/gate_index.gen.h:103` [L2]<br>`Gates/gate_table.cpp:137` [L2]<br>`Gates/gate_table.json:92` [L2]<br>`RenderCore/RendererFeatureRegistry.h:638,639` [L2]<br>`docs/flags.auto.yaml:677,685` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e008"></a>
### E008 MC2_COLORMAP_CPU_RETIRE

Registry/alias declarations: `Gates/gate_index.gen.h:121` [L2]<br>`Gates/gate_table.cpp:163,164` [L2]<br>`Gates/gate_table.json:110` [L2]<br>`RenderCore/RendererFeatureRegistry.h:148,403,404` [L2]<br>`docs/flags.auto.yaml:816,823` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-cpu-gpu-cogating.py:170` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"inert_by": {"kind": "env", "token": "MC2_COLORMAP_CPU_RETIRE", "reader_file": "GameOS/gameos/gos_terrain_indirect_cement.cpp"}, "guard": r"displaceScale\s*>\s*0\.0f\s*&&\s*tcm->cpuColorMap\s*&&\s*tcm->cpuDispAlpha",` |
| `scripts/check-split-path-contracts.py:537` [Sol/unowned] | schema/check/comment (not setter) | `// Default-ON killswitch (MC2_TERRAIN_TESS / MC2_COLORMAP_CPU_RETIRE shape):` |
| `tools/split/allow/gos_terrain_indirect.json:29` [Sol/unowned] | schema/check/comment (not setter) | `"_scripts_reason": "Reviewed: check-buffer-lifetime-ownership requires GL_COMMAND_BARRIER_BIT in the dispatch piece (ComputeDispatch owns the indirect command buffer barrier); check-cpu-gpu-cogating reads the MC2_COLORMAP_CPU_RETIRE reader from the cement piece; check-object-mvp-currency allows the pack piece (defines the snapshot) and the dispatch piece (defines the getter); the fbobind allowlist names the dispatch piece (the water reflection pass is the only raw bind); counter-honesty keys the mine/decal probe limiter to its piece; gl_shader_variants.json is regenerated; delivery-fixes.json and predict_cement_adopt.py cite old line numbers in notes and comments only; tools/gate_codemod.py and its test (lane 1) list the file they already migrated; the remaining hits are comments."` |

<a id="e009"></a>
### E009 MC2_COLORMAP_KTX2

Registry/alias declarations: `Gates/gate_index.gen.h:123` [L2]<br>`Gates/gate_table.cpp:167,168` [L2]<br>`Gates/gate_table.json:112` [L2]<br>`RenderCore/RendererFeatureRegistry.h:151,411,412` [L2]<br>`docs/flags.auto.yaml:832,839` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/mc2_launcher/mc2_launcher.cpp:219` [L2] | launcher option; default/on/off values shown | `{ "MC2_COLORMAP_KTX2",           "BC7 KTX2 colormap atlas (ON)",          true,  "1",    "Terrain", "0" },` |

<a id="e010"></a>
### E010 MC2_CONE_POOL_POLICY

Registry/alias declarations: `Gates/gate_index.gen.h:125` [L2]<br>`Gates/gate_table.cpp:170,171` [L2]<br>`Gates/gate_table.json:114` [L2]<br>`RenderCore/RendererFeatureRegistry.h:666,667` [L2]<br>`docs/flags.auto.yaml:847,855` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/cone_glow.h:65` [L1].

<a id="e011"></a>
### E011 MC2_DEBUG_RENDERER

Registry/alias declarations: `Gates/gate_index.gen.h:135` [L2]<br>`Gates/gate_table.cpp:189,190` [L2]<br>`Gates/gate_table.json:124` [L2]<br>`RenderCore/RendererFeatureRegistry.h:104,283,284` [L2]<br>`docs/flags.auto.yaml:925,932` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/viewmode.json:56` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on selected movers. Drawn IN-SCENE by the debug renderer (needs BOTH MC2_DEBUG_RENDERER=1 and MC2_TACTICAL_ARC_OVERLAY=1); NOT a composite mode. Requires a unit to be selected in-game to show anything.",` |
| `tests/smoke/matrices/viewmode.json:58` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DEBUG_RENDERER": "1",` |
| `tools/vk_not_ported.allowlist:77` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/mech.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:78` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/mech.vert &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:79` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/static_prop.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:80` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/static_prop.vert &#124; define MC2_COALESCE: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:81` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/static_prop_depth.frag &#124; define MC2_COALESCE: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/EditorInspector_sections.cpp:853,1174` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow.cpp:140` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_mech.cpp:35` [Sol/unowned].

<a id="e012"></a>
### E012 MC2_DETERMINISTIC_RNG

Registry/alias declarations: `Gates/gate_index.gen.h:152` [L2]<br>`Gates/gate_table.cpp:213,214` [L2]<br>`Gates/gate_table.json:141` [L2]<br>`RenderCore/RendererFeatureRegistry.h:722,723` [L2]<br>`docs/flags.auto.yaml:1055,1063` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:36` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DETERMINISTIC_RNG": "1", "MC2_RNG_SEED": "0xC0FFEE",` |
| `scripts/smoke_lib/logparse.py:204` [Sol/unowned] | schema/check/comment (not setter) | `# Two runs under MC2_SMOKE_FIXED_TIMESTEP + MC2_DETERMINISTIC_RNG must carry the same chain over their common frames (tools/sim_determinism.py).` |
| `tests/smoke/sim_determinism/mc2_03.json:25` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DETERMINISTIC_RNG": "1",` |
| `tests/smoke/sim_determinism/mc2_10.json:25` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DETERMINISTIC_RNG": "1",` |
| `tests/smoke/sim_determinism/mc2_16.json:25` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DETERMINISTIC_RNG": "1",` |
| `tests/smoke/sim_determinism/mc2_24.json:25` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_DETERMINISTIC_RNG": "1",` |
| `tests/unit/log_tag_baseline.json:2461` [L1] | historical/expected-output record (not setter) | `"MC2_DETERMINISTIC_RNG"` |
| `tests/unit/printf_corpus.jsonl:774` [L1] | historical/expected-output record (not setter) | `{"files": ["code/mission_init.cpp"], "new": "[DET_RNG] mission={} reseed=0x{:08X} (MC2_DETERMINISTIC_RNG=1)\\n", "old": "[DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)\\n", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}, {"cls": "", "conv": "X", "length": "", "wrap": "as_u32"}]}` |
| `tests/unit/printf_corpus_gen.inc:13382` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0773: [DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)n [maybe-null]")` |
| `tests/unit/printf_corpus_gen.inc:13388` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:13389` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[DET_RNG] mission={} reseed=0x{:08X} (MC2_DETERMINISTIC_RNG=1)\n", mc2::fmt::safe(v0), mc2::fmt::as_u32(v1));` |
| `tests/unit/printf_corpus_gen.inc:13392` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:13393` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[DET_RNG] mission={} reseed=0x{:08X} (MC2_DETERMINISTIC_RNG=1)\n", mc2::fmt::safe(v0), mc2::fmt::as_u32(v1));` |
| `tools/sim_determinism.py:7` [Sol/unowned] | value-bearing consumer; provisional role outside family | `MC2_DETERMINISTIC_RNG=1 and MC2_SIM_IDENTITY_TRACE=1 (the three gates that remove wall-clock and RNG-seed variation). '--baseline'` |
| `tools/sim_determinism.py:46` [Sol/unowned] | value-bearing consumer; provisional role outside family | `GATES = {"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_DETERMINISTIC_RNG": "1", "MC2_SIM_IDENTITY_TRACE": "1"}` |
| `tools/sim_determinism.py:351` [Sol/unowned] | value-bearing consumer; provisional role outside family | `fh.write("[DET_RNG] mission=m reseed=0x00AB12CD (MC2_DETERMINISTIC_RNG=1)\n")` |
| `tools/test_capture_vfx_oracle.py:20` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert "MC2_DETERMINISTIC_RNG=1" in cmd and "MC2_RNG_SEED=0xC0FFEE" in cmd` |
| `tools/vfx_oracle/lifecycle_platform.cpp:16` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `if (!borrowedRngState &#124;&#124; !mc2::gate::get<"MC2_DETERMINISTIC_RNG">()) std::abort(); return static_cast<int>(mc2_det_rng::next15(*borrowedRngState)); }` |
| `tools/vfx_oracle/lifecycle_test.cpp:14` [Sol/unowned] | setter; expression/value shown | `ContractEnvironment deterministic{"MC2_DETERMINISTIC_RNG", "1"}; ContractEnvironment fixedStep{"MC2_SMOKE_FIXED_TIMESTEP", "1"}; ContractEnvironment identity{"MC2_SIM_IDENTITY_TRACE", "1"};` |
| `tools/vfx_oracle/lifecycle_test.cpp:55` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `REQUIRE(mc2::gate::get<"MC2_DETERMINISTIC_RNG">()); REQUIRE(mc2::gate::get<"MC2_SMOKE_FIXED_TIMESTEP">()); REQUIRE(mc2::gate::get<"MC2_SIM_IDENTITY_TRACE">());` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/mission_init.cpp:82` [L1].

<a id="e013"></a>
### E013 MC2_DIRECT_FIRE_STRAIGHT

Registry/alias declarations: `Gates/gate_index.gen.h:158` [L2]<br>`Gates/gate_table.cpp:221,222` [L2]<br>`Gates/gate_table.json:147` [L2]<br>`RenderCore/RendererFeatureRegistry.h:736,737` [L2]<br>`docs/flags.auto.yaml:1103,1111` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/mc2_launcher/mc2_launcher.cpp:184` [L2] | schema/check/comment (not setter) | `//   - MC2_DIRECT_FIRE_STRAIGHT / MC2_PROJECTILE_SPEED_MULT (covered by Faster Weapons)` |
| `tools/mc2_launcher/mc2_launcher.cpp:2252` [L2] | setter; expression/value shown | `SetEnvironmentVariableA("MC2_DIRECT_FIRE_STRAIGHT", fw ? "1" : NULL); SetEnvironmentVariableA("MC2_PROJECTILE_SPEED_MULT", fw ? "2" : NULL); ` |
| `tools/mc2_launcher/mc2_launcher.cpp:2267` [L2] | schema/check/comment (not setter) | `// MC2_DIRECT_FIRE_STRAIGHT and MC2_PROJECTILE_SPEED_MULT are driven by` |
| `tools/mc2_launcher/mc2_launcher.cpp:2269` [L2] | consumer/fixture reference; provisional role outside family | `if (strcmp(k_envVars[i].key, "MC2_DIRECT_FIRE_STRAIGHT") == 0) continue; if (strcmp(k_envVars[i].key, "MC2_PROJECTILE_SPEED_MULT") == 0) continue; if (k_envVars[i].cfg) continue;   // an options.cfg setting: written by SaveCfgRows() below, not passed in the environment` |

<a id="e014"></a>
### E014 MC2_DYNAMIC_DECALS

Registry/alias declarations: `Gates/gate_index.gen.h:168` [L2]<br>`Gates/gate_table.cpp:232,233` [L2]<br>`Gates/gate_table.json:157` [L2]<br>`RenderCore/RendererFeatureRegistry.h:743,744` [L2]<br>`docs/flags.auto.yaml:1182,1189` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/GraphicsOptionsWindow_internal.h:10` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_terrain.cpp:406,408` [Sol/unowned]<br>`mclib/crater.cpp:42` [L1]<br>`mclib/txmmgr_internal.h:89` [L1].

<a id="e015"></a>
### E015 MC2_FAR_BACKDROP_LIT

Registry/alias declarations: `Gates/gate_index.gen.h:194` [L2]<br>`Gates/gate_table.cpp:260,261` [L2]<br>`Gates/gate_table.json:183` [L2]<br>`RenderCore/RendererFeatureRegistry.h:160,427,428` [L2]<br>`docs/flags.auto.yaml:1369,1377` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e016"></a>
### E016 MC2_FRAMEGRAPH_EXECUTOR

Registry/alias declarations: `Gates/gate_index.gen.h:214` [L2]<br>`Gates/gate_table.cpp:289,290` [L2]<br>`Gates/gate_table.json:203` [L2]<br>`RenderCore/RendererFeatureRegistry.h:799,800` [L2]<br>`docs/flags.auto.yaml:1527,1535` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/mcp/mc2_render_state_server.py:1043` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `MC2_FRAMEGRAPH_EXECUTOR (default OFF) enables the executor path. MC2_FRAMEGRAPH_DRYRUN (default OFF) enables the dryrun observer. """` |
| `scripts/run_smoke.py:663` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_FRAMEGRAPH_EXECUTOR, MC2_FRAMEGRAPH_DRYRUN.") ap.add_argument("--skip-preflight-cache", action="store_true", help="SMOKE-LATENCY-WINS-1: skip the preflight result "` |
| `scripts/run_smoke.py:1748` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_FRAMEGRAPH_EXECUTOR": "executor_owned_passes",` |

<a id="e017"></a>
### E017 MC2_FX_COST_SPLIT

Registry/alias declarations: `Gates/gate_index.gen.h:229` [L2]<br>`Gates/gate_table.cpp:306,307` [L2]<br>`Gates/gate_table.json:218` [L2]<br>`RenderCore/RendererFeatureRegistry.h:813,814` [L2]<br>`docs/flags.auto.yaml:1649,1657` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/mc2fx/CMakeLists.txt:28` [Sol/unowned] | schema/check/comment (not setter) | `# gosFX effect/clouds/tube time themselves with fx_cost_split (MC2_FX_COST_SPLIT, default off).` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/gosfx/effect.cpp:3` [L1]<br>`mclib/gosfx/particlecloud.cpp:2` [L1]<br>`mclib/gosfx/spinningcloud.cpp:2` [L1]<br>`mclib/gosfx/tube.cpp:3` [L1].

<a id="e018"></a>
### E018 MC2_FX_DEFS

Registry/alias declarations: `Gates/gate_index.gen.h:231` [L2]<br>`Gates/gate_table.cpp:310,311` [L2]<br>`Gates/gate_table.json:220` [L2]<br>`RenderCore/RendererFeatureRegistry.h:820,821` [L2]<br>`docs/flags.auto.yaml:1666,1674` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e019"></a>
### E019 MC2_FX_FORCE_SPAWN

Registry/alias declarations: `Gates/gate_index.gen.h:232` [L2]<br>`Gates/gate_table.cpp:313,314` [L2]<br>`Gates/gate_table.json:221` [L2]<br>`RenderCore/RendererFeatureRegistry.h:827,828` [L2]<br>`docs/flags.auto.yaml:1675,1684` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:37` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_FX_FORCE_SPAWN": fixture, "MC2_GPU_PARTICLES_LOG": "1",` |
| `tests/unit/test_gate.cpp:931` [L1] | consumer/fixture reference; provisional role outside family | `TEST_CASE("MC2_FX_FORCE_SPAWN is an Enum row whose first token is the unset/unknown default") { // The other four level gates are covered by "G6 level-valued gates are Enum rows". const gate::Row& row = g1b::rowNamed("MC2_FX_FORCE_SPAWN");` |
| `tests/unit/test_gate.cpp:933` [L1] | consumer/fixture reference; provisional role outside family | `const gate::Row& row = g1b::rowNamed("MC2_FX_FORCE_SPAWN"); CHECK(row.kind == Kind::Enum); CHECK(row.defStr.empty());` |
| `tests/visual/bookmarks/mc2_01_combat.json:4` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"_note": "Combat-framing poses (NOT baseline) aimed at mc2_01 mech spawn coords to capture weapon-tube FX under MC2_FX_FORCE_SPAWN. Werewolf(player) @ (2496,-1941); Centipede cluster(enemy) @ (2858,2112). Fire is one-shot at scenarioTime>5 (sim frame ~150 at 30Hz) -> capture trigger frame ~165. alt low to frame the mechs, not terrain.", "bookmarks": [ {` |
| `tests/visual/bookmarks/mc2_01_werewolf.json:4` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"_note": "Single pose on the player Werewolf spawn (2496,-1941), which fires 5 weapons under MC2_FX_FORCE_SPAWN at sim frame ~150. Single bookmark so the capture lands at triggerFrame+settle on the fire frame (use --trigger-frame 147 --settle 4 -> ~151).", "bookmarks": [ {` |
| `tools/test_capture_vfx_oracle.py:52` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert "MC2_FX_FORCE_SPAWN=1" in command(Path("a"), Path("b"), 152, "mlr", "1")` |
| `tools/vfx_oracle/bookmarks/mc2_01.json:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"_note": "Q2 fixture-authored pose around the existing mc2_01_combat player_werewolf location (2496,-1941). MC2_FX_FORCE_SPAWN=loop fires at +150 world X. Require visually populated regions before assigning effect coverage. Duplicate tests settle-window motion.",` |
| `tools/vfx_oracle/bookmarks/mc2_24.json:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"_note": "Q2 diagnostic PPC poses from the historical force-fire lance location (-1216,-6150) in .claude/VK-ARC-STATE.md. These are fixture-authored, not user bookmarks. Require a visible PPC before accepting coverage. Fixed capture frame and MC2_FX_FORCE_SPAWN=loop; animation frozen. Two identical poses expose settle-window motion.",` |

<a id="e020"></a>
### E020 MC2_GLSTATEGUARD_TERRAIN

Registry/alias declarations: `Gates/gate_index.gen.h:238` [L2]<br>`Gates/gate_table.cpp:324,325` [L2]<br>`Gates/gate_table.json:227` [L2]<br>`RenderCore/RendererFeatureRegistry.h:841,842` [L2]<br>`docs/flags.auto.yaml:1724,1731` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e021"></a>
### E021 MC2_GPUBUF_LIGHT_GROWONCE

Registry/alias declarations: `Gates/gate_index.gen.h:250` [L2]<br>`Gates/gate_table.cpp:339,340` [L2]<br>`Gates/gate_table.json:239` [L2]<br>`RenderCore/RendererFeatureRegistry.h:855,856` [L2]<br>`docs/flags.auto.yaml:1815,1822` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e022"></a>
### E022 MC2_GPUBUF_RING

Registry/alias declarations: `Gates/gate_index.gen.h:252` [L2]<br>`Gates/gate_table.cpp:343,344` [L2]<br>`Gates/gate_table.json:241` [L2]<br>`RenderCore/RendererFeatureRegistry.h:869,870` [L2]<br>`docs/flags.auto.yaml:1831,1838` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e023"></a>
### E023 MC2_GPU_PARTICLES

Registry/alias declarations: `Gates/gate_index.gen.h:287` [L2]<br>`Gates/gate_table.cpp:385,386,387` [L2]<br>`Gates/gate_table.json:276` [L2]<br>`RenderCore/RendererFeatureRegistry.h:890,891` [L2]<br>`docs/flags.auto.yaml:2106,2114` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:38` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_GPU_PARTICLES": "1" if arm == "gpu" else "0"}` |
| `scripts/capture_vfx_oracle.py:52` [Sol/unowned] | value-bearing consumer; provisional role outside family | `return {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": harvest,` |
| `tests/smoke/matrices/vfx.json:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "Default VFX settings (MC2_GPU_PARTICLES default-ON, gosFX CPU sim active, all payoff gates OFF). Byte-identical baseline.", "env": {} },` |
| `tests/smoke/matrices/vfx.json:14` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "GPU particle batcher disabled (MC2_GPU_PARTICLES=0). Pure legacy gosFX CPU path.",` |
| `tests/smoke/matrices/vfx.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_GPU_PARTICLES": "0"` |
| `tools/test_capture_vfx_oracle.py:22` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert ("MC2_GPU_PARTICLES=1" in cmd) == (arm == "gpu")` |
| `tools/test_capture_vfx_oracle.py:23` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert ("MC2_GPU_PARTICLES=0" in cmd) == (arm == "mlr")` |
| `tools/test_vfx_parity_oracle.py:101` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"gates": {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": enabled,` |
| `tools/vfx_oracle/lifecycle_driver.cpp:22` [Sol/unowned] | setter; expression/value shown | `ContractEnvironment gpu{"MC2_GPU_PARTICLES", arm}; std::wstring command = L"\"" + executable.wstring() + L"\" --no-breaks"; STARTUPINFOW startup{};` |
| `tools/vfx_parity_oracle.py:127` [Sol/unowned] | value-bearing consumer; provisional role outside family | `expected = {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": harvest,` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/EditorInspector.h:125` [Sol/unowned]<br>`GuiRuntime/EditorInspector_sections.cpp:940,943` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_internal.h:9` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_mech.cpp:26,299,302` [Sol/unowned].

<a id="e024"></a>
### E024 MC2_HUD_IMGUI_TABS

Registry/alias declarations: `Gates/gate_index.gen.h:307` [L2]<br>`Gates/gate_table.cpp:408,409,414` [L2]<br>`Gates/gate_table.json:296,300` [L2]<br>`RenderCore/RendererFeatureRegistry.h:897,898` [L2]<br>`docs/flags.auto.yaml:2253,2260,2288` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_hud_tab_registry.cpp:1` [L1] | schema/check/comment (not setter) | `// IN-MISSION-HUD-IMGUI-SCOPE-1 slice C (MC2_HUD_IMGUI_TABS):` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/controlgui.h:48` [L1]<br>`code/hudtabregistry.cpp:2` [L1]<br>`code/hudtabregistry.h:5` [L1].

<a id="e025"></a>
### E025 MC2_HUD_READABILITY_V1

Registry/alias declarations: `Gates/gate_index.gen.h:309` [L2]<br>`Gates/gate_table.cpp:411,412` [L2]<br>`Gates/gate_table.json:298` [L2]<br>`RenderCore/RendererFeatureRegistry.h:904,905` [L2]<br>`docs/flags.auto.yaml:2268,2275` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e026"></a>
### E026 MC2_HUD_TABS_TEST

Registry/alias declarations: `Gates/gate_index.gen.h:312` [L2]<br>`Gates/gate_table.cpp:415,416` [L2]<br>`Gates/gate_table.json:301` [L2]<br>`RenderCore/RendererFeatureRegistry.h:911,912` [L2]<br>`docs/flags.auto.yaml:2290,2297` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/controlgui.h:454` [L1]<br>`code/controlgui_hudtabs.cpp:134` [L1].

<a id="e027"></a>
### E027 MC2_HZB_BUILD

Registry/alias declarations: `Gates/gate_index.gen.h:313` [L2]<br>`Gates/gate_table.cpp:417,418` [L2]<br>`Gates/gate_table.json:302` [L2]<br>`RenderCore/RendererFeatureRegistry.h:135,137,379,380` [L2]<br>`docs/flags.auto.yaml:2298,2306` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/hzb.json:3` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "TRACKRV-HZB-VISIBILITY-OPUS-1 Hi-Z depth pyramid build + diagnostic occlusion probe. MC2_HZB_BUILD is the pyramid-build gate (default-OFF); MC2_HZB_PROBE is the diagnostic readback probe gate (default-OFF, requires MC2_HZB_BUILD). Neither gate suppresses draws — both are diagnostic substrate only. The depth/reduction contract is locked in docs/hzb-depth-convention.md + tests/unit/test_depth_hzb.cpp (reverse-Z, ZERO_TO_ONE, near=1/far=0, MIN-reduce pyramid).", "default_missions": ["mc2_17"], "default_duration": 20,` |
| `tests/smoke/matrices/hzb.json:14` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "HZB pyramid build enabled (MC2_HZB_BUILD=1). Reverse-Z MIN-reduction pyramid is built each frame from MainDepth; diagnostic substrate only — no culling, no draw suppression.",` |
| `tests/smoke/matrices/hzb.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_HZB_BUILD": "1"` |
| `tests/smoke/matrices/hzb.json:21` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "HZB pyramid build + diagnostic occlusion probe (MC2_HZB_BUILD=1 + MC2_HZB_PROBE=1). Probe reads back a parent/child HZB level pair, verifies MIN-reduction integrity, and logs would-cull/would-keep/mismatch counters. NEVER suppresses a draw.",` |
| `tests/smoke/matrices/hzb.json:23` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_HZB_BUILD": "1",` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_postprocess.cpp:285,557` [L2]<br>`GuiRuntime/GraphicsOptionsWindow_passes.cpp:543` [Sol/unowned]<br>`RenderCore/frame_executor.h:42` [L2]<br>`RenderCore/postprocess_subgraph.h:103` [L2].

<a id="e028"></a>
### E028 MC2_HZB_PROBE

Registry/alias declarations: `Gates/gate_index.gen.h:321` [L2]<br>`Gates/gate_table.cpp:426,427` [L2]<br>`Gates/gate_table.json:310` [L2]<br>`RenderCore/RendererFeatureRegistry.h:139,387,388` [L2]<br>`docs/flags.auto.yaml:2356,2364` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/hzb.json:3` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "TRACKRV-HZB-VISIBILITY-OPUS-1 Hi-Z depth pyramid build + diagnostic occlusion probe. MC2_HZB_BUILD is the pyramid-build gate (default-OFF); MC2_HZB_PROBE is the diagnostic readback probe gate (default-OFF, requires MC2_HZB_BUILD). Neither gate suppresses draws — both are diagnostic substrate only. The depth/reduction contract is locked in docs/hzb-depth-convention.md + tests/unit/test_depth_hzb.cpp (reverse-Z, ZERO_TO_ONE, near=1/far=0, MIN-reduce pyramid).", "default_missions": ["mc2_17"], "default_duration": 20,` |
| `tests/smoke/matrices/hzb.json:21` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "HZB pyramid build + diagnostic occlusion probe (MC2_HZB_BUILD=1 + MC2_HZB_PROBE=1). Probe reads back a parent/child HZB level pair, verifies MIN-reduction integrity, and logs would-cull/would-keep/mismatch counters. NEVER suppresses a draw.",` |
| `tests/smoke/matrices/hzb.json:24` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_HZB_PROBE": "1"` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_postprocess.cpp:557` [L2]<br>`GameOS/gameos/gos_postprocess.h:641` [L2]<br>`RenderCore/frame_executor.h:43` [L2]<br>`RenderCore/postprocess_subgraph.h:122` [L2].

<a id="e029"></a>
### E029 MC2_ICON_FAILLOUD

Registry/alias declarations: `Gates/gate_index.gen.h:324` [L2]<br>`Gates/gate_table.cpp:430,431` [L2]<br>`Gates/gate_table.json:313` [L2]<br>`RenderCore/RendererFeatureRegistry.h:918,919` [L2]<br>`docs/flags.auto.yaml:2379,2387` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e030"></a>
### E030 MC2_IMGUI

Registry/alias declarations: `Gates/gate_index.gen.h:326` [L2]<br>`Gates/gate_table.cpp:434,435` [L2]<br>`Gates/gate_table.json:315` [L2]<br>`RenderCore/RendererFeatureRegistry.h:102,267,268` [L2]<br>`docs/flags.auto.yaml:2397,2404` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `CMakePresets.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_IMGUI": "ON",` |
| `docs/modding/examples/imgui-editing/README.md:6` [Sol/unowned] | value-bearing consumer; provisional role outside family | `> - **Works on:** this checkout, **provided 'mc2.exe' was built with '-DMC2_IMGUI=ON'** (the CMake option 'MC2_IMGUI' defaults **OFF** — 'CMakeLists.txt:104' — but the canonical build command in this repo's 'CLAUDE.md' passes '-DMC2_IMGUI=ON', and the shipped 'A:/Games/mc2-opengl/mc2-win64-v0.4' deploy has it on, verified by the presence of 'imgui.ini' and a populated 'data/visual_tuning.json' at that path). If your own build was configured without that flag, every hotkey in this doc is a no-op and there's nothing to fix on the data side — you need a different build.` |
| `docs/modding/examples/imgui-editing/README.md:8` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `>   - 'MC2_IMGUI' — master runtime switch for the whole ImGui layer ('GuiRuntime/GuiRuntime.cpp:25-32'). Unset or non-'"0"' = on. >   - 'MC2_IMGUI_INSPECTOR' — gates **both** the Object Inspector *and* the Renderer Features window (they share one 'isEnabled()' check, 'GuiRuntime/EditorInspector.cpp:100-107, 220-221'). Unset or non-'"0"' = on. (If you've seen other docs say this one is opt-in/off-by-default, that's wrong for this checkout — verified by reading the source, not assumed.) > - **Hotkeys** (work in the live game, 'mc2.exe' — not editor-only): 'Ctrl+Shift+G' Graphics Options, 'Ctrl+Shift+I' Object Inspector, 'Ctrl+Shift+F' Renderer Features, 'Ctrl+Shift+LMB' pick an object for the inspector.` |
| `docs/modding/examples/imgui-editing/README.md:128` [Sol/unowned] | schema/check/comment (not setter) | `// off without touching MC2_IMGUI_INSPECTOR/MC2_IMGUI.` |
| `docs/modding/examples/imgui-editing/README.md:190` [Sol/unowned] | value-bearing consumer; provisional role outside family | `&#124; No panels open at all, any hotkey &#124; Build has 'MC2_IMGUI' compiled out (CMake option defaults OFF) &#124; Confirm your build was configured '-DMC2_IMGUI=ON'; look for 'imgui.ini' next to your 'mc2.exe' as a quick signal it was &#124;` |
| `docs/modding/examples/mechanism-map.md:82` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `&#124; ImGui runtime enable &#124; 'isEnabled()', 'GuiRuntime/GuiRuntime.cpp:24-31' &#124; 'MC2_IMGUI' (default ON in dev builds; any value starting with something other than '0' counts as on; '"0"' disables) &#124; Shipped &#124; &#124; Graphics Options window &#124; 'GuiRuntime/GraphicsOptionsWindow.cpp:1738-1744' &#124; Hotkey 'Ctrl+Shift+G' (in-game, no env var) &#124; Shipped &#124; &#124; Object Inspector &#124; 'GuiRuntime/EditorInspector.cpp:103' &#124; 'MC2_IMGUI_INSPECTOR' &#124; Shipped &#124;` |
| `scripts/bootstrap_worktree_build.py:8` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `at-a-time (SDL2 -> GLEW -> ZLIB -> MC2_IMGUI). We kept rediscovering the exact -D flags every time. This script captures them ONCE. ` |
| `tools/mc2_launcher/mc2_launcher.cpp:241` [L2] | launcher option; default/on/off values shown | `{ "MC2_IMGUI",                   "ImGui overlay",                         true,  "",     "Debug"     },` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gameos_graphics_draw.cpp:664` [L2]<br>`GameOS/gameos/gameos_graphics_material_internal.h:46` [L2]<br>`GameOS/gameos/gameosmain.cpp:171,412,461,759,1193,1369` [L2]<br>`GameOS/gameos/gameosmain_frame.cpp:26,249,329` [L2]<br>`GameOS/gameos/gameosmain_internal.h:77` [L2]<br>`GuiRuntime/EditorInspector_sections.cpp:1171` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_mech.cpp:33` [Sol/unowned]<br>`code/gamecam.cpp:1032` [L1]<br>`code/mechcmd2_frame.cpp:78` [L1]<br>`code/mechcmd2_internal.h:89` [L1]<br>`code/missiongui_internal.h:27` [L1]<br>`code/missiongui_style.cpp:529,899` [L1]<br>`code/simplecamera.cpp:16,459` [L1]<br>`editor/CampaignSummaryPanel.cpp:21,367` [Sol/unowned]<br>`editor/EditorDebugOverlay.cpp:25,329,379,686,733,867` [Sol/unowned]<br>`editor/EditorDebugOverlay.h:40` [Sol/unowned]<br>`editor/EditorGameOS.cpp:60,367,368,511,592,673,719,761,880` [Sol/unowned]<br>`editor/EditorGosRender.cpp:23,83` [Sol/unowned]<br>`editor/EditorInterface.cpp:272,785,837` [Sol/unowned]<br>`editor/EditorInterface.h:33` [Sol/unowned]<br>`editor/EditorInterface_editor.cpp:175` [Sol/unowned]<br>`editor/EditorInterface_imgui.cpp:7,48,152` [Sol/unowned]<br>`editor/EditorInterface_internal.h:69` [Sol/unowned]<br>`editor/EditorInterface_mouse.cpp:647,666,686,722` [Sol/unowned]<br>`editor/EditorInterface_wnd.cpp:105,116,417` [Sol/unowned]<br>`editor/EditorModProject.cpp:19,475,667` [Sol/unowned]<br>`editor/EditorModProject.h:79` [Sol/unowned]<br>`editor/EditorTaskRunner.cpp:13,505,596` [Sol/unowned]<br>`editor/EditorTaskRunner.h:98` [Sol/unowned]<br>`editor/MapGeneratorDialog.cpp:30,1173` [Sol/unowned]<br>`editor/MissionValidation.cpp:15,448,589,591` [Sol/unowned]<br>`editor/ModPicker.cpp:8,85` [Sol/unowned]<br>`editor/ModPicker.h:17` [Sol/unowned]<br>`editor/ObjectivesSummaryPanel.cpp:39,575` [Sol/unowned].

<a id="e031"></a>
### E031 MC2_IMGUI_INSPECTOR

Registry/alias declarations: `Gates/gate_index.gen.h:328` [L2]<br>`Gates/gate_table.cpp:437,438` [L2]<br>`Gates/gate_table.json:317` [L2]<br>`RenderCore/RendererFeatureRegistry.h:103,275,276` [L2]<br>`docs/flags.auto.yaml:2412,2419` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `docs/modding/examples/imgui-editing/README.md:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `>   - 'MC2_IMGUI_INSPECTOR' — gates **both** the Object Inspector *and* the Renderer Features window (they share one 'isEnabled()' check, 'GuiRuntime/EditorInspector.cpp:100-107, 220-221'). Unset or non-'"0"' = on. (If you've seen other docs say this one is opt-in/off-by-default, that's wrong for this checkout — verified by reading the source, not assumed.) > - **Hotkeys** (work in the live game, 'mc2.exe' — not editor-only): 'Ctrl+Shift+G' Graphics Options, 'Ctrl+Shift+I' Object Inspector, 'Ctrl+Shift+F' Renderer Features, 'Ctrl+Shift+LMB' pick an object for the inspector. > - **Two unrelated env vars sometimes confused with this feature do not exist on this checkout:** 'MC2_EQUIPMENT_ADD' and 'MC2_UNIT_PROFILE_DATA' (branch 'claude/unit-profile-seam' only). They have nothing to do with ImGui; mentioned here only because this doc's payload also touches 'data/', and stray copy-paste from other example docs is an easy mistake.` |
| `docs/modding/examples/imgui-editing/README.md:128` [Sol/unowned] | schema/check/comment (not setter) | `// off without touching MC2_IMGUI_INSPECTOR/MC2_IMGUI.` |
| `docs/modding/examples/imgui-editing/README.md:191` [Sol/unowned] | value-bearing consumer; provisional role outside family | `&#124; 'Ctrl+Shift+G' works but 'Ctrl+Shift+I'/'Ctrl+Shift+F' don't &#124; 'MC2_IMGUI_INSPECTOR=0' is set somewhere (env, or a wrapper script) &#124; 'echo $env:MC2_IMGUI_INSPECTOR' in the shell you're launching from; unset it or set it to '1' &#124;` |
| `docs/modding/examples/mechanism-map.md:84` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `&#124; Object Inspector &#124; 'GuiRuntime/EditorInspector.cpp:103' &#124; 'MC2_IMGUI_INSPECTOR' &#124; Shipped &#124; &#124; Panel dispatch &#124; 'GuiRuntime::Render()', 'GuiRuntime/GuiRuntime.cpp:321-338' &#124; — &#124; Hardcoded C++ call list ('EditorInspector::drawImGui()', 'GraphicsOptionsWindow::draw()') — **no data-driven panel registry exists** &#124; ` |
| `tools/mc2_launcher/mc2_launcher.cpp:242` [L2] | launcher option; default/on/off values shown | `{ "MC2_IMGUI_INSPECTOR",         "ImGui inspector panel",                 true,  "",     "Debug"     },` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/EditorInspector_sections.cpp:1172` [Sol/unowned].

<a id="e032"></a>
### E032 MC2_LEGACY_LIT_MC2_FRAME

Registry/alias declarations: `Gates/gate_index.gen.h:335` [L2]<br>`Gates/gate_table.cpp:446,447` [L2]<br>`Gates/gate_table.json:324` [L2]<br>`RenderCore/RendererFeatureRegistry.h:175,443,444,447` [L2]<br>`docs/flags.auto.yaml:2462,2467,2470` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/delivery-fixes.json:37` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"gate": "MC2_LEGACY_LIT_MC2_FRAME",` |
| `scripts/delivery-fixes.json:41` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `{"path": "mclib/txmmgr.cpp", "needle": "MC2_LEGACY_LIT_MC2_FRAME"}` |

<a id="e033"></a>
### E033 MC2_LETTERBOX_OPAQUE

Registry/alias declarations: `Gates/gate_index.gen.h:337` [L2]<br>`Gates/gate_table.cpp:450,451` [L2]<br>`Gates/gate_table.json:326` [L2]<br>`RenderCore/RendererFeatureRegistry.h:932,933` [L2]<br>`docs/flags.auto.yaml:2478,2486` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e034"></a>
### E034 MC2_LIGHTBAKE_REBAKE_ON_READY

Registry/alias declarations: `Gates/gate_index.gen.h:339` [L2]<br>`Gates/gate_table.cpp:454,455` [L2]<br>`Gates/gate_table.json:328` [L2]<br>`RenderCore/RendererFeatureRegistry.h:939,940` [L2]<br>`docs/flags.auto.yaml:2494,2502` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e035"></a>
### E035 MC2_LIGHT_GRID

Registry/alias declarations: `Gates/gate_index.gen.h:363` [L2]<br>`Gates/gate_table.cpp:490,491` [L2]<br>`Gates/gate_table.json:352` [L2]<br>`RenderCore/RendererFeatureRegistry.h:953,954` [L2]<br>`docs/flags.auto.yaml:2680,2688` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/tracy_capture_compare.py:873` [Sol/unowned] | confirmed setter: =0 linear-path selfcheck | `[f"MC2_LIGHT_POOL_MAX={cap}", "MC2_LIGHT_GRID=0"],` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `RenderCore/RenderResourceRegistry.h:59,60` [L2].

<a id="e036"></a>
### E036 MC2_LIGHT_POOL_COMPRESS

Registry/alias declarations: `Gates/gate_index.gen.h:366` [L2]<br>`Gates/gate_table.cpp:497,498` [L2]<br>`Gates/gate_table.json:355` [L2]<br>`RenderCore/RendererFeatureRegistry.h:967,968` [L2]<br>`docs/flags.auto.yaml:2707,2715` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e037"></a>
### E037 MC2_LIGHT_POS_COLUMN_FIX

Registry/alias declarations: `Gates/gate_index.gen.h:378` [L2]<br>`Gates/gate_table.cpp:514,515` [L2]<br>`Gates/gate_table.json:367` [L2]<br>`RenderCore/RendererFeatureRegistry.h:176,451,452,455` [L2]<br>`docs/flags.auto.yaml:2809,2814,2817` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/object_light_row.h:174` [L1].

<a id="e038"></a>
### E038 MC2_LIGHT_PREFIX_GPU_COPY

Registry/alias declarations: `Gates/gate_index.gen.h:379` [L2]<br>`Gates/gate_table.cpp:516,517` [L2]<br>`Gates/gate_table.json:368` [L2]<br>`RenderCore/RendererFeatureRegistry.h:981,982` [L2]<br>`docs/flags.auto.yaml:2818,2826` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e039"></a>
### E039 MC2_LIGHT_REALISM

Registry/alias declarations: `Gates/gate_index.gen.h:380` [L2]<br>`Gates/gate_table.cpp:521,522` [L2]<br>`Gates/gate_table.json:369` [L2]<br>`RenderCore/RendererFeatureRegistry.h:988,989` [L2]<br>`docs/flags.auto.yaml:2827,2835` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/build_review_pack.py:299` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `reads exported ids by eye, so 'gate/gates/MC2_LIGHT_REALISM-6-tunables'` |
| `scripts/build_review_pack.py:348` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `# section can share a lookup key ("MC2_LIGHT_REALISM` |
| `scripts/build_review_pack.py:349` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `# tunables" vs "MC2_LIGHT_REALISM (+6 tunables)") and must` |

<a id="e040"></a>
### E040 MC2_LIGHT_ROW_PURE

Registry/alias declarations: `Gates/gate_index.gen.h:388` [L2]<br>`Gates/gate_table.cpp:533,534,536` [L2]<br>`Gates/gate_table.json:377` [L2]<br>`RenderCore/RendererFeatureRegistry.h:204,483,484,487` [L2]<br>`docs/flags.auto.yaml:2898,2903,2906` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e041"></a>
### E041 MC2_MATERIAL_GPU

Registry/alias declarations: `Gates/gate_index.gen.h:414` [L2]<br>`Gates/gate_table.cpp:571,572` [L2]<br>`Gates/gate_table.json:403` [L2]<br>`RenderCore/RendererFeatureRegistry.h:105,291,292` [L2]<br>`docs/flags.auto.yaml:3093,3101` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/staticprop.json:37` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_MATERIAL_GPU": "0",` |
| `tests/unit/log_tag_baseline.json:4028` [L1] | historical/expected-output record (not setter) | `"MC2_MATERIAL_GPU"` |
| `tests/unit/log_tag_baseline.json:5810` [L1] | historical/expected-output record (not setter) | `"MC2_MATERIAL_GPU"` |
| `tests/unit/printf_corpus.jsonl:908` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/gos_materials.cpp"], "new": "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\\n", "old": "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\\n", "specs": []}` |
| `tests/unit/printf_corpus.jsonl:1099` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/gameosmain_init.cpp"], "new": "[MATERIAL_GPU v4] enabled={:d} sample={:d} binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\\n", "old": "[MATERIAL_GPU v4] enabled=%d sample=%d binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\\n", "specs": [{"cls": "", "conv": "d", "length": "", "wrap": "as_i32"}, {"cls": "", "conv": "d", "length": "", "wrap": "as_i32"}]}` |
| `tests/unit/printf_corpus_gen.inc:15796` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0907: [GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)n")` |
| `tests/unit/printf_corpus_gen.inc:15800` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");` |
| `tests/unit/printf_corpus_gen.inc:15801` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");` |
| `tests/unit/printf_corpus_gen.inc:15804` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");` |
| `tests/unit/printf_corpus_gen.inc:15805` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");` |
| `tests/unit/printf_corpus_gen.inc:19400` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[MATERIAL_GPU v4] enabled=%d sample=%d binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:19401` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[MATERIAL_GPU v4] enabled={:d} sample={:d} binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n", mc2::fmt::as_i32(v0), mc2::fmt::as_i32(v1));` |
| `tests/unit/printf_corpus_gen.inc:19404` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[MATERIAL_GPU v4] enabled=%d sample=%d binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n", v0, v1);` |
| `tests/unit/printf_corpus_gen.inc:19405` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[MATERIAL_GPU v4] enabled={:d} sample={:d} binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n", mc2::fmt::as_i32(v0), mc2::fmt::as_i32(v1));` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/draw_packet_emitter.h:104` [L2]<br>`GameOS/gameos/gameosmain_init.cpp:227` [L2]<br>`GameOS/gameos/gos_materials.cpp:448` [L2]<br>`GameOS/gameos/gos_static_prop_registry.h:228` [L2]<br>`GameOS/gameos/render_snapshot.h:173,179` [L2]<br>`GuiRuntime/EditorInspector_sections.cpp:1048,1123` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_mech.cpp:27` [Sol/unowned].

<a id="e042"></a>
### E042 MC2_MATERIAL_GPU_SAMPLE

Registry/alias declarations: `Gates/gate_index.gen.h:415` [L2]<br>`Gates/gate_table.cpp:573,574` [L2]<br>`Gates/gate_table.json:404` [L2]<br>`RenderCore/RendererFeatureRegistry.h:106,299,300` [L2]<br>`docs/flags.auto.yaml:3102,3110` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/staticprop.json:38` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_MATERIAL_GPU_SAMPLE": "0"` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/EditorInspector_sections.cpp:354` [Sol/unowned].

<a id="e043"></a>
### E043 MC2_MATERIAL_KTX

Registry/alias declarations: `Gates/gate_index.gen.h:416` [L2]<br>`Gates/gate_table.cpp:575,576` [L2]<br>`Gates/gate_table.json:405` [L2]<br>`RenderCore/RendererFeatureRegistry.h:107,142,307,308` [L2]<br>`docs/flags.auto.yaml:3111,3118` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_static_prop_batcher_internal.h:20` [L2].

<a id="e044"></a>
### E044 MC2_MOVE_RECON

Registry/alias declarations: `Gates/gate_index.gen.h:479` [L2]<br>`Gates/gate_table.cpp:642,643` [L2]<br>`Gates/gate_table.json:468` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1044,1045` [L2]<br>`docs/flags.auto.yaml:3576,3584` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `code/mech_internal.h:136` [L1]<br>`code/mover_internal.h:124` [L1]<br>`code/objmgr_internal.h:37` [L1]<br>`code/warrior_internal.h:106` [L1]<br>`mclib/move_internal.h:28` [L1].

<a id="e045"></a>
### E045 MC2_NIGHT_FOG_DIM

Registry/alias declarations: `Gates/gate_index.gen.h:482` [L2]<br>`Gates/gate_table.cpp:648,649,651` [L2]<br>`Gates/gate_table.json:471,472` [L2]<br>`RenderCore/RendererFeatureRegistry.h:168,435,436,1062` [L2]<br>`docs/flags.auto.yaml:3601,3609,3615` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/build_review_pack.py:190` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"""'MC2_NIGHT_FOG_DIM (+_SCALE)' -> 'MC2_NIGHT_FOG_DIM'.""" m = re.search(r"MC2_[A-Z0-9_]+", gate_label) return m.group(0) if m else gate_label.strip()` |
| `scripts/build_review_pack.py:772` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `anchor = '"name": "MC2_NIGHT_FOG_DIM"' if anchor not in txt: print("SELF-TEST INCONCLUSIVE: anchor gate not in %s -- pick another and "` |
| `scripts/build_review_pack.py:781` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `orig.replace(b'"MC2_NIGHT_FOG_DIM"', b'"MC2_NIGHT_F\xffG_DIM"', 1), "not valid UTF-8"), ("count field disagrees with the rows", txt.replace('"count": ', '"count": 1', 1).encode("utf-8"), "not a complete gate table"),` |

<a id="e046"></a>
### E046 MC2_OBJECT_ID_BUFFER

Registry/alias declarations: `Gates/gate_index.gen.h:490` [L2]<br>`Gates/gate_table.cpp:660,661` [L2]<br>`Gates/gate_table.json:479` [L2]<br>`RenderCore/RendererFeatureRegistry.h:98,235,236` [L2]<br>`docs/flags.auto.yaml:3666,3674` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-spirv-reflection-contract.py:27` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `appears IFF MC2_OBJECT_ID_BUFFER, ViewUniformsBlock@3 IFF MC2_USE_VIEW_UNIFORMS).  Exit 0 unless a FAIL is found.` |
| `scripts/gl_shader_variants.json:2909` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_IMPORTED_MECH_MATERIAL" ],` |
| `scripts/gl_shader_variants.json:2917` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_IMPORTED_MECH_MATERIAL 1\n", "source": "GameOS/gameos/gos_mech_batcher.cpp:506", "stages": {` |
| `scripts/gl_shader_variants.json:2927` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_USE_VIEW_UNIFORMS", "MC2_IMPORTED_MECH_MATERIAL"` |
| `scripts/gl_shader_variants.json:2937` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n#define MC2_IMPORTED_MECH_MATERIAL 1\n", "source": "GameOS/gameos/gos_mech_batcher.cpp:506", "stages": {` |
| `scripts/gl_shader_variants.json:3654` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_USE_VIEW_UNIFORMS", "MC2_STATICPROP_PBR_SLOTS"` |
| `scripts/gl_shader_variants.json:3665` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n#define MC2_STATICPROP_PBR_SLOTS 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:179", "stages": {` |
| `scripts/gl_shader_variants.json:3676` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER" ], "id": "static_prop_coalesce",` |
| `scripts/gl_shader_variants.json:3683` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n#define MC2_OBJECT_ID_BUFFER 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:179", "stages": {` |
| `scripts/gl_shader_variants.json:3758` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_USE_VIEW_UNIFORMS", "MC2_STATICPROP_PBR_SLOTS"` |
| `scripts/gl_shader_variants.json:3769` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n#define MC2_STATICPROP_PBR_SLOTS 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:318", "stages": {` |
| `scripts/gl_shader_variants.json:3780` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER" ], "id": "static_prop_depth",` |
| `scripts/gl_shader_variants.json:3787` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#extension GL_ARB_shader_draw_parameters : require\n#define MC2_COALESCE 1\n#define MC2_OBJECT_ID_BUFFER 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:318", "stages": {` |
| `scripts/gl_shader_variants.json:3848` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_USE_VIEW_UNIFORMS", "MC2_STATICPROP_PBR_SLOTS"` |
| `scripts/gl_shader_variants.json:3859` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n#define MC2_STATICPROP_PBR_SLOTS 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:318", "stages": {` |
| `scripts/gl_shader_variants.json:3869` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER" ], "id": "static_prop_depth",` |
| `scripts/gl_shader_variants.json:3876` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:318", "stages": {` |
| `scripts/gl_shader_variants.json:3920` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER", "MC2_USE_VIEW_UNIFORMS", "MC2_STATICPROP_PBR_SLOTS"` |
| `scripts/gl_shader_variants.json:3931` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n#define MC2_USE_VIEW_UNIFORMS 1\n#define MC2_STATICPROP_PBR_SLOTS 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:85", "stages": {` |
| `scripts/gl_shader_variants.json:3941` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_OBJECT_ID_BUFFER" ], "id": "static_prop",` |
| `scripts/gl_shader_variants.json:3948` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"prefix": "#version 430\n#define MC2_OBJECT_ID_BUFFER 1\n", "source": "GameOS/gameos/gos_static_prop_batcher_program.cpp:85", "stages": {` |
| `tests/shader_variants/static_prop.frag.splice-regression:127` [Sol/unowned] | schema/check/comment (not setter) | `#ifdef MC2_OBJECT_ID_BUFFER` |
| `tests/shader_variants/static_prop.frag.splice-regression:205` [Sol/unowned] | schema/check/comment (not setter) | `#ifdef MC2_OBJECT_ID_BUFFER` |
| `tests/shader_variants/static_prop.frag.splice-regression:354` [Sol/unowned] | schema/check/comment (not setter) | `#ifdef MC2_OBJECT_ID_BUFFER` |
| `tests/shader_variants/static_prop.frag.splice-regression:612` [Sol/unowned] | schema/check/comment (not setter) | `#ifdef MC2_OBJECT_ID_BUFFER` |
| `tests/shader_variants/static_prop.frag.splice-regression:659` [Sol/unowned] | schema/check/comment (not setter) | `#ifdef MC2_OBJECT_ID_BUFFER` |
| `tests/smoke/matrices/viewmode.json:3` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "TRACKV-TACTICAL-PRESENTATION-OPUS-1 ViewMode presentation layer. All modes are selected by the postprocess composite via MC2_VIEWMODE_FRAMEWORK (gate, default-OFF) + MC2_VIEW_MODE (seed mode, matches RenderCore::ViewMode). Visual (0) is byte-identical to baseline. ObjectIdDebug needs MC2_OBJECT_ID_BUFFER (default-ON). Thermal is a luminance PLACEHOLDER, not real IR. TacticalOverlay is drawn in-scene by the debug renderer (separate gate), not by the composite.", "default_missions": ["mc2_03", "mc2_17", "mc2_24"], "default_duration": 30,` |
| `tests/smoke/matrices/viewmode.json:26` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER": "1"` |
| `tests/smoke/matrices/viewmode.json:35` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER": "0"` |
| `tests/telemetry/tag-registry.json:342` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"notes": "Object ID buffer feature emit (MC2_OBJECT_ID_BUFFER=1, R32_UINT MRT M1.5)."` |
| `tools/asset_viewer/ModelPreviewEngineShader.cpp:101` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_STATICPROP_PBR_SLOTS/MC2_OBJECT_ID_BUFFER/MC2_OBJECT_PARITY_CHECK)"; report_.textureMode          = "legacy sampler2D u_tex"; report_.objectLightsStubActive = true;` |
| `tools/shader_offline_build/pilots.json:34` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_offline_build/pilots.json:46` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1",` |
| `tools/shader_reflect/expected/shaders__mech.frag__objectid.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/expected/shaders__mech.vert__objectid.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/expected/shaders__static_prop.frag__coalesce_objectid.json:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/expected/shaders__static_prop.frag__objectid.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/expected/shaders__static_prop.vert__coalesce_objectid.json:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/expected/shaders__static_prop.vert__objectid.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_OBJECT_ID_BUFFER=1"` |
| `tools/shader_reflect/reflect.py:70` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("objectid",          ["MC2_OBJECT_ID_BUFFER=1"]),` |
| `tools/shader_reflect/reflect.py:71` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("coalesce_objectid", ["MC2_COALESCE=1", "MC2_OBJECT_ID_BUFFER=1"]),` |
| `tools/shader_reflect/reflect.py:77` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("objectid",          ["MC2_OBJECT_ID_BUFFER=1"]),` |
| `tools/shader_reflect/reflect.py:78` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("coalesce_objectid", ["MC2_COALESCE=1", "MC2_OBJECT_ID_BUFFER=1"],` |
| `tools/shader_reflect/reflect.py:83` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("objectid", ["MC2_OBJECT_ID_BUFFER=1"]),` |
| `tools/shader_reflect/reflect.py:91` [Sol/unowned] | value-bearing consumer; provisional role outside family | `_v("objectid",     ["MC2_OBJECT_ID_BUFFER=1"]),` |
| `tools/shader_reflect/reflect.py:126` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `{"MC2_COALESCE", "MC2_OBJECT_ID_BUFFER"} ) ` |
| `tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py:17` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `KNOWN_VARIANT_MACROS only spans MC2_COALESCE / MC2_OBJECT_ID_BUFFER.  POLICY — WARN-first. Broad drift discovery is reported to stderr and NEVER` |
| `tools/vk_not_ported.allowlist:19` [Sol/unowned] | schema/check/comment (not setter) | `MC2_OBJECT_ID_BUFFER &#124; shaders/mech.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:20` [Sol/unowned] | schema/check/comment (not setter) | `MC2_OBJECT_ID_BUFFER &#124; shaders/mech.vert &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:21` [Sol/unowned] | schema/check/comment (not setter) | `MC2_OBJECT_ID_BUFFER &#124; shaders/static_prop.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:22` [Sol/unowned] | schema/check/comment (not setter) | `MC2_OBJECT_ID_BUFFER &#124; shaders/static_prop.vert &#124; define MC2_COALESCE: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:23` [Sol/unowned] | schema/check/comment (not setter) | `MC2_OBJECT_ID_BUFFER &#124; shaders/static_prop_depth.frag &#124; define MC2_COALESCE: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:77` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/mech.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:78` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/mech.vert &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/mech_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:79` [Sol/unowned] | schema/check/comment (not setter) | `MC2_DEBUG_RENDERER &#124; shaders/static_prop.frag &#124; define MC2_OBJECT_ID_BUFFER: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_mech_batcher.cpp:491` [L2]<br>`GameOS/gameos/gos_postprocess.cpp:1089` [L2]<br>`GameOS/gameos/gos_postprocess.h:692` [L2]<br>`GameOS/gameos/gos_static_prop_batcher.h:46` [L2]<br>`GameOS/gameos/gos_static_prop_batcher_program.cpp:51,70` [L2]<br>`GuiRuntime/EditorInspector_sections.cpp:355,1173` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow.cpp:138` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_mech.cpp:29` [Sol/unowned]<br>`GuiRuntime/GraphicsOptionsWindow_passes.cpp:513` [Sol/unowned]<br>`RenderWorld/RenderWorld.cpp:794` [Sol/unowned]<br>`mclib/render_contract.cpp:82` [L1]<br>`mclib/render_contract.h:63` [L1]<br>`shaders/mech.frag:128,407` [L2]<br>`shaders/mech.vert:121,293` [L2]<br>`shaders/static_prop.frag:122,197,341,590,624` [L2].

<a id="e047"></a>
### E047 MC2_OVERLAY_CEMENT_ADOPT

Registry/alias declarations: `Gates/gate_index.gen.h:506` [L2]<br>`Gates/gate_table.cpp:680,681` [L2]<br>`Gates/gate_table.json:495` [L2]<br>`RenderCore/RendererFeatureRegistry.h:188,467,468` [L2]<br>`docs/flags.auto.yaml:3785,3793` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/delivery-fixes.json:98` [Sol/unowned] | schema/check/comment (not setter) | `"gate": "MC2_OVERLAY_CEMENT_ADOPT",` |
| `scripts/delivery-fixes.json:100` [Sol/unowned] | schema/check/comment (not setter) | `"exe_needle": "MC2_OVERLAY_CEMENT_ADOPT",` |
| `scripts/delivery-fixes.json:103` [Sol/unowned] | schema/check/comment (not setter) | `"needle": "MC2_OVERLAY_CEMENT_ADOPT"}` |
| `scripts/delivery-fixes.json:114` [Sol/unowned] | schema/check/comment (not setter) | `"gate": "MC2_OVERLAY_CEMENT_ADOPT",` |
| `scripts/delivery-fixes.json:125` [Sol/unowned] | schema/check/comment (not setter) | `"note": "The TileIsRoadArt runway superset guard. Adds NO gate of its own; it runs inside 'if (OverlayCementAdoptEnabled() && Terrain::mapData)' (gos_terrain_indirect.cpp:1826), so MC2_OVERLAY_CEMENT_ADOPT is genuinely its gate and is recorded as such rather than left null -- a null would have graded the default axis an unconditional YES and claimed delivery on an install where the adopt gate is off. exe_needle is a printf format literal unique to this commit, so the binary tier can answer even though the fix adds no uniform."` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/vulkan_terrain_island.h:304` [L2]<br>`shaders/vulkan/include/terrain_overlay_vs.glsl:26` [L2].

<a id="e048"></a>
### E048 MC2_OVERLAY_TILE_HIRES

Registry/alias declarations: `Gates/gate_index.gen.h:509` [L2]<br>`Gates/gate_table.cpp:687,688` [L2]<br>`Gates/gate_table.json:498` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1079,1080` [L2]<br>`docs/flags.auto.yaml:3809,3817` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/log_tag_baseline.json:7119` [L1] | historical/expected-output record (not setter) | `"MC2_OVERLAY_TILE_HIRES"` |
| `tests/unit/test_gate.cpp:1016` [L1] | consumer/fixture reference; provisional role outside family | `for (const char* name : {"MC2_ABL_COMPILE_FATAL_REPRO", "MC2_OVERLAY_TILE_HIRES", "MC2_MECH_IMPORT_FORCE_CLIP", "MC2_MECH_SKEL_BONE_DUMP", "MC2_LOG_FILE_RESOLVE", "MC2_RESOLVE_TRACE_FILE"}) { INFO(name);` |
| `tools/overlay_upscale/overlay_tile_upscale.py:6` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `engine loader (MC2_OVERLAY_TILE_HIRES, mclib/terrtxm.cpp initOverlay header resync). Output goes to a '<edge>Overlays/' folder that the gated loader probes first, with per-file fallback to the legacy '64Overlays/' tile --` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/terrtxm.cpp:84` [L1].

<a id="e049"></a>
### E049 MC2_PAVED_OVERLAY_CLASS

Registry/alias declarations: `Gates/gate_index.gen.h:525` [L2]<br>`Gates/gate_table.cpp:704,705` [L2]<br>`Gates/gate_table.json:514` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1086,1087` [L2]<br>`docs/flags.auto.yaml:3937,3946` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/delivery-fixes.json:146` [Sol/unowned] | schema/check/comment (not setter) | `"gate": "MC2_PAVED_OVERLAY_CLASS",` |

<a id="e050"></a>
### E050 MC2_PROJECTILE_SPEED_MULT

Registry/alias declarations: `Gates/gate_index.gen.h:550` [L2]<br>`Gates/gate_table.cpp:732,733` [L2]<br>`Gates/gate_table.json:539` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1093,1094` [L2]<br>`docs/flags.auto.yaml:4121,4130` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/mc2_launcher/mc2_launcher.cpp:184` [L2] | schema/check/comment (not setter) | `//   - MC2_DIRECT_FIRE_STRAIGHT / MC2_PROJECTILE_SPEED_MULT (covered by Faster Weapons)` |
| `tools/mc2_launcher/mc2_launcher.cpp:2253` [L2] | setter; expression/value shown | `SetEnvironmentVariableA("MC2_PROJECTILE_SPEED_MULT", fw ? "2" : NULL);  // Data-driven definitions toggle (NOT a mod): flip the unit+equipment` |
| `tools/mc2_launcher/mc2_launcher.cpp:2267` [L2] | schema/check/comment (not setter) | `// MC2_DIRECT_FIRE_STRAIGHT and MC2_PROJECTILE_SPEED_MULT are driven by` |
| `tools/mc2_launcher/mc2_launcher.cpp:2270` [L2] | consumer/fixture reference; provisional role outside family | `if (strcmp(k_envVars[i].key, "MC2_PROJECTILE_SPEED_MULT") == 0) continue; if (k_envVars[i].cfg) continue;   // an options.cfg setting: written by SaveCfgRows() below, not passed in the environment ` |

<a id="e051"></a>
### E051 MC2_RENDER_BACKEND

Registry/alias declarations: `Gates/gate_index.gen.h:574` [L2]<br>`Gates/gate_table.cpp:761,762` [L2]<br>`Gates/gate_table.json:563` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1100,1101` [L2]<br>`docs/flags.auto.yaml:4304,4312` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:35` [Sol/unowned] | value-bearing consumer; provisional role outside family | `env = {"MC2_RENDER_BACKEND": "gl", "MC2_SMOKE_FREEZE_ANIM": "1",` |
| `scripts/run_vk_field_check.py:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `Launches mc2.exe with EXACTLY the product/field env -- MC2_RENDER_BACKEND=vulkan` |
| `scripts/run_vk_field_check.py:50` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_RENDER_BACKEND": "vulkan",` |
| `scripts/smoke_lib/capture_backend.py:4` [L2] | consumer/fixture reference; provisional role outside family | `WHAT: 'requested(env)' reads MC2_RENDER_BACKEND the way the engine does (render_backend_select.cpp); 'check(requested, sidecars)' returns one message per sidecar whose backend or readback source is not what was asked for. An empty list means the capture is what it says it is. WHY: a Vulkan context that fails to start puts the whole session back on GL ('forceGLFallback'). The GL frame is then read back from the GL` |
| `scripts/smoke_lib/capture_backend.py:25` [L2] | consumer/fixture reference; provisional role outside family | `text = str((env or {}).get("MC2_RENDER_BACKEND", "") or "") return VULKAN if text in ("vulkan", "VULKAN", "vk") else GL ` |
| `tests/smoke/test_capture_backend.py:23` [L2] | value-bearing consumer; provisional role outside family | `assert cb.requested({"MC2_RENDER_BACKEND": text}) == "vulkan"` |
| `tests/smoke/test_capture_backend.py:24` [L2] | value-bearing consumer; provisional role outside family | `for env in (None, {}, {"MC2_RENDER_BACKEND": ""}, {"MC2_RENDER_BACKEND": "gl"}, {"MC2_RENDER_BACKEND": "Vulkan "},` |
| `tests/smoke/test_capture_backend.py:25` [L2] | value-bearing consumer; provisional role outside family | `{"MC2_RENDER_BACKEND": "bogus"}):` |
| `tests/smoke/test_capture_backend.py:77` [L2] | value-bearing consumer; provisional role outside family | `env, _ = rvc.build_capture_env(*args, {"MC2_RENDER_BACKEND": "vulkan"})` |
| `tests/smoke/test_capture_backend.py:98` [L2] | value-bearing consumer; provisional role outside family | `declared_env={"MC2_RENDER_BACKEND": "vulkan"})` |
| `tests/smoke/test_capture_backend.py:140` [L2] | value-bearing consumer; provisional role outside family | `"--out-dir", str(tmp_path / "out"), "--env-declare", "MC2_RENDER_BACKEND=vulkan"])` |
| `tests/smoke/test_capture_backend.py:173` [L2] | value-bearing consumer; provisional role outside family | `"--env", "MC2_RENDER_BACKEND=vulkan"]) == rc` |
| `tests/unit/log_tag_baseline.json:8051` [L1] | historical/expected-output record (not setter) | `"MC2_RENDER_BACKEND"` |
| `tests/unit/printf_corpus.jsonl:1282` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/render_backend_select.cpp"], "new": "[RENDER_BACKEND] MC2_RENDER_BACKEND={} -> Vulkan backend selected.\\n", "old": "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s -> Vulkan backend selected.\\n", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}]}` |
| `tests/unit/printf_corpus.jsonl:1283` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/render_backend_select.cpp"], "new": "[RENDER_BACKEND] MC2_RENDER_BACKEND={} unrecognized (expected gl&#124;vulkan); coercing to GL.\\n", "old": "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s unrecognized (expected gl&#124;vulkan); coercing to GL.\\n", "specs": [{"cls": "maybe-null", "conv": "s", "length": "", "wrap": "safe"}]}` |
| `tests/unit/printf_corpus_gen.inc:22862` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 1281: [RENDER_BACKEND] MC2_RENDER_BACKEND=%s -> Vulkan backend selected.n [maybe-null]")` |
| `tests/unit/printf_corpus_gen.inc:22867` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s -> Vulkan backend selected.\n", v0);` |
| `tests/unit/printf_corpus_gen.inc:22868` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[RENDER_BACKEND] MC2_RENDER_BACKEND={} -> Vulkan backend selected.\n", mc2::fmt::safe(v0));` |
| `tests/unit/printf_corpus_gen.inc:22871` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s -> Vulkan backend selected.\n", v0);` |
| `tests/unit/printf_corpus_gen.inc:22872` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[RENDER_BACKEND] MC2_RENDER_BACKEND={} -> Vulkan backend selected.\n", mc2::fmt::safe(v0));` |
| `tests/unit/printf_corpus_gen.inc:22878` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 1282: [RENDER_BACKEND] MC2_RENDER_BACKEND=%s unrecognized (expected gl&#124;vulka [maybe-null]")` |
| `tests/unit/printf_corpus_gen.inc:22883` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s unrecognized (expected gl&#124;vulkan); coercing to GL.\n", v0);` |
| `tests/unit/printf_corpus_gen.inc:22884` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[RENDER_BACKEND] MC2_RENDER_BACKEND={} unrecognized (expected gl&#124;vulkan); coercing to GL.\n", mc2::fmt::safe(v0));` |
| `tests/unit/printf_corpus_gen.inc:22887` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[RENDER_BACKEND] MC2_RENDER_BACKEND=%s unrecognized (expected gl&#124;vulkan); coercing to GL.\n", v0);` |
| `tests/unit/printf_corpus_gen.inc:22888` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[RENDER_BACKEND] MC2_RENDER_BACKEND={} unrecognized (expected gl&#124;vulkan); coercing to GL.\n", mc2::fmt::safe(v0));` |
| `tools/split/allow/missiongui.json:46` [Sol/unowned] | schema/check/comment (not setter) | `"_scripts_reason": "Reviewed: each names code/missiongui.cpp for code that stays in the core file (select-all isOnGUI count, #42b hover guard, s_stage limiter, edge-scroll comments, MC2_RENDER_BACKEND note); the moved UI-command chain is not pinned by any of them."` |
| `tools/test_capture_vfx_oracle.py:21` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert "MC2_RENDER_BACKEND=gl" in cmd` |
| `tools/tracy_capture_compare.py:40` [Sol/unowned] | value-bearing consumer; provisional role outside family | `* VK arm: MC2_RENDER_BACKEND=vulkan + MC2_VK_SHARED_GBUFFER=1, shadow interop OFF` |
| `tools/tracy_capture_compare.py:122` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_RENDER_BACKEND": "vulkan",` |
| `tools/tracy_capture_compare.py:194` [Sol/unowned] | value-bearing consumer; provisional role outside family | `if k.startswith("MC2_VK") or k == "MC2_RENDER_BACKEND":` |
| `tools/visual_verify/ab.py:162` [L2] | value-bearing consumer; provisional role outside family | `["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + a.env)` |
| `tools/visual_verify/capture.py:264` [L2] | value-bearing consumer; provisional role outside family | `"MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1. "` |
| `tools/visual_verify/capture_ab.py:8` [L2] | value-bearing consumer; provisional role outside family | `writes <root>/<TAG>1-gl, <root>/<TAG>2-gl and <root>/<TAG>-vk (capture.py --clean; VK via the umbrella MC2_RENDER_BACKEND=vulkan` |
| `tools/visual_verify/capture_ab.py:113` [L2] | schema/check/comment (not setter) | `# The umbrella, not the bare selector: with MC2_RENDER_BACKEND=vulkan alone the engine finds the present image not VK-owned, forces GL` |
| `tools/visual_verify/capture_ab.py:115` [L2] | value-bearing consumer; provisional role outside family | `cmd += ["--env", "MC2_RENDER_BACKEND=vulkan", "--env", "MC2_VK_SHARED_GBUFFER=1"]` |
| `tools/visual_verify/test_capture_ab.py:55` [L2] | value-bearing consumer; provisional role outside family | `assert "MC2_RENDER_BACKEND=vulkan" in vk and "MC2_VK_SHARED_GBUFFER=1" in vk` |
| `tools/visual_verify/test_capture_ab.py:56` [L2] | consumer/fixture reference; provisional role outside family | `assert not any("MC2_RENDER_BACKEND" in a or "MC2_VK_SHARED_GBUFFER" in a for a in gl)  ` |
| `tools/visual_verify/test_vk_replay.py:57` [L2] | value-bearing consumer; provisional role outside family | `assert declared == ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"]       # the backend alone falls back to GL` |
| `tools/visual_verify/vk_replay.py:38` [L2] | value-bearing consumer; provisional role outside family | `VULKAN_ENV = ("MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1")` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gameosmain.cpp:153` [L2]<br>`GameOS/gameos/gameosmain_init.cpp:463` [L2]<br>`GameOS/gameos/gameosmain_internal.h:59` [L2]<br>`GameOS/gameos/render_backend_select.cpp:44,49` [L2]<br>`GameOS/gameos/render_backend_select.h:25,26` [L2].

<a id="e052"></a>
### E052 MC2_RENDER_BACKEND_IFACE

Registry/alias declarations: `Gates/gate_index.gen.h:575` [L2]<br>`Gates/gate_table.cpp:763,764` [L2]<br>`Gates/gate_table.json:564` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1107,1108` [L2]<br>`docs/flags.auto.yaml:4313,4321` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e053"></a>
### E053 MC2_RENDER_PASS_ORDER

Registry/alias declarations: `Gates/gate_index.gen.h:582` [L2]<br>`Gates/gate_table.cpp:771,772` [L2]<br>`Gates/gate_table.json:571` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1114,1115` [L2]<br>`docs/flags.auto.yaml:4367,4374` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_render_contract_3.cpp:9` [L1] | schema/check/comment (not setter) | `// MC2_RENDER_PASS_ORDER env var requirement so CI (no env vars) still passes.` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gameosmain.cpp:1135` [L2].

<a id="e054"></a>
### E054 MC2_SETUPTEXTURES_LEGACY_FORCE

Registry/alias declarations: `Gates/gate_index.gen.h:607` [L2]<br>`Gates/gate_table.cpp:803,804` [L2]<br>`Gates/gate_table.json:596` [L2]<br>`RenderCore/RendererFeatureRegistry.h:154,419,420` [L2]<br>`docs/flags.auto.yaml:4563,4570` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e055"></a>
### E055 MC2_SHADER_SPIRV

Registry/alias declarations: `Gates/gate_index.gen.h:610` [L2]<br>`Gates/gate_table.cpp:808,809` [L2]<br>`Gates/gate_table.json:599` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1135,1136` [L2]<br>`docs/flags.auto.yaml:4585,4592` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/shader_offline_build/build_variants.py:186` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"env_gate": "MC2_SHADER_SPIRV=1 (default OFF)",` |

<a id="e056"></a>
### E056 MC2_SHADOW_CASTER_CULL_CACHE

Registry/alias declarations: `Gates/gate_index.gen.h:614` [L2]<br>`Gates/gate_table.cpp:815,816` [L2]<br>`Gates/gate_table.json:603` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1156,1157` [L2]<br>`docs/flags.auto.yaml:4619,4627` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e057"></a>
### E057 MC2_SHADOW_DYNAMIC_PROP_CASTERS

Registry/alias declarations: `Gates/gate_index.gen.h:627` [L2]<br>`Gates/gate_table.cpp:830,831` [L2]<br>`Gates/gate_table.json:616` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1170,1171` [L2]<br>`docs/flags.auto.yaml:4725,4734` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/shadow.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "Sun-shadow capture variants. MC2_SHADOW_ENABLE is the master gate (default-OFF); dynamic prop casters (MC2_SHADOW_DYNAMIC_PROP_CASTERS) is default-ON and effective only when shadows are enabled, so its kill-switch entry pairs it with the master gate. MC2_SHADOW_DEBUG_MODE=1 visualizes the screen-shadow pass classification (sky / self-shadow-handled / lit / shadowed). MC2_SHADOW_BOUNDED_NEAR_FIT tightens the light-frustum near fit. Definitions only — no captures run here.",` |
| `tests/smoke/matrices/shadow.json:21` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "Shadows on, dynamic prop casters kill-switched (MC2_SHADOW_DYNAMIC_PROP_CASTERS=0). Isolates the registry-fed dynamic caster contribution vs the static/building casters.",` |
| `tests/smoke/matrices/shadow.json:24` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SHADOW_DYNAMIC_PROP_CASTERS": "0"` |
| `tests/unit/test_gate.cpp:1052` [L1] | consumer/fixture reference; provisional role outside family | `{"MC2_SHADOW_DYNAMIC_PROP_CASTERS", "1&#124;0&#124;2", "1"}, {"MC2_SPOTLIGHT_DIRECTION_FIX", "0&#124;1&#124;2", "0"}, };` |
| `tests/unit/test_gate.cpp:1065` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(g1b::intOf(g1b::rowNamed("MC2_SHADOW_DYNAMIC_PROP_CASTERS"), "0") == 1); } ` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `RenderCore/RenderResourceRegistry.h:47` [L2]<br>`RenderCore/vulkan_contract.h:140` [L2].

<a id="e058"></a>
### E058 MC2_SHADOW_ENABLE

Registry/alias declarations: `Gates/gate_index.gen.h:629` [L2]<br>`Gates/gate_table.cpp:833,834` [L2]<br>`Gates/gate_table.json:618` [L2]<br>`RenderCore/RendererFeatureRegistry.h:101,259,260` [L2]<br>`docs/flags.auto.yaml:4742,4750` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/shadow.json:3` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "Sun-shadow capture variants. MC2_SHADOW_ENABLE is the master gate (default-OFF); dynamic prop casters (MC2_SHADOW_DYNAMIC_PROP_CASTERS) is default-ON and effective only when shadows are enabled, so its kill-switch entry pairs it with the master gate. MC2_SHADOW_DEBUG_MODE=1 visualizes the screen-shadow pass classification (sky / self-shadow-handled / lit / shadowed). MC2_SHADOW_BOUNDED_NEAR_FIT tightens the light-frustum near fit. Definitions only — no captures run here.",` |
| `tests/smoke/matrices/shadow.json:14` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "Sun shadows enabled (MC2_SHADOW_ENABLE=1). Dynamic prop casters default-ON. The headline 'shadows on' shot.",` |
| `tests/smoke/matrices/shadow.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SHADOW_ENABLE": "1"` |
| `tests/smoke/matrices/shadow.json:23` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SHADOW_ENABLE": "1",` |
| `tests/smoke/matrices/shadow.json:31` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SHADOW_ENABLE": "1",` |
| `tests/smoke/matrices/shadow.json:39` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SHADOW_ENABLE": "1",` |
| `tests/unit/test_gate.cpp:1203` [L1] | consumer/fixture reference; provisional role outside family | `EnvVar kill("MC2_SHADOW_ENABLE");          // Lead0Kill, default ON kill.set(nullptr); gate::invalidate("MC2_SHADOW_ENABLE");` |
| `tests/unit/test_gate.cpp:1205` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_SHADOW_ENABLE"); auto st = gate::boolState("MC2_SHADOW_ENABLE"); REQUIRE(st.has_value());` |
| `tests/unit/test_gate.cpp:1206` [L1] | consumer/fixture reference; provisional role outside family | `auto st = gate::boolState("MC2_SHADOW_ENABLE"); REQUIRE(st.has_value()); CHECK_FALSE(st->set);` |
| `tests/unit/test_gate.cpp:1211` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_SHADOW_ENABLE"); st = gate::boolState("MC2_SHADOW_ENABLE"); CHECK(st->set);` |
| `tests/unit/test_gate.cpp:1212` [L1] | consumer/fixture reference; provisional role outside family | `st = gate::boolState("MC2_SHADOW_ENABLE"); CHECK(st->set); CHECK_FALSE(st->on);` |
| `tests/unit/test_gate.cpp:1216` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_SHADOW_ENABLE"); EnvVar str("MC2_VULKAN_SPV_DIR");          // not a Bool row: on = set and not a leading 0 str.set("shaders/x");` |
| `tools/dev_shell/mc2_cmd.py:13` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `py -3 tools/dev_shell/mc2_cmd.py get_gate --name MC2_SHADOW_ENABLE py -3 tools/dev_shell/mc2_cmd.py set_gate --name MC2_X --value 1 py -3 tools/dev_shell/mc2_cmd.py quit` |
Indirect fixture setter values (same EnvVar/test scope): `tests/unit/test_gate.cpp:1204` [L1] `kill.set(nullptr);`; `tests/unit/test_gate.cpp:1210` [L1] `kill.set("0");`; `tests/unit/test_gate.cpp:1215` [L1] `kill.set(nullptr);`.

<a id="e059"></a>
### E059 MC2_SHADOW_GUARD_CLAMPED_SUN

Registry/alias declarations: `Gates/gate_index.gen.h:636` [L2]<br>`Gates/gate_table.cpp:842,843` [L2]<br>`Gates/gate_table.json:625` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1184,1185` [L2]<br>`docs/flags.auto.yaml:4798,4806` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e060"></a>
### E060 MC2_SMART_LOAD

Registry/alias declarations: `Gates/gate_index.gen.h:658` [L2]<br>`Gates/gate_table.cpp:894,895` [L2]<br>`Gates/gate_table.json:647` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1219,1220` [L2]<br>`docs/flags.auto.yaml:4971,4978` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e061"></a>
### E061 MC2_SNAPSHOT_MECH_EXTRACT

Registry/alias declarations: `Gates/gate_index.gen.h:666` [L2]<br>`Gates/gate_table.cpp:903,904` [L2]<br>`Gates/gate_table.json:655` [L2]<br>`RenderCore/RendererFeatureRegistry.h:111,323,324` [L2]<br>`docs/flags.auto.yaml:5033,5041` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/render_snapshot.h:154` [L2]<br>`GuiRuntime/EditorInspector_sections.cpp:762` [Sol/unowned]<br>`RenderCore/RenderPassContract.h:204` [L2].

<a id="e062"></a>
### E062 MC2_SSAO

Registry/alias declarations: `Gates/gate_index.gen.h:679` [L2]<br>`Gates/gate_table.cpp:923,924,928` [L2]<br>`Gates/gate_table.json:668,669` [L2]<br>`RenderCore/RendererFeatureRegistry.h:128,363,364,1244` [L2]<br>`docs/flags.auto.yaml:5132,5140,5146` [Sol/unowned]. Named enum consumers: `tests/unit/test_rendercore.cpp:372` [L1].

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `docs/modding/examples/imgui-editing/README.md:66` [Sol/unowned] | value-bearing consumer; provisional role outside family | `Several Graphics Options checkboxes (Track V's **HDR Post**, **Bloom**, **Tonemap ACES**, **SSAO**, and others) read an 'MC2_*' env var *once at startup* to seed a runtime bool, then let the checkbox override it live for the rest of the session — but the checkbox never writes back to anything. If you want the same choice on your next launch, you have to set the env var yourself: 'MC2_HDR_POST=1', 'MC2_BLOOM=1', 'MC2_SSAO=1', etc. (Check the **Renderer Features** window, 'Ctrl+Shift+F', for the exact var name and default for any given feature — that's exactly what it's for.)` |
| `tests/smoke/matrices/ssao.json:3` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "SSAO-GTAO-LITE-MVP-1 screen-space ambient occlusion capture variants. MC2_SSAO is the gate (default-OFF); MC2_SSAO_DEBUG overwrites the scene with the raw grayscale AO buffer for inspection. Half-res world-space hemisphere AO that reuses the scene depth + GBuffer1 normals. Definitions only — no captures run here.", "default_missions": ["mc2_01", "mc2_10"], "default_duration": 20,` |
| `tests/smoke/matrices/ssao.json:14` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "SSAO enabled (MC2_SSAO=1). AO multiplied into scene color; look for contact darkening in crevices / under props.",` |
| `tests/smoke/matrices/ssao.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SSAO": "1"` |
| `tests/smoke/matrices/ssao.json:23` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_SSAO": "1",` |
| `tests/unit/test_rendercore.cpp:372` [L1] | consumer/fixture reference; provisional role outside family | `{ RendererFeature::Ssao,        "MC2_SSAO" }, }; for (const Row& r : trackV) {` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_postprocess.cpp:433` [L2]<br>`GuiRuntime/GraphicsOptionsWindow.cpp:39` [Sol/unowned]<br>`RenderCore/frame_executor.h:48` [L2]<br>`RenderCore/postprocess_subgraph.h:295` [L2].

<a id="e063"></a>
### E063 MC2_STATICPROP_BC7

Registry/alias declarations: `Gates/gate_index.gen.h:685` [L2]<br>`Gates/gate_table.cpp:933,934` [L2]<br>`Gates/gate_table.json:674` [L2]<br>`RenderCore/RendererFeatureRegistry.h:143,395,396` [L2]<br>`docs/flags.auto.yaml:5181,5188` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e064"></a>
### E064 MC2_STATICPROP_MATERIAL_PBR_SLOTS

Registry/alias declarations: `Gates/gate_index.gen.h:686` [L2]<br>`Gates/gate_table.cpp:936,937` [L2]<br>`Gates/gate_table.json:675` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1247,1248` [L2]<br>`docs/flags.auto.yaml:5189,5197` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/vk_not_ported.allowlist:63` [Sol/unowned] | schema/check/comment (not setter) | `MC2_STATICPROP_MATERIAL_PBR_SLOTS &#124; shaders/static_prop.frag &#124; define MC2_STATICPROP_PBR_SLOTS: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:64` [Sol/unowned] | schema/check/comment (not setter) | `MC2_STATICPROP_MATERIAL_PBR_SLOTS &#124; shaders/static_prop.vert &#124; define MC2_COALESCE: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |
| `tools/vk_not_ported.allowlist:65` [Sol/unowned] | schema/check/comment (not setter) | `MC2_STATICPROP_MATERIAL_PBR_SLOTS &#124; shaders/static_prop_depth.frag &#124; define MC2_COALESCE, define MC2_STATICPROP_PBR_SLOTS: no such name in the VK twin (shaders/vulkan/color_prop_tier1.*) shader, island C++ or gpu_abi` |

<a id="e065"></a>
### E065 MC2_STATIC_LIGHT_UPLOAD_SPLIT

Registry/alias declarations: `Gates/gate_index.gen.h:693` [L2]<br>`Gates/gate_table.cpp:944,945` [L2]<br>`Gates/gate_table.json:682` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1254,1255` [L2]<br>`docs/flags.auto.yaml:5242,5249` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e066"></a>
### E066 MC2_TACTICAL_ARC_OVERLAY

Registry/alias declarations: `Gates/gate_index.gen.h:718` [L2]<br>`Gates/gate_table.cpp:983,984` [L2]<br>`Gates/gate_table.json:707` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1268,1269` [L2]<br>`docs/flags.auto.yaml:5425,5432` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/viewmode.json:56` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"description": "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on selected movers. Drawn IN-SCENE by the debug renderer (needs BOTH MC2_DEBUG_RENDERER=1 and MC2_TACTICAL_ARC_OVERLAY=1); NOT a composite mode. Requires a unit to be selected in-game to show anything.",` |
| `tests/smoke/matrices/viewmode.json:59` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_TACTICAL_ARC_OVERLAY": "1"` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GuiRuntime/GraphicsOptionsWindow.cpp:140` [Sol/unowned].

<a id="e067"></a>
### E067 MC2_TERRAIN_AMBIENT_FLOOR

Registry/alias declarations: `Gates/gate_index.gen.h:726` [L2]<br>`Gates/gate_table.cpp:1000,1001` [L2]<br>`Gates/gate_table.json:715` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1275,1276` [L2]<br>`docs/flags.auto.yaml:5483,5492` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-lighting-divergence.py:180` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `("GameOS/gameos/gos_terrain_lod_chunk_upload.cpp", "MC2_TERRAIN_AMBIENT_FLOOR"), ("mclib/camera.cpp", "MC2_NIGHT_AMBIENT_FLOOR"), ("mclib/txmmgr_render_phases.cpp", "MC2_VEHICLE_AMBIENT_FLOOR"),` |
| `scripts/check-lighting-divergence.py:346` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `("GameOS/gameos/gos_terrain_lod_chunk_upload.cpp", "MC2_TERRAIN_AMBIENT_FLOOR", "present"), ("mclib/camera.cpp", "MC2_NIGHT_AMBIENT_FLOOR", "present"), ("mclib/txmmgr_render_phases.cpp", "MC2_VEHICLE_AMBIENT_FLOOR", "present"),` |
| `scripts/delivery-fixes.json:130` [Sol/unowned] | schema/check/comment (not setter) | `"gate": "MC2_TERRAIN_AMBIENT_FLOOR",` |
| `scripts/delivery-fixes.json:135` [Sol/unowned] | schema/check/comment (not setter) | `"needle": "MC2_TERRAIN_AMBIENT_FLOOR"}` |
| `tests/unit/test_gate.cpp:833` [L1] | consumer/fixture reference; provisional role outside family | `EnvVar env("MC2_TERRAIN_AMBIENT_FLOOR"); gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR"); CHECK_FALSE(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">());` |
| `tests/unit/test_gate.cpp:834` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR"); CHECK_FALSE(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK_FALSE(gate::isSet<"MC2_TERRAIN_AMBIENT_FLOOR">());` |
| `tests/unit/test_gate.cpp:835` [L1] | consumer/fixture reference; provisional role outside family | `CHECK_FALSE(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK_FALSE(gate::isSet<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.35f);     // unset reads the default` |
| `tests/unit/test_gate.cpp:836` [L1] | consumer/fixture reference; provisional role outside family | `CHECK_FALSE(gate::isSet<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.35f);     // unset reads the default env.set("0.5");` |
| `tests/unit/test_gate.cpp:837` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.35f);     // unset reads the default env.set("0.5"); gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");` |
| `tests/unit/test_gate.cpp:839` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR"); CHECK(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.5f);` |
| `tests/unit/test_gate.cpp:840` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">()); CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.5f); env.set("9");` |
| `tests/unit/test_gate.cpp:841` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.5f); env.set("9"); gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");` |
| `tests/unit/test_gate.cpp:843` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR"); CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 2.0f);      // clampRange(0, 2) env.set(nullptr);` |
| `tests/unit/test_gate.cpp:844` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 2.0f);      // clampRange(0, 2) env.set(nullptr); gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");` |
| `tests/unit/test_gate.cpp:846` [L1] | consumer/fixture reference; provisional role outside family | `gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR"); } ` |
| `tools/split/allow/gos_terrain_lod_chunk.json:28` [Sol/unowned] | schema/check/comment (not setter) | `"_scripts_reason": "Reviewed: check-lighting-divergence reads the MC2_TERRAIN_AMBIENT_FLOOR default from the upload piece (gos_TerrainAmbientFloor); check-vk-umbrella-readers pins the four MC2_VK_TERRAIN_* reads in the feed piece; check-object-mvp-currency allows the draw piece and the internal header that declares the getter; the raw-GL depthfunc/depthmask allowlists name the draw piece; counter-honesty keys follow their limiters to the draw piece; test_cook_terrain_layers reads the channel/layer tables from the matalbedo piece; check-cpu-gpu-cogating's anchor (the terrain_lod_chunk.vert program) stays in the core piece; gl_shader_variants.json is regenerated; delivery-fixes.json code_evidence is read at the historical fix commit; tools/gate_codemod.py, tools/gate_conflicts.py, tools/gate_g0_ledger.json and the codemod test (lane 1 gate tooling) cite the file by name or old line numbers only; the rest cite it in comments."` |
Indirect fixture setter values (same EnvVar/test scope): `tests/unit/test_gate.cpp:838` [L1] `env.set("0.5");`; `tests/unit/test_gate.cpp:842` [L1] `env.set("9");`; `tests/unit/test_gate.cpp:845` [L1] `env.set(nullptr);`.

<a id="e068"></a>
### E068 MC2_TERRAIN_AMBIENT_FLOOR_NIGHT

Registry/alias declarations: `Gates/gate_index.gen.h:727` [L2]<br>`Gates/gate_table.cpp:1006,1007` [L2]<br>`Gates/gate_table.json:716` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1282,1283` [L2]<br>`docs/flags.auto.yaml:5493,5502` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e069"></a>
### E069 MC2_TERRAIN_CLIFF_DISPLACE

Registry/alias declarations: `Gates/gate_index.gen.h:730` [L2]<br>`Gates/gate_table.cpp:1011,1012` [L2]<br>`Gates/gate_table.json:719` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1296,1297` [L2]<br>`docs/flags.auto.yaml:5519,5527` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e070"></a>
### E070 MC2_TERRAIN_CONTROLMAP

Registry/alias declarations: `Gates/gate_index.gen.h:733` [L2]<br>`Gates/gate_table.cpp:1030,1031` [L2]<br>`Gates/gate_table.json:722` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1310,1311` [L2]<br>`docs/flags.auto.yaml:5544,5552` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/terrain_beautify/control_map_tool.py:5` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_CONTROLMAP, search "TERRAIN_CONTROLMAP v1" / gos_TerrainLodChunk_UploadControlMap): - RGBA8 PNG, size side x side where side == Terrain::realVerticesMapSide` |

<a id="e071"></a>
### E071 MC2_TERRAIN_CONTROLMAP_FILE

Registry/alias declarations: `Gates/gate_index.gen.h:736` [L2]<br>`Gates/gate_table.cpp:1034,1035` [L2]<br>`Gates/gate_table.json:725` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1317,1318` [L2]<br>`docs/flags.auto.yaml:5569,5577` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e072"></a>
### E072 MC2_TERRAIN_LOD_CHUNK

Registry/alias declarations: `Gates/gate_index.gen.h:759` [L2]<br>`Gates/gate_table.cpp:1062,1063,1110` [L2]<br>`Gates/gate_table.json:748,779` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1331,1332` [L2]<br>`docs/flags.auto.yaml:5744,5751,5984` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-env-drive-guard.py:15` [Sol/unowned] | value-bearing consumer; provisional role outside family | `legitimate A/B runs -- an OFF arm setting MC2_TERRAIN_LOD_CHUNK=0 is exactly` |
| `scripts/check-env-drive-guard.py:63` [Sol/unowned] | value-bearing consumer; provisional role outside family | `{"MC2_TERRAIN_LOD_CHUNK": "0"}, {}, [])` |
| `scripts/check-env-drive-guard.py:67` [Sol/unowned] | value-bearing consumer; provisional role outside family | `{"MC2_TERRAIN_LOD_CHUNK": "0", "MC2_GPU_MECHS": "0"}, {}, [])` |
| `scripts/check-env-drive-guard.py:103` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `for gate in ("MC2_GPU_MECHS", "MC2_TERRAIN_LOD_CHUNK"): if gate in ua({gate: "0"}, {}): fails.append(f"{gate} reported undriveable; it passes through "` |
| `scripts/check-render-contract-gbuffer1.sh:307` [Sol/unowned] | schema/check/comment (not setter) | `#   MC2_TERRAIN_LOD_CHUNK=0, which is NOT an escape hatch for it.` |
| `scripts/run_smoke.py:1423` [Sol/unowned] | schema/check/comment (not setter) | `# legitimate A/B runs (an OFF arm setting MC2_TERRAIN_LOD_CHUNK=0 is` |
| `scripts/visual_diff.py:398` [Sol/unowned] | value-bearing consumer; provisional role outside family | `json.dump({"feature_gates": {"MC2_TERRAIN_LOD_CHUNK": 1}}, f)` |
| `scripts/visual_diff.py:400` [Sol/unowned] | value-bearing consumer; provisional role outside family | `json.dump({"feature_gates": {"MC2_TERRAIN_LOD_CHUNK": 0}}, f)` |
| `tests/smoke/test_marker_buckets.py:111` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `s = parse_log("[CONFIG v1] warn=premain_read name=MC2_TERRAIN_LOD_CHUNK premain_value=0\n") assert s.marker_counts.get("config_premain_vk", 0) == 0 ` |
| `tests/unit/printf_corpus.jsonl:588` [L1] | historical/expected-output record (not setter) | `{"files": ["mclib/terrain.cpp"], "new": "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\\n", "old": "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\\n", "specs": []}` |
| `tests/unit/printf_corpus_gen.inc:10021` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0587: [8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigi")` |
| `tests/unit/printf_corpus_gen.inc:10025` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\n");` |
| `tests/unit/printf_corpus_gen.inc:10026` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\n");` |
| `tests/unit/printf_corpus_gen.inc:10029` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\n");` |
| `tests/unit/printf_corpus_gen.inc:10030` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: slimReduce deleted (8z-A3), ACTIVE_AB/SOLID_AB retired (8z-A1). The chunk-OFF path retains makeLists but has no production renderer.\n");` |
| `tools/mc2_launcher/mc2_launcher.cpp:212` [L2] | schema/check/comment (not setter) | `// scattered getenv("MC2_TERRAIN_LOD_CHUNK") presence checks were consolidated` |
| `tools/mc2_launcher/mc2_launcher.cpp:215` [L2] | launcher option; default/on/off values shown | `{ "MC2_TERRAIN_LOD_CHUNK",       "Chunk terrain LOD renderer (ON)",       true,  "1",    "Terrain", "0" },` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `RenderCore/terrain_path_telemetry.h:19` [L2]<br>`mclib/terrain.cpp:66` [L1]<br>`mclib/vertex.h:100` [L1].

<a id="e073"></a>
### E073 MC2_TERRAIN_MATERIAL_TEXTURES

Registry/alias declarations: `Gates/gate_index.gen.h:781` [L2]<br>`Gates/gate_table.cpp:1022,1089,1090` [L2]<br>`Gates/gate_table.json:770` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1338,1339` [L2]<br>`docs/flags.auto.yaml:5906,5914` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/deploy_payload.py:138` [Sol/unowned] | schema/check/comment (not setter) | `# loader (gos_terrain_lod_chunk.cpp) when MC2_TERRAIN_MATERIAL_TEXTURES=1.` |

<a id="e074"></a>
### E074 MC2_TERRAIN_OVERLAY_V2

Registry/alias declarations: `Gates/gate_index.gen.h:785` [L2]<br>`Gates/gate_table.cpp:1096,1097` [L2]<br>`Gates/gate_table.json:774` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1352,1353` [L2]<br>`docs/flags.auto.yaml:5939,5947` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/terrain_beautify/overlay_extract.py:7` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_OVERLAY_V2, search "TERRAIN_OVERLAY_V2 v1" / gos_TerrainLodChunk_UploadOverlaySidecar; shaders/terrain_lod_chunk.frag u_overlaySidecar/u_useOverlaySidecar):` |

<a id="e075"></a>
### E075 MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS

Registry/alias declarations: `Gates/gate_index.gen.h:786` [L2]<br>`Gates/gate_table.cpp:1099,1100` [L2]<br>`Gates/gate_table.json:775` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1359,1360` [L2]<br>`docs/flags.auto.yaml:5948,5955` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e076"></a>
### E076 MC2_TERRAIN_OVERLAY_V2_FILE

Registry/alias declarations: `Gates/gate_index.gen.h:787` [L2]<br>`Gates/gate_table.cpp:1101,1102` [L2]<br>`Gates/gate_table.json:776` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1366,1367` [L2]<br>`docs/flags.auto.yaml:5956,5964` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e077"></a>
### E077 MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN

Registry/alias declarations: `Gates/gate_index.gen.h:794` [L2]<br>`Gates/gate_table.cpp:1114,1115` [L2]<br>`Gates/gate_table.json:783` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1373,1374` [L2]<br>`docs/flags.auto.yaml:6007,6015` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e078"></a>
### E078 MC2_TERRAIN_TESS

Registry/alias declarations: `Gates/gate_index.gen.h:811` [L2]<br>`Gates/gate_table.cpp:1132,1133` [L2]<br>`Gates/gate_table.json:800` [L2]<br>`RenderCore/RendererFeatureRegistry.h:99,243,244` [L2]<br>`docs/flags.auto.yaml:6140,6148` [Sol/unowned]. Named enum consumers: `tests/unit/test_rendercore.cpp:358` [L1].

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-split-path-contracts.py:33` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `tokens RendererFeature::TerrainTessellation / MC2_TERRAIN_TESS may appear ONLY in the allowlist below; all other code must consume gos_IsModernShadowPathActive() / gos_IsTerrainTessellationActive().` |
| `scripts/check-split-path-contracts.py:37` [Sol/unowned] | value-bearing consumer; provisional role outside family | `the nightly MC2_TERRAIN_TESS=0/1 A/B leg is the behavioral guard.)` |
| `scripts/check-split-path-contracts.py:166` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `r"RendererFeature::TerrainTessellation&#124;MC2_TERRAIN_TESS\b") # CONTRACT-GREEN-1 (2026-07-31): entries are graded, not asserted. Each one below # was measured to still shelter a hit in CODE (comments lexed out); the two that` |
| `scripts/check-split-path-contracts.py:537` [Sol/unowned] | schema/check/comment (not setter) | `// Default-ON killswitch (MC2_TERRAIN_TESS / MC2_COLORMAP_CPU_RETIRE shape):` |
| `scripts/check-split-path-contracts.py:717` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"#include <cstdlib>\nbool f(){return getenv(\"MC2_TERRAIN_TESS\");}\n" }), True))) ` |
| `scripts/check-split-path-contracts.py:732` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"bool f(){return getenv(\"MC2_TERRAIN_TESS\");}\n" "int g(){ return 1 /* closes the phantom block */; }\n" }), True)))` |
| `scripts/check-split-path-contracts.py:751` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"// Default-ON killswitch (MC2_TERRAIN_TESS shape):\n" "// unset = on. RendererFeature::TerrainTessellation\n" "int f(){return 0;}\n"` |
| `scripts/checklib.py:110` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `'getenv("MC2_TERRAIN_TESS")' at :5184 -- a line a rule was specifically trying to see. ` |
| `tests/unit/log_tag_baseline.json:10404` [L1] | historical/expected-output record (not setter) | `"MC2_TERRAIN_TESS"` |
| `tests/unit/printf_corpus.jsonl:1513` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/gameos_graphics.cpp"], "new": "[TESS] MC2_TERRAIN_TESS=0 \u2014 terrain tessellation material load skipped (kill-switch)\\n", "old": "[TESS] MC2_TERRAIN_TESS=0 \u2014 terrain tessellation material load skipped (kill-switch)\\n", "specs": []}` |
| `tests/unit/printf_corpus_gen.inc:27255` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 1512: [TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped")` |
| `tests/unit/printf_corpus_gen.inc:27259` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)\n");` |
| `tests/unit/printf_corpus_gen.inc:27260` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)\n");` |
| `tests/unit/printf_corpus_gen.inc:27263` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)\n");` |
| `tests/unit/printf_corpus_gen.inc:27264` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)\n");` |
| `tests/unit/test_rendercore.cpp:352` [L1] | schema/check/comment (not setter) | `// (TerrainTessellation gained the MC2_TERRAIN_TESS default-ON killswitch` |
| `tests/unit/test_rendercore.cpp:361` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(std::strcmp(e.envVar, "MC2_TERRAIN_TESS") == 0); } ` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gameos_graphics.cpp:320` [L2].

<a id="e079"></a>
### E079 MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE

Registry/alias declarations: `Gates/gate_index.gen.h:815` [L2]<br>`Gates/gate_table.cpp:1138,1139` [L2]<br>`Gates/gate_table.json:804` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1380,1381` [L2]<br>`docs/flags.auto.yaml:6173,6180` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/vk_not_ported.allowlist:47` [Sol/unowned] | schema/check/comment (not setter) | `MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE &#124; shaders/terrain_lod_chunk.vert &#124; uniform u_visualDampOn: no such name in the VK twin (shaders/vulkan/terrain_tier1.*, shaders/vulkan/terrain_tier1_skirt.*) shader, island C++ or gpu_abi` |

<a id="e080"></a>
### E080 MC2_TERRAIN_VISUAL_HEIGHT

Registry/alias declarations: `Gates/gate_index.gen.h:817` [L2]<br>`Gates/gate_table.cpp:1141,1142` [L2]<br>`Gates/gate_table.json:806` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1387,1388` [L2]<br>`docs/flags.auto.yaml:6189,6197` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_gpu_buffer_owner.cpp:167` [L1] | schema/check/comment (not setter) | `// create path is gated upstream (MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + a bake that` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `RenderCore/RenderResourceRegistry.h:41` [L2].

<a id="e081"></a>
### E081 MC2_UNIT_PROFILE_DATA

Registry/alias declarations: `Gates/gate_index.gen.h:850` [L2]<br>`Gates/gate_table.cpp:1191,1192` [L2]<br>`Gates/gate_table.json:839` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1457,1458` [L2]<br>`docs/flags.auto.yaml:6445,6453` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `docs/modding/examples/getting-started.md:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `> - Two env vars you may see referenced elsewhere — 'MC2_EQUIPMENT_ADD' and 'MC2_UNIT_PROFILE_DATA' — **do not exist on this checkout.** They live only on the unmerged local branch 'claude/unit-profile-seam' (tip '59f16541'). If an example doc in this collection marks a feature "Coming soon", that's why.  ---` |
| `docs/modding/examples/imgui-editing/README.md:11` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `> - **Two unrelated env vars sometimes confused with this feature do not exist on this checkout:** 'MC2_EQUIPMENT_ADD' and 'MC2_UNIT_PROFILE_DATA' (branch 'claude/unit-profile-seam' only). They have nothing to do with ImGui; mentioned here only because this doc's payload also touches 'data/', and stray copy-paste from other example docs is an easy mistake.  Prerequisite reading: ['getting-started.md'](../getting-started.md) (mod folder anatomy, 'MC2_ACTIVE_MOD', the launcher, 'mc2mod.py') and ['mechanism-map.md'](../mechanism-map.md) (this doc's row is "In-game debug UI" near the bottom).` |
| `docs/modding/examples/mechanism-map.md:92` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `&#124; Unit-profile data-source flip &#124; Same branch &#124; 'MC2_UNIT_PROFILE_DATA' &#124; **Does not exist on this checkout.** Same caveat as above &#124;` |
| `docs/modding/examples/variant-add/README.md:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `> - This doc does **not** use 'MC2_EQUIPMENT_ADD' or 'MC2_UNIT_PROFILE_DATA' — those are branch-only ('claude/unit-profile-seam', unmerged) and unnecessary here. Adding a *variant* of an *existing* chassis needs none of that; see the [Coming soon](#coming-soon-branch-only-unit-profile-system) box at the bottom for what those actually cover.  Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, 'MC2_ACTIVE_MOD', the launcher picker, 'mc2mod.py' packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) row for "Variant block layout". This doc assumes you already know what 'mods/<id>/data/...' means.` |
| `docs/modding/examples/variant-add/README.md:189` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `This doc's technique — append a fixed 97-row block to a chassis CSV, one weapon 'masterID' per hardpoint — is the mechanism that **ships today, on this checkout**. There is a separate, more ergonomic data-driven unit/equipment system ('MC2_UNIT_PROFILE_DATA', 'MC2_EQUIPMENT_ADD') that exists only on the **unmerged local branch 'claude/unit-profile-seam'** (tip '59f16541') — it is **not present on this checkout**, 'grep'ing either env var here returns nothing, and setting them here does nothing. If you're reading this on a checkout that doesn't have that branch merged in, ignore any other doc that references those two env vars — use the CSV-block technique in this doc instead.  ## 6. Related mechanisms (not covered here)` |
| `docs/modding/examples/weapon-add/README.md:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `> - **'MC2_EQUIPMENT_ADD' / 'MC2_UNIT_PROFILE_DATA' do not exist on this checkout** — they live only on the unmerged local branch 'claude/unit-profile-seam' (tip '59f16541'). See the "Coming soon" box in [§4](#4-coming-soon-the-branch-only-equipment-add-registry).  Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, 'MC2_ACTIVE_MOD', the launcher picker, 'mc2mod.py' packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) rows for 'compbas.csv' and the mod-overlay core. This doc assumes you already know what 'mods/<id>/data/...' means. It's also worth skimming the sibling ['weapon-edit' example](../weapon-edit/README.md) — it covers *editing* an existing weapon's stats and has the full 'compbas.csv' column-by-column reference; this doc covers *adding* a new one and focuses on the two things that are different: filling a free row, and the display-name trap.` |
| `docs/modding/examples/weapon-edit/README.md:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `> - This doc does **not** use 'MC2_EQUIPMENT_ADD' or 'MC2_UNIT_PROFILE_DATA' — those are branch-only ('claude/unit-profile-seam', unmerged) and irrelevant here. Editing an *existing* weapon's stats needs none of that; it's plain whole-file overlay.  Before you start, read [Chapter 0 — Getting Started](../getting-started.md) (mod folder anatomy, 'MC2_ACTIVE_MOD', the launcher picker, 'mc2mod.py' packaging, diagnostics) and skim the [Mechanism Map](../mechanism-map.md) row for 'compbas.csv'. This doc assumes you already know what 'mods/<id>/data/...' means.` |
| `tools/mc2_launcher/mc2_launcher.cpp:2260` [L2] | setter; expression/value shown | `SetEnvironmentVariableA("MC2_UNIT_PROFILE_DATA",           dd ? "1" : NULL); SetEnvironmentVariableA("MC2_UNIT_DEFINITION_SOURCE",      dd ? "generated" : NULL); SetEnvironmentVariableA("MC2_EQUIPMENT_DEFINITION_SOURCE", dd ? "generated" : NULL);` |
| `tools/mc2_launcher/mc2_launcher.cpp:2934` [L2] | schema/check/comment (not setter) | `// weapons. NOT a mod: on Launch it sets MC2_UNIT_PROFILE_DATA=1 +` |
| `tools/unit_converter/shadow_compare.py:157` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `Reads the 'unitProfile.rows' section (present only when MC2_UNIT_PROFILE_DATA was ON). Rows without a 'chassis' are skipped (can't be matched). Multiple movers of the same chassis collapse to one entry; a facet CONFLICT between` |
| `tools/unit_converter/unit_preview.py:6` [Sol/unowned] | value-bearing consumer; provisional role outside family | `under MC2_UNIT_PROFILE_DATA=1) and print a clean, per-unit report a modder can` |
| `tools/unit_converter/unit_preview.py:215` [Sol/unowned] | value-bearing consumer; provisional role outside family | `help="debug_state dump JSON (needs unitPreview.rows; MC2_UNIT_PROFILE_DATA=1)")` |
| `tools/unit_converter/weapon_shadow_compare.py:154` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `MC2_UNIT_PROFILE_DATA was ON). Keyed by masterId. Sentinel slots are already dropped engine-side.""" out: Dict[int, ComponentFacets] = {}` |

<a id="e082"></a>
### E082 MC2_VEHICLE_AMBIENT_FLOOR

Registry/alias declarations: `Gates/gate_index.gen.h:858` [L2]<br>`Gates/gate_table.cpp:1201,1202` [L2]<br>`Gates/gate_table.json:847` [L2]<br>`RenderCore/RendererFeatureRegistry.h:194,475,476` [L2]<br>`docs/flags.auto.yaml:6505,6513` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-lighting-divergence.py:182` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `("mclib/txmmgr_render_phases.cpp", "MC2_VEHICLE_AMBIENT_FLOOR"),` |
| `scripts/check-lighting-divergence.py:348` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `("mclib/txmmgr_render_phases.cpp", "MC2_VEHICLE_AMBIENT_FLOOR", "present"),` |
| `scripts/delivery-fixes.json:52` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"gate": "MC2_VEHICLE_AMBIENT_FLOOR",` |
| `scripts/delivery-fixes.json:56` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `{"path": "mclib/txmmgr.cpp", "needle": "MC2_VEHICLE_AMBIENT_FLOOR"}` |
| `tools/split/allow/txmmgr.json:18` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"_scripts_reason": "Reviewed: check-lighting-divergence pins the MC2_VEHICLE_AMBIENT_FLOOR reader and check-no-raw-gl-from-game allows the renderLists GL calls in txmmgr_render.cpp; counter-honesty keys follow their limiters to txmmgr_render/lights.cpp; the env-registry comments name the typed reads; delivery-fixes code_evidence is read at the historical fix commit; the rest cite txmmgr.cpp in comments, and the viewer stubs re-define exported functions by name.",` |

<a id="e083"></a>
### E083 MC2_VEHICLE_LIT_PATH

Registry/alias declarations: `Gates/gate_index.gen.h:859` [L2]<br>`Gates/gate_table.cpp:1204,1205` [L2]<br>`Gates/gate_table.json:848` [L2]<br>`RenderCore/RendererFeatureRegistry.h:182,459,460` [L2]<br>`docs/flags.auto.yaml:6514,6522` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/delivery-fixes.json:67` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"gate": "MC2_VEHICLE_LIT_PATH",` |
| `scripts/delivery-fixes.json:69` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"exe_needle": "MC2_VEHICLE_LIT_PATH",` |
| `scripts/delivery-fixes.json:71` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `{"path": "mclib/gvactor.cpp", "needle": "MC2_VEHICLE_LIT_PATH"}` |
| `scripts/delivery-fixes.json:82` [Sol/unowned] | confirmed non-setter: schema/check/docstring/comment consumer | `"gate": "MC2_VEHICLE_LIT_PATH",` |

<a id="e084"></a>
### E084 MC2_VFX_AGE_SAMPLE

Registry/alias declarations: `Gates/gate_index.gen.h:863` [L2]<br>`Gates/gate_table.cpp:1209,1210` [L2]<br>`Gates/gate_table.json:852` [L2]<br>`RenderCore/RendererFeatureRegistry.h:114,331,332` [L2]<br>`docs/flags.auto.yaml:6547,6554` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.
Additional name/conditional/diagnostic consumers (role provisional outside family): `mclib/particles/spawn.cpp:32` [L1].

<a id="e085"></a>
### E085 MC2_VFX_GPU_SIM_CARDCLOUD

Registry/alias declarations: `Gates/gate_index.gen.h:869` [L2]<br>`Gates/gate_table.cpp:1217,1218` [L2]<br>`Gates/gate_table.json:858` [L2]<br>`RenderCore/RendererFeatureRegistry.h:123,347,348` [L2]<br>`docs/flags.auto.yaml:6594,6601` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:54` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"}` |
| `tools/test_capture_vfx_oracle.py:26` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD", "MC2_VFX_GPU_SIM_COMPARE")) assert "--no-lease" not in cmd and "--show-window" not in cmd ` |
| `tools/test_capture_vfx_oracle.py:46` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `assert gates["MC2_VFX_GPU_SIM_CARDCLOUD"] == gates["MC2_VFX_GPU_SIM_COMPARE"] == "0" cmd = command(Path("a"), Path("b"), 152, arm, experiment="harvested") assert all(f"{key}={value}" in cmd for key, value in gates.items())` |
| `tools/test_vfx_parity_oracle.py:103` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"},` |
| `tools/vfx_oracle/lifecycle_driver.cpp:17` [Sol/unowned] | setter; expression/value shown | `ContractEnvironment gpuSim{"MC2_VFX_GPU_SIM_CARDCLOUD", "0"}; ContractEnvironment log{"MC2_GPU_PARTICLES_LOG", "0"}; ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};` |
| `tools/vfx_parity_oracle.py:129` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"}` |

<a id="e086"></a>
### E086 MC2_VFX_GPU_SIM_COMPARE

Registry/alias declarations: `Gates/gate_index.gen.h:870` [L2]<br>`Gates/gate_table.cpp:1219,1220` [L2]<br>`Gates/gate_table.json:859` [L2]<br>`RenderCore/RendererFeatureRegistry.h:124,355,356` [L2]<br>`docs/flags.auto.yaml:6602,6609` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:54` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"}` |
| `tools/test_capture_vfx_oracle.py:26` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD", "MC2_VFX_GPU_SIM_COMPARE")) assert "--no-lease" not in cmd and "--show-window" not in cmd ` |
| `tools/test_capture_vfx_oracle.py:46` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `assert gates["MC2_VFX_GPU_SIM_CARDCLOUD"] == gates["MC2_VFX_GPU_SIM_COMPARE"] == "0" cmd = command(Path("a"), Path("b"), 152, arm, experiment="harvested") assert all(f"{key}={value}" in cmd for key, value in gates.items())` |
| `tools/test_vfx_parity_oracle.py:103` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"},` |
| `tools/vfx_parity_oracle.py:129` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"}` |

<a id="e087"></a>
### E087 MC2_VFX_ORACLE_RENDER

Registry/alias declarations: `Gates/gate_index.gen.h:871` [L2]<br>`Gates/gate_table.cpp:1221,1222` [L2]<br>`Gates/gate_table.json:860` [L2]<br>`RenderCore/RendererFeatureRegistry.h:118,339,340` [L2]<br>`docs/flags.auto.yaml:6610,6618` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:52` [Sol/unowned] | value-bearing consumer; provisional role outside family | `return {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": harvest,` |
| `tests/smoke/matrices/vfx.json:59` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_ORACLE_RENDER": "1",` |
| `tests/smoke/matrices/vfx.json:68` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_ORACLE_RENDER": "0"` |
| `tools/test_capture_vfx_oracle.py:25` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `("MC2_VFX_ORACLE_RENDER", "MC2_VFX_ORACLE_TUBE", "MC2_VFX_ORACLE_SHAPE", "MC2_VFX_GPU_SIM_CARDCLOUD", "MC2_VFX_GPU_SIM_COMPARE")) assert "--no-lease" not in cmd and "--show-window" not in cmd` |
| `tools/test_vfx_parity_oracle.py:101` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"gates": {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": enabled,` |
| `tools/vfx_oracle/lifecycle_driver.cpp:14` [Sol/unowned] | setter; expression/value shown | `ContractEnvironment oracle{"MC2_VFX_ORACLE_RENDER", "0"}; ContractEnvironment tube{"MC2_VFX_ORACLE_TUBE", "0"}; ContractEnvironment shape{"MC2_VFX_ORACLE_SHAPE", "0"};` |
| `tools/vfx_parity_oracle.py:127` [Sol/unowned] | value-bearing consumer; provisional role outside family | `expected = {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": harvest,` |

<a id="e088"></a>
### E088 MC2_VFX_ORACLE_TUBE

Registry/alias declarations: `Gates/gate_index.gen.h:874` [L2]<br>`Gates/gate_table.cpp:1226,1227` [L2]<br>`Gates/gate_table.json:863` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1471,1472` [L2]<br>`docs/flags.auto.yaml:6633,6640` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/capture_vfx_oracle.py:53` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_ORACLE_TUBE": harvest, "MC2_VFX_ORACLE_SHAPE": harvest,` |
| `tools/test_capture_vfx_oracle.py:25` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `("MC2_VFX_ORACLE_RENDER", "MC2_VFX_ORACLE_TUBE", "MC2_VFX_ORACLE_SHAPE", "MC2_VFX_GPU_SIM_CARDCLOUD", "MC2_VFX_GPU_SIM_COMPARE")) assert "--no-lease" not in cmd and "--show-window" not in cmd` |
| `tools/test_vfx_parity_oracle.py:102` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_ORACLE_TUBE": enabled, "MC2_VFX_ORACLE_SHAPE": enabled,` |
| `tools/vfx_oracle/lifecycle_driver.cpp:15` [Sol/unowned] | setter; expression/value shown | `ContractEnvironment tube{"MC2_VFX_ORACLE_TUBE", "0"}; ContractEnvironment shape{"MC2_VFX_ORACLE_SHAPE", "0"}; ContractEnvironment gpuSim{"MC2_VFX_GPU_SIM_CARDCLOUD", "0"};` |
| `tools/vfx_parity_oracle.py:128` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VFX_ORACLE_TUBE": harvest, "MC2_VFX_ORACLE_SHAPE": harvest,` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_particle_bridge.h:30` [L2]<br>`GameOS/gameos/gos_particle_bridge_internal.h:31` [L2].

<a id="e089"></a>
### E089 MC2_VIEWMODE_FRAMEWORK

Registry/alias declarations: `Gates/gate_index.gen.h:878` [L2]<br>`Gates/gate_table.cpp:1232,1233,1236` [L2]<br>`Gates/gate_table.json:867` [L2]<br>`RenderCore/RendererFeatureRegistry.h:130,371,372` [L2]<br>`docs/flags.auto.yaml:6663,6671` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/smoke/matrices/viewmode.json:3` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"description": "TRACKV-TACTICAL-PRESENTATION-OPUS-1 ViewMode presentation layer. All modes are selected by the postprocess composite via MC2_VIEWMODE_FRAMEWORK (gate, default-OFF) + MC2_VIEW_MODE (seed mode, matches RenderCore::ViewMode). Visual (0) is byte-identical to baseline. ObjectIdDebug needs MC2_OBJECT_ID_BUFFER (default-ON). Thermal is a luminance PLACEHOLDER, not real IR. TacticalOverlay is drawn in-scene by the debug renderer (separate gate), not by the composite.", "default_missions": ["mc2_03", "mc2_17", "mc2_24"], "default_duration": 30,` |
| `tests/smoke/matrices/viewmode.json:16` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VIEWMODE_FRAMEWORK": "1",` |
| `tests/smoke/matrices/viewmode.json:24` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VIEWMODE_FRAMEWORK": "1",` |
| `tests/smoke/matrices/viewmode.json:33` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VIEWMODE_FRAMEWORK": "1",` |
| `tests/smoke/matrices/viewmode.json:42` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VIEWMODE_FRAMEWORK": "1",` |
| `tests/smoke/matrices/viewmode.json:50` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VIEWMODE_FRAMEWORK": "1",` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_postprocess.cpp:608` [L2].

<a id="e090"></a>
### E090 MC2_VIEW_UNIFORMS

Registry/alias declarations: `Gates/gate_index.gen.h:880` [L2]<br>`Gates/gate_table.cpp:1240,1241` [L2]<br>`Gates/gate_table.json:869` [L2]<br>`RenderCore/RendererFeatureRegistry.h:109,315,316` [L2]<br>`docs/flags.auto.yaml:6681,6689` [Sol/unowned]. Named enum consumers: `tests/unit/test_rendercore.cpp:344` [L1].

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/shader_variants/static_prop.frag.splice-regression:23` [Sol/unowned] | schema/check/comment (not setter) | `// the include is suppressed when MC2_VIEW_UNIFORMS=0 (process-lifetime gate` |
| `tests/shader_variants/static_prop.frag.splice-regression:78` [Sol/unowned] | schema/check/comment (not setter) | `// force-uploads strength=0 when MC2_VIEW_UNIFORMS=0, complementing this` |
| `tests/telemetry/tag-registry.json:567` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"notes": "ViewUniforms UBO feature init emit (MC2_VIEW_UNIFORMS)." }, {` |
| `tests/telemetry/test_manifest_schema.py:109` [Sol/unowned] | value-bearing consumer; provisional role outside family | `env = {"MC2_GPU_CULL": "1", "MC2_VIEW_UNIFORMS": "0", "PATH": "x", "HOME": "y"}` |
| `tests/telemetry/test_manifest_schema.py:112` [Sol/unowned] | value-bearing consumer; provisional role outside family | `assert d == {"MC2_GPU_CULL": "1", "MC2_VIEW_UNIFORMS": "0"}` |
| `tests/unit/log_tag_baseline.json:4179` [L1] | historical/expected-output record (not setter) | `"MC2_VIEW_UNIFORMS"` |
| `tests/unit/printf_corpus.jsonl:937` [L1] | historical/expected-output record (not setter) | `{"files": ["GameOS/gameos/gos_static_prop_batcher_submit.cpp"], "new": "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\\n", "old": "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\\n", "specs": []}` |
| `tests/unit/printf_corpus_gen.inc:16308` [L1] | historical/expected-output record (not setter) | `TEST_CASE("printf corpus 0936: [GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP pat")` |
| `tests/unit/printf_corpus_gen.inc:16312` [L1] | historical/expected-output record (not setter) | `const int na = std::snprintf(a, sizeof a, "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\n");` |
| `tests/unit/printf_corpus_gen.inc:16313` [L1] | historical/expected-output record (not setter) | `const int nb = mc2::fmt::to_buf(b, "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\n");` |
| `tests/unit/printf_corpus_gen.inc:16316` [L1] | historical/expected-output record (not setter) | `const int nc = std::snprintf(c, sizeof c, "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\n");` |
| `tests/unit/printf_corpus_gen.inc:16317` [L1] | historical/expected-output record (not setter) | `const int nd = mc2::fmt::to_buf(d, "[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\n");` |
| `tests/unit/test_rendercore.cpp:347` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(std::strcmp(e.envVar, "MC2_VIEW_UNIFORMS") == 0); } ` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/gos_static_prop_batcher_submit.cpp:1073` [L2].

<a id="e091"></a>
### E091 MC2_VK_CSM_ARRAY_INTEROP

Registry/alias declarations: `Gates/gate_index.gen.h:898` [L2]<br>`Gates/gate_table.cpp:1260,1261` [L2]<br>`Gates/gate_table.json:887` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1499,1500` [L2]<br>`docs/flags.auto.yaml:6818,6826` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e092"></a>
### E092 MC2_VK_CSM_FULLMAP_INTEROP

Registry/alias declarations: `Gates/gate_index.gen.h:899` [L2]<br>`Gates/gate_table.cpp:1262,1263` [L2]<br>`Gates/gate_table.json:888` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1506,1507` [L2]<br>`docs/flags.auto.yaml:6827,6835` [Sol/unowned]. Named enum consumers: none found.
External name/alias setters and consumers: none found in repository text; provisional outside the reviewed family.

<a id="e093"></a>
### E093 MC2_VK_IMGUI

Registry/alias declarations: `Gates/gate_index.gen.h:905` [L2]<br>`Gates/gate_table.cpp:1272,1273` [L2]<br>`Gates/gate_table.json:894` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1527,1528` [L2]<br>`docs/flags.auto.yaml:6875,6883` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/run_vk_field_check.py:59` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"MC2_VK_PRESENT_SKIP_GL_HANDOFF", "MC2_VK_IMGUI", "MC2_VK_NO_GL_INTEROP_STRICT", "MC2_VISUAL_BOOKMARK_CAPTURE", "MC2_SMOKE_FIXED_TIMESTEP", "MC2_SMOKE_MODE",` |

<a id="e094"></a>
### E094 MC2_VK_SHADOW_INTEROP

Registry/alias declarations: `Gates/gate_index.gen.h:936` [L2]<br>`Gates/gate_table.cpp:1317,1318` [L2]<br>`Gates/gate_table.json:925` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1597,1598` [L2]<br>`docs/flags.auto.yaml:7122,7130` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tools/tracy_capture_compare.py:114` [Sol/unowned] | schema/check/comment (not setter) | `# MC2_VK_SHADOW_INTEROP) measured a PHANTOM config: MC2_VK_WATER_INTEROP is a dead` |
| `tools/tracy_capture_compare.py:116` [Sol/unowned] | schema/check/comment (not setter) | `# declined not-VK-owned and force-fell back to GL), and MC2_VK_SHADOW_INTEROP is an` |
| `tools/tracy_capture_compare.py:120` [Sol/unowned] | schema/check/comment (not setter) | `# OFF. Override per-capture with --env (e.g. --env MC2_VK_SHADOW_INTEROP=1).` |
| `tools/tracy_capture_compare.py:124` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VK_SHADOW_INTEROP": "0",` |
| `tools/tracy_capture_compare.py:967` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"defaults + the MC2_VK* scrub (e.g. --env MC2_VK_SHADOW_INTEROP=1). "` |

<a id="e095"></a>
### E095 MC2_VK_SHARED_GBUFFER

Registry/alias declarations: `Gates/gate_index.gen.h:938` [L2]<br>`Gates/gate_table.cpp:1322,1323` [L2]<br>`Gates/gate_table.json:927` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1611,1612` [L2]<br>`docs/flags.auto.yaml:7140,7148` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/check-vk-umbrella-readers.py:4` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `MC2_VK_SHARED_GBUFFER is the one VK family gate (gate amnesty wave 1 folded the former per-member Tri rows into it). It is read by one function, mc2vkgate::umbrellaOn() in mclib/vk_umbrella_gate.cpp, which adds the session fail-soft veto the static Gates library cannot see.` |
| `scripts/check-vk-umbrella-readers.py:9` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `MC2_VK_SHARED_GBUFFER outside mclib/vk_umbrella_gate.cpp, skips the veto, so a mid-session fail-soft would arm an island the GL present can never show. R2  PER-CALL READERS STAY UNCACHED. The sites in PER_CALL re-read umbrellaOn() on every call so that a` |
| `scripts/check-vk-umbrella-readers.py:213` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `_UMBRELLA_DIRECT_RE = re.compile(r'\b(?:get&#124;state&#124;isSet)\s*<\s*"MC2_VK_SHARED_GBUFFER"')  ` |
| `scripts/check-vk-umbrella-readers.py:226` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `errs.append("R1 %s:%d MC2_VK_SHARED_GBUFFER is read directly outside %s; use " "mc2vkgate::umbrellaOn() (it adds the fail-soft veto)" % (rel, line_of(code, m.start()), UMBRELLA_READER))` |
| `scripts/check-vk-umbrella-readers.py:332` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `{"a.cpp": 'bool f() { return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">(); }'}, check_direct_effective, True), ("R1 the umbrella reader may read it", {UMBRELLA_READER: 'bool umbrellaOn() { return gate::get<"MC2_VK_SHARED_GBUFFER">(); }'}, check_direct_effective, False),` |
| `scripts/check-vk-umbrella-readers.py:334` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `{UMBRELLA_READER: 'bool umbrellaOn() { return gate::get<"MC2_VK_SHARED_GBUFFER">(); }'}, check_direct_effective, False), ("R2 a static cache is caught", {gc: 'bool f() { static const bool s = mc2vkgate::umbrellaOn(); return s; }'}, cached, True),` |
| `scripts/check-vk-umbrella-readers.py:366` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `{UMBRELLA_READER: 'bool umbrellaOn() { static bool s = gate::get<"MC2_VK_SHARED_GBUFFER">(); return s; }'}, lambda t: [e for e in check_per_call(t) if "must hold no state" in e], True), ]` |
| `scripts/run_vk_field_check.py:5` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `plus the MC2_VK_SHARED_GBUFFER umbrella plus the launcher's default GL gates, and NOTHING ELSE. No runner-only strictness/present/composite gates. This is the config the tracy-vk-compare.bat "on" state and a real launcher-driven VK session actually` |
| `scripts/run_vk_field_check.py:51` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VK_SHARED_GBUFFER": "1",` |
| `tools/perf_snapshot.py:21` [Sol/unowned] | value-bearing consumer; provisional role outside family | `--mission mc2_01 --duration 20 --env MC2_VK_SHARED_GBUFFER=1` |
| `tools/tracy_capture_compare.py:40` [Sol/unowned] | value-bearing consumer; provisional role outside family | `* VK arm: MC2_RENDER_BACKEND=vulkan + MC2_VK_SHARED_GBUFFER=1, shadow interop OFF` |
| `tools/tracy_capture_compare.py:123` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"MC2_VK_SHARED_GBUFFER": "1",` |
| `tools/visual_verify/ab.py:162` [L2] | value-bearing consumer; provisional role outside family | `["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + a.env)` |
| `tools/visual_verify/capture.py:264` [L2] | value-bearing consumer; provisional role outside family | `"MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1. "` |
| `tools/visual_verify/capture_ab.py:9` [L2] | value-bearing consumer; provisional role outside family | `AND MC2_VK_SHARED_GBUFFER=1: the selector alone makes the engine force GL and the "VK" set would be a GL capture).` |
| `tools/visual_verify/capture_ab.py:115` [L2] | value-bearing consumer; provisional role outside family | `cmd += ["--env", "MC2_RENDER_BACKEND=vulkan", "--env", "MC2_VK_SHARED_GBUFFER=1"]` |
| `tools/visual_verify/test_capture_ab.py:52` [L2] | schema/check/comment (not setter) | `# Without MC2_VK_SHARED_GBUFFER=1 the engine forces GL for the session and the Vulkan arm is a GL capture.` |
| `tools/visual_verify/test_capture_ab.py:55` [L2] | value-bearing consumer; provisional role outside family | `assert "MC2_RENDER_BACKEND=vulkan" in vk and "MC2_VK_SHARED_GBUFFER=1" in vk` |
| `tools/visual_verify/test_capture_ab.py:56` [L2] | consumer/fixture reference; provisional role outside family | `assert not any("MC2_RENDER_BACKEND" in a or "MC2_VK_SHARED_GBUFFER" in a for a in gl)  ` |
| `tools/visual_verify/test_vk_replay.py:57` [L2] | value-bearing consumer; provisional role outside family | `assert declared == ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"]       # the backend alone falls back to GL` |
| `tools/visual_verify/vk_replay.py:38` [L2] | value-bearing consumer; provisional role outside family | `VULKAN_ENV = ("MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1")` |

<a id="e096"></a>
### E096 MC2_VK_TERRAIN_LODFADE

Registry/alias declarations: `Gates/gate_index.gen.h:945` [L2]<br>`Gates/gate_table.cpp:1334,1335` [L2]<br>`Gates/gate_table.json:934` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1632,1633` [L2]<br>`docs/flags.auto.yaml:7196,7204` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_gate.cpp:898` [L1] | consumer/fixture reference; provisional role outside family | `"MC2_VK_TERRAIN_LODFADE", "MC2_VK_TERRAIN_VISUALDISPLACE_FAR"}; const char* killSwitch[] = {"MC2_VK_LIGHTFEED_DEDUP", "MC2_VK_MECH_FENCE_DEFER", "MC2_VK_POST_SCREENFX_CLOUD",` |
| `tests/visual/bookmarks/mc2_01_terrain_near.json:24` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"_note": "VK-TERRAIN-LODFADE seam-pose: camDist (ground distance + 0.7*altBoost, terrain_tier1.frag mv_computeLodNear) placed near the middle of the fade band [LODFADE_NEAR=4000, LODFADE_NEAR_FADE=4500]. Same XY area as near_land_low/near_land_off (known height-stable ground on mc2_01 -- avoids the mc2_24 land-follow altitude-divergence pitfall) but pulled back+up so camDist ~= 4250wu, i.e. lodNear ~= 0.5 (mid-fade) when MC2_VK_TERRAIN_LODFADE=1.",` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `GameOS/gameos/vulkan_terrain_island.h:138` [L2].

<a id="e097"></a>
### E097 MC2_WATER_SHINE

Registry/alias declarations: `Gates/gate_index.gen.h:980` [L2]<br>`Gates/gate_table.cpp:1378,1379` [L2]<br>`Gates/gate_table.json:969` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1653,1654` [L2]<br>`docs/flags.auto.yaml:7463,7471` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_gate.cpp:1172` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(g1b::rowNamed("MC2_WATER_SHINE").kind == Kind::String); } ` |
| `tools/vk_not_ported.allowlist:73` [Sol/unowned] | schema/check/comment (not setter) | `MC2_WATER_SHINE &#124; shaders/gos_terrain_water_mdi.frag &#124; uniform u_waterSpecIntensity: no such name in the VK twin (shaders/vulkan/water_geom.*) shader, island C++ or gpu_abi` |
Additional name/conditional/diagnostic consumers (role provisional outside family): `shaders/gos_terrain_water_mdi.frag:119` [L2].

<a id="e098"></a>
### E098 MC2_WATER_SHORE_MASK

Registry/alias declarations: `Gates/gate_index.gen.h:983` [L2]<br>`Gates/gate_table.cpp:1384,1385` [L2]<br>`Gates/gate_table.json:972` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1660,1661` [L2]<br>`docs/flags.auto.yaml:7489,7497` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/build_release.sh:50` [Sol/unowned] | schema/check/comment (not setter) | `# MC2_WATER_SHORE_MASK (default-ON) degrades to a complete no-op.` |
| `scripts/check-beauty-sidecars.py:12` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `carry on. 'MC2_WATER_SHORE_MASK' in particular is DEFAULT-ON and degrades to a pure no-op when 'shoreline_mask.png' is absent. ` |
| `scripts/check-beauty-sidecars.py:754` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `"loaders fail SILENTLY on a missing sidecar -- MC2_WATER_SHORE_MASK " "is default-ON and no-ops entirely. Fix with --install, or delete " "the cook if it is not meant to ship.")` |
| `scripts/check-contracts.sh:1086` [Sol/unowned] | schema/check/comment (not setter) | `# MC2_WATER_SHORE_MASK reader in GameOS/gameos/gameos_graphics.cpp from` |
| `scripts/check-contracts.sh:1238` [Sol/unowned] | schema/check/comment (not setter) | `# loaders fail SILENTLY (one diagnostic line, then carry on). MC2_WATER_SHORE_MASK` |
| `scripts/delivery-fixes.json:156` [Sol/unowned] | schema/check/comment (not setter) | `"gate": "MC2_WATER_SHORE_MASK",` |
| `scripts/delivery-fixes.json:158` [Sol/unowned] | schema/check/comment (not setter) | `"exe_needle": "MC2_WATER_SHORE_MASK",` |
| `scripts/delivery-fixes.json:160` [Sol/unowned] | schema/check/comment (not setter) | `{"path": "GameOS/gameos/gameos_graphics.cpp", "needle": "MC2_WATER_SHORE_MASK"}` |
| `tools/terrain_beautify/README.md:90` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `prints one diagnostic line and carries on. 'MC2_WATER_SHORE_MASK' is **default-ON** and degrades to a complete no-op. Nothing crashes, nothing warns, and the feature you just cooked simply is not there. This has already shipped once (v0.5.2 carried` |

<a id="e099"></a>
### E099 MC2_WATER_SHORE_MEANDER

Registry/alias declarations: `Gates/gate_index.gen.h:984` [L2]<br>`Gates/gate_table.cpp:960,1377,1387,1388,1390` [L2]<br>`Gates/gate_table.json:973,974` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1667,1668,1678` [L2]<br>`docs/flags.auto.yaml:7498,7507,7513` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `scripts/tests/test_gl_vk_feature_coverage.py:336` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `allow = {("MC2_WATER_SHORE_MEANDER", "shaders/w.frag"): "meander missing in VK"} off, on = (cov.gate_defaults(None, seed_text(MC2_WATER_SHORE_MEANDER=v)) for v in (0.0, 75.0)) assert cov.flip_check(allow, off, on)` |
| `scripts/tests/test_gl_vk_feature_coverage.py:337` [Sol/unowned] | value-bearing consumer; provisional role outside family | `off, on = (cov.gate_defaults(None, seed_text(MC2_WATER_SHORE_MEANDER=v)) for v in (0.0, 75.0))` |
| `scripts/tests/test_gl_vk_feature_coverage.py:371` [Sol/unowned] | consumer/fixture reference; provisional role outside family | `for pair in (("MC2_WATER_SHORE_MEANDER", "shaders/gos_terrain_water_mdi.frag"),): assert pair in np_pairs allow = cov.load_allowlist(os.path.join(ROOT, cov.ALLOWLIST))` |
| `tests/visual/bookmarks/mc2_10_water.json:4` [Sol/unowned] | value-bearing consumer; provisional role outside family | `"_note": "WATER-MEANDER-CLAMP-1. THE mc2_10 WATER REGRESSION POSES. mc2_10 is the map whose MC2_WATER_SHORE_MEANDER=75 look the user APPROVED, and until this file no mc2_10 bookmark framed water at all (mc2_10_sunclamp / mc2_10_building_faces frame terrain and props), so no shoreline change on this map could be regression-checked. Coordinates are NOT guessed: the mission .pak MapData packet was decoded (side=120, half-extent 7616 wu, 128 wu/vertex) and the water mask taken as elev <= [Water] Elevation = 80. Each pose centres on a water/land boundary cell whose 25x25-cell neighbourhood is ~50% water, i.e. an actual coastline with open water on one side and land on the other -- not a shore-adjacent patch of empty ground. Wide full-frame framing at normal RTS gameplay altitude (alt 800-880, proj 30-34), matching mc2_10_sunclamp's conventions. Local shore &#124;grad&#124; at these three sites is 0.23 / 0.17 / 0.23 wu-elev per wu-horizontal, i.e. representative of the map's median (0.219), so they are the right places to judge whether a horizontal-distance clamp binds here.",` |
| `tools/vk_not_ported.allowlist:48` [Sol/unowned] | schema/check/comment (not setter) | `MC2_WATER_SHORE_MEANDER_FREQ &#124; shaders/gos_terrain_water_mdi.frag &#124; Companion of MC2_WATER_SHORE_MEANDER (u_shoreMeanderFreq): the VK water FS has no meander block, so the frequency knob has nothing to drive there. Inert in GL while the amplitude gate is 0.` |
| `tools/vk_not_ported.allowlist:74` [Sol/unowned] | schema/check/comment (not setter) | `MC2_WATER_SHORE_MEANDER &#124; shaders/gos_terrain_water_mdi.frag &#124; VK water_geom.frag has no shore-meander block (u_shoreMeanderWu fbm offset on WaterThickness), so VK shorelines stay straight where GL meanders once the amplitude is non-zero. Pinned as the waterBaseColor debt in tools/glsl_port_diff.allowlist.` |

<a id="e100"></a>
### E100 MC2_WATER_SHORE_MEANDER_FREQ

Registry/alias declarations: `Gates/gate_index.gen.h:985` [L2]<br>`Gates/gate_table.cpp:1389,1390` [L2]<br>`Gates/gate_table.json:974` [L2]<br>`RenderCore/RendererFeatureRegistry.h:1674,1675` [L2]<br>`docs/flags.auto.yaml:7508,7517` [Sol/unowned]. Named enum consumers: none found.

| File:line [owner] | Role | Source/value context |
|---|---|---|
| `tests/unit/test_gate.cpp:808` [L1] | consumer/fixture reference; provisional role outside family | `{"MC2_WATER_SHORE_MEANDER_FREQ", false, 0.0, kFltMax}, }; for (const Want& w : want) {` |
| `tests/unit/test_gate.cpp:819` [L1] | consumer/fixture reference; provisional role outside family | `CHECK(g1b::rowNamed("MC2_WATER_SHORE_MEANDER_FREQ").defFloat == 1.0f / 384.0f); // The old reader tested 's > 0.05f' in float: exactly 0.05 is rejected, the next float up is accepted. const gate::Row& dist = g1b::rowNamed("MC2_TERRAIN_LOD_CHUNK_DIST_SCALE");` |
| `tools/vk_not_ported.allowlist:48` [Sol/unowned] | schema/check/comment (not setter) | `MC2_WATER_SHORE_MEANDER_FREQ &#124; shaders/gos_terrain_water_mdi.frag &#124; Companion of MC2_WATER_SHORE_MEANDER (u_shoreMeanderFreq): the VK water FS has no meander block, so the frequency knob has nothing to drive there. Inert in GL while the amplitude gate is 0.` |

## Proof and acceptance checkpoint

- `git status --short`, `git branch --show-current`, `git rev-parse HEAD`: expected branch at the SHA above; only pre-existing .scratch/ untracked before this task.
- `git ls-files --eol docs/flags.auto.yaml .claude/cleanup/gate-amnesty-wave1.md`: tracked inputs LF; new report/scripts use UTF-8 LF.
- `py -3 .scratch/ga-wave3/collect.py`: fresh registry/typed/raw/alias/enum/all-text inventory; 100 Feature, 980 total. The report and verifier exclude scratch and this report from deletion evidence.
- `py -3 .scratch/ga-wave3/build_report.py`: render all 100 inventory rows, 12 final family rulings, source/value appendix and owner-bound batches.
- `py -3 .scratch/ga-wave3/verify.py`: **PASS**: 100/100 inventory, 12 reviewed/11 DELETE/88 provisional, 4381 input hashes unchanged, independent family ripgrep 277 matching lines, arithmetic and UTF-8 LF. Re-run after the final report generation.
- `git diff --no-index --check -- NUL .claude/cleanup/gate-amnesty-wave3.md`: no whitespace diagnostics (exit 1 denotes the new-file diff); `git diff --stat` has no tracked changes; final git status shows only this report plus scratch untracked.
- `py -3 tools/gate_gen.py --check`: **exit 1, pre-existing drift**: MC2_CONTRACT_REPO_ROOT and MC2_IBL_ASSET_STRICT marked verified but no gate::get<> reader. Neither is a Feature candidate here. No sync/write performed in audit-only scope.
- Read-only Get-Content/rg/Python source checks: handoff rules + newest 20 entries; wave-1/deletion rulings; selected family branches, alias/enum/generic consumers, conditional CMake lists, launcher rows and setter values. Two Windows wildcard-path rg attempts and one wrong GameOS CMake path were corrected with -g and the actual gameos/src CMake locations; no absence verdict uses their errors. Get-CimInstance process inspection was denied by the sandbox and was not needed.
- Not run: Git Bash check-contracts (forbidden); git add/commit (index unavailable, orchestrator owns commits); engine builds/doctest/deploy/runtime smoke (no engine change in this audit). No compiler probe or launch was started.

Before any future deletion lands: orchestrator reviews the owner-bound diff, records W2/VETO decisions, commits exact pathspecs, regenerates gate outputs and any line-sensitive shader manifests, runs offline contracts/doctest/all affected targets, then builds the committed fingerprint, deploys only lane3 and runs the required GL/VK day/night/runtime proofs. Do not regenerate tests/callers.json, tests/unit/log_tag_baseline.json or docs/background-launch-inventory.md in this task; their eventual line/census diffs belong to merge regeneration.

Scratch created and retained: `.scratch/ga-wave3/collect.py`, `.scratch/ga-wave3/evidence.json`, `.scratch/ga-wave3/build_report.py`, `.scratch/ga-wave3/verify.py`. No files deleted. Existing .scratch contents were left alone.

Proposed documentation commit: `docs: audit gate amnesty wave 3`; exact pathspec `.claude/cleanup/gate-amnesty-wave3.md`; trailer `Co-Authored-By: GPT-6 <noreply@openai.com>`. Scratch is evidence for review/reproduction, not part of the proposed commit.
