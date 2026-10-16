/***************************************************************
* FILENAME: ObjectivesSummaryPanel.cpp
* DESCRIPTION: Read-only objectives explainer / mission-logic viewer.
*   Phase 2 polish: three-pane shell (Objective List | WHEN/THEN flow |
*   Inspector). Walks EditorData::instance->TeamsRef() -> per team CObjectives
*   (an EList of CObjective*).
*
*   LEFT pane lists every objective grouped first by team, then by CATEGORY
*   (Primary / Secondary / Hidden Triggers) with expanded status badges per row
*   ([Primary]/[Secondary], [Hidden], [Visible], [Marker], [Flag-gated],
*   [No Failure], and [!] when the objective carries a data warning).
*
*   CENTER pane renders the selected objective's readable card: a header line
*   with the badges, an Activation line in plain English, a data-only Warnings
*   block, then the WHEN (Completion) / THEN / Failure flow. RIGHT pane dumps the
*   raw scalar fields (inspector).
*
*   Condition/action lines are built from Description() (the type label) +
*   InstanceDescription() (the params). CRITICAL: the specific-unit and
*   specific-structure condition types deref a live object pointer with no null
*   guard inside InstanceDescription() (Objective.cpp:269 / :383, and they even
*   assert(displayName)). For those species we render Description() only plus an
*   "(object ref)" note, never InstanceDescription(). Area/flag/time/count
*   InstanceDescription() impls touch no object pointer and are safe.
*
*   Designer terminology lives ONLY in this panel's own label strings (e.g.
*   "Completion" for success conditions, "Objective Type" for priority). The
*   engine Description()/species-string arrays and .rc strings are never touched.
*
*   PURE-ADDITIVE, READ-ONLY: no mutate buttons, no save/load. Selection is a raw
*   CObjective* compared by identity; a mission reload simply clears it (the old
*   pointer no longer appears in the rebuilt list).
* DATE: 2026-06-27
****************************************************************/

// Pull stdafx first (MFC / Windows headers).
#include "stdafx.h"

#ifdef MC2_IMGUI

#include "imgui.h"

#include "ObjectivesSummaryPanel.h"

#include "EditorData.h"   // EditorData::instance, CTeams/CTeam, ObjectivesRef()
#include "Objective.h"    // CObjective(s), condition/action lists, species enums
#include <string>

#include <cstdio>
#include <cstring>

// GAME_MAX_PLAYERS is pulled in transitively (CTeams holds CTeam[GAME_MAX_PLAYERS]).

// ---------------------------------------------------------------------------
// Panel + selection state
// ---------------------------------------------------------------------------
static bool        s_open    = false;
static CObjective* s_sel     = nullptr;   // selected objective (identity compare)
static int         s_selTeam = -1;        // team the selection belongs to (label only)

