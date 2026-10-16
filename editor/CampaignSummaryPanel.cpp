/***************************************************************
* FILENAME: CampaignSummaryPanel.cpp
* DESCRIPTION: Read-only campaign explainer. See CampaignSummaryPanel.h.
*   The editor keeps no always-loaded campaign object, so this panel loads a
*   campaign .fit on demand via the same Win32 GetOpenFileNameA idiom the Map
*   Generator import uses, reads it into a file-static CCampaignData, and renders
*   a designer-facing overview: a top-level campaign summary block, an aggregated
*   warnings roll-up, and per-operation cards with data-only validation badges.
*
*   CCampaignData::Read() (CampaignData.cpp:225) may assert() on a malformed
*   file; that is acceptable in RelWithDebInfo per the slice contract -- we do
*   not add engine guards.
*
*   PURE-ADDITIVE, READ-ONLY: no save, no editing, no mutation of CCampaignData.
* DATE: 2026-06-27
****************************************************************/

// Pull stdafx first (MFC / Windows headers, incl. commdlg GetOpenFileNameA).
#include "stdafx.h"

#ifdef MC2_IMGUI

#include "imgui.h"

#include "CampaignSummaryPanel.h"

#include "campaignData.h"   // CCampaignData / CGroupData / CMissionData

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool          s_open = false;
static bool          s_loaded = false;       // a campaign has been read at least once
static CCampaignData s_campaign;             // cached, read-only display copy
static char          s_loadedPath[1024] = "";
static char          s_status[256] = "";

void CampaignSummaryPanel::Open()   { s_open = true; }
void CampaignSummaryPanel::Close()  { s_open = false; }
void CampaignSummaryPanel::Toggle() { s_open = !s_open; }
bool CampaignSummaryPanel::IsOpen() { return s_open; }

bool CampaignSummaryPanel::Load(const char* path)
{
    CCampaignData fresh;
    if (!fresh.Read(path))
    {
        std::snprintf(s_status, sizeof(s_status), "Failed to read: %s", path);
        return false;
    }
    s_campaign = fresh;
    s_loaded = true;
    std::snprintf(s_loadedPath, sizeof(s_loadedPath), "%s", path);
    std::snprintf(s_status, sizeof(s_status), "Loaded: %s", s_loadedPath);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Win32 file picker -> dst. Returns true if the user picked a file. Mirrors the
// BrowseForFile idiom in MapGeneratorDialog.cpp.
static bool campBrowseForFile(const char* filter, char* dst, size_t dstLen)
{
    char path[1024] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = ::GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return false;
    strncpy(dst, path, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return true;
}

// Colored "[warning] ..." line (data-only validation badge).
static void campWarn(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.20f, 1.0f));
    ImGui::TextWrapped("[warning] %s", buf);
    ImGui::PopStyleColor();
}

