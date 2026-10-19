/***************************************************************
* FILENAME: AssetBrowser.cpp
* DESCRIPTION: Asset Browser Lite.
*   See AssetBrowser.h. A searchable, grouped view of the existing
*   EditorObjectMgr object catalog. Clicking an entry calls the existing
*   EditorInterface::selectBuildingObject placement path -- no new catalog,
*   no new placement/undo system.
****************************************************************/

#include "stdafx.h"

#include "AssetBrowser.h"
#include "AssetThumbnailCache.h"

#include "imgui.h"

#include "EditorObjectMgr.h"
#include "EditorInterface.h"   // selectBuildingObject (existing placement path)

#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;
static bool s_focusRequested = false;

void AssetBrowser::Open()   { s_open = true; s_focusRequested = true; }
void AssetBrowser::Close()  { s_open = false; }
void AssetBrowser::Toggle() { s_open = !s_open; }
bool AssetBrowser::IsOpen() { return s_open; }

// Case-insensitive substring match; empty filter matches everything.
static bool assetFilterMatch(const char* filter, const char* label)
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

// ---------------------------------------------------------------------------
// Catalog tally
// ---------------------------------------------------------------------------
int AssetBrowser::GroupCount()
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    return mgr ? mgr->getBuildingGroupCount() : 0;
}

// ---------------------------------------------------------------------------
// Smoke helper: activate the first placeable object
// ---------------------------------------------------------------------------
bool AssetBrowser::ActivateFirstObject()
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    EditorInterface* ei  = EditorInterface::instance();
    if (!mgr || !ei)
        return false;

    const int groups = mgr->getBuildingGroupCount();
    for (int g = 0; g < groups; ++g)
    {
        if (mgr->getNumberBuildingsInGroup(g) > 0)
            return ei->selectBuildingObject(g, 0);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void AssetBrowser::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(280.f, 420.f), ImGuiCond_FirstUseEver);
    if (s_focusRequested) { ImGui::SetNextWindowFocus(); s_focusRequested = false; }
    if (!ImGui::Begin("Asset Browser", &s_open))
    {
        ImGui::End();
        return;
    }

    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
    {
        ImGui::TextDisabled("Object catalog not loaded.");
        ImGui::End();
        return;
    }

    static char s_filter[64] = "";
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##assetFilter", "search assets...", s_filter, sizeof(s_filter));

    const int groupCount = mgr->getBuildingGroupCount();
    ImGui::Text("Groups: %d", groupCount);
    ImGui::TextDisabled("Click an object to start placing it.");
    ImGui::Separator();

    const bool filtering = (s_filter[0] != '\0');

    // Deferred placement: never call selectBuildingObject (which deletes/replaces
    // curBrush) while iterating; record the pick and apply after the widgets.
    int pendGroup = -1, pendIndex = -1;

    ImGui::BeginChild("assetList", ImVec2(0, 0), false);

    for (int g = 0; g < groupCount; ++g)
    {
        const char* groupName = mgr->getGroupName(g);
        const int   count     = mgr->getNumberBuildingsInGroup(g);
        if (count <= 0)
            continue;

        // Gather this group's object names once.
        const char* names[512] = { 0 };
        int n = 0;
        mgr->getBuildingNamesInGroup(g, names, n);
        const int cap = (count <= 512) ? count : 512;
        if (n > cap) n = cap;

        // When filtering, only show the group if its name matches OR it has a
        // matching child; auto-open matching groups so hits are visible.
        bool groupNameHit = assetFilterMatch(s_filter, groupName);
        int  childHits = 0;
        if (filtering && !groupNameHit)
        {
            for (int i = 0; i < n; ++i)
                if (assetFilterMatch(s_filter, names[i])) { ++childHits; }
            if (childHits == 0)
                continue;   // nothing in this group matches; hide it
        }

        if (filtering)
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        char header[128];
        std::snprintf(header, sizeof(header), "%s (%d)###grp%d",
                      (groupName && groupName[0]) ? groupName : "(group)", count, g);
        if (!ImGui::CollapsingHeader(header, g == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
            continue;

        for (int i = 0; i < n; ++i)
        {
            const char* nm = names[i] ? names[i] : "?";
            // When filtering by child, hide non-matching children (unless the
            // group name itself matched, in which case show all its children).
            if (filtering && !groupNameHit && !assetFilterMatch(s_filter, nm))
                continue;

            // Packed object ID: same encoding as EditorObjectMgr::getGroup/
            // getIndexInGroup (group<<16 | index<<8).  Used only for thumbnail
            // lookup; selectBuildingObject(g, i) is still the placement path.
            const int objID = (g << 16) | (i << 8);

            // Thumbnail (32x32).  Falls back to text-only when 0.
            static const float kThumbSize = 32.f;
            const AssetThumbnailCache::TexHandle thumb =
                AssetThumbnailCache::get(objID);

            ImGui::PushID(g * 1000 + i);

            if (thumb != 0)
            {
                // Thumbnail + invisible selectable on the same row.
                // Draw image first, then overlay a same-height Selectable.
                ImGui::Image(
                    (ImTextureID)(uintptr_t)thumb,
                    ImVec2(kThumbSize, kThumbSize));
                ImGui::SameLine();
            }

            // Selectable height matches thumb when present; auto-height when not.
            const float selectH = (thumb != 0) ? kThumbSize : 0.f;
            if (ImGui::Selectable(nm, false, 0, ImVec2(0.f, selectH)))
            {
                pendGroup = g;
                pendIndex = i;
            }

            ImGui::PopID();
        }
    }

    ImGui::EndChild();

    ImGui::End();

    // Apply the deferred pick through the EXISTING placement path (creates the
    // BuildingBrush/ScatterBrush; placement itself goes through the existing
    // brush -> Action undo system). Done after End() so no ImGui state is live.
    if (pendGroup >= 0 && pendIndex >= 0)
    {
        EditorInterface* ei = EditorInterface::instance();
        if (ei)
            ei->selectBuildingObject(pendGroup, pendIndex);
    }
}