void ObjectivesSummaryPanel::Open()   { s_open = true; }
void ObjectivesSummaryPanel::Close()  { s_open = false; }
void ObjectivesSummaryPanel::Toggle() { s_open = !s_open; }
bool ObjectivesSummaryPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Objective category for LIST grouping. PRIMARY = priority 1 (and not a hidden
// trigger). HIDDEN = IsHiddenTrigger(). SECONDARY = everything else (visible,
// non-primary). A hidden trigger always sorts into HIDDEN regardless of priority.
enum class obj_category_type { OBJCAT_PRIMARY, OBJCAT_SECONDARY, OBJCAT_HIDDEN };
using enum obj_category_type;

static obj_category_type objCategory(CObjective* obj)
{
    if (!obj)
        return OBJCAT_SECONDARY;
    if (obj->IsHiddenTrigger())
        return OBJCAT_HIDDEN;
    if (obj->Priority() == 1)
        return OBJCAT_PRIMARY;
    return OBJCAT_SECONDARY;
}

// True for the condition species whose InstanceDescription() dereferences a live
// EditorObject pointer (m_pUnit / m_pBuilding) with no null guard. We must NOT
// call InstanceDescription() on these from a read-only viewer.
static bool objIsObjectRefCondition(condition_species_type sp)
{
    switch (sp)
    {
        case DESTROY_SPECIFIC_ENEMY_UNIT:
        case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT:
        case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT:
        case CAPTURE_UNIT:
        case GUARD_SPECIFIC_UNIT:
        case DESTROY_SPECIFIC_STRUCTURE:
        case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE:
        case CAPTURE_STRUCTURE:
        case GUARD_SPECIFIC_STRUCTURE:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Per-objective warnings (computed from data only -- never deref object refs).
// Drives both the center-pane "Warnings" block and the [!] list-row badge.
// ---------------------------------------------------------------------------
constexpr int OBJ_MAX_WARNINGS = 4;

// Fill 'warn[]' with up to OBJ_MAX_WARNINGS plain-English warning strings and
// return the count. Returns 0 (no warnings) for a null objective.
static int objCollectWarnings(CObjective* obj, const char* warn[OBJ_MAX_WARNINGS])
{
    int n = 0;
    if (!obj)
        return 0;

    // No completion (success) condition: the objective can never be completed.
    if (obj->m_conditions.entries.size() == 0 && n < OBJ_MAX_WARNINGS)
        warn[n++] = "No completion condition -- objective can never be met.";

    // Hidden trigger that also asks to show a marker: the marker is meaningless
    // for a hidden trigger and will not be presented to the player.
    if (obj->IsHiddenTrigger() && obj->DisplayMarker() && n < OBJ_MAX_WARNINGS)
        warn[n++] = "Hidden trigger has 'show objective marker' set (marker ignored).";

    // Activate-on-flag with an empty flag id: the gate has no flag to watch.
    if (obj->ActivateOnFlag() && n < OBJ_MAX_WARNINGS)
    {
        const std::string& flag = obj->ActivateFlagID();
        const char* f = flag.c_str();
        if (!f || !f[0])
            warn[n++] = "Starts when flag is set, but no flag id is specified.";
    }

    // Reset-on-flag with an empty flag id: same problem on the reset gate.
    if (obj->ResetStatusOnFlag() && n < OBJ_MAX_WARNINGS)
    {
        const std::string& flag = obj->ResetStatusFlagID();
        const char* f = flag.c_str();
        if (!f || !f[0])
            warn[n++] = "Resets when flag is set, but no flag id is specified.";
    }

    return n;
}

static bool objHasWarning(CObjective* obj)
{
    const char* warn[OBJ_MAX_WARNINGS];
    return objCollectWarnings(obj, warn) > 0;
}

// Render one completion/failure condition line. Object-ref species print only
// their safe Description() label plus an "(object ref)" note. All others append
// the (safe) InstanceDescription() params when present.
static void objDrawCondition(CObjectiveCondition* cond)
{
    if (!cond)
    {
        ImGui::BulletText("(null condition)");
        return;
    }

    std::string desc = cond->Description();   // Own the localized label while ImGui uses its pointer.
    const char* label = desc.c_str();
    if (!label) label = "(condition)";

    if (objIsObjectRefCondition(cond->Species()))
    {
        ImGui::BulletText("%s  (object ref)", label);
        return;
    }

    std::string inst = cond->InstanceDescription();
    const char* params = inst.c_str();
    if (params && params[0])
        ImGui::BulletText("%s  %s", label, params);
    else
        ImGui::BulletText("%s", label);
}

// Action descriptions depend only on owned text and integers, so deleted object references cannot invalidate them.
static void objDrawAction(CObjectiveAction* act)
{
    if (!act)
    {
        ImGui::BulletText("(null action)");
        return;
    }

    std::string desc = act->Description();
    const char* label = desc.c_str();
    if (!label) label = "(action)";

    std::string inst = act->InstanceDescription();
    const char* params = inst.c_str();
    if (params && params[0])
        ImGui::BulletText("%s  %s", label, params);
    else
        ImGui::BulletText("%s", label);
}

// Compose the expanded status-badge string for a row / header, e.g.
// "[Primary][Visible][Marker][Flag-gated][No Failure][!]". 'cap' must be large
// enough; callers pass a 128-byte buffer.
static void objBadges(CObjective* obj, char* out, size_t cap)
{
    out[0] = '\0';
    if (!obj || cap == 0) return;

    // Append helper (bounded; no-op once the buffer is full).
    #define OBJ_APPEND(s) do { \
        size_t used = std::strlen(out); \
        if (used + 1 < cap) std::strncat(out, (s), cap - used - 1); \
    } while (0)

    // Objective type.
    if (obj->Priority() == 1) OBJ_APPEND("[Primary]");
    else                      OBJ_APPEND("[Secondary]");

    // Visibility.
    if (obj->IsHiddenTrigger()) OBJ_APPEND("[Hidden]");
    else                        OBJ_APPEND("[Visible]");

    // Marker.
    if (obj->DisplayMarker()) OBJ_APPEND("[Marker]");

    // Flag-gated activation.
    if (obj->ActivateOnFlag()) OBJ_APPEND("[Flag-gated]");

    // No failure path at all.
    if (obj->m_failureConditionList.entries.size() == 0 &&
        obj->m_failureActionList.entries.size() == 0)
        OBJ_APPEND("[No Failure]");

    // Warning marker.
    if (objHasWarning(obj)) OBJ_APPEND("[!]");

    #undef OBJ_APPEND
}

// Build the readable title text for a row / header.
static void objTitleText(CObjective* obj, int objIndex, char* out, size_t cap)
{
    if (obj->TitleUseResourceString())
    {
        std::snprintf(out, cap, "%d: (resource string #%d)", objIndex,
                      obj->TitleResourceStringID());
        return;
    }
    const std::string& title = obj->Title();
    const char* t = title.c_str();
    std::snprintf(out, cap, "%d: %s", objIndex, (t && t[0]) ? t : "(untitled)");
}

// Plain-English activation line for the card header.
static void objActivationText(CObjective* obj, char* out, size_t cap)
{
    if (!obj || cap == 0) { if (cap) out[0] = '\0'; return; }

    if (obj->ActivateOnFlag())
    {
        const std::string& flag = obj->ActivateFlagID();
        const char* f = flag.c_str();
        if (f && f[0])
            std::snprintf(out, cap, "Starts when flag '%s' is set", f);
        else
            std::snprintf(out, cap, "Starts when flag is set (no flag id)");
        return;
    }

    if (obj->PreviousPrimaryObjectiveMustBeComplete())
    {
        std::snprintf(out, cap, "Starts after the previous primary objective");
        return;
    }
    if (obj->AllPreviousPrimaryObjectivesMustBeComplete())
    {
        std::snprintf(out, cap, "Starts after all previous primary objectives");
        return;
    }

    std::snprintf(out, cap, "Active from mission start");
}

// CENTER pane: readable card -- header badges, activation, warnings, then the
// WHEN (Completion) / THEN / Failure flow for one objective.
static void objDrawFlow(CObjective* obj)
{
    if (!obj)
    {
        ImGui::TextDisabled("Select an objective from the list.");
        return;
    }

    // Title (strip the leading "idx: " the list adds; the card shows no index).
    char title[160];
    objTitleText(obj, 0, title, sizeof(title));
    const char* colon = std::strchr(title, ':');
    ImGui::TextWrapped("%s", colon ? colon + 2 : title);

    // Header badge line.
    char badges[128];
    objBadges(obj, badges, sizeof(badges));
    ImGui::TextDisabled("%s", badges);

    // Activation line in plain English.
    char activation[160];
    objActivationText(obj, activation, sizeof(activation));
    ImGui::Text("Activation: %s", activation);

    ImGui::Separator();

    // Objective type + visibility (designer terminology).
    ImGui::Text("Objective Type: %s",
                (obj->Priority() == 1) ? "Primary" : "Secondary");
    ImGui::Text("Visibility: %s",
                obj->IsHiddenTrigger() ? "Hidden trigger" : "Visible");

    if (obj->DisplayMarker())
        ImGui::Text("Objective marker: shown at (%.1f, %.1f)",
                    obj->MarkerX(), obj->MarkerY());
    else
        ImGui::TextDisabled("Objective marker: none");

    // Warnings block (data-only).
    const char* warn[OBJ_MAX_WARNINGS];
    int nWarn = objCollectWarnings(obj, warn);
    if (nWarn > 0)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Warnings");
        for (int i = 0; i < nWarn; ++i)
            ImGui::BulletText("%s", warn[i]);
    }

    // WHEN (Completion): success conditions. CObjective inherits
    // CObjectiveConditionList, so iterating *this* objective walks its
    // completion conditions.
    ImGui::Separator();
    ImGui::TextUnformatted("WHEN (Completion conditions)");
    if (obj->m_conditions.entries.size() == 0)
        ImGui::TextDisabled("  (no completion conditions)");
    else
        for (auto it = obj->m_conditions.entries.begin(); it != obj->m_conditions.entries.end(); it++)
            objDrawCondition(*it);

    // THEN: actions fired on completion.
    ImGui::TextUnformatted("THEN (actions)");
    if (obj->m_actionList.entries.size() == 0)
        ImGui::TextDisabled("  (no actions)");
    else
        for (auto it = obj->m_actionList.entries.begin(); it != obj->m_actionList.entries.end(); it++)
            objDrawAction(*it);

    // Failure block (only if present).
    if (obj->m_failureConditionList.entries.size() > 0 || obj->m_failureActionList.entries.size() > 0)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Failure conditions");
        if (obj->m_failureConditionList.entries.size() == 0)
            ImGui::TextDisabled("  (none)");
        else
            for (auto it = obj->m_failureConditionList.entries.begin(); it != obj->m_failureConditionList.entries.end(); it++)
                objDrawCondition(*it);

        ImGui::TextUnformatted("Failure actions");
        if (obj->m_failureActionList.entries.size() == 0)
            ImGui::TextDisabled("  (none)");
        else
            for (auto it = obj->m_failureActionList.entries.begin(); it != obj->m_failureActionList.entries.end(); it++)
                objDrawAction(*it);
    }
    else
    {
        ImGui::Separator();
        ImGui::TextDisabled("No failure path (objective cannot be failed).");
    }
}