// Inline colored badge (e.g. "[Unnamed]"); call SameLine() before/after to chain.
static void campBadge(const char* text, const ImVec4& col)
{
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// CString -> const char* for display (MFC CString has operator LPCTSTR()).
static const char* campStr(const CString& s) { return (const char*)s; }
static bool        campEmpty(const CString& s) { return s.GetLength() == 0; }

// Case-insensitive "standin" substring test (data-only placeholder detection).
static bool campIsStandin(const CString& s)
{
    const char* p = (const char*)s;
    if (!p || !*p)
        return false;
    for (; *p; ++p)
    {
        const char* a = p;
        const char* b = "standin";
        while (*a && *b &&
               (std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)))
        {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

// Total missions across every operation (data-only aggregation).
static unsigned long campTotalMissions(const CCampaignData& camp)
{
    unsigned long total = 0;
    for (const CGroupData& group : camp.m_GroupList)
        total += static_cast<unsigned long>(group.m_MissionList.size());
    return total;
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void CampaignSummaryPanel::Draw()
{
    if (!s_open)
        return;

    // NoDocking + UI-scale default so it floats resizable (never trapped in the
    // narrow autodock column) and stays readable at HiDPI font sizes.
    const float k = ImGui::GetIO().FontGlobalScale > 0.f ? ImGui::GetIO().FontGlobalScale : 1.f;
    ImGui::SetNextWindowSize(ImVec2(480.f * k, 560.f * k), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Campaign Overview", &s_open, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }

    const ImVec4 kWarnCol(1.0f, 0.78f, 0.20f, 1.0f);
    const ImVec4 kInfoCol(0.55f, 0.78f, 1.0f, 1.0f);

    ImGui::TextDisabled("Read-only campaign overview. Edit via the legacy Campaign dialog.");
    ImGui::Separator();

    if (ImGui::Button("Load campaign .fit..."))
    {
        char path[1024] = "";
        if (campBrowseForFile("Campaign (*.fit)\0*.fit\0All\0*.*\0", path, sizeof(path)))
        {
            Load(path);
        }
    }
    if (s_status[0])
        ImGui::TextDisabled("%s", s_status);

    ImGui::Separator();

    if (!s_loaded)
    {
        ImGui::TextDisabled("Load a campaign .fit to view its overview.");
        ImGui::End();
        return;
    }

    const unsigned long opCount      = static_cast<unsigned long>(s_campaign.m_GroupList.size());
    const unsigned long totalMissions = campTotalMissions(s_campaign);

    // -- Campaign summary block (designer labels) ---------------------------
    ImGui::TextColored(kInfoCol, "CAMPAIGN SUMMARY");
    if (s_campaign.m_NameUseResourceString)
        ImGui::Text("Campaign Title: (resource string #%d)", s_campaign.m_NameResourceStringID);
    else
        ImGui::Text("Campaign Title: %s",
                    campEmpty(s_campaign.m_Name) ? "(unnamed)" : campStr(s_campaign.m_Name));
    ImGui::Text("Starting C-Bills: %d", s_campaign.m_CBills);
    ImGui::Text("Structure: %lu Operations, %lu Missions total", opCount, totalMissions);
    ImGui::Text("Finale: %s",
                campEmpty(s_campaign.m_FinalVideo) ? "(none)" : campStr(s_campaign.m_FinalVideo));

    ImGui::Separator();

    // -- Aggregated warnings roll-up ----------------------------------------
    // Campaign and operation warnings let designers triage the whole campaign.
    ImGui::TextColored(kInfoCol, "WARNINGS");
    {
        int warnCount = 0;

        if (opCount == 0)
        {
            campWarn("Campaign has no operations.");
            ++warnCount;
        }
        if (campEmpty(s_campaign.m_FinalVideo))
        {
            campWarn("Final video missing.");
            ++warnCount;
        }

        for (size_t opIdx = 0; opIdx < s_campaign.m_GroupList.size(); ++opIdx)
        {
            const CGroupData& grp = s_campaign.m_GroupList[opIdx];
            const unsigned long mc = static_cast<unsigned long>(grp.m_MissionList.size());

            if (mc == 0)
            {
                campWarn("Operation %zu: no missions.", opIdx);
                ++warnCount;
            }
            if ((unsigned long)grp.m_NumMissionsToComplete > mc)
            {
                campWarn("Operation %zu: requires %d missions but only %lu exist.",
                         opIdx, grp.m_NumMissionsToComplete, mc);
                ++warnCount;
            }
            if (campEmpty(grp.m_Label))
            {
                campWarn("Operation %zu: no label.", opIdx);
                ++warnCount;
            }
            if (campIsStandin(grp.m_PreVideoFile))
            {
                campWarn("Operation %zu: briefing video is a STANDIN placeholder.", opIdx);
                ++warnCount;
            }
            if (campIsStandin(grp.m_VideoFile))
            {
                campWarn("Operation %zu: operation/debrief video is a STANDIN placeholder.", opIdx);
                ++warnCount;
            }
            for (const CMissionData& mission : grp.m_MissionList)
            {
                if (campEmpty(mission.m_MissionFile))
                {
                    campWarn("Operation %zu: has a mission with an empty filename.", opIdx);
                    ++warnCount;
                    break;
                }
            }
        }

        if (warnCount == 0)
            ImGui::TextDisabled("No warnings. Campaign data looks complete.");
    }

    ImGui::Separator();

    // -- Operation cards ----------------------------------------------------
    ImGui::TextColored(kInfoCol, "OPERATIONS");

    for (size_t opIndex = 0; opIndex < s_campaign.m_GroupList.size(); ++opIndex)
    {
        const CGroupData& grp = s_campaign.m_GroupList[opIndex];
        const unsigned long missionCount = static_cast<unsigned long>(grp.m_MissionList.size());
        const bool unnamed   = campEmpty(grp.m_Label);
        const bool standinPre = campIsStandin(grp.m_PreVideoFile);
        const bool standinVid = campIsStandin(grp.m_VideoFile);
        const bool noMissions = (missionCount == 0);
        const bool reqExceeds =
            ((unsigned long)grp.m_NumMissionsToComplete > missionCount);

        char opHdr[160];
        std::snprintf(opHdr, sizeof(opHdr), "Operation %zu: %s###op%zu",
                      opIndex,
                      unnamed ? "Unnamed" : campStr(grp.m_Label),
                      opIndex);

        if (!ImGui::CollapsingHeader(opHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::Indent();

        // Badge row (inline colored chips, data-only).
        {
            char nofm[32];
            std::snprintf(nofm, sizeof(nofm), "[%d of %lu]",
                          grp.m_NumMissionsToComplete, missionCount);
            campBadge(nofm, kInfoCol);
            if (unnamed)     { ImGui::SameLine(); campBadge("[Unnamed]", kWarnCol); }
            if (noMissions)  { ImGui::SameLine(); campBadge("[No missions]", kWarnCol); }
            if (reqExceeds)  { ImGui::SameLine(); campBadge("[Requires>Exists]", kWarnCol); }
            if (standinPre || standinVid)
            {
                ImGui::SameLine();
                campBadge("[Stand-in video]", kWarnCol);
            }
        }

        // Progression, in plain English.
        if (missionCount > 0 &&
            (unsigned long)grp.m_NumMissionsToComplete >= missionCount)
        {
            ImGui::Text("All missions required (%lu of %lu)",
                        missionCount, missionCount);
        }
        else if (missionCount > 0)
        {
            ImGui::Text("Complete %d of %lu missions to advance",
                        grp.m_NumMissionsToComplete, missionCount);
            ImGui::TextDisabled("Player may skip %lu",
                                missionCount - (unsigned long)grp.m_NumMissionsToComplete);
        }
        else
        {
            ImGui::Text("Missions Required to Advance: %d", grp.m_NumMissionsToComplete);
        }

        ImGui::Text("Briefing Video: %s",
                    campEmpty(grp.m_PreVideoFile) ? "(none)" : campStr(grp.m_PreVideoFile));
        ImGui::Text("Operation/Debrief Video: %s",
                    campEmpty(grp.m_VideoFile) ? "(none)" : campStr(grp.m_VideoFile));
        ImGui::Text("Operation File: %s",
                    campEmpty(grp.m_OperationFile) ? "(none)" : campStr(grp.m_OperationFile));
        ImGui::Text("Music Track: %d", grp.m_TuneNumber);

        // Missions.
        if (missionCount == 0)
        {
            ImGui::TextDisabled("Missions: (none)");
        }
        else
        {
            ImGui::Text("Missions:");
            for (const CMissionData& m : grp.m_MissionList)
            {
                ImGui::BulletText("%s",
                    campEmpty(m.m_MissionFile) ? "(empty filename)" : campStr(m.m_MissionFile));
            }
        }

        ImGui::Unindent();
    }

    ImGui::End();
}

#endif // MC2_IMGUI
