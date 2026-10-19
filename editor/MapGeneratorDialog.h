#pragma once
/***************************************************************
* FILENAME: MapGeneratorDialog.h
* DESCRIPTION: ImGui Map Generator dialog for the MC2 Mission Editor.
*   Replaces the MFC TerrainDlg + MapSizeDlg flow with a single integrated
*   window that has biome/size/height/material controls and a fast thumbnail
*   preview before full generation.
* DATE: 2026-06-05
****************************************************************/

#ifndef MAP_GENERATOR_DIALOG_H
#define MAP_GENERATOR_DIALOG_H

namespace MapGeneratorDialog {

    // Pending deferred action requested by the dialog (set each frame; acted
    // on in EditorInterface::update() OUTSIDE the ImGui render pass).
    enum class PendingAction {
        None,
        Preview,      // run python --preview, load thumbnail
        Generate,     // run python full, apply to editor, close dialog
        LoadPreset,   // copy pre-baked preset files then apply (no python run)
        Import,       // run import_gaea_height.py on an external heightfield, apply
    };

    // Open (or re-open) the dialog.  Call from toolbar "Generate Map" button
    // and from the startup path when ID_MAPGENERATOR is returned.
    void Open();

    // Close + clean up preview texture.
    void Close();

    bool IsOpen();

    // Draw the ImGui window.  Call each frame from renderToolbarImGui()
    // (or equivalent editor render hook).  Sets the pending action if the
    // user clicks Preview or Generate.
    void Draw();

    // Query the pending action and clear it atomically.
    PendingAction TakeAction();

    // Execute the pending action (called from update(), outside ImGui pass).
    // Preview/Generate START an async EditorTaskRunner task and return immediately;
    // the terrain apply runs later on the main thread from the task's success
    // callback (drained by EditorTaskRunner::PumpMainThread()).
    void ExecutePreview();
    void ExecuteGenerate();
    // Import an external heightfield (Gaea / Blender / EXR/PNG). Runs
    // import_gaea_height.py to write terrain_gen_out/genmap.* then applies via the
    // SAME s_pendingApply* / GenerateReady() / ApplyPendingGenerate() path as Generate.
    void ExecuteImport();
    // Load a pre-baked flat preset (no Python run).
    // Checks terrain_gen_presets/<biome>_<sizeN>/ for pre-generated files.
    void ExecuteLoadPreset();

    // True once an async Generate task has finished and the terrain apply is pending.
    // EditorInterface::update() must, in order: eye->reset() -> ApplyPendingGenerate()
    // -> eye->setPosition(). The reset-before-apply order is load-bearing: the apply
    // primes the terrain face cache against the camera, so resetting afterwards would
    // leave the terrain culled/black.
    bool GenerateReady();

    // Perform the deferred terrain apply (main thread; camera must already be reset).
    // Returns true on a clean apply (then closes the dialog).
    bool ApplyPendingGenerate();

    // True while an async generate/preview task is in flight (disables re-entry).
    bool IsTaskActive();

    // ---------------------------------------------------------------------------
    // Foliage Detail panel (separate from the base Generate). Works on the map you
    // already generated this session: tweak tree/rock/bush/clumpiness/shoreline
    // knobs and regenerate ONLY the foliage (fast --foliage-only path) onto the
    // visible terrain, iteratively. Requires a genmap_recipe.json from a prior
    // Generate. Toggled from the Tools palette; drawn each frame.
    // ---------------------------------------------------------------------------
    void ToggleFoliagePanel();
    bool FoliagePanelOpen();
    void DrawFoliagePanel();

} // namespace MapGeneratorDialog

#endif // MAP_GENERATOR_DIALOG_H