// RIGHT pane: raw scalar fields for the selected objective (designer labels).
static void objDrawInspector(CObjective* obj)
{
    if (!obj)
    {
        ImGui::TextDisabled("(no selection)");
        return;
    }

    ImGui::Text("Objective Type:  %s", (obj->Priority() == 1) ? "Primary" : "Secondary");
    ImGui::Text("Priority value:  %d", obj->Priority());
    ImGui::Text("Resource pts:    %d", obj->ResourcePoints());
    ImGui::Text("Hidden trigger:  %s", obj->IsHiddenTrigger() ? "yes" : "no");
    ImGui::Text("Show marker:     %s", obj->DisplayMarker() ? "yes" : "no");
    ImGui::Text("Marker X/Y:      %.1f / %.1f", obj->MarkerX(), obj->MarkerY());
    ImGui::Separator();
    ImGui::Text("Prev primary req:    %s", obj->PreviousPrimaryObjectiveMustBeComplete() ? "yes" : "no");
    ImGui::Text("All prev primary:    %s", obj->AllPreviousPrimaryObjectivesMustBeComplete() ? "yes" : "no");
    ImGui::Text("Starts on flag:      %s", obj->ActivateOnFlag() ? "yes" : "no");
    {
        const std::string& flag = obj->ActivateFlagID();
        const char* f = flag.c_str();
        ImGui::Text("Start flag id:       %s", (f && f[0]) ? f : "(none)");
    }
    ImGui::Text("Resets on flag:      %s", obj->ResetStatusOnFlag() ? "yes" : "no");
    {
        const std::string& flag = obj->ResetStatusFlagID();
        const char* f = flag.c_str();
        ImGui::Text("Reset flag id:       %s", (f && f[0]) ? f : "(none)");
    }
    ImGui::Separator();
    ImGui::Text("Completion conds:    %lu", (unsigned long)obj->m_conditions.entries.size());
    ImGui::Text("Completion actions:  %lu", (unsigned long)obj->m_actionList.entries.size());
    ImGui::Text("Failure conditions:  %lu", (unsigned long)obj->m_failureConditionList.entries.size());
    ImGui::Text("Failure actions:     %lu", (unsigned long)obj->m_failureActionList.entries.size());
    ImGui::Separator();
    ImGui::Text("Title resource str:  %s", obj->TitleUseResourceString() ? "yes" : "no");
    ImGui::Text("Model ID:            %ld", obj->ModelID());
}

