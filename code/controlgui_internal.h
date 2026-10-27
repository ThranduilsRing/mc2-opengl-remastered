#include "Gates/preferences_legacy.h"
// Shared surface of the controlgui.cpp translation units (controlgui.cpp, controlgui_{hudtabs,render,objectives,update,vehicle,init,chat}.cpp).
//
// controlgui.cpp keeps the construction, the ControlButton methods and the small accessors; the rest of ControlGui is split by
// responsibility (see DOC in each piece).
#ifndef CONTROLGUI_INTERNAL_H
#define CONTROLGUI_INTERNAL_H

#include <cstdlib>
#include "Gates/gate.h"
#include"controlgui.h"
#include"unitprofile.h"   // EQUIP-ADD-UI-NAME-1 (S3): equipmentAddUiTextForStringId
#include"tacticaloverview.h"  // Tactical Overview: strategic-icon overlay
#include"contact.h"           // Tactical Overview: friendly sensor coverage tint
#include"objmgr.h"            // Tactical Overview: enemy/contact marker scan
#include"mover.h"
// Squad-level overview move arrow (defined in warrior.cpp).
void drawOverviewArrowWorld( const glm::vec3& from, const glm::vec3& to, unsigned long color );
extern HGOSFONT3D gosFontHandle;   // global font for sensor-view range labels
extern float worldUnitsPerMeter;   // sensor range is in world units; /this = meters
#include"team.h"
#include"gamesound.h"
#include"sounds.h"        // BUTTON5 (formation line release sfx)
#include"comndr.h"

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif


#ifndef GAMETACMAP_H
#include"gametacmap.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef PAUSEWINDOW_H
#include"pausewindow.h"
#endif

#include"objective.h"
#include"mission.h"
#include"infowindow.h"

#include"render_submissions.h"

#include "../resource.h"
#include"malloc.h"

#include"chatwindow.h"
#include"../GameOS/gameos/gos_profiler.h"
#include"gameos.hpp"        // OBJ-IMGUI-SLICE-1: gos_ComputeHudCanvasBox + gos_GetGLTextureName
#include"imgui.h"           // OBJ-IMGUI-SLICE-1: ImGui objectives overlay
#include"hud_imgui_font.h"  // FONT-IMGUI-SLICE-1: legacy bitmap-font text
#include<cstdint>           // OBJ-IMGUI-SLICE-1: intptr_t (ImTextureID cast)
#include <chrono>
#include"hudimguipanel.h"
#include "imgui.h"

#define TEXT_SKIP 5.f * Environment.screenHeight/600.f
inline constexpr int CHAT_DISPLAY_TIME = 30;

// Globals defined in controlgui.cpp (or elsewhere in the engine) that the split pieces share.
extern unsigned long long g_ifaceCgFrameNs[8];
extern bool g_imguiInitialized;
extern long helpTextHeaderID;
extern long helpTextID;
// Hovered world object's name string id (0 = none) and cursor pos, set by updateTarget.
extern long g_hoverTipNameID;
extern long g_hoverTipX;
extern long g_hoverTipY;
extern bool neverEndingStory;
extern bool drawGUIOn;
extern bool paintingMyVtol;
extern int __cdecl sortStats( const void* pPlayer1, const void* pPlayer2 );

#endif // CONTROLGUI_INTERNAL_H
