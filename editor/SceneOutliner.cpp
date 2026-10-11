/***************************************************************
* FILENAME: SceneOutliner.cpp
* DESCRIPTION: Read-only Scene Outliner Lite (Phase 1 modder usability).
*   See SceneOutliner.h. Lists placed mission objects grouped by type,
*   with counts, a search filter, and click-to-select via the existing
*   EditorObjectMgr selection path. No save/load, no PacketFile, no
*   property editing -- selection is the only state mutation.
* DATE: 2026-06-09
****************************************************************/

#include "stdafx.h"

#include "SceneOutliner.h"

#include "imgui.h"

#include "EditorObjectMgr.h"
#include "EditorObjects.h"
#include "EditorInterface.h"  // EditorInterface::frameSelectedObjects (double-click)
#include "Forest.h"

#include "terrain.h"   // extern TerrainPtr land (selection clear parity)

#include <vector>
#include <cctype>
#include <cstdio>
#include <cstring>

static bool s_open = false;

void SceneOutliner::Open()   { s_open = true; }
void SceneOutliner::Close()  { s_open = false; }
void SceneOutliner::Toggle() { s_open = !s_open; }
bool SceneOutliner::IsOpen() { return s_open; }


// True when an object is a nav marker (own class, but reports BLDG_TYPE; the
// catalog special-type is the reliable discriminator). Guarded so a malformed
// id never escalates to a crash.
static bool outlinerIsNavMarker(const EditorObject* obj)
{
    if (!obj)
        return false;
    return obj->getSpecialType() == EditorObjectMgr::NAV_MARKER;
}

// Snapshot the forest list using EditorObjectMgr's two-call size protocol.
static void outlinerGatherForests(std::vector<Forest*>& out)
{
    out.clear();
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return;

    long count = 0;
    mgr->getForests(nullptr, count);   // first call: count <- forests.Count()
    if (count <= 0)
        return;

    out.resize((size_t)count);
    long filled = count;
    mgr->getForests(out.data(), filled);
    if (filled < 0)
        filled = 0;
    out.resize((size_t)filled);
}

