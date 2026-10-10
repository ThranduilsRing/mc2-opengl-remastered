//-------------------------------------------------------------------------------------------------
// EditorPlaytestResults.cpp -- see EditorPlaytestResults.h.
//
// Pure post-hoc reader of a playtest stdout log. Extracts the child exit code, the [SMOKE v1]
// summary verdict, frame count, [PERF v1] p50/p99, [MOVER v1] line/unique/team counts, and any
// warning/fatal/crash markers; classifies a PASS/WARN/FAIL status chip per the v0 spec:
//
//   PASS  = child exit 0 AND (no SMOKE summary OR SMOKE result=pass) AND no warnings/crash.
//   WARN  = child exit 0 but SMOKE result=fail/early_exit, OR warning markers present.
//   FAIL  = nonzero/absent child exit, a crash/fatal marker, or a missing expected log.
//
// The torrin case (result=fail reason=early_exit with exit 0) lands in WARN -- never silent green.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"   // MFC / <windows.h>

#include "EditorPlaytestResults.h"
#include "EditorPlaytest.h"

#include "imgui.h"

#include <shellapi.h>   // ShellExecuteA

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace EditorPlaytestResults
{
namespace {

Summary s_result;          // current panel result (valid=false until first run/open parse)
bool    s_open = false;
bool    s_triedLazyOpen = false;   // parsed LastLogPath() once on open already?

// --- small text helpers ---------------------------------------------------------------

// Read an entire file into a std::string. Returns false if unreadable/missing.
bool ReadWholeFile(const char* path, std::string& out)
{
	out.clear();
	if (!path || !path[0])
		return false;
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || !f)
		return false;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return std::string();
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

bool Contains(const std::string& hay, const char* needle)
{
	return hay.find(needle) != std::string::npos;
}

// Find "key=" in `line` (token-boundary-ish: preceded by start or space) and return its
// space-delimited value, or "" if absent.
std::string KvValue(const std::string& line, const char* key)
{
	std::string k = key;
	k += "=";
	size_t pos = 0;
	while ((pos = line.find(k, pos)) != std::string::npos)
	{
		// require the char before to be a boundary so "reason=" doesn't match "x_reason="
		if (pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '\t')
		{
			size_t vstart = pos + k.size();
			size_t vend = line.find_first_of(" \t\r\n", vstart);
			return line.substr(vstart, (vend == std::string::npos) ? std::string::npos
			                                                        : vend - vstart);
		}
		pos += k.size();
	}
	return std::string();
}

} // anonymous namespace

// --- parser ---------------------------------------------------------------------------

Summary ParseLogText(const std::string& text, const char* logPath)
{
	Summary s;
	s.logPath = (logPath ? logPath : "");
	s.valid   = !text.empty();
	if (!s.valid)
		return s;

	std::set<long> moverIds;
	std::set<long> moverTeams;

	// Split on lines, walking each. (One pass; markers are sparse so this is cheap.)
	size_t start = 0;
	while (start <= text.size())
	{
		size_t nl = text.find('\n', start);
		std::string line = text.substr(start, (nl == std::string::npos) ? std::string::npos
		                                                                 : nl - start);
		start = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

		// Strip a trailing CR.
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;

		// Child exit tail: "[playtest] exit code N (PASS|FAIL)"
		if (Contains(line, "[playtest] exit code "))
		{
			int code = 0;
			if (sscanf_s(line.c_str(), "[playtest] exit code %d", &code) == 1)
			{
				s.childExit = code;
				s.haveExit  = true;
			}
			continue;
		}

		// [SMOKE v1] event=summary result=.. reason=.. frames=..
		if (Contains(line, "[SMOKE v1] event=summary"))
		{
			s.haveSmoke   = true;
			s.smokeResult = KvValue(line, "result");
			s.smokeReason = KvValue(line, "reason");
			std::string fr = KvValue(line, "frames");
			if (!fr.empty())
				s.frames = atol(fr.c_str());
			continue;
		}

		// [PERF v1] avg_fps=.. p50_ms=.. p99_ms=.. samples=..
		if (Contains(line, "[PERF v1]"))
		{
			s.havePerf = true;
			std::string a = KvValue(line, "avg_fps");
			std::string p50 = KvValue(line, "p50_ms");
			std::string p99 = KvValue(line, "p99_ms");
			std::string samp = KvValue(line, "samples");
			if (!a.empty())   s.avgFps = (float)atof(a.c_str());
			if (!p50.empty()) s.p50ms  = (float)atof(p50.c_str());
			if (!p99.empty()) s.p99ms  = (float)atof(p99.c_str());
			if (s.frames < 0 && !samp.empty())
				s.frames = atol(samp.c_str());
			continue;
		}

		// [MOVER v1] id=.. team=.. (per-mover line; ignore the "begin" delimiter).
		if (line.compare(0, 11, "[MOVER v1] ") == 0)
		{
			++s.moverLines;
			std::string id = KvValue(line, "id");
			std::string tm = KvValue(line, "team");
			if (!id.empty())
				moverIds.insert(atol(id.c_str()));
			if (!tm.empty())
				moverTeams.insert(atol(tm.c_str()));
			continue;
		}

		// Warning / fatal / crash markers.
		if (Contains(line, "FATAL") || Contains(line, "ASSERT") ||
		    Contains(line, "Crash Report") || Contains(line, "[ESMOKE v1] event=modal"))
		{
			std::string t = Trim(line);
			if (s.warnings.size() < 64)
				s.warnings.push_back(t);
			// HARD crash/fatal markers -> FAIL: only FATAL and an actual "Crash Report".
			// ASSERT (incl. the engine's benign "[ASSERT 0] couldn't open file ..." soft
			// asserts that fire while the game runs to a clean exit 0) and modal events are
			// WARN-level markers, not hard failures -- they collect into warnings without
			// forcing FAIL when the child still exited 0.
			if (Contains(line, "FATAL") || Contains(line, "Crash Report"))
				s.haveCrashMarker = true;
		}
	}

	s.uniqueMovers = (int)moverIds.size();
	s.teamCount    = (int)moverTeams.size();

	// --- classification (v0 spec) ---------------------------------------------------
	const bool exitOk = s.haveExit && s.childExit == 0;
	const bool smokeFail = s.haveSmoke &&
		(s.smokeResult == "fail" || s.smokeResult == "early_exit");
	const bool smokeEarly = (s.smokeReason == "early_exit");
	const bool anyWarnings = !s.warnings.empty();

	if (!s.haveExit)
	{
		s.status = Status::Fail;
		s.statusReason = "no [playtest] exit-code line (run incomplete / missing log)";
	}
	else if (s.childExit != 0)
	{
		s.status = Status::Fail;
		char r[96];
		snprintf(r, sizeof(r), "child exited nonzero (%d)", s.childExit);
		s.statusReason = r;
	}
	else if (s.haveCrashMarker)
	{
		s.status = Status::Fail;
		s.statusReason = "crash/fatal marker present despite exit 0";
	}
	else if (smokeFail || smokeEarly)
	{
		s.status = Status::Warn;
		std::string why = "exit 0 but [SMOKE v1] result=" +
			(s.smokeResult.empty() ? std::string("?") : s.smokeResult);
		if (!s.smokeReason.empty())
			why += " reason=" + s.smokeReason;
		s.statusReason = why;
	}
	else if (anyWarnings)
	{
		s.status = Status::Warn;
		s.statusReason = "exit 0 + SMOKE ok but warning markers present";
	}
	else
	{
		s.status = Status::Pass;
		if (s.haveSmoke)
			s.statusReason = "child exit 0 and [SMOKE v1] result=pass";
		else
			s.statusReason = "child exit 0 (no SMOKE summary in log)";
	}

	(void)exitOk;
	return s;
}

Summary ParseLogFile(const char* path)
{
	std::string text;
	if (!ReadWholeFile(path, text))
	{
		Summary s;
		s.logPath = (path ? path : "");
		s.valid   = false;
		s.status  = Status::Fail;
		s.statusReason = "log file missing/unreadable";
		return s;
	}
	return ParseLogText(text, path);
}

void SetResult(const Summary& s)
{
	s_result = s;
}

// --- panel visibility -----------------------------------------------------------------

void Open()  { s_open = true; }
void Close() { s_open = false; }
void Toggle() { s_open = !s_open; }
bool IsOpen() { return s_open; }

// --- panel ----------------------------------------------------------------------------

namespace {

// Status chip colour + label.
void DrawChip(Status st, const char* reason)
{
	ImVec4 col;
	const char* label;
	switch (st)
	{
	case Status::Pass: col = ImVec4(0.20f, 0.75f, 0.30f, 1.0f); label = "PASS"; break;
	case Status::Warn: col = ImVec4(0.90f, 0.70f, 0.15f, 1.0f); label = "WARN"; break;
	case Status::Fail: col = ImVec4(0.85f, 0.25f, 0.25f, 1.0f); label = "FAIL"; break;
	default:           col = ImVec4(0.50f, 0.50f, 0.50f, 1.0f); label = "----"; break;
	}
	ImGui::PushStyleColor(ImGuiCol_Button, col);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
	ImGui::Button(label, ImVec2(72.f, 0.f));
	ImGui::PopStyleColor(3);
	if (reason && reason[0])
	{
		ImGui::SameLine();
		ImGui::TextWrapped("%s", reason);
	}
}

} // anonymous namespace

void Draw()
{
	if (!s_open)
		return;

	// Lazy first-open parse: if no result yet, parse the last archived log so a prior
	// run (even after editor restart) shows immediately.
	if (!s_triedLazyOpen && !s_result.valid)
	{
		s_triedLazyOpen = true;
		const char* lp = EditorPlaytest::LastLogPath();
		if (lp && lp[0])
			s_result = ParseLogFile(lp);
	}

	ImGui::SetNextWindowSize(ImVec2(520.f, 440.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Playtest Results", &s_open))
	{
		ImGui::End();
		return;
	}

	// Re-parse button (re-read the same log; or pick up the most recent run).
	if (ImGui::Button("Refresh"))
	{
		const char* lp = s_result.valid ? s_result.logPath.c_str()
		                                : EditorPlaytest::LastLogPath();
		if (lp && lp[0])
			s_result = ParseLogFile(lp);
	}

	const Summary& s = s_result;

	if (!s.valid)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("No playtest run parsed yet. Run a Playtest, then open this panel.");
		ImGui::End();
		return;
	}

	// --- Open log folder / Copy summary --------------------------------------------
	ImGui::SameLine();
	if (ImGui::Button("Open log folder") && !s.logPath.empty())
	{
		// explorer /select,"<path>" highlights the log in its folder.
		std::string param = "/select,\"" + s.logPath + "\"";
		ShellExecuteA(nullptr, "open", "explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
	}

	ImGui::SameLine();
	if (ImGui::Button("Copy summary"))
	{
		char buf[1024];
		snprintf(buf, sizeof(buf),
			"Playtest Results: %s\n"
			"  reason: %s\n"
			"  child exit: %s%d\n"
			"  SMOKE: %s%s%s\n"
			"  frames: %ld\n"
			"  PERF p50/p99 ms: %s%.2f / %.2f\n"
			"  movers: %d lines, %d unique, %d teams\n"
			"  warnings: %d\n"
			"  log: %s\n",
			(s.status == Status::Pass) ? "PASS" :
			(s.status == Status::Warn) ? "WARN" :
			(s.status == Status::Fail) ? "FAIL" : "UNKNOWN",
			s.statusReason.c_str(),
			s.haveExit ? "" : "(absent) ", s.haveExit ? s.childExit : -1,
			s.haveSmoke ? s.smokeResult.c_str() : "(none)",
			s.smokeReason.empty() ? "" : " reason=",
			s.smokeReason.c_str(),
			s.frames,
			s.havePerf ? "" : "(absent) ", s.p50ms, s.p99ms,
			s.moverLines, s.uniqueMovers, s.teamCount,
			(int)s.warnings.size(),
			s.logPath.c_str());
		ImGui::SetClipboardText(buf);
	}

	ImGui::Separator();

	// --- Status chip ----------------------------------------------------------------
	DrawChip(s.status, s.statusReason.c_str());
	ImGui::Separator();

	// --- Last-run summary -----------------------------------------------------------
	if (ImGui::CollapsingHeader("Summary", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (s.haveExit)
			ImGui::Text("Child exit code: %d", s.childExit);
		else
			ImGui::TextColored(ImVec4(0.85f, 0.25f, 0.25f, 1.f),
				"Child exit code: (no [playtest] exit line)");

		if (s.haveSmoke)
		{
			ImVec4 c = (s.smokeResult == "pass") ? ImVec4(0.4f, 0.9f, 0.5f, 1.f)
			                                      : ImVec4(0.9f, 0.7f, 0.15f, 1.f);
			ImGui::TextColored(c, "SMOKE summary: result=%s%s%s",
				s.smokeResult.c_str(),
				s.smokeReason.empty() ? "" : "  reason=",
				s.smokeReason.c_str());
		}
		else
			ImGui::TextDisabled("SMOKE summary: (not present)");

		if (s.frames >= 0)
			ImGui::Text("Frames: %ld", s.frames);
		else
			ImGui::TextDisabled("Frames: (unknown)");
	}

	// --- Telemetry counts -----------------------------------------------------------
	if (ImGui::CollapsingHeader("Telemetry", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (s.havePerf)
			ImGui::Text("PERF: avg %.1f fps   p50 %.2f ms   p99 %.2f ms",
				s.avgFps, s.p50ms, s.p99ms);
		else
			ImGui::TextDisabled("PERF: (no [PERF v1] line)");

		if (s.moverLines > 0)
			ImGui::Text("Movers: %d lines   %d unique id   %d distinct teams",
				s.moverLines, s.uniqueMovers, s.teamCount);
		else
			ImGui::TextDisabled("Movers: (no [MOVER v1] telemetry)");
	}

	// --- Warnings -------------------------------------------------------------------
	if (s.warnings.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("No warning/fatal/crash markers.");
	}
	else
	{
		ImVec4 wc = s.haveCrashMarker ? ImVec4(0.85f, 0.25f, 0.25f, 1.f)
		                              : ImVec4(0.90f, 0.70f, 0.15f, 1.f);
		char hdr[64];
		snprintf(hdr, sizeof(hdr), "Warnings / markers (%d)###pt_warn",
			(int)s.warnings.size());
		ImGui::PushStyleColor(ImGuiCol_Text, wc);
		bool openHdr = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleColor();
		if (openHdr)
		{
			ImGui::BeginChild("##warns", ImVec2(0.f, 120.f), true);
			for (size_t i = 0; i < s.warnings.size(); ++i)
				ImGui::TextWrapped("%s", s.warnings[i].c_str());
			ImGui::EndChild();
		}
	}

	ImGui::Separator();
	ImGui::TextDisabled("Log: %s", s.logPath.c_str());

	ImGui::End();
}

} // namespace EditorPlaytestResults
