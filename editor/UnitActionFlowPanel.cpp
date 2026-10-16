//------------------------------------------------------------------------------
// UnitActionFlowPanel. See header.
//------------------------------------------------------------------------------
#include "stdafx.h"             // first: winsock2/windows include order

#include "UnitActionFlowPanel.h"
#include "imgui.h"

#include <cstring>
#include <string>
#include <vector>

namespace UnitActionFlowPanel
{
	// One flow row. kind: 'S'tate header, 'T'rigger, 'C'ondition, 'A'ction.
	struct FlowItem { char kind; std::string text; };

	static std::string trimClip(const char* b, const char* e)
	{
		while (b < e && (*b == ' ' || *b == '\t')) ++b;
		while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) --e;
		std::string out(b, e);
		if (out.size() > 90) out = out.substr(0, 90) + "...";
		return out;
	}

	static bool wordAt(const char* line, const char* p, const char* w)
	{
		size_t n = strlen(w);
		if (strncmp(p, w, n) != 0) return false;
		if (p != line && p[-1] != ' ' && p[-1] != '\t') return false;
		return true;
	}

	// Heuristic ABL idiom scan over the in-memory .abl text.
	static void parse(const char* text, std::string& fsmOut, std::vector<FlowItem>& items)
	{
		items.clear();
		fsmOut.clear();
		if (!text || !text[0]) return;

		const char* p = text;
		while (*p)
		{
			const char* nl = strchr(p, '\n');
			const char* end = nl ? nl : (p + strlen(p));
			// work on this line [p, end)
			std::string lineStr(p, end);
			const char* line = lineStr.c_str();

			// fsm <name>;
			if (fsmOut.empty()) {
				const char* f = strstr(line, "fsm ");
				if (f && (f == line || f[-1] == ' ' || f[-1] == '\t')) {
					f += 4; while (*f == ' ' || *f == '\t') ++f;
					std::string s;
					while (*f && *f != ';' && *f != ' ' && *f != '\t' && *f != '\r') s += *f++;
					if (!s.empty()) fsmOut = s;
				}
			}

			// state <name>;  -> new flow group.
			const char* st = strstr(line, "state ");
			bool isState = st && wordAt(line, st, "state");
			if (isState) {
				const char* q = st + 6; while (*q == ' ' || *q == '\t') ++q;
				std::string s;
				while (*q && *q != ';' && *q != ' ' && *q != '\t' && *q != '\r') s += *q++;
				if (!s.empty()) items.push_back({ 'S', s });
			} else {
				// Triggers: pilot events.
				if (strstr(line, "PILOT_EVENT_"))
					items.push_back({ 'T', trimClip(line, line + lineStr.size()) });
				else {
					const char* iff = strstr(line, "if");
					if (iff && wordAt(line, iff, "if") && (iff[2] == ' ' || iff[2] == '(') && strchr(line, '('))
						items.push_back({ 'C', trimClip(line, line + lineStr.size()) });
				}
				// Actions (the verbs that drive the unit).
				if (strstr(line, "corePower"))
					items.push_back({ 'A', strstr(line, "corePower(true") ? "power up" : "power down" });
				else if (strstr(line, "coreGuard"))                                  items.push_back({ 'A', "guard / hold" });
				else if (strstr(line, "coreEject"))                                  items.push_back({ 'A', "eject" });
				else if (strstr(line, "magicAttack") || strstr(line, "coreAttack"))  items.push_back({ 'A', "attack target" });
				else if (strstr(line, "coreMoveTo") || strstr(line, "PatrolPath") ||
				         strstr(line, "corePatrol"))                                 items.push_back({ 'A', "move / patrol" });
				else if (strstr(line, "transBack"))                                  items.push_back({ 'A', "-> previous state" });
				else if (strstr(line, "setState") ||
				         (strstr(line, "trans ") && !strstr(line, "transBack")))     items.push_back({ 'A', trimClip(line, line + lineStr.size()) });
			}

			if (!nl) break;
			p = nl + 1;
		}
	}

	void DrawInline(const char* ablText)
	{
		if (!ImGui::CollapsingHeader("Unit Action Flow", ImGuiTreeNodeFlags_DefaultOpen))
			return;

		// Cache the parse by text content (loadBrainText returns a stable buffer per
		// brain; re-parse only when the selected unit's brain text changes).
		static std::string s_cached = "\x01";
		static std::string s_fsm;
		static std::vector<FlowItem> s_items;
		const std::string cur = ablText ? ablText : "";
		if (cur != s_cached) { s_cached = cur; parse(cur.c_str(), s_fsm, s_items); }

		if (!s_fsm.empty()) ImGui::TextDisabled("fsm: %s", s_fsm.c_str());

		if (s_items.empty()) {
			ImGui::TextWrapped("No recognized state/trigger/action idioms in this brain "
			                   "(or .abl text unavailable).");
			return;
		}

		for (size_t i = 0; i < s_items.size(); ++i) {
			const FlowItem& it = s_items[i];
			if (it.kind == 'S') {
				ImGui::SeparatorText(it.text.c_str());
			} else {
				const char* tag = (it.kind == 'T') ? "[trigger]"
				                : (it.kind == 'C') ? "[when]"
				                                   : "[do]";
				ImVec4 col = (it.kind == 'T') ? ImVec4(1.00f, 0.80f, 0.25f, 1.f)   // trigger = amber
				           : (it.kind == 'C') ? ImVec4(0.55f, 0.80f, 1.00f, 1.f)   // condition = blue
				                              : ImVec4(0.55f, 1.00f, 0.55f, 1.f);  // action = green
				ImGui::Bullet(); ImGui::SameLine();
				ImGui::TextColored(col, "%s %s", tag, it.text.c_str());
			}
		}
	}
}
