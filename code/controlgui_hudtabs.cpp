// ControlGui: the MC2_HUD_IMGUI_TABS data-driven tab registry and its ImGui chrome.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"
#include "net/runtime.h"
#include "sim_speed.h"
#include "../GameOS/gameos/ui_texture.h"
#include "../mclib/texture_assets.h"
#include <cmath>




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
	if (!disabled && (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)))
	{
		const float speed = simSpeedGetMultiplier();
		const bool down = ImGui::GetIO().KeyShift || ImGui::IsItemClicked(ImGuiMouseButton_Right);
		const float next = mc2::sim::speedCycle(speed, down, disabled);
		simSpeedSetMultiplier(next);
	}
	auto* draw = ImGui::GetWindowDrawList();
	const bool selected = !disabled && (pressed || simSpeedGetMultiplier() != 1.f);
	const int state = disabled ? ControlButton::DISABLED :
		(selected ? ControlButton::PRESSED : (hover ? ControlButton::AMBIGUOUS : ControlButton::ENABLED));
	const auto& art = *button->data;
	// MCUI_High3's unused fourth-row successor is the stock empty command
	// frame, including the orange striped pressed face and grey disabled face.
	const float u = static_cast<float>((disabled ? 2 : selected ? 1 : 0) * 42);
	const auto texture = gos_GetUiTextureId(mc2::textures::sourceId(art.textureHandle));
	draw->AddImage(static_cast<ImTextureID>(texture), p, ImVec2(p.x + size.x, p.y + size.y),
		ImVec2((u + .1f) / art.fileWidth, 144.1f / art.fileHeight),
		ImVec2((u + 42.1f) / art.fileWidth, 180.1f / art.fileHeight));
	// Icon-only command buttons have no Text* fields. Use the stock HUD
	// blue/white/grey/orange palette rather than their unset text colours.
	constexpr unsigned long colors[] = {0xff5c96c2, 0xffffffff, 0xff6b6b6b, 0xfff26f00};
	const auto color = colors[state];
	const ImU32 ink = IM_COL32((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
	const auto font = helpFont.getTempHandle();
	// The legacy atlas uses nearest sampling; integer glyph scales keep the
	// Agency face crisp at both 1080p and 4K, just like the adjoining HUD text.
	const float scale = std::max(1.f, std::floor(size.y / 36.f));
	const float line = HudFont::LineHeight(font, scale);
	const float offset = pressed && !disabled ? scale : 0.f;
	char label[16];
	mc2::fmt::to_buf(label, "{:g}x", static_cast<double>(simSpeedGetMultiplier()));
	for (int row = 0; row < 2; ++row)
	{
		const auto* value = row == 0 ? ">>>" : label;
		const float width = HudFont::Measure(font, value, scale, scale).x;
		HudFont::Draw(draw, font, std::floor(p.x + (size.x - width) * .5f) + offset,
			std::floor(p.y + (size.y - 2.f * line) * .5f + row * line) + offset,
			ink, value, scale, scale);
	}
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
