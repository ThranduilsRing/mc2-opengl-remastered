// G7b K3: the render extract hook and the GameObject defaults (storage: render_view_store.cpp).
#include "render_view.h"
#include "gameobj.h"
#include "objmgr.h"
#include "missiongui.h"
#include "mission.h"
#include "impact_presentation.h"
#include "../mclib/camera.h"
#include "../mclib/timing.h"
#include <tracy/Tracy.hpp>

#include <atomic>
extern std::atomic<uint32_t> g_mc2FrameCounter;
extern unsigned long scenarioResult;

// The overlap extract hook (installed by code/mission_update.cpp), main thread at ExtractRenderSnapshot with the sim
// quiescent: stamp the clock, then capture everything the object draws and the impact decals read from the sim.
void extractRenderFrame(float tickAlpha)
{
	ZoneScopedN("ExtractRenderList");
	auto& clk = mc2::render_view::clock();
	clk.turn = static_cast<int32_t>(turn);
	clk.scenarioTime = scenarioTime;
	clk.dt = frameLength;
	clk.frame = g_mc2FrameCounter;
	clk.missionResult = static_cast<uint32_t>(scenarioResult);
	clk.missionActive = mission && mission->isActive();
	clk.alpha = tickAlpha;
	auto& lists = mc2::render_view::list();
	lists.clear();
	if (eye && eye->active && turn > 1) {
		if (ObjectManager) ObjectManager->extractRenderList(lists);
		impact_presentation::extract();
	}
	if (auto* ui = MissionInterfaceManager::instance()) ui->extractHudSnapshot();
	TracyPlot("RenderList.terrain", int64_t(lists.terrain.size()));
	TracyPlot("RenderList.movers", int64_t(lists.movers.size()));
	TracyPlot("RenderList.other", int64_t(lists.other.size()));
	const auto& hud = mc2::render_view::hud();
	TracyPlot("HudSnapshot.movers", int64_t(hud.movers.size()));
	TracyPlot("HudSnapshot.roster", int64_t(hud.roster.size()));
	TracyPlot("HudSnapshot.sensors", int64_t(hud.sensors.size()));
	TracyPlot("HudSnapshot.coverage", int64_t(hud.coverage.size()));
	TracyPlot("HudSnapshot.objectives", int64_t(hud.objectives.size()));
	TracyPlot("HudSnapshot.blips", int64_t(hud.blips.size()));
	TracyPlot("HudSnapshot.vtols", int64_t(hud.vtols.size()));
}

void GameObject::extractRenderView(mc2::render_view::RenderView& view)
{
	view.position = position;
	view.watchId = getWatchID(false);
	view.objClass = static_cast<uint8_t>(getObjectClass());
}

void GameObject::renderView(const mc2::render_view::RenderView&)
{
	render();
}