// ---------------------------------------------------------------------------
// LEFT pane: one objective row (Selectable). Returns true if this row is the
// live selection (so the caller can mark the selection still valid).
// ---------------------------------------------------------------------------
static bool objDrawListRow(CObjective* obj, int team, int objIndex)
{
    if (!obj)
        return false;

    char title[160], badges[128], row[320];
    objTitleText(obj, objIndex, title, sizeof(title));
    objBadges(obj, badges, sizeof(badges));
    std::snprintf(row, sizeof(row), "%s  %s###obj_%p", title, badges, (void*)obj);

    bool selected = (obj == s_sel);
    if (ImGui::Selectable(row, selected))
    {
        s_sel = obj;
        s_selTeam = team;
    }
    return (obj == s_sel);
}

// Render the objectives of one team grouped by category, under collapsing
// section headers. Updates *selStillValid when the live selection is drawn.
static void objDrawTeamCategories(CObjectives& objs, int team, bool* selStillValid)
{
    static const char*           kCatLabel[3] = { "PRIMARY", "SECONDARY", "HIDDEN TRIGGERS" };
    static const obj_category_type kCatOrder[3] = { OBJCAT_PRIMARY, OBJCAT_SECONDARY, OBJCAT_HIDDEN };

    for (int c = 0; c < 3; ++c)
    {
        // Count members of this category first; skip empty sections.
        int catCount = 0;
        {
            for (CObjectives::EIterator it = objs.Begin(); !it.IsDone(); it++)
            {
                CObjective* obj = *it;
                if (obj && objCategory(obj) == kCatOrder[c])
                    ++catCount;
            }
        }
        if (catCount == 0)
            continue;

        char secHdr[80];
        std::snprintf(secHdr, sizeof(secHdr), "%s (%d)###sec_%d_%d",
                      kCatLabel[c], catCount, team, c);
        if (!ImGui::CollapsingHeader(secHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::Indent();
        int idx = 0;   // index across the team's full list (stable per objective)
        for (CObjectives::EIterator it = objs.Begin(); !it.IsDone(); it++, ++idx)
        {
            CObjective* obj = *it;
            if (!obj || objCategory(obj) != kCatOrder[c])
                continue;
            if (objDrawListRow(obj, team, idx))
                *selStillValid = true;
        }
        ImGui::Unindent();
    }
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void ObjectivesSummaryPanel::Draw()
{
    if (!s_open)
        return;

    // Float free + NoDocking so the autodock right-column can never swallow it into
    // the narrow tab strip (where a single window can't be resized). Default size +
    // the child-pane widths below scale with the editor UI scale (FontGlobalScale)
    // so the 3-pane layout stays readable at HiDPI font sizes.
    const float k = ImGui::GetIO().FontGlobalScale > 0.f ? ImGui::GetIO().FontGlobalScale : 1.f;
    ImGui::SetNextWindowSize(ImVec2(820.f * k, 520.f * k), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Objectives Overview", &s_open, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only mission-logic viewer. Edit via the legacy Objectives dialog.");
    ImGui::Separator();

    if (!EditorData::instance)
    {
        ImGui::TextDisabled("No mission loaded.");
        ImGui::End();
        return;
    }

    CTeams& teams = EditorData::instance->TeamsRef();

    // Validate the current selection still exists in the live data; otherwise drop
    // it (mission reloads rebuild the objective objects).
    bool selStillValid = false;

    // ---- LEFT pane: objective list grouped by team, then category ----------
    ImGui::BeginChild("obj_list", ImVec2(280.f * k, 0.f), true);
    bool anyObjectives = false;
    for (int t = 0; t < GAME_MAX_PLAYERS; ++t)
    {
        CObjectives& objs = teams.TeamRef(t).ObjectivesRef();
        if (objs.Count() == 0)
            continue;
        anyObjectives = true;

        char teamHdr[64];
        std::snprintf(teamHdr, sizeof(teamHdr), "Team %d (%lu)###objteam%d",
                      t, (unsigned long)objs.Count(), t);
        if (!ImGui::CollapsingHeader(teamHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        objDrawTeamCategories(objs, t, &selStillValid);
    }
    if (!anyObjectives)
        ImGui::TextDisabled("No objectives defined.");
    ImGui::EndChild();

    if (!selStillValid)
    {
        s_sel = nullptr;
        s_selTeam = -1;
    }

    // ---- CENTER pane: readable card ----------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("obj_flow", ImVec2(-230.f * k, 0.f), true);
    if (s_sel && s_selTeam >= 0)
        ImGui::TextDisabled("Team %d", s_selTeam);
    objDrawFlow(s_sel);
    ImGui::EndChild();

    // ---- RIGHT pane: inspector ---------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("obj_inspector", ImVec2(0.f, 0.f), true);
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();
    objDrawInspector(s_sel);
    ImGui::EndChild();

    ImGui::End();
}

#endif // MC2_IMGUI
