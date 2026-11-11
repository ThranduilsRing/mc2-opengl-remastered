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
| Contacts / Sensor / TeamSensor / ContactInfo / Ecm | yes (private), `code/contacts_archive.h` (tag CNTC) | `tests/unit/test_contacts_archive.cpp` | none found in `Mission::save`; contacts are rescanned on load (design says this is wrong) |
| ABL modules / values / instances | yes, `mclib/abl_module_archive.h`, `abl_value_archive.h` | `test_abl_archive.cpp`, `test_abl_module_archive.cpp` | legacy `ABLi_saveEnvironment` (`saveload.cpp:270`) |
| ABL environment (libs/modules/files) | yes, `mclib/abl_environment_archive.h` | `test_abl_environment_archive.cpp` | marker section only (`saveload.cpp:348`); bytes from legacy |
| Mission / Part | Part yes, `code/mission_parts_archive.cpp`, `save_archive.h:270`; Mission record no | `test_part_archive.cpp` (Part only) | legacy FIT text (`Part::Save` `saveload.cpp:42`, mission block 191-210) |
| ObjectiveSet / BooleanFlag / Objective / Condition / Action | yes (private), `code/objective_archive.h` (tag OBJS) | `tests/unit/test_objective_archive.cpp` | legacy FIT (`Team::home->objectives.Save`, `saveload.cpp:231`; `code/objective.cpp` `Save` methods) |
| SupportCraft / support state | yes (private), `code/support_craft_archive.h` (state in `code/missiongui.h:160-171`) | `tests/unit/test_environment_archive.cpp` | `missionInterface->Save` FIT (`saveload.cpp:219`) |
| Logistics / Campaign / Variant / Pilot / Mech | no | no | legacy FIT, `LogisticsData::save` (`code/logisticsdata_save.cpp:124`) |
| MediaCueState / LogicalRequest / CueTrajectory | yes (new, this lane), `code/media_cue_archive.h` | `tests/unit/test_media_cue_archive.cpp` | none; no runtime owner exists yet |
| Environment (weather schedule) | yes (private), `code/environment_archive.h` | `tests/unit/test_environment_archive.cpp` | legacy FIT, `Weather::save` (`saveload.cpp:223`) |
| Presentation / ObjectVisual / RenderWeather | no | no | legacy camera/FIT (`eye->save` `saveload.cpp:182`) |
| FIX1 music/backoff extras | yes, `code/save_fix_section.h:24-71` | `test_save_sections.cpp` | codec, optional (`saveload.cpp:384-385`) |

Missing codecs (cheapest first; MediaCueState, Environment, SupportCraft, Contacts and ObjectiveSet are landed, private and unused by the save path): Logistics family, Mission record, SimRules, Presentation.

## Schema rows from the code that saves and loads today

Contacts (landed): `code/contact.h` ContactInfo `:51` (contactStatus[MAX_TEAMS] u8 < 6, contactCount[MAX_TEAMS] u16, sensors[MAX_SENSORS] u16 with
0xFFFF = none, teams[MAX_TEAMS] u16, teamSpotter[MAX_TEAMS] u32), SensorSystem `:133` (id, master team + masterIndex, owner, range, skill, broken, notShutdown,
hasLOSCapability, ecmEffect, nextScanUpdate, lastScanUpdate, contacts[numContacts] mover ids, numExclusives, totalContacts), TeamSensorSystem `:258` (teamId,
nextContactId, numContactUpdatesPerPass, curContactUpdate, contacts[], numEnemyContacts, sensors[], ecms/jammers SystemTracker lists `code/team.h:40`: owner,
masterId, effect), SensorSystemManager `:345` (sensor pool + free list, teamSensors[MAX_TEAMS], ecms[MAX_ECMS], teamToUpdate, static updateFrequency,
enemyInLOS; TeamSensorSystem::homeTeamInContact and SensorSystem::scanFrequency are statics). The codec encodes ContactInfo.sensors sparsely and validates ids,
ordering, free-list disjointness and team-to-sensor references. Not encoded: the intrusive prev/next pointers and sensor owner back pointers (rebuilt on restore).

ObjectiveSet (landed, private; FIT writer `code/objective.cpp:2214-2420`; codec `code/objective_archive.h`): per team `CObjectives::Save` writes Version 3, NumObjectives, the
BooleanArray flags (`boolFlags.save`: id + bool per flag, ids unique ignoring ASCII case), then per CObjective: title/description text plus UseResourceString and ResourceStringID pairs, priority (1 primary, 2 secondary),
resourcePoints, previousPrimaryMustBeComplete, allPreviousPrimariesMustBeComplete, displayMarker, markerX/Y, hiddenTrigger, activateOnFlag + activateFlagID, resetStatusOnFlag +
resetStatusFlagID, counts of the four lists (conditions, actions, failureConditions, failureActions), model name, modelType, baseColor, highlightColor(2),
modelScale, state (isActive, activationTime double, resolved, resolvedStatus 0..2, changedStatus). Condition payload by species (`code/objective.h:43`, 24 species):
none (species 0, 5, 10: destroy / capture-or-destroy / dead-or-fled ALL), num u32 (1, 6, 11), unit watch id (3, 8, 13, 14, 16: specific unit, CAPTURE_UNIT, GUARD_SPECIFIC_UNIT), structure watch id (4, 9, 15, 17),
area (18-21: centerX, centerY, radius floats), flag (22: id string + bool), elapsed time (23: float). Action payload by species (`:342`, 7 species): path string (0 BIK, 1 WAV),
message string (2), resourceStringID u32 (3), flag (4: id + bool), purchase file string (5), building WID (6, _REMOVE_STRUCTURE).
Corrections made while writing the codec: (1) the earlier schema row listed the three *_ENEMY_UNIT_GROUP species (2, 7, 12) under "none"; they have no class and no factory case
(`CObjective::new_CObjectiveCondition`, `code/objective.cpp:1347`), so a live objective never holds one and the codec rejects them. (2) `m_displayMarker` is an int (MARKER_TYPES 0..2) but the FIT writer
stores it with `writeIdBoolean`, so the legacy file collapses NAV/NUMERIC to true; the codec keeps the 0..2 value. (3) `m_markerText` (char[4]) is derived on load and not stored.
Validation: species/priority/status/marker ranges, watch ids > 0 (the writers STOP on a null target), non-negative radius/time/scale/activation time, text <= 2000 bytes (`objective_text::kMaxTextLength`), flag-trigger boolean consistent with its flag id, unique flag ids, ascending unique team alignment < 8, species-foreign payload fields must be default (canonical re-encode).
Flag references (activate/reset/condition/action flag ids) are not checked against the flag table: `setElementValue` adds a flag on demand and `getElementValue` reads an absent flag as false, so a reference to an unlisted flag is legal.
Not encoded: observation-only fields (`observationOrdinal`, `observationPayload`, `m_fitOrdinal`, `m_observationState`) and the object-manager existence check of the watch ids (restore-time, needs the live object table).
Enum values in the new media/cue codec are provisional until B7.5 fixes the runtime kinds.
