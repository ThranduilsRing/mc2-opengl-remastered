// ControlGui: the MC2_HUD_IMGUI_TABS data-driven tab registry and its ImGui chrome.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"




bool ControlGui::hudTabsGateOn() const
{
	return mc2::gate::get<"MC2_HUD_IMGUI_TABS">() && !HudImGui::pinnedToLegacy();
}

// Registry thunks. Registration order == hotkey cycle order, which the legacy
// switchTabs table defines as TACMAP -> VEHICLE -> INFO (direction > 0) — NOT
// the Commands enum order.
void ControlGui::hudTabRenderTacMap( void* user )	{ (static_cast<ControlGui*>(user))->tacMap.render(); }
void ControlGui::hudTabRenderInfo( void* user )		{ (static_cast<ControlGui*>(user))->renderInfoTab(); }
void ControlGui::hudTabRenderVehicle( void* user )	{ (static_cast<ControlGui*>(user))->renderVehicleTab(); }
void ControlGui::hudTabUpdateTacMap( void* user )	{ (static_cast<ControlGui*>(user))->tacMap.update(); }
void ControlGui::hudTabUpdateInfo( void* user )
{
	ControlGui* self = static_cast<ControlGui*>(user);
	if ( self->infoWnd )
		self->infoWnd->update();
}

void ControlGui::initHudTabRegistry()
{
	hudTabRegistry.clear();

	HudTabs::Tab t;

	t.id = "tacmap";
	t.buttonCommand = TACMAP_TAB;
	t.render = &ControlGui::hudTabRenderTacMap;
	t.update = &ControlGui::hudTabUpdateTacMap;
	t.helpTextHeaderID = IDS_TACMAP;
	t.helpTextID = IDS_TACMAP_DESC;
	t.countsAsTacMap = true;
	hudTabRegistry.add( t );

	t.id = "vehicle";
	t.buttonCommand = VEHICLE_TAB;
	t.render = &ControlGui::hudTabRenderVehicle;
	t.update = 0;   // updateVehicleTab runs unconditionally each frame (legacy shape)
	t.helpTextHeaderID = IDS_VEHICLE_TAB;
	t.helpTextID = IDS_VEHICLE_TAB_DESC;
	t.countsAsTacMap = false;
	hudTabRegistry.add( t );

	t.id = "info";
	t.buttonCommand = INFO_TAB;
	t.render = &ControlGui::hudTabRenderInfo;
	t.update = &ControlGui::hudTabUpdateInfo;
	t.helpTextHeaderID = IDS_INFOTAB;
	t.helpTextID = IDS_INFOTAB_DESC;
	t.countsAsTacMap = false;
	hudTabRegistry.add( t );

	// SIM-SPEED-BUTTON-1: double-gated sim-speed tab (MC2_HUD_SIM_SPEED;
	// needs MC2_HUD_IMGUI_TABS like every registry consumer). 1x/2x/4x
	// buttons drive the runtime multiplier behind MC2_SIM_SPEED
	// (simSpeedSetMultiplier, mission.cpp) — the same mechanism, no second
	// sim-speed path. Disabled in multiplayer (would desync the sim).
	if ( mc2::gate::get<"MC2_HUD_SIM_SPEED">() )
	{
		t.id = "speed";
		t.buttonCommand = -1;   // no legacy ControlButton; chrome is ImGui-drawn
		t.render = &ControlGui::hudTabRenderSimSpeed;
		t.update = 0;
		t.helpTextHeaderID = 0;
		t.helpTextID = 0;
		t.countsAsTacMap = false;
		hudTabRegistry.add( t );
	}
}

void ControlGui::setActiveHudTab( int index, const char* via )
{
	const HudTabs::Tab* t = hudTabRegistry.at( index );
	if ( !t )
		return;
	// keep every legacy PRESSED-bit poller coherent: mutual-exclusion press,
	// same effect as the old TACMAP_TAB..+3 loop for the stock 3
	for ( int i = 0; i < hudTabRegistry.count(); ++i )
	{
		const HudTabs::Tab* o = hudTabRegistry.at( i );
		if ( o->buttonCommand >= 0 )
			getButton( o->buttonCommand )->press( false );
	}
	if ( t->buttonCommand >= 0 )
		getButton( t->buttonCommand )->press( true );
	hudTabRegistry.setActive( index );
	mc2::fmt::print_err("[HUD-TABS v1] dispatch tab '{}' (index {:d}, via {})\n", mc2::fmt::safe(t->id), mc2::fmt::as_i32(index), mc2::fmt::safe(via));
}

