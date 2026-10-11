<!-- doc-status: current | reviewed:2026-10-11 -->
# LG14 design note: unscoped enum -> `enum class` + `using enum` (lane 2, tip 90bfbfed9). NO BATCH RUN; waiting for review.

WHAT was measured: 178 named enums with a body (the 101 anonymous and 101 typedef enums counted at rotation stay: constants / aliases). The token census passes 68; 9 of those are never-touch; the other 59 were PROBED by compile:
all 59 rewritten at once (`rewrite_text`, scratch `.claude/l2cfg/enum_probe.py`), built in `build64-ninja` AND the all-targets root tree (game, editor, tools, tests; 2 x ~15 min), errors mapped to enums, tree restored (git clean).
WHY a probe: a scoped enum turns every int use into a compile error and changes nothing else (underlying type stays int / the fixed type), so "does it compile" IS the arithmetic / index / int-API / assignment detector; the objhash proof then confirms the object is unchanged.
RULE: SKIP when the value is used as arithmetic, an array index, a case label of an int, serialized (FitIni `writeId*`, record fields), passed through an int/DWORD API, or mirrored in a shader. Everything compile-clean and not in those groups converts.

## CONVERT (strict plain objhash, scripted by `rewrite_enums.py`): 23 enums + 13 deferred
- Narrow (<= 150 includers, 19 enums / 15 files): FireExecutorKind, FireExecutorResolution (fire_executor_registry.h); CohRule (mission_update_audit.cpp); obj_category_type (ObjectivesSummaryPanel.cpp); DigestKind (sim_identity.h);
  SEEK_TYPE (stream.h); control_species_data_type (mpparameterscreen.h); status_type (optionsscreenwrapper.h); GoalType, GoalLinkType (goal.h); ClassPolicy (object_light_row.h, CPU-side policy tag, no shader mirror);
  EquipmentAddFillDecision, EquipmentUiStringKind (equipment_registry.h); EquipmentTag (equipment_definition.h); UnitProfileReasonCode (unit_profile_validator.h); FormationLineState (tacticaloverview.h);
  list_id_type, pick_target_type (editor/Objective.h); FeatureMask (terrain_runtime.h).
- Medium (172-173 includers, needs `--max-header-includers 200`): Capability (capability.h), CarnageEnumType (dcarnage.h).
- Hold, 0 tracked including objects (only the unit-test / harness targets include them, so the rewriter has nothing to compare): ActorState (actor.h), EquipmentReasonCode (equipment_validator.h).
- Deferred, wide header: MC_TextureKey (txmmgr.h, 574 includers) and the 12 compile-clean GameOS enums (gameos.hpp 820, toolos.hpp 694 includers): gosVideo_Command, gosVideo_PlayMode, gosAudio_SpeakerConfig,
  gosEnum_KeyDeviceType, GOSJoystickAxis, gosForce, gosType, gosSetting, MachineInfo, gosThreadPriority, gosEnum_FileWriteStatus, gosEnum_FileSeekType. A 700-object strict proof is possible but `editor/MapGeneratorDialog`
  drops its stb tables under any header edit (ledger), and these are the public platform API with a Linux/macOS future (ruling 4): needs an explicit proof-mode ruling, otherwise SKIP.

## SKIP (23 + 9 never-touch), reason per enum
- Int / DWORD API boundary: gos_AlphaMode (101 errors), gos_FilterMode (80), gos_TextureAddressMode (42), gos_ShadeAddressMode (44), gos_CullMode (14), gos_MutitextureModes (5): every value is passed as `gos_SetRenderState(state, int)`;
  gos_CompareMode, gos_StencilFunctions: stored into `uint32_t` state words; DeathCause (mover.h): `GroundVehicle::disable(unsigned long)` and the mech/vehicle callers; CgPhase (controlgui_update.cpp): `CgScope(int)` ctor.
- Serialized / record field: LOGISTICS_COMPONENT_LOCATION (written with `FitIniFile::writeIdLong`); MARKER_TYPES (assigned to an int member of the objective record); RadioNoise (`uint32_t` radio message field);
  ComponentFormOrdinal (int `formOrdinal` field + int `case` labels, 56 errors).
- Array index / arithmetic / int compare: SamplerType (`table[type]`, cast from GL constants); SpecialtySkillType (indexes the skill arrays); SORT_ORDERS (int `case` labels, 10 errors); EPreset (`i < PRESET_COUNT`, `sel >= PRESET_COUNT`
  through the qualified name, which the token census cannot see); CameraClass (returned as `long`); LegacyGestureId (int initializer lists of `AnimFallbackChain`).
