//------------------------------------------------------------------------------
// BeautySidecarPreview — EDITOR-SIDECAR-PREVIEW-1 (B7a). See header.
//------------------------------------------------------------------------------
#include "BeautySidecarPreview.h"
#include "Gates/gate.h"

#include "EditorData.h"
#include "terrain.h"
#include "mapdata.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace BeautySidecarPreview
{
	static std::vector<float> s_origElev;   // pre-apply snapshot (full grid)
	static int  s_side    = 0;
	static int  s_changed = 0;
	static bool s_applied = false;
	static bool s_autoApplyDone = false;
	static char s_status[256] = "no sidecar loaded";

	// B7b heatmap: retained delta + toggle (no terrain mutation needed to view).
	static std::vector<float> s_delta;
	static int   s_deltaSide   = 0;
	static float s_deltaMaxAbs = 0.0f;
	static bool  s_showHeatmap = false;

	// B7c protected-zone overlay.
	static std::vector<unsigned char> s_protected;
	static int  s_protSide      = 0;
	static bool s_showProtected = false;

	// Sidecar dirs are named by the mission FILE stem (mc2_01.beauty). Use the
	// terrain/colormap name (Terrain::terrainName, set on load to the mission stem,
	// terrain.cpp:640) — NOT EditorData::MissionName(), which returns the .fit
	// display TITLE (e.g. "Operation ...") and never matches the sidecar filename.
	static const char* missionStem()
	{
		if (Terrain::terrainName && Terrain::terrainName[0])
			return Terrain::terrainName;
		return EditorData::instance ? EditorData::instance->MissionName().Data() : "";
	}

	static void retainDelta(const std::vector<float>& d, int side)
	{
		s_delta = d;
		s_deltaSide = side;
		s_deltaMaxAbs = 0.0f;
		for (float v : d) { float a = v < 0 ? -v : v; if (a > s_deltaMaxAbs) s_deltaMaxAbs = a; }
	}

	// Locate <mission>.beauty/height_delta.r32 in a few candidate dirs (editor
	// CWD = deploy root). Returns true + fills delta/side on success.
	static bool readDelta(const char* mission, std::vector<float>& delta, int& side)
	{
		if (!mission || !mission[0]) { strcpy(s_status, "no mission loaded"); return false; }
		const char* fmts[] = {
			"data/missions/%s.beauty/height_delta.r32",
			"%s.beauty/height_delta.r32",
			"tests/terrain/beautify/%s.beauty/height_delta.r32",
		};
		char path[512];
		FILE* fp = nullptr;
		for (int i = 0; i < 3 && !fp; ++i) {
			snprintf(path, sizeof(path), fmts[i], mission);
			fp = fopen(path, "rb");
		}
		if (!fp) {
			snprintf(s_status, sizeof(s_status), "no sidecar for '%s' (looked in data/missions, cwd, tests)", mission);
			return false;
		}
		fseek(fp, 0, SEEK_END);
		long bytes = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		long n = bytes / 4;
		int s = (int)(sqrt((double)n) + 0.5);
		if (s * s != n || n <= 0) {
			fclose(fp);
			snprintf(s_status, sizeof(s_status), "sidecar not square (%ld floats)", n);
			return false;
		}
		delta.resize((size_t)n);
		size_t got = fread(delta.data(), sizeof(float), (size_t)n, fp);
		fclose(fp);
		if (got != (size_t)n) { strcpy(s_status, "sidecar read short"); return false; }
		side = s;
		return true;
	}

	bool Apply()
	{
		if (!land || !Terrain::mapData) { strcpy(s_status, "no terrain loaded"); return false; }
		const char* mission = missionStem();

		std::vector<float> delta;
		int dside = 0;
		if (!readDelta(mission, delta, dside)) return false;   // status set inside
		retainDelta(delta, dside);                             // for the heatmap overlay

		const int side = land->realVerticesMapSide;
		if (dside != side) {
			snprintf(s_status, sizeof(s_status), "grid mismatch: sidecar=%d terrain=%d", dside, side);
			return false;
		}
		const int total = side * side;
		PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
		if (!blocks) { strcpy(s_status, "terrain blocks null"); return false; }

		// Snapshot once (so repeated Apply/Restore stay anchored to stock).
		if (!s_applied) {
			s_origElev.resize((size_t)total);
			for (int i = 0; i < total; ++i) s_origElev[(size_t)i] = blocks[i].elevation;
			s_side = side;
		}

		int changed = 0;
		for (int i = 0; i < total; ++i) {
			if (delta[(size_t)i] != 0.0f) {
				land->setVertexHeight(i, s_origElev[(size_t)i] + delta[(size_t)i]);
				++changed;
			}
		}
		Terrain::mapData->calcLight();          // recompute per-vertex normals
		EditorData::refreshTerrainAfterEdit();  // rebuild face cache + GPU recipes

		s_applied = true;
		s_changed = changed;
		snprintf(s_status, sizeof(s_status), "applied %d cells (%s)", changed, mission);
		return true;
	}

	void Restore()
	{
		if (!s_applied || !land || s_side <= 0) return;
		// EDITOR-CRASH-HARDENING-1: if a different-size map was loaded after Apply,
		// s_side (old grid) no longer matches the live terrain -> setVertexHeight(i,..)
		// for i<s_side*s_side writes OOB into the new (smaller) vertex grid, and a
		// null Terrain::mapData would crash calcLight(). Drop the stale snapshot and
		// bail rather than corrupt memory.
		if (!Terrain::mapData || s_side != land->realVerticesMapSide ||
		    (int)s_origElev.size() != s_side * s_side) {
			s_applied = false;
			s_origElev.clear();
			s_side = 0;
			strcpy(s_status, "restore skipped: terrain changed since apply");
			return;
		}
		const int total = s_side * s_side;
		for (int i = 0; i < total; ++i) land->setVertexHeight(i, s_origElev[(size_t)i]);
		Terrain::mapData->calcLight();
		EditorData::refreshTerrainAfterEdit();
		s_applied = false;
		strcpy(s_status, "restored original terrain");
	}

	bool        IsApplied()    { return s_applied; }
	int         ChangedCount() { return s_changed; }
	const char* Status()       { return s_status; }

	// B7b: load delta for the heatmap WITHOUT mutating terrain.
	bool LoadDeltaForPreview()
	{
		const char* mission = missionStem();
		std::vector<float> delta;
		int dside = 0;
		if (!readDelta(mission, delta, dside)) return false;
		retainDelta(delta, dside);
		int nz = 0;
		for (float v : delta) if (v != 0.0f) ++nz;
		snprintf(s_status, sizeof(s_status), "loaded delta: %d changed cells, max|d|=%.2fwu (%s)",
		         nz, s_deltaMaxAbs, mission);
		return true;
	}

	bool         HasDelta()    { return s_deltaSide > 0 && !s_delta.empty(); }
	int          DeltaSide()   { return s_deltaSide; }
	float        DeltaMaxAbs() { return s_deltaMaxAbs; }
	const float* DeltaData()   { return s_delta.empty() ? nullptr : s_delta.data(); }
	bool         ShowHeatmap() { return s_showHeatmap && HasDelta(); }

	// B7c: load <mission>.beauty/protected.r8 (uint8 side*side). No terrain edit.
	static bool LoadProtected()
	{
		const char* mission = missionStem();
		if (!mission || !mission[0]) return false;
		const char* fmts[] = {
			"data/missions/%s.beauty/protected.r8",
			"%s.beauty/protected.r8",
			"tests/terrain/beautify/%s.beauty/protected.r8",
		};
		char path[512];
		FILE* fp = nullptr;
		for (int i = 0; i < 3 && !fp; ++i) { snprintf(path, sizeof(path), fmts[i], mission); fp = fopen(path, "rb"); }
		if (!fp) return false;
		fseek(fp, 0, SEEK_END); long bytes = ftell(fp); fseek(fp, 0, SEEK_SET);
		int s = (int)(sqrt((double)bytes) + 0.5);
		if (s * s != bytes || bytes <= 0) { fclose(fp); return false; }
		s_protected.resize((size_t)bytes);
		size_t got = fread(s_protected.data(), 1, (size_t)bytes, fp);
		fclose(fp);
		if (got != (size_t)bytes) { s_protected.clear(); return false; }
		s_protSide = s;
		return true;
	}

	bool                 HasProtected()  { return s_protSide > 0 && !s_protected.empty(); }
	int                  ProtectedSide() { return s_protSide; }
	const unsigned char* ProtectedData() { return s_protected.empty() ? nullptr : s_protected.data(); }
	bool                 ShowProtected() { return s_showProtected && HasProtected(); }

	void DrawImGui()
	{
		ImGui::SeparatorText("Beauty Sidecar Preview");
		if (ImGui::Button(s_applied ? "Re-apply Beauty Sidecar" : "Apply Beauty Sidecar", ImVec2(-1.f, 0.f)))
			Apply();
		ImGui::BeginDisabled(!s_applied);
		if (ImGui::Button("Restore Original Terrain", ImVec2(-1.f, 0.f)))
			Restore();
		ImGui::EndDisabled();
		// B7b: heatmap preview — toggle auto-loads the delta (no terrain edit).
		if (ImGui::Checkbox("Show delta heatmap", &s_showHeatmap)) {
			if (s_showHeatmap && !HasDelta())
				LoadDeltaForPreview();   // status reflects success/failure
		}
		if (ImGui::Button("Reload Delta (heatmap)", ImVec2(-1.f, 0.f)))
			LoadDeltaForPreview();
		if (HasDelta())
			ImGui::TextDisabled("raised=red  lowered=blue  max|d|=%.1fwu  grid=%d",
			                    s_deltaMaxAbs, s_deltaSide);
		// B7c: protected-zone overlay — toggle auto-loads protected.r8.
		if (ImGui::Checkbox("Show protected zones", &s_showProtected)) {
			if (s_showProtected && !HasProtected()) LoadProtected();
		}
		if (HasProtected())
			ImGui::TextDisabled("red=structural no-touch (roads/buildings)  blue=water (info)  rest=editable");
		ImGui::TextWrapped("%s", s_status);
	}

	void MaybeAutoApply()
	{
		if (s_autoApplyDone) return;
		if (!mc2::gate::get<"MC2_EDITOR_BEAUTY_AUTOAPPLY">()) { s_autoApplyDone = true; return; }
		if (!land || !Terrain::mapData) return;  // wait until terrain is loaded
		s_autoApplyDone = true;
		bool ok = Apply();
		fprintf(stderr, "[EDITOR_BEAUTY_AUTOAPPLY] %s (%s)\n", ok ? "OK" : "FAIL", s_status);
	}
}
