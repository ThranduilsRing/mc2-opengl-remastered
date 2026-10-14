#ifndef CAMPAIGN_SUMMARY_PANEL_H
#define CAMPAIGN_SUMMARY_PANEL_H
/***************************************************************
* FILENAME: CampaignSummaryPanel.h
* DESCRIPTION: Read-only campaign explainer (campaign editor redesign,
*   Phase 1). Loads a campaign .fit on demand (Win32 file picker), reads it
*   into a local CCampaignData, and renders the operation/mission outline with
*   cheap data-only validation badges. PURE-ADDITIVE: no engine model changes,
*   no save, no editing. Modeled on InspectorPanel + MapGeneratorDialog import.
*   Edit campaigns via the legacy Campaign dialog.
* DATE: 2026-06-27
****************************************************************/

class CampaignSummaryPanel
{
public:
    // Panel visibility (own ImGui window; safe with no campaign loaded).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();
    static bool Load(const char* path);

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Read-only.
    static void Draw();
};

#endif // CAMPAIGN_SUMMARY_PANEL_H
