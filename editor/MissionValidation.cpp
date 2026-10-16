/***************************************************************
* FILENAME: MissionValidation.cpp
* DESCRIPTION: Mission save-readiness checks and ImGui checklist control surface.
* DATE: 2026-06-07
****************************************************************/

#include "stdafx.h"
#include "MissionValidation.h"
#include "GuiRuntime.h"   // GuiRuntime::AutoDockActive (skip SetNextWindowPos when docking)
#include "EditorData.h"
#include "EditorObjectMgr.h"   // live unit list for staffing checks
#include "EditorObjects.h"
#include "MCLib.h"        // extern TerrainPtr land

#ifdef MC2_IMGUI
#include <imgui.h>
#endif

// ---------------------------------------------------------------------------
// ValidateForPakSave
// ---------------------------------------------------------------------------

std::vector<MissionCheck> MissionValidator::ValidateForPakSave() {
    std::vector<MissionCheck> checks;

    auto push = [&](const char* id, const char* label,
                    MissionCheckSeverity sev, bool passed,
                    ChecklistAction action, std::string details) {
        MissionCheck c;
        c.id      = id;
        c.label   = label;
        c.severity = sev;
        c.passed  = passed;
        c.action  = action;
        c.details = std::move(details);
        checks.push_back(c);
    };

    // 1. Terrain loaded -- BLOCKING
    //    land is the global TerrainPtr (extern in MCLib.h / terrain.h).
    //    Save() crashes at land->realVerticesMapSide when land is null.
    bool terrainLoaded = (land != nullptr);
    push("terrain_loaded",
         "Terrain is loaded",
         MissionCheckSeverity::Blocking,
         terrainLoaded,
         ChecklistAction::OpenMapGenerator,
         terrainLoaded
             ? "Terrain data is in memory.  Save can proceed."
             : "No terrain is loaded.  Generate or open a map first.");

    if (!terrainLoaded)
        return checks;

    // 2. Map has a real save path -- INFO
    //    Default 'newmap' path makes Save() redirect to SaveAs() (file dialog).
    //    Not a hard block, but surfaced so users know what to expect.
    const char* mapName = EditorData::instance->getMapName();
    bool hasPath = mapName && strcmp(mapName, "data\\missions\\newmap.pak") != 0;
    push("save_path",
         "Map has a save path",
         MissionCheckSeverity::Info,
         hasPath,
         ChecklistAction::OpenSaveAs,
         hasPath
             ? (std::string("Save path: ") + mapName)
             : "Using the default 'newmap' path.  File > Save (Ctrl+S) will open a "
               "Save As dialog to let you choose a filename before writing the .pak.");

    // 3. MOVE pathfinding data -- two states:
    //    a) Map too large for legacy MOVE  → Info (not a failure; expected for generated maps)
    //    b) Map within range but not built → Warning with "Build MOVE" button
    //
    //    Legacy MOVE grid limit: moveSide = cellSide * MAPCELL_DIM(3) <= MAX_MAP_CELL_WIDTH(720)
    //    → max supported cellSide = 240 cells/side.
    //    Generated maps at 260/520/1020 cells all exceed this and should never see
    //    a "Build MOVE" button — just a clear explanation of why MOVE is unavailable.
    {
        const int cellSide  = land->realVerticesMapSide - 1;
        const int moveSide  = cellSide * MAPCELL_DIM;     // MAPCELL_DIM = 3
        const int maxCellSide = MAX_MAP_CELL_WIDTH / MAPCELL_DIM;  // 240
        const bool tooLarge = (moveSide > MAX_MAP_CELL_WIDTH);
        const bool moveReady = EditorData::IsMoveDataReadyForFullSave();

        if (tooLarge) {
            // Informational — not a blocking or warning failure, just a capability note.
            // No "Build MOVE" button: clicking it would do nothing (size gate in
            // RebuildMoveFromCurrentTerrain will reject it and log an error).
            char details[512];
            snprintf(details, sizeof(details),
                "Legacy AI navigation (MOVE) is not supported for maps this large.\n\n"
                "This map:    %d x %d cells  (MOVE grid would be %d x %d)\n"
                "Legacy limit: %d cells/side  (MOVE grid <= %d x %d)\n\n"
                "Terrain editing and terrain save/load still work normally.\n"
                "Future chunked navigation will support large generated maps.",
                cellSide, cellSide, moveSide, moveSide,
                maxCellSide, MAX_MAP_CELL_WIDTH, MAX_MAP_CELL_WIDTH);
            push("move_data_ready",
                 "Legacy MOVE unsupported for this map size",
                 MissionCheckSeverity::Info,
                 false,   // not passed, so [i] icon shows; but Info = not counted as warning
                 ChecklistAction::None,
                 details);
        } else {
            // Map is within legacy MOVE range.  Warn if not built, OK if ready.
            char details[512];
            if (moveReady) {
                snprintf(details, sizeof(details),
                    "MOVE pathfinding data is initialized (GameMap + GlobalMoveMap ready).\n"
                    "Map: %d x %d cells -- MOVE grid: %d x %d.",
                    cellSide, cellSide, moveSide, moveSide);
            } else {
                snprintf(details, sizeof(details),
                    "MOVE pathfinding data is not built yet for this terrain.\n"
                    "AI movement and the passability grid both require it.\n\n"
                    "This map: %d x %d cells (within the %d-cell legacy MOVE limit).\n\n"
                    "Click 'Build MOVE' to initialize it in-place -- no save needed.\n"
                    "MOVE is also built automatically on every non-quickSave.",
                    cellSide, cellSide, maxCellSide);
            }
            push("move_data_ready",
                 "MOVE pathfinding data ready",
                 MissionCheckSeverity::Warning,
                 moveReady,
                 moveReady ? ChecklistAction::None : ChecklistAction::BuildMove,
                 details);
        }
    }

    // 4. Objectives have conditions -- WARNING
    //    Same check the editor shows as a MessageBox during save.  Surfaced here
    //    so users can fix it before save rather than being surprised mid-save.
    bool objsOk = !EditorData::instance->TeamsRef().ThereAreObjectivesWithNoConditions();
    push("objectives_conditions",
         "All objectives have conditions",
         MissionCheckSeverity::Warning,
         objsOk,
         ChecklistAction::OpenObjectives,
         objsOk
             ? "All objectives have at least one condition (or there are no objectives)."
             : "One or more objectives have no conditions -- they can never be completed.\n\n"
               "Open Mission > Teams, select the objective, and add at least one condition.  "
               "Or remove objectives that are not yet ready.");

    // 5. Unit staffing -- WARNING (mirrors the save-time EMessageBox warnings in
    //    EditorObjectMgr::saveMechs: no enemy units / no player units / players
    //    with no units / too many pilots). These are surfaced live here so modders
    //    see them before Save instead of as a modal mid-save. Non-blocking: a save
    //    still works; the mission just may not be playable as intended.
    //
    //    Units bucket to a player by alignment (0..7), exactly as saveMechs does.
    {
        int perPlayer[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int totalUnits = 0;
        if (EditorObjectMgr::instance()) {
            EditorObjectMgr::UNIT_LIST units = EditorObjectMgr::instance()->getUnits();
            for (auto it = units.begin(); it != units.end(); it++) {
                Unit* u = (*it);
                if (!u || !u->appearance())   // alignment read is unsafe without appearance
                    continue;
                ++totalUnits;
                int a = u->getAlignment();
                if (a >= 0 && a < 8)
                    ++perPlayer[a];
            }
        }

        const bool singlePlayer = EditorData::instance && EditorData::instance->IsSinglePlayer();

        if (singlePlayer) {
            // Enemy units: any non-user player (default team != user's team 0) with >0 units.
            bool enemyFound = false;
            if (EditorData::instance) {
                for (int p = 1; p < 8; ++p) {
                    if (EditorData::instance->PlayersRef().PlayerRef(p).DefaultTeam() != 0
                        && perPlayer[p] > 0) {
                        enemyFound = true;
                        break;
                    }
                }
            }
            push("enemy_units",
                 "Mission has enemy units",
                 MissionCheckSeverity::Warning,
                 enemyFound,
                 ChecklistAction::None,
                 enemyFound
                     ? "At least one enemy (non-player-team) unit is placed."
                     : "No enemy units are placed.  A single-player mission with no enemies "
                       "has nothing to fight.  Place units for a player whose default team is "
                       "not the player's team.");

            const bool playerStaffed = (perPlayer[0] > 0);
            push("player_units",
                 "Player has units",
                 MissionCheckSeverity::Warning,
                 playerStaffed,
                 ChecklistAction::None,
                 playerStaffed
                     ? "The player (team 0) has at least one unit."
                     : "The player (team 0) has no units.  The mission will start with nothing "
                       "for the player to command.  Place at least one player unit.");
        } else {
            // Multiplayer: every active player (0 .. MaxPlayers-1) should be staffed.
            int maxPlayers = EditorData::instance ? EditorData::instance->MaxPlayers() : 0;
            int firstEmpty = -1;
            for (int p = 0; p < maxPlayers && p < 8; ++p) {
                if (perPlayer[p] < 1) { firstEmpty = p; break; }
            }
            const bool allStaffed = (firstEmpty < 0);
            char details[256];
            if (allStaffed) {
                snprintf(details, sizeof(details),
                    "All %d active players have at least one unit.", maxPlayers);
            } else {
                snprintf(details, sizeof(details),
                    "Player %d has no units.  Every active player in a multiplayer mission "
                    "should have at least one unit, or that player starts with nothing.",
                    firstEmpty);
            }
            push("players_staffed",
                 "All players have units",
                 MissionCheckSeverity::Warning,
                 allStaffed,
                 ChecklistAction::None,
                 details);
        }

        // Too many pilots -- the save warns above 104 units (engine pilot cap).
        const bool pilotCountOk = (totalUnits <= 104);
        char pdetails[256];
        snprintf(pdetails, sizeof(pdetails),
            pilotCountOk
                ? "Unit count: %d (within the 104-pilot limit)."
                : "Unit count: %d exceeds the 104-pilot limit.  The engine cannot load more "
                  "than 104 pilots; remove units before saving.",
            totalUnits);
        push("pilot_count",
             "Unit count within pilot limit",
             MissionCheckSeverity::Warning,
             pilotCountOk,
             ChecklistAction::None,
             pdetails);
    }

    // 6. Missing appearance -- BLOCKING
    //    EditorObjects.h:68: getPosition() = appearance()->position — derefs appearance().
    //    EditorObjectMgr::saveMechs() calls appearance() without null check on every
    //    placed object; a null crashes on save.  Check buildings and units separately.
    //    Guard: never deref appearance() to test for null — test the pointer itself first.
    {
        int nullAppCount = 0;

        if (EditorObjectMgr::instance()) {
            // Buildings
            EditorObjectMgr::BUILDING_LIST buildings = EditorObjectMgr::instance()->getBuildings();
            for (EditorObjectMgr::BUILDING_LIST::EIterator it = buildings.Begin(); !it.IsDone(); it++) {
                EditorObject* b = (*it);
                if (b && !b->appearance())
                    ++nullAppCount;
            }

            // Units (reuse already-iterated pattern from pilot_count above)
            EditorObjectMgr::UNIT_LIST units2 = EditorObjectMgr::instance()->getUnits();
            for (auto it = units2.begin(); it != units2.end(); it++) {
                Unit* u = (*it);
                if (u && !u->appearance())
                    ++nullAppCount;
            }
        }

        const bool appOk = (nullAppCount == 0);
        char adetails[256];
        snprintf(adetails, sizeof(adetails),
            appOk
                ? "All placed objects have a valid appearance pointer."
                : "%d placed object(s) have a null appearance.  saveMechs() will crash when "
                  "writing these objects.  Remove or replace them before saving.",
            nullAppCount);
        push("missing_appearance",
             "All placed objects have appearances",
             MissionCheckSeverity::Blocking,
             appOk,
             ChecklistAction::None,
             adetails);
    }

    // 7. Object out of bounds -- WARNING
    //    Terrain::worldUnitsMapSide (mclib/terrain.h:189) is the total world-unit span.
    //    Half-extent = worldUnitsMapSide / 2, matching Camera::setPosition bound
    //    (mclib/camera.cpp:2867: `(Terrain::worldUnitsMapSide / 2) - ...`).
    //    Position is in appearance()->position; skip any object whose appearance is null
    //    (already flagged by check 6).
    {
        int oobCount = 0;
        const float halfExtent = Terrain::worldUnitsMapSide * 0.5f;

        if (EditorObjectMgr::instance() && halfExtent > 0.f) {
            // Buildings
            EditorObjectMgr::BUILDING_LIST buildings = EditorObjectMgr::instance()->getBuildings();
            for (EditorObjectMgr::BUILDING_LIST::EIterator it = buildings.Begin(); !it.IsDone(); it++) {
                EditorObject* b = (*it);
                if (!b || !b->appearance()) continue;
                const Stuff::Vector3D& pos = b->getPosition();
                if (pos.x < -halfExtent || pos.x > halfExtent ||
                    pos.y < -halfExtent || pos.y > halfExtent)
                    ++oobCount;
            }

            // Units
            EditorObjectMgr::UNIT_LIST units3 = EditorObjectMgr::instance()->getUnits();
            for (auto it = units3.begin(); it != units3.end(); it++) {
                Unit* u = (*it);
                if (!u || !u->appearance()) continue;
                const Stuff::Vector3D& pos = u->getPosition();
                if (pos.x < -halfExtent || pos.x > halfExtent ||
                    pos.y < -halfExtent || pos.y > halfExtent)
                    ++oobCount;
            }
        }

        const bool boundsOk = (oobCount == 0);
        char bdetails[256];
        snprintf(bdetails, sizeof(bdetails),
            boundsOk
                ? "All placed objects are within the terrain extent (+/-%.0f world units)."
                : "%d placed object(s) are outside the terrain extent (+/-%.0f world units).  "
                  "Out-of-bounds objects may corrupt cell lookups on save or load.",
            halfExtent, halfExtent);
        push("object_out_of_bounds",
             "All objects are within terrain bounds",
             MissionCheckSeverity::Warning,
             boundsOk,
             ChecklistAction::None,
             bdetails);
    }

    // 8. Invalid alignment -- WARNING
    //    EditorObjects.h:48: getAlignment() = appearInfo->appearance->teamId.
    //    saveMechs buckets units by alignment into perPlayer[0..7]; an out-of-range
    //    value (< 0 or >= 8) skips the tally entirely (existing guard in check 5),
    //    silently producing wrong player counts in the saved .pak.
    //    No auto-fix: the fix pattern in this file has no mutation hook.
    {
        int badAlignCount = 0;

        if (EditorObjectMgr::instance()) {
            EditorObjectMgr::UNIT_LIST units4 = EditorObjectMgr::instance()->getUnits();
            for (auto it = units4.begin(); it != units4.end(); it++) {
                Unit* u = (*it);
                if (!u || !u->appearance()) continue;   // null appearance already flagged
                const int a = u->getAlignment();
                if (a < 0 || a >= 8)
                    ++badAlignCount;
            }
        }

        const bool alignOk = (badAlignCount == 0);
        char ldetails[256];
        snprintf(ldetails, sizeof(ldetails),
            alignOk
                ? "All unit alignments are in the valid range [0..7]."
                : "%d unit(s) have an alignment outside [0..7].  These units are silently "
                  "excluded from the per-player tally on save, producing incorrect staffing "
                  "data.  Open the unit settings dialog and set alignment to 0-7.",
            badAlignCount);
        push("invalid_alignment",
             "All unit alignments are valid (0-7)",
             MissionCheckSeverity::Warning,
             alignOk,
             ChecklistAction::None,
             ldetails);
    }

    return checks;
}

// ---------------------------------------------------------------------------
// Issue tallies + targeted helpers (used by the in-panel summary and -smoke-validate)
// ---------------------------------------------------------------------------

/*static*/ void MissionValidator::GetIssueCounts(int& blocking, int& warning, int& info) {
    blocking = warning = info = 0;
    for (const auto& c : ValidateForPakSave()) {
        if (c.passed) continue;
        switch (c.severity) {
            case MissionCheckSeverity::Blocking: ++blocking; break;
            case MissionCheckSeverity::Warning:  ++warning;  break;
            default:                             ++info;     break;
        }
    }
}

/*static*/ bool MissionValidator::HasUnitStaffingWarning() {
    for (const auto& c : ValidateForPakSave()) {
        if (c.passed || c.severity != MissionCheckSeverity::Warning)
            continue;
        if (strcmp(c.id, "enemy_units") == 0 || strcmp(c.id, "player_units") == 0
            || strcmp(c.id, "players_staffed") == 0 || strcmp(c.id, "pilot_count") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Quick helpers
// ---------------------------------------------------------------------------

/*static*/ bool MissionValidator::HasBlockingFailures() {
    for (const auto& c : ValidateForPakSave())
        if (!c.passed && c.severity == MissionCheckSeverity::Blocking) return true;
    return false;
}

/*static*/ bool MissionValidator::HasWarnings() {
    for (const auto& c : ValidateForPakSave())
        if (!c.passed && c.severity == MissionCheckSeverity::Warning) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

static bool                      s_open         = false;
static std::vector<MissionCheck> s_lastChecks;
static bool                      s_needsRefresh = true;
static ChecklistAction           s_pendingAction = ChecklistAction::None;

void MissionValidator::Open()    { s_open = true;  s_needsRefresh = true; }
void MissionValidator::Close()   { s_open = false; }
bool MissionValidator::IsOpen()  { return s_open; }

/*static*/ ChecklistAction MissionValidator::TakeAction() {
    ChecklistAction act = s_pendingAction;
    s_pendingAction = ChecklistAction::None;
    return act;
}

/*static*/ void MissionValidator::QueueAction(ChecklistAction act) {
    s_pendingAction = act;
}

// ---------------------------------------------------------------------------
// ImGui panel
// ---------------------------------------------------------------------------

#ifdef MC2_IMGUI

namespace {
    constexpr ImVec4 kColBlock { 0.95f, 0.25f, 0.20f, 1.0f };
    constexpr ImVec4 kColWarn  { 0.95f, 0.75f, 0.10f, 1.0f };
    constexpr ImVec4 kColInfo  { 0.50f, 0.70f, 1.00f, 1.0f };
    constexpr ImVec4 kColPass  { 0.30f, 0.85f, 0.30f, 1.0f };
    constexpr ImVec4 kColGrey  { 0.55f, 0.55f, 0.55f, 1.0f };

    const char* ActionLabel(ChecklistAction act) {
        switch (act) {
            case ChecklistAction::OpenMapGenerator: return "Generate Map...";
            case ChecklistAction::OpenSaveAs:       return "Save As...";
            case ChecklistAction::OpenObjectives:   return "Open Objectives";
            case ChecklistAction::BuildMove:        return "Build MOVE";
            default:                                return nullptr;
        }
    }
}

void MissionValidator::Draw() {
    if (!s_open) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float    sc = io.DisplaySize.x / 1280.f;

    ImGui::SetNextWindowSize(ImVec2(450.f * sc, 0.f), ImGuiCond_Once);
    // Skip explicit pos under autodock (it would float the window out of the dock).
    // AlwaysAutoResize also fights docking, so drop it when autodocking.
    const bool autodock = GuiRuntime::AutoDockActive();
    if (!autodock)
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5f - 225.f * sc,
                   io.DisplaySize.y * 0.5f - 150.f * sc),
            ImGuiCond_Once);

    bool open = s_open;
    ImGuiWindowFlags msrFlags = ImGuiWindowFlags_NoCollapse;
    if (!autodock) msrFlags |= ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("Mission Save Readiness", &open, msrFlags)) {
        ImGui::End();
        s_open = open;
        return;
    }
    s_open = open;

    if (s_needsRefresh) {
        s_lastChecks   = MissionValidator::ValidateForPakSave();
        s_needsRefresh = false;
    }

    if (ImGui::Button("Refresh"))
        s_lastChecks = MissionValidator::ValidateForPakSave();
    ImGui::SameLine();
    ImGui::TextDisabled("Checks run against live editor state.");

    ImGui::Separator();

    // Summary
    int blockCount = 0, warnCount = 0;
    for (const auto& c : s_lastChecks) {
        if (!c.passed) {
            if      (c.severity == MissionCheckSeverity::Blocking) ++blockCount;
            else if (c.severity == MissionCheckSeverity::Warning)  ++warnCount;
        }
    }

    if (blockCount > 0) {
        ImGui::TextColored(kColBlock,
            "%d blocking issue%s, %d warning%s  --  fix blocking issues before saving.",
            blockCount, blockCount == 1 ? "" : "s",
            warnCount,  warnCount  == 1 ? "" : "s");
    } else if (warnCount > 0) {
        ImGui::TextColored(kColWarn,
            "No blocking issues.  %d warning%s  --  save will work but review warnings.",
            warnCount, warnCount == 1 ? "" : "s");
    } else if (!s_lastChecks.empty()) {
        ImGui::TextColored(kColPass, "All checks passed -- ready to save.");
    } else {
        ImGui::TextColored(kColGrey, "(No checks run -- click Refresh)");
    }

    ImGui::Spacing();

    // Check rows
    for (const auto& c : s_lastChecks) {
        const char* icon;
        ImVec4      colour;
        if (c.passed) {
            icon   = "OK";
            colour = kColPass;
        } else {
            switch (c.severity) {
                case MissionCheckSeverity::Blocking: icon = "!!"; colour = kColBlock; break;
                case MissionCheckSeverity::Warning:  icon = " !"; colour = kColWarn;  break;
                default:                             icon = " i"; colour = kColInfo;  break;
            }
        }

        ImGui::TextColored(colour, "[%s]", icon);
        if (ImGui::IsItemHovered() && !c.details.empty())
            ImGui::SetTooltip("%s", c.details.c_str());

        ImGui::SameLine();
        ImGui::TextUnformatted(c.label);
        if (ImGui::IsItemHovered() && !c.details.empty())
            ImGui::SetTooltip("%s", c.details.c_str());

        // Per-check action button (only on failing checks that have an action)
        const char* btnLabel = (!c.passed) ? ActionLabel(c.action) : nullptr;
        if (btnLabel) {
            ImGui::SameLine();
            char btnId[128];
            snprintf(btnId, sizeof(btnId), "%s##%s", btnLabel, c.id);
            if (ImGui::SmallButton(btnId))
                s_pendingAction = c.action;
            if (ImGui::IsItemHovered() && !c.details.empty())
                ImGui::SetTooltip("%s", c.details.c_str());
        }
    }

    ImGui::Separator();

    // "Prepare Saveable Mission" bottom button.
    // Re-validates and triggers the first failing check's action.
    // Does NOT create mission content -- only navigates to the right editor panel.
    bool anyFailing = (blockCount > 0 || warnCount > 0);
    if (!anyFailing) ImGui::BeginDisabled();
    if (ImGui::Button("Prepare Saveable Mission", ImVec2(-1.f, 0.f)))
        s_pendingAction = ChecklistAction::PrepareSaveable;
    if (!anyFailing) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(anyFailing
            ? "Navigates to the first issue that needs attention.\n"
              "Does NOT create fake mission content."
            : "No issues to fix.");
    }

    ImGui::End();
}

#else // !MC2_IMGUI
void MissionValidator::Draw() {}
#endif // MC2_IMGUI
