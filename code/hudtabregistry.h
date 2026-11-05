#ifndef HUDTABREGISTRY_H
#define HUDTABREGISTRY_H
#include "mc2_czstring.h"
/*************************************************************************************************\
hudtabregistry.h : data-driven registry for the in-mission command-bar tab strip
                   (IN-MISSION-HUD-IMGUI-SCOPE-1 slice C, MC2_HUD_IMGUI_TABS).

Replaces the hardcoded "3 consecutive enum ids + hand-written cycle table"
model in ControlGui (controlgui.cpp render dispatch / top-draw loop / update
help-text / isOverTacMap / handleClick / updateVehicleTab / switchTabs).

Contract:
  - Pure logic, no GameOS/GL/ImGui includes -- unit-testable offline
    (tests/unit/test_hud_tab_registry.cpp).
  - Registration ORDER == hotkey CYCLE order. The legacy cycle is
    TACMAP -> VEHICLE -> INFO -> TACMAP (direction > 0), which is NOT the
    Commands enum order; ControlGui registers in cycle order.
  - buttonCommand >= 0  -> tab strip chrome is the legacy ControlButton with
    that ControlGui::Commands id (rendered/pressed by ControlGui as before).
    buttonCommand < 0   -> no legacy button; chrome is caller-drawn (ImGui,
    via the HudImGui helper) -- this is the modder / 4th-tab path.
  - The registry only stores WHICH tab is active. Pressing/unpressing the
    legacy buttons (so every other PRESSED-bit poller in the engine stays
    coherent) is ControlGui's job in setActiveHudTab().
  - Callbacks are plain function pointers + void* user (ControlGui*), so the
    header stays dependency-free.
\*************************************************************************************************/

namespace HudTabs
{
	struct Tab
	{
		const char* id;          // stable string id, e.g. "tacmap" (string literal)
		int   buttonCommand;     // ControlGui::Commands id, or -1 = ImGui-drawn chrome
		void (*render)( void* user );   // draw the tab BODY (tac area content)
		void (*update)( void* user );   // per-frame update while active (may be 0)
		int   helpTextHeaderID;  // IDS_* shown when mouse is inside the tac area
		int   helpTextID;
		bool  countsAsTacMap;    // ControlGui::isOverTacMap() contribution
	};

	class Registry
	{
	public:
		static constexpr int MAX_TABS = 8;

		Registry() : count_( 0 ), active_( 0 ) {}

		// Returns the tab's index, or -1 when full / id null. First tab added
		// becomes active.
		int add( const Tab& t )
		{
			if ( !t.id || count_ >= MAX_TABS )
				return -1;
			tabs_[count_] = t;
			return count_++;
		}

		void clear() { count_ = 0; active_ = 0; }

		int count() const { return count_; }

		const Tab* at( int i ) const
		{
			return ( i >= 0 && i < count_ ) ? &tabs_[i] : nullptr;
		}

		int activeIndex() const { return active_; }

		const Tab* activeTab() const { return at( active_ ); }

		void setActive( int i )
		{
			if ( i >= 0 && i < count_ )
				active_ = i;
		}

		// -1 when no registered tab uses that legacy button id.
		int indexOfButton( int buttonCommand ) const
		{
			if ( buttonCommand < 0 )
				return -1;
			for ( int i = 0; i < count_; ++i )
				if ( tabs_[i].buttonCommand == buttonCommand )
					return i;
			return -1;
		}

		int indexOfId( mc2::czstring id ) const;

		// Next index from active in registration order (direction > 0 forward,
		// otherwise backward), wrapping. Active index unchanged.
		int cycle( int direction ) const
		{
			if ( count_ <= 0 )
				return -1;
			const int step = ( direction > 0 ) ? 1 : -1;
			return ( active_ + step + count_ ) % count_;
		}

	private:
		Tab tabs_[MAX_TABS];
		int count_;
		int active_;
	};
}

#endif // HUDTABREGISTRY_H
