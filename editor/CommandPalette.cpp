/***************************************************************
* FILENAME: CommandPalette.cpp
* DESCRIPTION: Searchable Command Palette (UE/VSCode style) ImGui panel.
*   See CommandPalette.h. A search InputText at top filters a static
*   command table by label and category (case-insensitive substring).
*   Clicking a row or pressing Enter on the top filtered result
*   dispatches via EditorInterface::handleNewMenuMessage(mfcId),
*   which routes through the MFC ON_COMMAND switch and preserves undo.
* DATE: 2026-06-10
****************************************************************/

#include "stdafx.h"

#include "CommandPalette.h"

#include "imgui.h"

// EditorObjectMgr.h must precede EditorInterface.h: it establishes the
// winsock2.h include order (before windows.h pulls the legacy winsock.h),
// matching SceneOutliner.cpp. Including EditorInterface.h standalone first
// triggers winsock struct-redefinition errors.
#include "EditorObjectMgr.h"
#include "EditorInterface.h"   // handleNewMenuMessage, instance()

#include "resource.h"          // menu ID constants

#include <cctype>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Command table
// ---------------------------------------------------------------------------

struct PaletteCommand
{
    int         mfcId;
    const char* label;
    const char* category;
    const char* hotkey;   // display-only; "" if unknown
};

// ~25 commands covering the most-used editor actions.
static const PaletteCommand s_commands[] =
{
    // File
    { ID_FILE_NEW2,          "New",                  "File",    "Ctrl+N"  },
    { ID_FILE_OPEN2,         "Open",                 "File",    "Ctrl+O"  },
    { ID_FILE_SAVE,          "Save",                 "File",    "Ctrl+S"  },
    { ID_FILE_SAVEAS,        "Save As",              "File",    ""        },
    { ID_FILE_QUICKSAVE,     "Quick Save",           "File",    "F5"      },
    { ID_FILE_EXIT,          "Exit",                 "File",    "Alt+F4"  },

    // Edit
    { ID_EDIT_UNDO2,         "Undo",                 "Edit",    "Ctrl+Z"  },
    { ID_EDIT_REDO2,         "Redo",                 "Edit",    "Ctrl+Y"  },

    // Tools
    { ID_OTHER_SELECT,       "Select",               "Tools",   ""        },
    { ID_OTHER_FLATTEN,      "Flatten",              "Tools",   ""        },
    { ID_OTHER_ERASE,        "Erase",                "Tools",   ""        },
    { ID_OTHER_LAYMINES,     "Lay Mines",            "Tools",   ""        },
    { ID_OTHER_DAMAGE,       "Damage",               "Tools",   ""        },

    // Overlays
    { ID_OVERLAYS_DIRTROAD,  "Paint Dirt Road",      "Overlays","" },
    { ID_OVERLAYS_PAEVEDROAD,"Paint Paved Road",     "Overlays","" },
    { ID_OVERLAYS_ROUGH,     "Paint Rocks",          "Overlays","" },

    // Terrain
    { ID_PURGE_TRANSITIONS,       "Purge Transitions",   "Terrain", "" },
    { ID_OTHER_REFRACTALIZETERRAIN,"Refractalize",        "Terrain", "" },
    { ID_SELECT_TERRAIN_TYPE,     "Select Terrain Type", "Terrain", "" },
    { ID_OTHER_RELOADBASETEXTURE, "Reload Base Texture", "Terrain", "" },

    // View
    { ID_VIEW_ORTHOGRAPHICCAMERA, "Orthographic Camera", "View",    "" },
    { ID_VIEW_SHOWPASSABILITYMAP, "Show Passability Map","View",    "" },

    // Mission
    { ID_MISSION_SETTINGS,   "Mission Settings",     "Mission", "" },
    { ID_MISSION_REFRESH_DEFINITIONS, "Refresh mission definitions from catalog", "Mission", "" },
    { ID_MISSION_AUTHOR_CAMERA_ZOOM, "Use current zoom for mission camera", "Mission", "" },
    { ID_CAMPAIGN_EDITOR,    "Campaign Editor",      "Mission", "" },

    // Foliage
    { ID_FOREST_TOOL,        "Forest Tool",          "Foliage", "" },
};

