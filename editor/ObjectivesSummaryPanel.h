#ifndef OBJECTIVES_SUMMARY_PANEL_H
#define OBJECTIVES_SUMMARY_PANEL_H
/***************************************************************
* FILENAME: ObjectivesSummaryPanel.h
* DESCRIPTION: Read-only objectives explainer. Walks EditorData::instance->TeamsRef()
*   and renders each team's
*   CObjectives as readable "WHEN/THEN/Failure" cards. PURE-ADDITIVE: no engine
*   model changes, no save/load, no editing. Modeled on InspectorPanel.
*   Edit objectives via the legacy Objectives dialog.
****************************************************************/

class ObjectivesSummaryPanel
{
public:
    // Panel visibility (own ImGui window; safe with no map / no objectives).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Read-only.
    static void Draw();
};

#endif // OBJECTIVES_SUMMARY_PANEL_H
