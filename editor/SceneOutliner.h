#ifndef SCENE_OUTLINER_H
#define SCENE_OUTLINER_H
/***************************************************************
* FILENAME: SceneOutliner.h
* DESCRIPTION: Read-only modder-friendly Scene Outliner Lite.
*   Lists placed mission objects grouped by type with per-group counts,
*   a search filter, and single-click selection through the existing
*   EditorObjectMgr selection mechanism. No mutation except selection.
*   Does NOT touch save/load or PacketFile.
****************************************************************/

// Per-group object tallies, also consumed by the -smoke-outliner case.
struct OutlinerCounts
{
    int units      = 0;
    int buildings  = 0;
    int dropZones  = 0;
    int navMarkers = 0;
    int forests    = 0;
    int other      = 0;

    int total() const { return units + buildings + dropZones + navMarkers + forests + other; }
};

class SceneOutliner
{
public:
    // Panel visibility (own ImGui window; safe to call with no map loaded).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Pure read-only except for selection.
    static void Draw();

    // Read-only enumeration of placed objects. Empty/absent map -> all zero.
    static OutlinerCounts ComputeCounts();

    // Select the first selectable placed object (unit -> building/navmarker ->
    // drop zone). Returns true if anything was selected. Used by smoke; safe
    // with an empty map (returns false).
    static bool SelectFirstObject();
};

#endif // SCENE_OUTLINER_H