// Case-insensitive substring match. Empty filter matches everything.
static bool outlinerFilterMatch(const char* filter, const char* label)
{
    if (!filter || !filter[0])
        return true;
    if (!label)
        return false;

    for (const char* base = label; *base; ++base)
    {
        const char* a = base;
        const char* b = filter;
        while (*a && *b &&
               std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b))
        {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

// Clear the current selection the same way a plain (non-toggle) viewport click
// does, so the outliner stays consistent with existing editor behavior.
static void outlinerClearSelection()
{
    if (land)
        land->unselectAll();
    if (EditorObjectMgr::instance())
        EditorObjectMgr::instance()->unselectAll();
}

static void outlinerSelectObject(EditorObject* obj)
{
    if (!obj)
        return;
    outlinerClearSelection();
    EditorObjectMgr::instance()->select(*obj, true);
}

// Call immediately after an outliner row's Selectable: frame the camera on the
// current selection when that row was double-clicked. The double-click's first
// click already ran outlinerSelectObject (Selectable returns true on each
// click), so the selection is the intended object by the time we frame it.
// Pure camera move — no object mutation, no undo, no mission-dirty.
static void outlinerFrameOnDoubleClick()
{
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        EditorInterface::frameSelectedObjects();
}

// Build a readable, pointer-safe row label into buf.
static void outlinerLabelForObject(const EditorObject* obj, char* buf, size_t bufLen)
{
    const char* name = obj ? obj->getDisplayName() : nullptr;
    if (!name || !name[0])
        name = "(unnamed)";

    // teamId access is not null-safe inside EditorObject; gate on appearance().
    if (obj && obj->appearance())
        std::snprintf(buf, bufLen, "%s  [team %d]", name, obj->getAlignment());
    else
        std::snprintf(buf, bufLen, "%s", name);
}

OutlinerCounts SceneOutliner::ComputeCounts()
{
    OutlinerCounts c;
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return c;

    EditorObjectMgr::UNIT_LIST units = mgr->getUnits();
    c.units = (int)units.Count();

    EditorObjectMgr::BUILDING_LIST buildings = mgr->getBuildings();
    for (EditorObjectMgr::BUILDING_LIST::EIterator it = buildings.Begin();
         !it.IsDone(); it++)
    {
        EditorObject* obj = (*it);
        if (!obj)
            c.other++;
        else if (outlinerIsNavMarker(obj))
            c.navMarkers++;
        else
            c.buildings++;
    }

    EditorObjectMgr::DROPZONE_LIST dropZones = mgr->getDropZones();
    c.dropZones = (int)dropZones.Count();

    std::vector<Forest*> forests;
    outlinerGatherForests(forests);
    c.forests = (int)forests.size();

    return c;
}

bool SceneOutliner::SelectFirstObject()
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return false;

    EditorObjectMgr::UNIT_LIST units = mgr->getUnits();
    for (EditorObjectMgr::UNIT_LIST::EIterator it = units.Begin(); !it.IsDone(); it++)
    {
        if (*it)
        {
            outlinerSelectObject(*it);
            return true;
        }
    }

    EditorObjectMgr::BUILDING_LIST buildings = mgr->getBuildings();
    for (EditorObjectMgr::BUILDING_LIST::EIterator it = buildings.Begin();
         !it.IsDone(); it++)
    {
        if (*it)
        {
            outlinerSelectObject(*it);
            return true;
        }
    }

    EditorObjectMgr::DROPZONE_LIST dropZones = mgr->getDropZones();
    for (EditorObjectMgr::DROPZONE_LIST::EIterator it = dropZones.Begin();
         !it.IsDone(); it++)
    {
        if (*it)
        {
            outlinerSelectObject(*it);
            return true;
        }
    }

    return false;
}


static void outlinerDrawObjectGroup(const char* groupLabel, int count,
                                    EditorObjectMgr::BUILDING_LIST& list,
                                    bool navMarkersOnly, const char* filter)
{
    char header[96];
    std::snprintf(header, sizeof(header), "%s (%d)###%s", groupLabel, count, groupLabel);
    if (!ImGui::CollapsingHeader(header))
        return;

    int row = 0;
    for (EditorObjectMgr::BUILDING_LIST::EIterator it = list.Begin(); !it.IsDone(); it++, ++row)
    {
        EditorObject* obj = (*it);
        const bool isNav = outlinerIsNavMarker(obj);
        if (navMarkersOnly != isNav)
            continue;

        char label[160];
        outlinerLabelForObject(obj, label, sizeof(label));
        if (!outlinerFilterMatch(filter, label))
            continue;

        ImGui::PushID(obj ? (void*)obj : (void*)(intptr_t)row);
        const bool selected = obj && obj->isSelected();
        if (ImGui::Selectable(label, selected))
            outlinerSelectObject(obj);
        outlinerFrameOnDoubleClick();
        ImGui::PopID();
    }
}

void SceneOutliner::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(300.f, 380.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Outliner", &s_open))
    {
        ImGui::End();
        return;
    }

    static char s_filter[64] = "";
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##outlinerFilter", "search...", s_filter, sizeof(s_filter));

    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
    {
        ImGui::TextDisabled("No map loaded.");
        ImGui::End();
        return;
    }

    OutlinerCounts counts = ComputeCounts();
    ImGui::Text("Objects: %d", counts.total());
    ImGui::Separator();

    {
        char header[96];
        std::snprintf(header, sizeof(header), "Units (%d)###Units", counts.units);
        if (ImGui::CollapsingHeader(header))
        {
            EditorObjectMgr::UNIT_LIST units = mgr->getUnits();
            for (EditorObjectMgr::UNIT_LIST::EIterator it = units.Begin(); !it.IsDone(); it++)
            {
                EditorObject* obj = (*it);
                char label[160];
                outlinerLabelForObject(obj, label, sizeof(label));
                if (!outlinerFilterMatch(s_filter, label))
                    continue;
                ImGui::PushID(obj);
                if (ImGui::Selectable(label, obj && obj->isSelected()))
                    outlinerSelectObject(obj);
                outlinerFrameOnDoubleClick();
                ImGui::PopID();
            }
        }
    }

    // Buildings + NavMarkers both live in the buildings list; split by special type.
    EditorObjectMgr::BUILDING_LIST buildings = mgr->getBuildings();
    outlinerDrawObjectGroup("Buildings", counts.buildings, buildings, /*navOnly*/ false, s_filter);
    outlinerDrawObjectGroup("NavMarkers", counts.navMarkers, buildings, /*navOnly*/ true, s_filter);

    {
        char header[96];
        std::snprintf(header, sizeof(header), "DropZones (%d)###DropZones", counts.dropZones);
        if (ImGui::CollapsingHeader(header))
        {
            EditorObjectMgr::DROPZONE_LIST dropZones = mgr->getDropZones();
            for (EditorObjectMgr::DROPZONE_LIST::EIterator it = dropZones.Begin();
                 !it.IsDone(); it++)
            {
                DropZone* dz = (*it);
                char label[160];
                const char* tag = (dz && dz->isVTol()) ? "DropZone [VTOL]" : "DropZone";
                if (dz && dz->appearance())
                    std::snprintf(label, sizeof(label), "%s  [team %d]", tag, dz->getAlignment());
                else
                    std::snprintf(label, sizeof(label), "%s", tag);
                if (!outlinerFilterMatch(s_filter, label))
                    continue;
                ImGui::PushID(dz);
                if (ImGui::Selectable(label, dz && dz->isSelected()))
                    outlinerSelectObject(dz);
                outlinerFrameOnDoubleClick();
                ImGui::PopID();
            }
        }
    }

    // Forests (separate Forest objects; selection via selectForest()).
    {
        char header[96];
        std::snprintf(header, sizeof(header), "Forests (%d)###Forests", counts.forests);
        if (ImGui::CollapsingHeader(header))
        {
            std::vector<Forest*> forests;
            outlinerGatherForests(forests);
            for (size_t i = 0; i < forests.size(); ++i)
            {
                Forest* f = forests[i];
                const char* name = f ? f->getName() : nullptr;
                char label[160];
                std::snprintf(label, sizeof(label), "%s (forest %ld)",
                              (name && name[0]) ? name : "(unnamed)",
                              f ? f->getID() : -1L);
                if (!outlinerFilterMatch(s_filter, label))
                    continue;
                ImGui::PushID((void*)f);
                if (ImGui::Selectable(label, false) && f)
                    mgr->selectForest(f->getID());
                ImGui::PopID();
            }
        }
    }

    // Double-click any unit / building / nav-marker / drop-zone row to frame the
    // camera on it (EditorInterface::frameSelectedObjects via setPosition). Forests
    // select through selectForest() (ID-based, not in getSelectedObjectList) and are
    // not framed in this pass.

    ImGui::End();
}
