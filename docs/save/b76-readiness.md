# B7.6 readiness census (save schema v1 codecs)

Design: `.claude/cleanup/cxx23-step4-save-schema-design.md` (record tables lines 26-148, batches 201-211).
Census at nifty 84f3ded43. "Writer today" is the code that produces the bytes in a shipping save;
no codec below is the sole writer (B7.6/B7.7 cutover not done; it needs the owner repin note).
Codecs marked "private" are called by tests and the loader's value path only. Line numbers are file starts, not exact sites.

| Design row | Codec (file) | Roundtrip doctest | Writer today |
|---|---|---|---|
| Clock / combat clock | yes, `code/combat_clock_archive.h` | `tests/unit/test_save_sections.cpp` | codec (packet 3, `code/saveload.cpp:248-252`) |
| SimRules / TickSchedule | no | no | none (no schedule registry yet) |
| RngState / RngStream / AblReseed | yes, `code/sim_rng.h:29-53`, `sim_rng.cpp:89` | `test_sim_rng.cpp` | no save writer found in `Mission::save` (`saveload.cpp:118-390`) |
| Navigation: MissionMap, GlobalMap, GlobalArea/Door/Link, MapCell | yes, `code/navigation_save_data.h` (tags MCEL/GARE/GDOR/GLNK) | `test_navigation_archive.cpp` | legacy `MOVE_saveData` (`saveload.cpp:259`) plus empty tag sections (`saveload.cpp:335-338`) |
| PathManager / PathRequest | partial: `code/save_fix_section.h:38` (FixPathRequest, optional FIX1) | `test_save_sections.cpp` | codec, dual-written (`saveload.cpp:384-385`) |
| ObjectManager / WatchEntry | yes, `code/object_manager_save_data.h`, `watch_list_save_data.h` | `test_object_manager_archive.cpp`, `test_watch_list_archive.cpp` | legacy `ObjectManager->Save` (`saveload.cpp:294`); tag sections empty markers |
| GameObject..WeaponBolt leaf objects (11 types) | yes, `code/{object,mech,mover,vehicle,terrain,building,turret,gate,artillery,carnage,weaponbolt}_value_codec.h`, `object_slot_codec.h` | `test_object_value_codec.cpp` | legacy per-class Save; OBJS marker section (`saveload.cpp:345`) |
| MechAnimation / VehicleAnimation | yes, `mclib/unit_anim_state.h` | `test_save_sections.cpp` | codec (ANIM v3, `saveload.cpp:367`) |
| Warrior / WarriorStatic / WarriorPilot | yes, `code/warrior_value_codec.h`, `warrior_pilot_codec.h` | `test_warrior_archive.cpp`, `test_warrior_pilot_codec.cpp` | legacy `MechWarrior::Save` (`saveload.cpp:288`) |
| TacticalOrder / OrderExecution / OrderIngress | yes, `code/tactical_order_queue.h`, `tacorder_terrain_queue.h` | `test_tactical_order_queue.cpp` | codec (ORDQ, `saveload.cpp:377-380`) |
| BrainRuntime / BrainTaskQueue / VarStore | yes, `code/save_brain_section.cpp` | `test_brain_save_policy_seam.cpp` | codec, optional BRN1 (`saveload.cpp:382-383`) |
| Team / TeamStatic | yes, `code/team_save_data.h` | `test_team_archive.cpp` | legacy `Team::Save` (`saveload.cpp:292`) |
| Commander / MoverGroup | yes, `code/commander_save_data.h` | `test_commander_archive.cpp` | legacy `Commander::Save` (`saveload.cpp:298`) |
| Contacts / Sensor / TeamSensor / ContactInfo / Ecm | no | no | none found in `Mission::save`; contacts are rescanned on load (design says this is wrong) |
| ABL modules / values / instances | yes, `mclib/abl_module_archive.h`, `abl_value_archive.h` | `test_abl_archive.cpp`, `test_abl_module_archive.cpp` | legacy `ABLi_saveEnvironment` (`saveload.cpp:270`) |
| ABL environment (libs/modules/files) | yes, `mclib/abl_environment_archive.h` | `test_abl_environment_archive.cpp` | marker section only (`saveload.cpp:348`); bytes from legacy |
| Mission / Part | Part yes, `code/mission_parts_archive.cpp`, `save_archive.h:270`; Mission record no | `test_part_archive.cpp` (Part only) | legacy FIT text (`Part::Save` `saveload.cpp:42`, mission block 191-210) |
| ObjectiveSet / BooleanFlag / Objective / Condition / Action | no | no | legacy FIT (`Team::home->objectives.Save`, `saveload.cpp:231`; `code/objective.cpp` `Save` methods) |
| SupportCraft / support state | no (state in `code/missiongui.h:160-171`) | no | `missionInterface->Save` FIT (`saveload.cpp:219`) |
| Logistics / Campaign / Variant / Pilot / Mech | no | no | legacy FIT, `LogisticsData::save` (`code/logisticsdata_save.cpp:124`) |
| MediaCueState / LogicalRequest / CueTrajectory | yes (new, this lane), `code/media_cue_archive.h` | `tests/unit/test_media_cue_archive.cpp` | none; no runtime owner exists yet |
| Environment (weather schedule) | no | no | legacy FIT, `Weather::save` (`saveload.cpp:223`) |
| Presentation / ObjectVisual / RenderWeather | no | no | legacy camera/FIT (`eye->save` `saveload.cpp:182`) |
| FIX1 music/backoff extras | yes, `code/save_fix_section.h:24-71` | `test_save_sections.cpp` | codec, optional (`saveload.cpp:384-385`) |

Missing codecs (cheapest first): MediaCueState (landed here, pure values), Environment, SupportCraft, ObjectiveSet family
(large: three payload variants), Logistics family, Contacts family, Mission record, SimRules, Presentation.
Enum values in the new media/cue codec are provisional until B7.5 fixes the runtime kinds.
