#ifndef ASSET_BROWSER_H
#define ASSET_BROWSER_H
/***************************************************************
* FILENAME: AssetBrowser.h
* DESCRIPTION: Modder-friendly Asset Browser Lite.
*   Surfaces the EXISTING EditorObjectMgr object catalog (building/object
*   groups) in one searchable ImGui panel, so a modder can pick any placeable
*   object without diving the legacy MFC menus. Clicking an entry activates the
*   EXISTING placement path (EditorInterface::selectBuildingObject ->
*   BuildingBrush/ScatterBrush), so a placement is one entry in the existing
*   undo system. No new asset database, no new placement system.
****************************************************************/

class AssetBrowser
{
public:
    // Panel visibility (own ImGui window; safe with no map loaded).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed.
    static void Draw();

    // Read-only catalog tally (smoke + display): number of building/object
    // groups in the catalog. 0 if the object manager is not up yet.
    static int GroupCount();

    // Smoke/manual helper: activate the first placeable object in the catalog
    // through the existing placement path. Returns true if a placement brush was
    // activated. Used by -smoke-asset-browser.
    static bool ActivateFirstObject();
};

#endif // ASSET_BROWSER_H