// SIM-SPEED-BUTTON-1 tab body: 1x/2x/4x sim-speed buttons over the tac area
// rect (same panel placement as the test tab: bottom-band shrink + canvas
// transform via the slice-B helper). Clicks route through
// simSpeedSetMultiplier (mission.cpp) — the shipped MC2_SIM_SPEED mechanism.
// Guards: buttons disabled in multiplayer (sim-speed change would desync);
// the multiplier itself resets to the env seed at every Mission::init, and
// pause already freezes scenarioTime regardless of the multiplier.
void ControlGui::hudTabRenderSimSpeed( void* user )
{
	ControlGui* self = static_cast<ControlGui*>(user);
	if ( !self->rectInfos )
		return;
	const GUI_RECT& r = self->rectInfos[0].rect;
	float l = static_cast<float>(r.left),  tY = static_cast<float>(r.top);
	float rt = static_cast<float>(r.right), b = static_cast<float>(r.bottom);
	const float cy = 0.5f * ( tY + b );
	HudImGui::hudShrinkPoint( l, tY, cy );
	HudImGui::hudShrinkPoint( rt, b, cy );

	float fontScale = 1.f;
	if ( !HudImGui::beginPanel( "##hud_tab_simspeed", l, tY, rt, b, &fontScale ) )
		return;
	ImGui::SetWindowFontScale( fontScale );

	const float current = simSpeedGetMultiplier();
	ImGui::Text( "Sim speed: %.2fx", current );

	if ( MPlayer )
	{
		ImGui::TextUnformatted( "(unavailable in multiplayer)" );
		HudImGui::endPanel();
		return;
	}

	static const float kSpeeds[3] = { 1.0f, 2.0f, 4.0f };
	static const char* kLabels[3] = { "1x##simspeed", "2x##simspeed", "4x##simspeed" };
	for ( int i = 0; i < 3; ++i )
	{
		if ( i )
			ImGui::SameLine();
		const bool active = ( current > kSpeeds[i] - 0.01f && current < kSpeeds[i] + 0.01f );
		if ( active )
			ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.1f, 0.45f, 0.1f, 1.f ) );
		if ( ImGui::Button( kLabels[i], ImVec2( 48.f, 0.f ) ) )
		{
			mc2::fmt::print_err("[SIM-SPEED-BUTTON v1] click {:.0f}x (was {:.2f}x)\n", static_cast<double>(kSpeeds[i]), static_cast<double>(current));
			fflush( stderr );
			simSpeedSetMultiplier( kSpeeds[i] );
		}
		if ( active )
			ImGui::PopStyleColor();
	}

	ImGui::TextUnformatted( "Resets at mission start." );

	HudImGui::endPanel();
}

// Tab-strip chrome for registry tabs with no legacy button (buttonCommand<0):
// a small ImGui button placed just right of the last legacy tab button, in
// authored HUD space, through shrink + canvas transform.
void ControlGui::renderHudTabChromeImGui()
{
	int lastLegacy = -1;
	for ( int i = 0; i < hudTabRegistry.count(); ++i )
		if ( hudTabRegistry.at( i )->buttonCommand >= 0 )
			lastLegacy = hudTabRegistry.at( i )->buttonCommand;

	int slot = 0;
	for ( int i = 0; i < hudTabRegistry.count(); ++i )
	{
		const HudTabs::Tab* t = hudTabRegistry.at( i );
		if ( t->buttonCommand >= 0 )
			continue;

		// anchor off the INFO/VEHICLE-style legacy tab button quad:
		// x span location[0]->[3], y span location[0]->[1]
		float aL, aT, aR, aB;
		if ( lastLegacy >= 0 )
		{
			ControlButton* btn = getButton( lastLegacy );
			aL = btn->location[0].x; aT = btn->location[0].y;
			aR = btn->location[3].x; aB = btn->location[1].y;
		}
		else
		{
			aL = 0.f; aT = 0.f; aR = 64.f; aB = 24.f;
		}
		const float w = aR - aL;
		float l = aR + static_cast<float>(slot) * w, tY = aT, r = aR + static_cast<float>(( slot + 1 )) * w, b = aB;
		const float cy = 0.5f * ( tY + b );
		HudImGui::hudShrinkPoint( l, tY, cy );
		HudImGui::hudShrinkPoint( r, b, cy );

		char wid[32];
		mc2::fmt::to_buf(wid, "##hud_tab_chrome_{:d}", mc2::fmt::as_i32(i));
		if ( !HudImGui::beginPanel( wid, l, tY, r, b ) )
			return;
		const bool active = ( hudTabRegistry.activeIndex() == i );
		if ( active )
			ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.1f, 0.45f, 0.1f, 1.f ) );
		if ( ImGui::Button( t->id, ImGui::GetContentRegionAvail() ) )
			setActiveHudTab( i, "chrome-click" );
		if ( active )
			ImGui::PopStyleColor();
		HudImGui::endPanel();
		++slot;
	}
}
