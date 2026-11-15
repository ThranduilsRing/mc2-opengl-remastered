// ControlGui: the MC2_HUD_IMGUI_TABS data-driven tab registry and its ImGui chrome.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"
#include "net/runtime.h"
#include <array>
#include <cfloat>




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
	t.update = nullptr;   // updateVehicleTab runs unconditionally each frame (legacy shape)
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

void ControlGui::renderSimSpeedButton()
{
	// This square belongs to specialist orders while such a unit is selected.
	for (int id : {LAYMINES, REPAIR, SALVAGE, GUARDTOWER})
		if (getButton(id)->state != ControlButton::HIDDEN) return;
	const auto* button = getButton(LAYMINES);
	float l = button->location[0].x, t = button->location[0].y;
	float r = button->location[2].x, b = button->location[2].y;
	const float cy = (t + b) * 0.5f;
	HudImGui::hudShrinkPoint(l, t, cy);
	HudImGui::hudShrinkPoint(r, b, cy);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
	const bool open = HudImGui::beginPanel("##sim_speed", l, t, r, b);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	if (!open) return;
	const bool disabled = mc2::net::active();
	const ImVec2 p = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	ImGui::SetCursorScreenPos(p);
	ImGui::InvisibleButton("##cycle", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool hover = ImGui::IsItemHovered();
	const bool pressed = ImGui::IsItemActive();
	if (!disabled && (ImGui::IsItemClicked(ImGuiMouseButtonLeft) || ImGui::IsItemClicked(ImGuiMouseButtonRight)))
	{
		const float speed = simSpeedGetMultiplier();
		const bool down = ImGui::GetIO().KeyShift || ImGui::IsItemClicked(ImGuiMouseButtonRight);
		simSpeedSetMultiplier(down ? (speed <= 0.5f ? 4.f : speed * 0.5f) : (speed >= 4.f ? 0.5f : speed * 2.f));
	}
	auto* draw = ImGui::GetWindowDrawList();
	const ImU32 ink = disabled ? IM_COL32(92, 95, 95, 255) :
		(hover || pressed ? IM_COL32(242, 111, 0, 255) : IM_COL32(200, 204, 204, 255));
	const float w = size.x, h = size.y, offset = pressed && !disabled ? w * 0.025f : 0.f;
	if (hover && !disabled)
		draw->AddRect(p, ImVec2(p.x + w, p.y + h), ink, 0.f, 0, w * 0.025f);
	// Vector chevrons stay sharp at the final drawable resolution.
	for (int i = 0; i < 3; ++i)
	{
		const float x = p.x + w * (0.20f + i * 0.21f) + offset;
		draw->AddPolyline(std::array<ImVec2, 3>{ImVec2(x, p.y + h * 0.20f + offset),
			ImVec2(x + w * 0.13f, p.y + h * 0.38f + offset),
			ImVec2(x, p.y + h * 0.56f + offset)}.data(), 3, ink, 0, w * 0.055f);
	}
	char label[16];
	mc2::fmt::to_buf(label, "{:g}x", static_cast<double>(simSpeedGetMultiplier()));
	const float fontSize = h * 0.30f;
	const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0, label);
	draw->AddText(ImGui::GetFont(), fontSize, ImVec2(p.x + (w - text.x) * 0.5f, p.y + h * 0.64f), ink, label);
	if (hover)
		ImGui::SetTooltip("Sim speed: %gx\nClick: faster; right-click / Shift-click: slower.%s",
			simSpeedGetMultiplier(), disabled ? "\nUnavailable in multiplayer." : "");
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