- Shader / GPU mirror: GpuActorCategory + GpuActorFlags (gpu_cull_record.h, mirrored by `shaders/gpu_cull.comp`, flags OR-ed into a `uint32_t` record), TerrainMaterialProfile (stored in an `int`, constants in `terrain_common.hglsl`).
- Never-touch (rule, not compile): ProcType (mechcmd2_internal.h), MissionType (EditorData_internal.h), NETMESSAGETYPE (multplyr.h), FileClass (file.h), FilterMode / TextureWrapMode / WireFrameMode (mlr), WriteStatus / SeekType (stuff).
- The other 110 census refusals stay as they are: arithmetic-or-index 66, named outside C++ 20, defined twice 19, declarator after body 3, enumerator name shared 2.

## Findings about the tool (1 and 2 fixed in 38e5a1dd9; none of it touches a proof default)
1. The token census misses qualified uses (`Cls::PRESET_COUNT` has `::` on its left), so it over-reports candidates (EPreset); the compile proof catches it, a batch only wastes a bisect. Fixed: the qualifier is stripped before the left-context test (census 68 -> 65 candidates).
2. `rewrite_enums.py` works per FILE: equipment_registry.h holds one failing enum (ComponentFormOrdinal) next to two clean ones, so the whole file would be reverted. Fixed: `tools/tidy/enum_skip.txt` (name + reason, 23 lines) is read by `run` and `census`, `--skip-enums` overrides it.
3. The probe built the game, editor and tools, not the standalone harnesses and `mc2_tests`; the batch's `build_all_targets.py` stage 3 covers them (ActorState and EquipmentReasonCode are only reachable from them).
4. Elaborated `enum Name x;` on a scoped enum (gameos.hpp: `enum gosVideo_Command vc`) compiles on MSVC; it is a portability point for the deferred GameOS group only.
5. Serialization: a scan of the 36 compile-clean enums found no `write*` / `Save*` / packet call next to a variable of these types; the layout is unchanged anyway (same underlying type, objhash identical), and the `saveload_roundtrip` row runs after each batch.

## Proposal for review
Batch A (strict, one commit per header-size tier): 19 narrow enums; Batch B: Capability + CarnageEnumType with `--max-header-includers 200`; then the ratchet LG14 drops by the number kept. Wide/GameOS: your ruling (relaxed proof with a reason, or SKIP).
Expected floor after A+B: about 21 fewer unscoped named enums of 178; the 23 skipped and 9 never-touch enums are listed above as the documented floor.

## Results after the coordinator's rulings (2026-10-11, strict plain objhash, `--vs-gate`: Ninja trees + mc2_tests + harness trees, <= 10 enums per commit)
- CONVERTED 18 enums in 4 commits (929ecc662 5, cab34b9e2 6, edb92d936 1, b31a71d36 6): SEEK_TYPE, CohRule, control_species_data_type, status_type, obj_category_type; GoalType, GoalLinkType, FormationLineState, list_id_type,
  pick_target_type, FeatureMask; CarnageEnumType; EquipmentTag, EquipmentAddFillDecision, EquipmentUiStringKind, FireExecutorKind, FireExecutorResolution, DigestKind. Every including object IDENTICAL (Ninja trees and VS trees).
- The new harness / unit-test gate (`tools/tidy/vs_trees.py`, `proof_ext.Gate`) refused 4 more: EquipmentReasonCode, UnitProfileReasonCode, Capability (tests use `X__COUNT` as array bound, loop limit and index) and ClassPolicy (the
  standalone object_light_row_harness is not a C++20 build: `using enum` is C7572). Converting them means casts in 16+ test lines and `seen[code]` index sites: contortion, so they join the skip ledger. ActorState: `code/actor.h` is
  included by no translation unit in any tree, so there is nothing to prove and nothing to gain (dead header, deletion candidate); skip ledger too.
- The 13 deferred (`gos*`, `MC_TextureKey`) are on the skip ledger by ruling. Final floor: 41 enums on `tools/tidy/enum_skip.txt`, 9 never-touch, the rest census-refused.
- First gate run blamed every object of a failed target (it reverted 4 good headers); fixed in de31eebb1 (a failed target blames only the objects its errors name) and the 6 innocent enums went through in b31a71d36.