static const int s_commandCount = (int)(sizeof(s_commands) / sizeof(s_commands[0]));

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

static bool s_open        = false;
static bool s_justOpened  = false;   // true on the first frame after Open()

void CommandPalette::Open()
{
    s_open       = true;
    s_justOpened = true;
}
void CommandPalette::Close()  { s_open = false; }
void CommandPalette::Toggle() { if (s_open) Close(); else Open(); }
bool CommandPalette::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Case-insensitive substring check. Empty needle matches everything.
static bool cpContains(const char* haystack, const char* needle)
{
    if (!needle || !needle[0])
        return true;
    if (!haystack)
        return false;

    for (const char* h = haystack; *h; ++h)
    {
        const char* a = h;
        const char* b = needle;
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

// True when the command matches the filter text (label OR category).
static bool cpMatchesFilter(const PaletteCommand& cmd, const char* filter)
{
    if (!filter || !filter[0])
        return true;
    return cpContains(cmd.label, filter) || cpContains(cmd.category, filter);
}

// Dispatch a command and close the palette.
static void cpDispatch(int mfcId)
{
    EditorInterface* ei = EditorInterface::instance();
    if (ei)
        ei->handleNewMenuMessage((long)mfcId);
    CommandPalette::Close();
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void CommandPalette::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(440.f, 360.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
               ImGui::GetIO().DisplaySize.y * 0.2f),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.0f));

    if (!ImGui::Begin("Command Palette", &s_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::End();
        return;
    }

    // Search box ─────────────────────────────────────────────────────────────
    static char s_filter[128] = "";

    // Auto-focus the search box on the first frame the palette opens.
    if (s_justOpened)
    {
        ImGui::SetKeyboardFocusHere(0);
        s_filter[0] = '\0';
        s_justOpened = false;
    }

    ImGui::SetNextItemWidth(-1.f);
    bool filterEdited = ImGui::InputTextWithHint(
        "##cpFilter", "Type to search commands...",
        s_filter, sizeof(s_filter));
    (void)filterEdited;

    ImGui::Separator();

    // Filtered command list ───────────────────────────────────────────────────
    // Track the first visible row so Enter dispatches it.
    int firstVisibleIdx = -1;

    // Check for Enter key to dispatch the first visible result.
    const bool enterPressed = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                           && ImGui::IsKeyPressed(ImGuiKey_Enter);

    ImGui::BeginChild("##cpList", ImVec2(0.f, 0.f), false);

    for (int i = 0; i < s_commandCount; ++i)
    {
        const PaletteCommand& cmd = s_commands[i];
        if (!cpMatchesFilter(cmd, s_filter))
            continue;

        if (firstVisibleIdx < 0)
            firstVisibleIdx = i;

        // Row label: "Category: Label   (hotkey)" or "Category: Label"
        char rowLabel[192];
        if (cmd.hotkey && cmd.hotkey[0])
            std::snprintf(rowLabel, sizeof(rowLabel),
                          "%s: %s   (%s)", cmd.category, cmd.label, cmd.hotkey);
        else
            std::snprintf(rowLabel, sizeof(rowLabel),
                          "%s: %s", cmd.category, cmd.label);

        ImGui::PushID(i);
        if (ImGui::Selectable(rowLabel, false, ImGuiSelectableFlags_SpanAllColumns))
            cpDispatch(cmd.mfcId);
        ImGui::PopID();
    }

    ImGui::EndChild();

    // Enter key fires the first filtered result.
    if (enterPressed && firstVisibleIdx >= 0)
        cpDispatch(s_commands[firstVisibleIdx].mfcId);

    // Escape closes without dispatching.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        Close();
    }

    ImGui::End();
}
