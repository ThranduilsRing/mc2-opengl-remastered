/***************************************************************
* FILENAME: TelemetryPanel.cpp
* DESCRIPTION: Editor authoring-telemetry panel (S17 edit depth).
*   See TelemetryPanel.h. Read-only window over editor singletons:
*   FPS, selection / placed-object counts, undo-redo depth, dirty
*   state, map dimensions + mission file. Every source is NULL-guarded
*   so the panel is safe with no map loaded and no selection.
* DATE: 2026-06-15
****************************************************************/

#include "stdafx.h"

#include "TelemetryPanel.h"

#include "imgui.h"

#include "EditorObjectMgr.h"   // selection + object-list counts
#include "EditorData.h"        // dirty flag + map name
#include "Action.h"            // ActionUndoMgr (undo/redo depth + strings)
#include "terrain.h"           // Terrain::realVerticesMapSide / worldUnitsMapSide

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;

void TelemetryPanel::Open()   { s_open = true; }
void TelemetryPanel::Close()  { s_open = false; }
void TelemetryPanel::Toggle() { s_open = !s_open; }
bool TelemetryPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void TelemetryPanel::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(300.f, 320.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Telemetry", &s_open))
    {
        ImGui::End();
        return;
    }

    // --- Frame ----------------------------------------------------------
    const float fps = ImGui::GetIO().Framerate;
    ImGui::SeparatorText("Frame");
    ImGui::Text("FPS:        %.0f", fps);
    ImGui::Text("Frame time: %.2f ms", (fps > 0.f) ? (1000.0f / fps) : 0.0f);

    // --- Scene ----------------------------------------------------------
    ImGui::SeparatorText("Scene");
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (mgr)
    {
        // getUnits/getBuildings/getDropZones return EList COPIES (O(n) each) --
        // acceptable once per frame for an editor map.
        const long nUnits     = (long)mgr->getUnits().size();
        const long nBuildings = (long)mgr->getBuildings().Count();
        const long nDropZones = (long)mgr->getDropZones().Count();
        const long nTotal     = nUnits + nBuildings + nDropZones;
        ImGui::Text("Selected:   %d", mgr->getSelectionCount());
        ImGui::Text("Objects:    %ld", nTotal);
        ImGui::Text("  units %ld / buildings %ld / dropzones %ld",
                    nUnits, nBuildings, nDropZones);
    }
    else
    {
        ImGui::TextDisabled("(no object manager)");
    }

    // --- Edit state -----------------------------------------------------
    ImGui::SeparatorText("Edit state");
    const bool dirty = (EditorData::instance != nullptr)
                       && EditorData::instance->MissionNeedsSaving();
    ImGui::Text("Unsaved:    %s", dirty ? "YES (dirty)" : "no");

    if (ActionUndoMgr::instance != nullptr)
    {
        ActionUndoMgr* um = ActionUndoMgr::instance;
        const int pos   = um->GetCurrentPosition();
        const int count = um->GetActionCount();
        const int redo  = (count > pos) ? (count - pos) : 0;
        ImGui::Text("Undo depth: %d", pos);
        ImGui::Text("Redo depth: %d", redo);
        if (um->HaveUndo())
        {
            const char* s = um->GetUndoString();
            ImGui::Text("Next undo:  %s", (s && s[0]) ? s : "(action)");
        }
        if (um->HaveRedo())
        {
            const char* s = um->GetRedoString();
            ImGui::Text("Next redo:  %s", (s && s[0]) ? s : "(action)");
        }
    }
    else
    {
        ImGui::TextDisabled("(no undo manager)");
    }

    // --- Map / mission --------------------------------------------------
    ImGui::SeparatorText("Map");
    const long vside = Terrain::realVerticesMapSide;
    if (vside > 0)
    {
        // Cell grid side is one fewer than the vertex side.
        ImGui::Text("Cells:      %ld x %ld", vside - 1, vside - 1);
        ImGui::Text("World:      %.0f units", Terrain::worldUnitsMapSide);
    }
    else
    {
        ImGui::TextDisabled("(no terrain loaded)");
    }
    const char* mapName = EditorData::getMapName();
    ImGui::Text("Mission:    %s", (mapName && mapName[0]) ? mapName : "(unsaved)");

    ImGui::End();
}
