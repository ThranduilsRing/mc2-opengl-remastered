#include "Gates/preferences_legacy.h"
// Shared surface of the missiongui.cpp translation units (missiongui.cpp, missiongui_{diag,update,style,orders,cursor,support,debugkeys,input}.cpp).
//
// missiongui.cpp keeps the manager lifecycle, the hotkey table and render; the rest of MissionInterfaceManager is split by
// responsibility (see DOC in each piece). The UI-command channel (missiongui_uicmd*.cpp) shares missiongui_internal.h, which this
// header includes first.
#ifndef MISSIONGUI_SHARED_INTERNAL_H
#define MISSIONGUI_SHARED_INTERNAL_H

// Include Files
#include "missiongui_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>(), raw<>()
#include "../RenderCore/axis_convert.h"

// MC2_MIF_SPLIT / MC2_IFACE_COST_SPLIT phase timing and the GPU-ground-pick parity oracle: used by update() and the
// order-style paths, defined in missiongui_diag.cpp.
namespace missiongui_detail {
	enum MifPhase { MF_TOTAL=0, MF_INVPROJ, MF_LOS, MF_CTRLGUI, MF_UPDTGT, MF_POSTTGT, MF_DRAWBARS, MF_ROLLOVERS,
		// ControlGui::update sub-phases, folded from g_ifaceCgFrameNs[] --
		// order matches CgPhase in controlgui.cpp.
		MF_CG_PAUSE, MF_CG_BTNHOVER, MF_CG_ROSTER, MF_CG_MOVERSTATE, MF_CG_TACMAP, MF_CG_INFOWND, MF_CG_VEHTAB, MF_CG_FGBAR,
		MF_COUNT };
	const int MF_CG_FIRST = MF_CG_PAUSE;
	extern const bool s_mfOn;
	extern unsigned long long s_mfFrameNs[MF_COUNT];
	extern unsigned long long s_mfInvWalks, s_mfInvHits, s_mfWinWalks, s_mfWinHits;
	extern const bool s_gpParityOn;
	extern const bool s_gpCenter;
	struct MifScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit MifScope(int i):idx(i){ if(s_mfOn) t0=std::chrono::steady_clock::now(); }
		~MifScope(){ if(!s_mfOn) return; s_mfFrameNs[idx]+=(unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(); }
	};
	void mfEmitWindow(std::string_view event);
	void mfFrameEnd(unsigned long long totalNs);
	void gpCenterProbe(int cxPix, int cyPix, float ccx, float ccy, float ccz);
	void gpParitySite(int mouseX, int mouseY, unsigned long long cpuNs,
	                  float cx, float cy, float cz, std::string_view site);
}
using namespace missiongui_detail;

// Globals defined in missiongui.cpp (or elsewhere in the engine) that the split pieces share.
extern void testKeyStuff();
extern long helpTextHeaderID;
extern long helpTextID;
extern CPrefs prefs;
extern bool invulnerableON;
extern bool MLRVertexLimitReached;
extern float frameLength;
extern void DEBUGWINS_print (const char* s, long window = 0);
extern void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern void DEBUGWINS_toggle (bool* windowsOpen);
extern void DEBUGWINS_display (bool* windowsOpen);
extern void DEBUGWINS_viewGameObject (long debugObj);
extern MidLevelRenderer::MLRClipper * theClipper;
extern GameObjectPtr DebugGameObject[3];
extern int s_hoverLastMouseX;
extern int s_hoverLastMouseY;
extern GameObject* s_hoverCachedTarget;
extern unsigned long s_hoverCachedTargetWID;
extern bool s_hoverCacheValid;
extern unsigned s_hoverMissionSerial;
extern bool s_hoverSuppressed;
extern long s_hoverSettleFrames;
extern unsigned long s_targetWatchID;
extern unsigned long s_oldTargetWatchID[MAX_ICONS];
extern bool drawTerrainTiles;
extern bool drawTerrainOverlays;
extern bool renderObjects;
extern bool drawTerrainGrid;
extern bool drawLOSGrid;
extern bool useFog;
extern bool renderTGLShapes;
extern bool drawGUIOn;
extern bool toggledGUI;
extern long lightState;
extern bool ShowMovers;
extern bool CullPathAreas;
extern bool ZeroHPrime;
extern bool CalcValidAreaTable;
extern bool EnemiesGoalPlan;
extern bool paintingMyVtol;
extern long 	turn;
extern char DebugStatusBarString[256];

extern const char* terrainStr[NUM_TERRAIN_TYPES];   // defined in missiongui.cpp
#ifndef FINAL
extern float CheatHitDamage;
#endif

//--------------------------------------------------------------------------------------
//class MissionInterfaceManager : public InterfaceManager
//--------------------------------------------------------------------------------------

inline constexpr auto CTRL = 0x10000000;
inline constexpr auto SHIFT = 0x01000000;
inline constexpr auto ALT = 0x00100000;
inline constexpr auto WAYPT = 0x20000000;

#endif // MISSIONGUI_SHARED_INTERNAL_H
