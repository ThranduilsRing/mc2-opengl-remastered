// Editor: ImGui key mapping and the toolbar, companion, info, place and mission-tools panels.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"



#ifdef MC2_IMGUI
// Minimal Win32 virtual-key -> ImGuiKey map for the keys an ImGui text field needs
// (editing/navigation + the letters/digits used in Ctrl+ shortcuts). Printable text
// itself arrives via WM_CHAR -> AddInputCharacter, not through this map.
static ImGuiKey EditorInterface_VkToImGuiKey(int vk)
{
	switch (vk)
	{
	case VK_TAB:        return ImGuiKey_Tab;
	case VK_LEFT:       return ImGuiKey_LeftArrow;
	case VK_RIGHT:      return ImGuiKey_RightArrow;
	case VK_UP:         return ImGuiKey_UpArrow;
	case VK_DOWN:       return ImGuiKey_DownArrow;
	case VK_PRIOR:      return ImGuiKey_PageUp;
	case VK_NEXT:       return ImGuiKey_PageDown;
	case VK_HOME:       return ImGuiKey_Home;
	case VK_END:        return ImGuiKey_End;
	case VK_INSERT:     return ImGuiKey_Insert;
	case VK_DELETE:     return ImGuiKey_Delete;
	case VK_BACK:       return ImGuiKey_Backspace;
	case VK_SPACE:      return ImGuiKey_Space;
	case VK_RETURN:     return ImGuiKey_Enter;
	case VK_ESCAPE:     return ImGuiKey_Escape;
	case VK_CONTROL:    return ImGuiKey_LeftCtrl;
	case VK_SHIFT:      return ImGuiKey_LeftShift;
	case VK_MENU:       return ImGuiKey_LeftAlt;
	case VK_OEM_MINUS:  return ImGuiKey_Minus;
	case VK_OEM_PERIOD: return ImGuiKey_Period;
	default:
		if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
		if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
		return ImGuiKey_None;
	}
}
#endif

LRESULT EditorInterface::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
	//We're not ready to draw anything.
	// If you let an event through it'll probably crash!
	//
#ifdef MC2_IMGUI
	// Route the keyboard to ImGui ONLY while a text field is active (WantTextInput),
	// and CONSUME the message so editor single-key shortcuts (handleKeyDown) and the
	// GameOS input path do not also fire while the user is typing in a search/text
	// box. When no field is focused, WantTextInput is false and keys flow normally.
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)
	{
		ImGuiIO& io = ImGui::GetIO();
		switch (message)
		{
		case WM_CHAR:
			if (wParam > 0 && wParam < 0x10000)
				io.AddInputCharacterUTF16((unsigned short)wParam);
			return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYUP:
		{
			const bool down = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
			ImGuiKey key = EditorInterface_VkToImGuiKey((int)wParam);
			if (key != ImGuiKey_None)
				io.AddKeyEvent(key, down);
			return 0;
		}
		default:
			break;
		}
	}
#endif

	if ( message == WM_KEYDOWN )
		handleKeyDown( wParam );

	if ( message == WM_CREATE )
	{
		gActive = true;
		gGotFocus= true;
	}

	if ( WM_MOUSEWHEEL == message)
	{
	}
	
	int retVal  = 0;
	
	retVal = CWnd::WindowProc(message, wParam, lParam);

	if ( message == WM_KEYDOWN && wParam == VK_SCROLL )
		return 0;

	if ( (eye || message == WM_MOVE) )
	{
		switch (message)
		{
		case WM_SYSCOLORCHANGE:
		case WM_DISPLAYCHANGE:
		case WM_SETCURSOR:
		case WM_ACTIVATEAPP:
		case WM_KILLFOCUS:
		case WM_SETFOCUS:
		case WM_MOVE:
		case WM_ERASEBKGND:
		case 0x20b:
		case 0x20c:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		case WM_CLOSE:
			retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
			break;

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
			{
				retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
			}
			break;

		case WM_SIZE:
			{
				float w = LOWORD(lParam);
				float h = HIWORD(lParam);
				windowSizeChanged = true;
				g_newWidth = w + 16/*scrollbar thickness*/;
				g_newHeight = h + 16/*scrollbar thickness*/;
				retVal = GameOSWinProc( m_hWnd,message,wParam,lParam );
				if (editor && bThisIsInitialized)
				{
					editor->update();
				}
			}
			break;
		}
	}		
	
	return retVal;
	
}

#ifdef MC2_IMGUI
static bool s_placePanelOpen   = false;
static bool s_missionToolsOpen = false;

// Global ImGui UI scale (drives io.FontGlobalScale so every panel scales at once).
// Initialised once from the editor window's DPI so HiDPI displays are readable by
// default; the user can override it from the Tools palette. Persists for the run.
static float s_uiScale     = 1.0f;
static bool  s_uiScaleInit = false;

// Floating tool palette (Photoshop-style). Buttons re-post the same WM_COMMAND
// the menu items use, so they share the existing tool-switch handlers. The active
// tool is highlighted. Drawn each frame from EditorGameOS.cpp's ImGui block.
void EditorInterface::renderToolbarImGui()
{
	struct ToolDef { const char* label; int cmdId; };
	static const ToolDef tools[] = {
		{ "Select",      ID_OTHER_SELECT },
		{ "Flatten (F)", ID_OTHER_FLATTEN },
		{ "Erase (E)",   ID_OTHER_ERASE },
		{ "Mine",        ID_OTHER_LAYMINES },
		{ "Link (L)",    ID_OTHER_LINK },
		{ "Damage (D)",  ID_OTHER_DAMAGE },
	};

	ImGuiIO& io = ImGui::GetIO();

	// One-time DPI-aware default: scale the whole UI by the editor window's DPI
	// (96 dpi = 1.0x). Keeps panels readable on HiDPI displays out of the box.
	if (!s_uiScaleInit)
	{
		s_uiScaleInit = true;
		UINT dpi = 96;
		if (m_hWnd && ::IsWindow(m_hWnd))
			dpi = ::GetDpiForWindow(m_hWnd);
		float guess = (dpi > 0 ? (float)dpi / 96.0f : 1.0f);
		// Bias up a touch (these panels read small even at 100%) and clamp.
		guess *= 1.25f;
		s_uiScale = (guess < 1.0f) ? 1.0f : (guess > 3.0f ? 3.0f : guess);
	}
	// Apply the master scale before any window is drawn this frame.
	io.FontGlobalScale = s_uiScale;

	// --- Collapsible panels -------------------------------------------------------
	// The full-window scene (RTT off, the exact-picking path) means the docked panel
	// column OVERLAYS the map's right edge. Let the user collapse it to reveal the
	// whole map. Toggle with backtick (`) or the always-visible button (top-right).
	// The status HUD and the toggle button stay visible when collapsed.
	static bool s_panelsVisible = true;
	{
		static bool s_togPrev = false;
		const bool tog = (::GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;   // backtick `
		if (tog && !s_togPrev && !io.WantTextInput)
			s_panelsVisible = !s_panelsVisible;
		s_togPrev = tog;
	}

	// Status HUD (top-left) — always visible, even when panels are collapsed.
	{
		ImGui::SetNextWindowPos( ImVec2( 8.f, 8.f ), ImGuiCond_Always );
		ImGui::SetNextWindowBgAlpha( 0.35f );
		extern const char* EditorStartupWarning();
		extern void EditorClearStartupWarning();
		const bool hudHasWarning = ( EditorStartupWarning() != nullptr );
		ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
		// Accept input only while the dismissable warning banner is showing (its "x"
		// button must be clickable); otherwise the HUD is a passthrough overlay.
		if ( !hudHasWarning )
			hudFlags |= ImGuiWindowFlags_NoInputs;
		if ( ImGui::Begin( "##statushud", nullptr, hudFlags ) )
		{
			const char* mod = ModPicker::ActiveMod();
			ImGui::Text( "Mod: %s", ( mod && mod[0] ) ? mod : "None (stock)" );
			if ( EditorModProject::IsActive() )
				ImGui::Text( "Project: %s", EditorModProject::Id() );
			if ( land && Terrain::realVerticesMapSide > 1 )
				ImGui::Text( "Terrain: %ld x %ld", Terrain::realVerticesMapSide, Terrain::realVerticesMapSide );
			else
				ImGui::TextDisabled( "Terrain: (none)" );
			int selCount = EditorObjectMgr::instance() ? EditorObjectMgr::instance()->getSelectionCount() : 0;
			ImGui::Text( "Selected: %d", selCount );
			ImGui::Text( "Foliage: %d", FoliageRender::Count() );
			ImGui::Text( "%.0f fps", ImGui::GetIO().Framerate );

			// Non-blocking startup warning (e.g. empty-install / no assets). Yellow,
			// wrapped, with a dismiss "x" that clears it for the session. Replaces the
			// old blocking ::MessageBoxA the user had to OK on every launch.
			if ( const char* warn = EditorStartupWarning() )
			{
				ImGui::Separator();
				ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.78f, 0.20f, 1.0f ) );
				if ( ImGui::SmallButton( "x##startupwarn" ) )
					EditorClearStartupWarning();
				ImGui::SameLine();
				ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + 320.f );
				ImGui::TextUnformatted( warn );
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
		}
		ImGui::End();
	}

	// Always-visible collapse toggle (top-right overlay; never docked).
	{
		ImGui::SetNextWindowPos( ImVec2( io.DisplaySize.x - 160.f * s_uiScale, 8.f ), ImGuiCond_Always );
		ImGui::SetNextWindowBgAlpha( 0.35f );
		ImGuiWindowFlags tf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if ( ImGui::Begin( "##panelToggle", nullptr, tf ) )
		{
			if ( ImGui::Button( s_panelsVisible ? "Hide Panels (`)" : "Show Panels (`)" ) )
				s_panelsVisible = !s_panelsVisible;
		}
		ImGui::End();
	}

	if ( !s_panelsVisible )
		return;   // collapsed: skip the Tools window + all docked panels (HUD + toggle stay)

	const float toolbarW = 195.0f * s_uiScale;
	// SetNextWindowPos pulls a window OUT of the DockBuilder assignment (floats it), so
	// skip it under autodock -- the Tools panel then docks into the right column.
	if (!GuiRuntime::AutoDockActive())
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - toolbarW - 16.0f, 16.0f), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(toolbarW, 0.f), ImGuiCond_Once);
	ImGui::Begin("Tools", nullptr, 0);

	// UI scale control — affects every editor panel (FontGlobalScale). +/- and a
	// slider; the value persists for the session.
	ImGui::TextUnformatted("UI Scale");
	if (ImGui::SmallButton("-")) s_uiScale -= 0.1f;
	ImGui::SameLine();
	if (ImGui::SmallButton("+")) s_uiScale += 0.1f;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.f);
	ImGui::SliderFloat("##uiscale", &s_uiScale, 0.8f, 3.0f, "%.1fx");
	if (s_uiScale < 0.8f) s_uiScale = 0.8f;
	if (s_uiScale > 3.0f) s_uiScale = 3.0f;
	ImGui::Separator();

	// Pre-load mod selector: mount a mod's content BEFORE loading a mission (default
	// None = stock). Above the load/generate actions so it's set first.
	ModPicker::Draw();
	ImGui::Separator();

	// Mod Project — bind the session to a mods/<id> folder (Open/New/Close). When active,
	// File save dialogs default to <root>\data\missions and the mod is mounted for assets.
	if (ImGui::Button("Mod Project", ImVec2(-1.f, 0.f)))
		EditorModProject::Toggle();
	EditorModProject::Draw();
	if (EditorModProject::IsActive())
		ImGui::TextDisabled("Project: %s", EditorModProject::Id());
	ImGui::Separator();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
	if (ImGui::Button("Generate Map", ImVec2(-1.f, 0.f)))
		MapGeneratorDialog::Open();
	ImGui::PopStyleColor();

	MapGeneratorDialog::Draw();

	// Foliage Detail — add/tweak trees, rocks, bushes on the generated map.
	// IN PROGRESS: greyed out until real prop-instance foliage rendering lands
	// (the preview only produced flat placeholder cards). The panel itself also
	// shows an "(in progress)" banner + disabled controls if reached.
	ImGui::BeginDisabled(true);
	ImGui::Button("Foliage Detail  (in progress)", ImVec2(-1.f, 0.f));
	ImGui::EndDisabled();
	MapGeneratorDialog::DrawFoliagePanel();

	// Beauty Sidecar Preview (EDITOR-SIDECAR-PREVIEW-1) — apply/restore an offline
	// terrain beautify delta on the live terrain.
	BeautySidecarPreview::MaybeAutoApply();
	BeautySidecarPreview::DrawImGui();

	// Mission Save Checklist — shows why .pak save is ready/blocked + warnings.
	if (ImGui::Button("Mission Checklist", ImVec2(-1.f, 0.f)))
		MissionValidator::Open();
	MissionValidator::Draw();

	EditorTaskRunner::RenderImGui();

	// Debug overlay control panel (chunk/superchunk grid toggles + stats).
	EditorDebugOverlay::RenderImGui();
	// (Status HUD is drawn at the top of this function so it stays visible when the
	// panel column is collapsed.)

	if (ImGui::Button("Scene Outliner", ImVec2(-1.f, 0.f)))
		SceneOutliner::Toggle();
	SceneOutliner::Draw();

	if (ImGui::Button("Inspector", ImVec2(-1.f, 0.f)))
		InspectorPanel::Toggle();
	InspectorPanel::Draw();

	if (ImGui::Button("Objectives", ImVec2(-1.f, 0.f)))
		ObjectivesSummaryPanel::Toggle();
	ObjectivesSummaryPanel::Draw();

	if (ImGui::Button("Campaign", ImVec2(-1.f, 0.f)))
		CampaignSummaryPanel::Toggle();
	CampaignSummaryPanel::Draw();

	if (ImGui::Button("AI / Brain / Orders", ImVec2(-1.f, 0.f)))
		UnitBrainPanel::Toggle();
	UnitBrainPanel::Draw();

	if (ImGui::Button("Telemetry", ImVec2(-1.f, 0.f)))
		TelemetryPanel::Toggle();
	TelemetryPanel::Draw();

	if (ImGui::Button("Asset Browser", ImVec2(-1.f, 0.f)))
		AssetBrowser::Toggle();
	AssetBrowser::Draw();

	// Gameplay Debugger — read-only runtime state for the selection. Editor has
	// no live sim (ObjectManager is null), so it shows static editor data + a
	// "not simulating" notice; wired now for when a sim path exists.
	if (ImGui::Button("Gameplay Debugger", ImVec2(-1.f, 0.f)))
		GameplayDebugger::Toggle();
	GameplayDebugger::Draw();

	// Playtest Results — read-only summary of the most recent playtest's archived
	// stdout (status chip, child exit, SMOKE verdict, PERF p50/p99, mover counts,
	// warning/fatal markers). Pure post-hoc log reader; no engine telemetry.
	if (ImGui::Button("Playtest Results", ImVec2(-1.f, 0.f)))
		EditorPlaytestResults::Toggle();
	EditorPlaytestResults::Draw();

	if (ImGui::Button("Undo History", ImVec2(-1.f, 0.f)))
		UndoHistoryPanel::Toggle();
	UndoHistoryPanel::Draw();

	if (ImGui::Button("Command Palette", ImVec2(-1.f, 0.f)))
		CommandPalette::Toggle();
	CommandPalette::Draw();

	// === Authoring mode selector (contextual tool settings) =================
	// EDITOR-CONTEXTUAL-TOOLS-1: one active mode; only its controls render below,
	// replacing the old flat wall where every tool's settings stacked at once.
	// s_toolMode tracks the active brush (brush-pointer change = re-derive) so a
	// brush switch from the legacy menu or a hotkey moves the selector too; the
	// non-brush modes (Relief/Mission/Place-help) are pure UI and therefore stick.
	ImGui::Separator();
	enum ToolMode { MODE_SELECT = 0, MODE_SCULPT, MODE_STAMP, MODE_PAINT, MODE_RELIEF, MODE_PLACE, MODE_MISSION };
	static int   s_toolMode = MODE_SELECT;
	static void* s_lastBrushForMode = (void*)-1;
	if (curBrush != s_lastBrushForMode)
	{
		s_lastBrushForMode = curBrush;
		if      (dynamic_cast<HeightBrush*>(curBrush))   s_toolMode = MODE_SCULPT;
		else if (dynamic_cast<StampBrush*>(curBrush))    s_toolMode = MODE_STAMP;
		else if (dynamic_cast<TerrainBrush*>(curBrush))  s_toolMode = MODE_PAINT;
		else if (dynamic_cast<BuildingBrush*>(curBrush) ||
		         dynamic_cast<ScatterBrush*>(curBrush))  s_toolMode = MODE_PLACE;
		else                                             s_toolMode = MODE_SELECT;
	}
	{
		struct ModeDef { const char* label; int mode; };
		static const ModeDef modes[] = {
			{ "Select",  MODE_SELECT  }, { "Sculpt",  MODE_SCULPT },
			{ "Stamp",   MODE_STAMP   }, { "Paint",   MODE_PAINT  },
			{ "Relief",  MODE_RELIEF  }, { "Place",   MODE_PLACE  },
			{ "Mission", MODE_MISSION },
		};
		const int nModes = (int)(sizeof(modes) / sizeof(modes[0]));
		const float colW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		for (int i = 0; i < nModes; ++i)
		{
			const bool on = (s_toolMode == modes[i].mode);
			if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
			if (ImGui::Button(modes[i].label, ImVec2(colW, 0.f)))
			{
				s_toolMode = modes[i].mode;
				// Arm the matching brush on entering a brush-backed mode so it is
				// usable immediately (no second click in the sub-controls).
				if      (modes[i].mode == MODE_SCULPT && !dynamic_cast<HeightBrush*>(curBrush))  setSculptBrush(0);
				else if (modes[i].mode == MODE_STAMP  && !dynamic_cast<StampBrush*>(curBrush))   setStampBrush(0);
				else if (modes[i].mode == MODE_PAINT  && !dynamic_cast<TerrainBrush*>(curBrush)) setPaintMaterialBrush(m_paintMaterialType);
			}
			if (on) ImGui::PopStyleColor();
			if ((i % 2) == 0 && i + 1 < nModes) ImGui::SameLine();
		}
	}
	ImGui::Separator();

	// Place Tool / Mission Tools own their own windows and must submit every frame
	// regardless of the active mode, or an already-open window would vanish.
	renderPlacePanelImGui();
	renderMissionToolsImGui();

	if (s_toolMode == MODE_PLACE)
	{
		if (ImGui::Button("Place Tool", ImVec2(-1.f, 0.f)))
			s_placePanelOpen = !s_placePanelOpen;
		ImGui::TextWrapped("Pick an object from the Objects menu to start placing. The "
			"Objects palette (sibling swap / recent) appears on the left while a "
			"placement brush is active.");
	}

	if (s_toolMode == MODE_MISSION)
	{
		if (ImGui::Button("Mission Tools", ImVec2(-1.f, 0.f)))
			s_missionToolsOpen = !s_missionToolsOpen;

		// Playtest in Game: one-click save -> launch mc2.exe -> capture log. Stop
		// while running; status (state / exit code / last line) below the button.
		{
			const bool running = EditorPlaytest::IsRunning();
			if (running)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
				if (ImGui::Button("Stop Playtest", ImVec2(-1.f, 0.f)))
					EditorPlaytest::Stop();
				ImGui::PopStyleColor();
			}
			else
			{
				const bool can = EditorPlaytest::CanPlaytest();
				if (!can) ImGui::BeginDisabled();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
				if (ImGui::Button("Playtest", ImVec2(-1.f, 0.f)))
					EditorPlaytest::Start();
				ImGui::PopStyleColor();
				if (!can) ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !can)
					ImGui::SetTooltip("Load + save a mission first.");
			}
			ImGui::TextWrapped("%s", EditorPlaytest::StatusLine());
		}
	}

	if (s_toolMode == MODE_SELECT)
	for (int i = 0; i < (int)(sizeof(tools) / sizeof(tools[0])); ++i)
	{
		const bool active = (currentBrushMenuID == tools[i].cmdId);
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
		if (ImGui::Button(tools[i].label, ImVec2(-1.f, 0.f)))
			handleNewMenuMessage(tools[i].cmdId);  // direct call (reliable; these are brush switches, no dialogs)
		if (active)
			ImGui::PopStyleColor();
	}

	if (s_toolMode == MODE_SCULPT)
	{
		ImGui::Text("Sculpt Terrain");
		HeightBrush* hb = dynamic_cast<HeightBrush*>(curBrush);
		struct SM { const char* label; int mode; };
		static const SM sm[] = { { "Raise", 0 }, { "Lower", 1 }, { "Smooth", 2 } };
		for (int i = 0; i < 3; ++i)
		{
			const bool sactive = hb && (int)hb->getMode() == sm[i].mode;
			if (sactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.70f, 1.0f));
			if (ImGui::Button(sm[i].label, ImVec2(-1.f, 0.f)))
				setSculptBrush(sm[i].mode);
			if (sactive)
				ImGui::PopStyleColor();
		}
		if (ImGui::SliderFloat("Size", &m_sculptRadius, 64.0f, 3000.0f, "%.0f") && hb)
			hb->setRadius(m_sculptRadius);
		if (ImGui::SliderFloat("Strength", &m_sculptStrength, 1.0f, 300.0f, "%.0f") && hb)
			hb->setStrength(m_sculptStrength);
	}

	if (s_toolMode == MODE_STAMP)
	{
		ImGui::Text("Terrain Stamp");
		StampBrush* st = dynamic_cast<StampBrush*>(curBrush);
		struct SD { const char* label; int type; };
		static const SD sd[] = { { "Pad", 0 }, { "Crater", 1 }, { "Hill", 2 } };
		for (int i = 0; i < 3; ++i)
		{
			const bool sactive = st && (int)st->getType() == sd[i].type;
			if (sactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.70f, 0.85f, 1.0f));
			if (ImGui::Button(sd[i].label, ImVec2(-1.f, 0.f)))
				setStampBrush(sd[i].type);
			if (sactive)
				ImGui::PopStyleColor();
		}
		if (ImGui::SliderFloat("StampSize", &m_stampRadius, 64.0f, 3000.0f, "%.0f") && st)
			st->setRadius(m_stampRadius);
		if (ImGui::SliderFloat("StampAmt", &m_stampStrength, 1.0f, 500.0f, "%.0f") && st)
			st->setStrength(m_stampStrength);
	}

	if (s_toolMode == MODE_PAINT)
	{
		ImGui::Text("Paint Material");
		// TERRAIN-MATERIAL-PAINT-1 (Slice 1): surface the dormant TerrainBrush.
		// Each button installs a TerrainBrush painting one TerrainType (mclib/dmapdata.h)
		// via setPaintMaterialBrush() -> setActiveBrush() (same path as Sculpt/Stamp).
		// The 4 buttons map to the 4 visual PBR buckets the renderer blends
		// (terrain_mat_layers.hglsl): Rock->Mountain(6), Grass(9), Dirt(4), Concrete(10).
		TerrainBrush* tb = dynamic_cast<TerrainBrush*>(curBrush);
		struct PM { const char* label; int type; };
		static const PM pm[] = {
			{ "Rock",     MC_MOUNTAIN_TYPE },
			{ "Grass",    MC_GRASS_TYPE    },
			{ "Dirt",     MC_DIRT_TYPE     },
			{ "Concrete", MC_CONCRETE_TYPE },
		};
		for (int i = 0; i < 4; ++i)
		{
			// Highlight only while a TerrainBrush is the active tool AND it matches the
			// last material we installed (TerrainBrush exposes no type getter).
			const bool pactive = tb && (m_paintMaterialType == pm[i].type);
			if (pactive)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.55f, 0.20f, 1.0f));
			if (ImGui::Button(pm[i].label, ImVec2(-1.f, 0.f)))
				setPaintMaterialBrush(pm[i].type);
			if (pactive)
				ImGui::PopStyleColor();
		}
		if (ImGui::SliderFloat("PaintSize", &m_paintMaterialSize, 64.0f, 3000.0f, "%.0f") && tb)
			tb->setRadius(m_paintMaterialSize);
		// LOD-chunk colormap caveat: only concrete is type-gated in-shader today.
		ImGui::TextWrapped("Concrete paints live; grass/dirt/rock may need a reload to show (LOD-chunk colormap).");
	}

	if (s_toolMode == MODE_RELIEF)
	{
		ImGui::Text("Terrain Relief");
		// Amplify/flatten the whole heightmap about its mean (exaggerate hills +
		// deepen basins, or smooth toward flat). Repeatable per click.
		if (ImGui::Button("Amplify x1.5", ImVec2(-1.f, 0.f)))
			EditorData::amplifyTerrain(1.5f);
		if (ImGui::Button("Flatten x0.67", ImVec2(-1.f, 0.f)))
			EditorData::amplifyTerrain(0.6667f);

		// Live water-height slider (same effect as the Water dialog: sets the
		// water elevation/plane; recalcWater reflows which cells are submerged).
		// Raise it into the terrain range to flood basins with a real water surface.
		if (land && Terrain::mapData)
		{
			m_waterHeight = Terrain::mapData->waterDepth;   // sync display to current
			if (ImGui::SliderFloat("Water Ht", &m_waterHeight, -50.0f, 800.0f, "%.0f"))
			{
				Terrain::waterElevation     = m_waterHeight;
				Terrain::mapData->waterDepth = m_waterHeight;
				land->recalcWater();
			}
			// On release, rebuild the water recipe set so the new level re-evaluates
			// which cells are water against the current (possibly sculpted) terrain
			// -- this is what makes water divide around ridges/pathways.
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				WaterStream::Reset();
				WaterStream::Build();
			}
		}
	}

	ImGui::End();

	renderObjectCompanionPanel();
	renderObjectInfoPanel();
}

// Companion panel for object placement: when a BuildingBrush is active, show the
// current TEAM, the active object's same-group siblings (one-click swap), and an
// MRU "Recent" strip. Lets the user bounce between related objects (wall <-> gate)
// without re-walking the Objects menu. Shares selectBuildingObject() with the menu.
void EditorInterface::renderObjectCompanionPanel()
{
	// Panel is active for either object-placement brush (single place or scatter).
	int group = -1, activeIdx = -1;
	if ( BuildingBrush* bb = dynamic_cast<BuildingBrush*>(curBrush) )
	{
		group = bb->getGroup();
		activeIdx = bb->getIndexInGroup();
	}
	else if ( ScatterBrush* sb = dynamic_cast<ScatterBrush*>(curBrush) )
	{
		group = sb->getGroup();
		activeIdx = sb->getIndexInGroup();
	}
	else
		return;

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	if (group < 0 || group >= pMgr->getBuildingGroupCount())
		return;

	// Same-group object palette. Pop it up on the LEFT edge of the screen so it does
	// not collide with the right-column dock, and scale its geometry by the editor UI
	// scale (s_uiScale). Font size is inherited from io.FontGlobalScale (which is set
	// to s_uiScale) like every other panel -- the old SetWindowFontScale(1.3f) here
	// double-scaled it and made this palette look oversized / "funny" vs the rest of
	// the editor, and ignored DPI. ImGuiCond_Always on the position keeps it reliably
	// on the left each time it pops (it is a transient per-brush context palette).
	const float w = 240.0f * s_uiScale;
	ImGui::SetNextWindowPos(ImVec2(16.0f * s_uiScale, 360.0f * s_uiScale), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(w, 0.f), ImGuiCond_Once);
	ImGui::Begin("Objects", nullptr, ImGuiWindowFlags_NoScrollbar);

	// Clicking a button must not delete curBrush (this bb) or mutate the recent
	// ring mid-render, so defer the actual swap until after all widgets are drawn.
	int pendGroup = -1, pendIndex = -1;

	// TEAM indicator (display only in v1).
	const int align = currentAlignmentFromMenu();
	if (align == EDITOR_TEAMNONE)
		ImGui::Text("TEAM: Neutral");
	else
		ImGui::Text("TEAM: %d", align - EDITOR_TEAM1 + 1);

	// Scatter mode: paint many jittered copies in a radius (generalised forest
	// brush). Toggling re-creates the current brush in the new mode (deferred).
	bool scatterToggled = false;
	if (ImGui::Checkbox("Scatter mode", &m_scatterMode))
		scatterToggled = true;
	if (m_scatterMode)
	{
		ImGui::SliderFloat("Radius",  &ScatterBrush::s_radius,     64.0f, 3000.0f, "%.0f");
		ImGui::SliderInt  ("Density", &ScatterBrush::s_density,    1,     60);
		ImGui::SliderFloat("Spacing", &ScatterBrush::s_minSpacing, 0.0f,  512.0f, "%.0f");
		ImGui::SliderFloat("ScaleJit",&ScatterBrush::s_scaleJitter,0.0f,  0.9f,   "%.2f");
		ImGui::SliderFloat("MaxSlope",&ScatterBrush::s_maxSlopeRise,0.0f, 256.0f, "%.0f");
		ImGui::Checkbox   ("Rand rot", &ScatterBrush::s_randomRotation);
	}
	ImGui::Separator();

	ImGui::Text("%s", pMgr->getGroupName(group));
	const int count = pMgr->getNumberBuildingsInGroup(group);
	if (count > 0)
	{
		const char* names[512] = { 0 };
		int n = 0;
		const int cap = (count <= 512) ? count : 512;
		pMgr->getBuildingNamesInGroup(group, names, n);
		if (n > cap) n = cap;
		for (int i = 0; i < n; ++i)
		{
			const bool active = (i == activeIdx);
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
			ImGui::PushID(i);
			if (ImGui::Button(names[i] ? names[i] : "?", ImVec2(-1.f, 0.f)))
			{
				pendGroup = group;
				pendIndex = i;
			}
			ImGui::PopID();
			if (active)
				ImGui::PopStyleColor();
		}
	}

	// Ecosystem Components section -------------------------------------------
	// When the selected building is part of a functional system (turrets,
	// sensors, spotlights) show all related building types grouped by role so
	// the user can quickly place every component of that system.
	{
		using BT = EditorObjectMgr::BuildingType;

		// Three ecosystems: each entry lists the trigger types, the role
		// names, and which specialType maps to which role index.
		struct EcoRole { const char* label; BT type; };
		struct Ecosystem
		{
			const char* header;
			EcoRole     roles[4];
			int         roleCount;
		};
		static const Ecosystem k_ecosystems[] =
		{
			{ "Turret Components",    { {"Controls",   BT::TURRET_CONTROL},
			                            {"Generators", BT::TURRET_GENERATOR},
			                            {"Turrets",    BT::EDITOR_TURRET} }, 3 },
			{ "Sensor Components",   { {"Controls",   BT::SENSOR_CONTROL},
			                            {"Sensors",    BT::SENSOR_TOWER}  }, 2 },
			{ "Spotlight Components",{ {"Controls",   BT::SPOTLIGHT_CONTROL},
			                            {"Spotlights", BT::SPOTLIGHT}     }, 2 },
		};
		static const int k_numEco = (int)(sizeof(k_ecosystems) / sizeof(k_ecosystems[0]));

		const BT curType = (group >= 0 && activeIdx >= 0)
		    ? pMgr->getBuildingSpecialType(group, activeIdx)
		    : BT::UNSPECIAL;

		const Ecosystem* eco = nullptr;
		for (int e = 0; e < k_numEco && !eco; ++e)
			for (int r = 0; r < k_ecosystems[e].roleCount && !eco; ++r)
				if (k_ecosystems[e].roles[r].type == curType)
					eco = &k_ecosystems[e];

		if (eco)
		{
			ImGui::Separator();
			if (ImGui::CollapsingHeader(eco->header, ImGuiTreeNodeFlags_DefaultOpen))
			{
				const int totalGroups = pMgr->getBuildingGroupCount();
				for (int r = 0; r < eco->roleCount; ++r)
				{
					const BT roleType = eco->roles[r].type;
					bool headerPrinted = false;
					int  ecoID = 2000 + r * 256; // unique widget ID base
					for (int g = 0; g < totalGroups; ++g)
					{
						const int cnt = pMgr->getNumberBuildingsInGroup(g);
						for (int i = 0; i < cnt; ++i)
						{
							if (pMgr->getBuildingSpecialType(g, i) != roleType)
								continue;
							if (!headerPrinted)
							{
								ImGui::TextDisabled("%s", eco->roles[r].label);
								headerPrinted = true;
							}
							const bool isActive = (g == group && i == activeIdx);
							if (isActive)
								ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
							ImGui::PushID(ecoID++);
							if (ImGui::Button(pMgr->getBuildingName(g, i), ImVec2(-1.f, 0.f)))
							{
								pendGroup = g;
								pendIndex = i;
							}
							ImGui::PopID();
							if (isActive)
								ImGui::PopStyleColor();
						}
					}
				}
			}
		}
	}

	// Recent (MRU across groups). Validate each entry against the current palette.
	const ObjectRecentRing& ring = objectRecentRing();
	if (!ring.items().empty())
	{
		ImGui::Separator();
		ImGui::Text("Recent");
		int rid = 0;
		for (const RecentObject& r : ring.items())
		{
			if (r.group < 0 || r.group >= pMgr->getBuildingGroupCount())
				continue;
			const int rc = pMgr->getNumberBuildingsInGroup(r.group);
			if (r.indexInGroup < 0 || r.indexInGroup >= rc)
				continue;

			const char* rnames[512] = { 0 };
			int rn = 0;
			pMgr->getBuildingNamesInGroup(r.group, rnames, rn);
			const char* nm = (r.indexInGroup < rn && rnames[r.indexInGroup]) ? rnames[r.indexInGroup] : "?";

			char label[160];
			sprintf(label, "%s: %s", pMgr->getGroupName(r.group), nm);
			ImGui::PushID(1000 + rid);
			if (ImGui::Button(label, ImVec2(-1.f, 0.f)))
			{
				pendGroup = r.group;
				pendIndex = r.indexInGroup;
			}
			ImGui::PopID();
			++rid;
		}
	}

	// Toggling scatter mode re-creates the current object's brush in the new mode.
	if (scatterToggled && pendGroup < 0)
	{
		pendGroup = group;
		pendIndex = activeIdx;
	}

	ImGui::End();

	// Apply the deferred selection now that no widget code holds the brush / iterates the ring.
	if (pendGroup >= 0)
		selectBuildingObject(pendGroup, pendIndex);
}

// ---------------------------------------------------------------------------
// Object info panel (left-pinned overlay): shows the FIRST selected object's
// identity/position and the common manipulation actions (locate, rotate,
// inspector, delete). Overlay (NOT a dockspace split) on purpose -- a left dock
// would shift the scene viewport origin and break mouse picking; an overlay
// window floats over the viewport without reflowing it. Selection retrieval
// mirrors InspectorPanel's null-safe path (EditorObjectMgr::instance() +
// getSelectedObjectList()); every deref is guarded since the editor god-class
// crashes easily on a half-built selection.
// ---------------------------------------------------------------------------
void EditorInterface::renderObjectInfoPanel()
{
	// Left "current object" DOCK COLUMN. Pinned flush-left, full viewport height so
	// it reads as a docked left column (not a floating popup). It is NOT a real
	// dockspace split: a true left dock would shrink the scene from the left and
	// shift its origin, drifting object picking (gated on the projZ->GL slice, see
	// PROJZ-GL-UNIFICATION-RECON-1). Pinned-overlay instead — it covers only the
	// scene's left strip; picking in the rest of the scene stays correct. Always
	// pos/size + NoSavedSettings so a stale imgui.ini can never float or hide it.
	ImGuiIO& objIo = ImGui::GetIO();
	const float w    = 270.0f * s_uiScale;
	const float topY = 26.0f * s_uiScale;   // clear the main menu bar
	ImGui::SetNextWindowPos(ImVec2(0.0f, topY), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(w, objIo.DisplaySize.y - topY), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.92f);
	ImGui::Begin("Object", nullptr,
		ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing);

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	int sel = mgr ? mgr->getSelectionCount() : 0;
	if (sel == 0)
	{
		ImGui::TextDisabled("No selection");
		ImGui::TextWrapped("Select mode: click an object to inspect/manipulate it.");
		ImGui::End();
		return;
	}

	EditorObject* obj = nullptr;
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST list = mgr->getSelectedObjectList();
		for (EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = list.Begin(); !it.IsDone(); it++)
		{
			if (*it)
			{
				obj = (*it);
				break;
			}
		}
	}

	// getPosition() derefs appearance() unconditionally, so the appearance guard
	// here also makes the position read below safe.
	if (!obj || !obj->appearance())
	{
		ImGui::TextDisabled("Selection not ready");
		ImGui::End();
		return;
	}

	const char* name = obj->getDisplayName();
	ImGui::Text("%s", (name && name[0]) ? name : "(unnamed)");
	ImGui::Text("ID %ld   %s", obj->getID(), InspectorPanel::CategoryToken(obj));
	const Stuff::Vector3D& p = obj->getPosition();
	ImGui::Text("Pos  %.0f, %.0f, %.0f", p.x, p.y, p.z);
	if (sel > 1)
		ImGui::TextDisabled("(+%d more selected)", sel - 1);

	ImGui::Separator();

	if (ImGui::Button("Locate (center camera)", ImVec2(-1.f, 0.f)))
	{
		if (eye)
			eye->setPosition(obj->getPosition(), false);
	}

	if (ImGui::Button("Rotate -45", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4, 0.f)))
		rotateSelectedObjects(-1);
	ImGui::SameLine();
	if (ImGui::Button("Rotate +45", ImVec2(-1.f, 0.f)))
		rotateSelectedObjects(1);

	if (ImGui::Button("Open Inspector (full detail)", ImVec2(-1.f, 0.f)))
		InspectorPanel::Open();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
	if (ImGui::Button("Delete", ImVec2(-1.f, 0.f)))
		mgr->deleteSelectedObjects();
	ImGui::PopStyleColor();

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Place Tool panel (Phase 1d): a single always-available control surface for the
// EXISTING object-placement brushes. Shows the active brush, toggles scatter
// mode, and exposes the scatter parameters (the same ScatterBrush statics the
// companion panel edits). No new placement system -- placement still happens via
// BuildingBrush/ScatterBrush -> Action undo.
// ---------------------------------------------------------------------------
void EditorInterface::renderPlacePanelImGui()
{
	if (!s_placePanelOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Place Tool", &s_placePanelOpen))
	{
		ImGui::End();
		return;
	}

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();

	int group = -1, idx = -1;
	if (BuildingBrush* bb = dynamic_cast<BuildingBrush*>(curBrush))
	{
		group = bb->getGroup();
		idx   = bb->getIndexInGroup();
	}
	else if (ScatterBrush* sb = dynamic_cast<ScatterBrush*>(curBrush))
	{
		group = sb->getGroup();
		idx   = sb->getIndexInGroup();
	}

	if (pMgr && group >= 0 && group < pMgr->getBuildingGroupCount()
	    && idx >= 0 && idx < pMgr->getNumberBuildingsInGroup(group))
	{
		ImGui::Text("Placing: %s / %s",
		            pMgr->getGroupName(group), pMgr->getBuildingName(group, idx));
	}
	else
	{
		ImGui::TextDisabled("No object selected.");
		ImGui::TextDisabled("Pick one in the Asset Browser.");
	}

	const int align = currentAlignmentFromMenu();
	if (align == EDITOR_TEAMNONE)
		ImGui::Text("Team: Neutral");
	else
		ImGui::Text("Team: %d", align - EDITOR_TEAM1 + 1);

	ImGui::Separator();

	// Scatter mode toggle. Re-create the active brush in the new mode so the
	// change takes effect immediately (selectBuildingObject reads m_scatterMode).
	if (ImGui::Checkbox("Scatter mode", &m_scatterMode))
	{
		if (group >= 0 && idx >= 0)
			selectBuildingObject(group, idx);
	}

	if (m_scatterMode)
	{
		ImGui::SliderFloat("Radius",   &ScatterBrush::s_radius,       64.0f, 3000.0f, "%.0f");
		ImGui::SliderInt  ("Density",  &ScatterBrush::s_density,      1,     60);
		ImGui::SliderFloat("Spacing",  &ScatterBrush::s_minSpacing,   0.0f,  512.0f,  "%.0f");
		ImGui::SliderFloat("ScaleJit", &ScatterBrush::s_scaleJitter,  0.0f,  0.9f,    "%.2f");
		ImGui::SliderFloat("MaxSlope", &ScatterBrush::s_maxSlopeRise, 0.0f,  256.0f,  "%.0f");
		ImGui::Checkbox   ("Rand rot", &ScatterBrush::s_randomRotation);
	}

	ImGui::Separator();
	if (ImGui::Button("Clear brush", ImVec2(-1.f, 0.f)))
		KillCurBrush();

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Mission Tools panel (Phase 1d): the smallest useful Test Mission + Build Mod
// Package flows, wrapping existing pieces (EditorTaskRunner for the launch,
// getMapName() for the current .pak, plain file copies for the package). No new
// mission/launch/package SYSTEM -- just buttons over what already exists.
// Editable path fields keep it safe to use on any machine.
// ---------------------------------------------------------------------------
void EditorInterface::renderMissionToolsImGui()
{
	if (!s_missionToolsOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Mission Tools", &s_missionToolsOpen))
	{
		ImGui::End();
		return;
	}

	const char* missionPath = EditorData::instance ? EditorData::instance->getMapName() : 0;
	const bool  haveMission = (missionPath && missionPath[0]);

	ImGui::Text("Current mission: %s", haveMission ? missionPath : "(unsaved -- save first)");
	ImGui::Separator();

	// --- Test Mission: launch the game on the current .pak --------------------
	ImGui::TextUnformatted("Test Mission");
	static char s_gameExe[512] = "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe";
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##gameexe", s_gameExe, sizeof(s_gameExe));

	// GAP 1 -- active-mod propagation. ModPicker::Activate() set MC2_ACTIVE_MOD in
	// THIS (editor) process via _putenv_s. EditorTaskRunner launches the game with
	// CreateProcessA(lpEnvironment=NULL), so the child INHERITS the editor's full
	// environment -- MC2_ACTIVE_MOD included. No explicit env block needed; we just
	// surface the active mod so the modder knows which overrides the playtest uses.
	const char* activeMod = ModPicker::ActiveMod();          // "" == stock
	const bool  haveMod    = (activeMod && activeMod[0]);
	const char* modLabel   = haveMod ? activeMod : "stock (no mod)";
	ImGui::Text("Active mod: %s", modLabel);

	// GAP 2 -- stale-build advisory (governance sec 6). Compare game exe mtime to
	// the editor exe's own mtime. If the game exe is OLDER, the modder likely
	// rebuilt the editor but forgot to rebuild/deploy the game -> stale playtest.
	// Non-blocking yellow warning only.
	auto fileWriteTime = [](const char* path, FILETIME* out) -> bool {
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
			return false;
		*out = fad.ftLastWriteTime;
		return true;
	};
	bool     gameExeStale = false;
	FILETIME gameExeFT{};
	bool     haveGameFT = fileWriteTime(s_gameExe, &gameExeFT);
	{
		char editorExe[MAX_PATH] = {0};
		FILETIME editorFT{};
		if (haveGameFT && GetModuleFileNameA(nullptr, editorExe, MAX_PATH) > 0 &&
		    fileWriteTime(editorExe, &editorFT))
		{
			gameExeStale = (CompareFileTime(&gameExeFT, &editorFT) < 0);
		}
	}
	if (!haveGameFT)
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
		                   "WARNING: game exe not found at the path above.");
	else if (gameExeStale)
		ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
		                   "WARNING: game exe is older than the editor;\n"
		                   "you may be playtesting a stale build.");

	if (!haveMission) ImGui::BeginDisabled();
	char launchLabel[160];
	snprintf(launchLabel, sizeof(launchLabel),
	         "Launch Game on this Mission [mod: %s]", modLabel);
	if (ImGui::Button(launchLabel, ImVec2(-1.f, 0.f)))
	{
		// Game loads a mission directly from a -mission <pak> argument (same flag
		// the editor itself accepts). Launch async via the existing task runner so
		// the editor UI never blocks; progress/exit show in the Task Monitor.
		// Child inherits MC2_ACTIVE_MOD from the editor env (see GAP 1 above).
		EditorTaskRunner::TaskSpec spec;
		spec.name = "Test Mission";
		char cmd[1100];
		snprintf(cmd, sizeof(cmd), "\"%s\" -mission \"%s\"", s_gameExe, missionPath);
		spec.commandLine = cmd;
		// cwd = the game exe's directory (best effort; "" inherits if unknown).
		std::string exeDir(s_gameExe);
		size_t slash = exeDir.find_last_of("/\\");
		spec.workingDirectory = (slash != std::string::npos) ? exeDir.substr(0, slash) : std::string();

		// GAP 3 -- playtest archive (ruling C3). When the launch completes, write a
		// small record to mods/<id-or-"stock">/.modproject/playtest/<ts>/playtest.json.
		// Snapshot every input the callback needs NOW (the lambda runs later, on the
		// main thread, when the game process exits).
		std::string gameExeSnap   = s_gameExe;
		std::string missionSnap    = missionPath;
		std::string modSnap        = haveMod ? activeMod : "stock";
		bool        staleSnap      = gameExeStale;
		// Format the game exe mtime as FILETIME ticks (stable, opaque) for the record.
		unsigned long long gameMtimeTicks = 0;
		if (haveGameFT)
			gameMtimeTicks = ((unsigned long long)gameExeFT.dwHighDateTime << 32) |
			                 (unsigned long long)gameExeFT.dwLowDateTime;

		auto archive = [gameExeSnap, missionSnap, modSnap, staleSnap, gameMtimeTicks]
		               (const EditorTaskRunner::TaskResult& r)
		{
			// Timestamp: tool artifact, not a deterministic capture -> system time OK.
			SYSTEMTIME st; GetLocalTime(&st);
			char ts[32];
			snprintf(ts, sizeof(ts), "%04u%02u%02u-%02u%02u%02u",
			         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

			// mods/<id>/.modproject/playtest/<ts>/  (create each level; ignore EEXIST).
			char dir[1024];
			snprintf(dir, sizeof(dir), "mods");                                      CreateDirectoryA(dir, nullptr);
			snprintf(dir, sizeof(dir), "mods/%s", modSnap.c_str());                  CreateDirectoryA(dir, nullptr);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject", modSnap.c_str());      CreateDirectoryA(dir, nullptr);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject/playtest", modSnap.c_str()); CreateDirectoryA(dir, nullptr);
			snprintf(dir, sizeof(dir), "mods/%s/.modproject/playtest/%s", modSnap.c_str(), ts);
			CreateDirectoryA(dir, nullptr);

			// Persist the captured stdout/stderr tail next to the json.
			char logPath[1100];
			snprintf(logPath, sizeof(logPath), "%s/playtest.log", dir);
			if (FILE* lf = fopen(logPath, "wb"))
			{
				fwrite(r.log.data(), 1, r.log.size(), lf);
				fclose(lf);
			}

			char jsonPath[1100];
			snprintf(jsonPath, sizeof(jsonPath), "%s/playtest.json", dir);
			if (FILE* jf = fopen(jsonPath, "wb"))
			{
				// Minimal hand-written JSON (no json dep in the editor). Paths use
				// forward slashes already; no escaping needed for these fields.
				fprintf(jf,
				    "{\n"
				    "  \"timestamp\": \"%s\",\n"
				    "  \"missionPak\": \"%s\",\n"
				    "  \"gameExe\": \"%s\",\n"
				    "  \"gameExeMtimeTicks\": %llu,\n"
				    "  \"gameExeStale\": %s,\n"
				    "  \"activeMod\": \"%s\",\n"
				    "  \"exitCode\": %d,\n"
				    "  \"logPath\": \"%s\"\n"
				    "}\n",
				    ts, missionSnap.c_str(), gameExeSnap.c_str(),
				    gameMtimeTicks, staleSnap ? "true" : "false",
				    modSnap.c_str(), r.exitCode, logPath);
				fclose(jf);
			}
		};
		spec.onSuccessMainThread = archive;
		spec.onFailureMainThread = archive;   // archive crashes/nonzero exits too.

		EditorTaskRunner::StartTask(spec);
	}
	if (!haveMission) ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !haveMission)
		ImGui::SetTooltip("Save the mission to a .pak first (File > Save As).");

	ImGui::Separator();

	ImGui::TextUnformatted("Build Mod Package");
	static char s_modName[128]  = "my_mission";
	static char s_modsRoot[512] = "mods";
	static char s_pkgStatus[256] = "";
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("Mod name##modname", s_modName, sizeof(s_modName));
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("Mods root##modsroot", s_modsRoot, sizeof(s_modsRoot));

	if (!haveMission) ImGui::BeginDisabled();
	if (ImGui::Button("Build Package", ImVec2(-1.f, 0.f)))
	{
		// Derive <dir>/<base>.pak, then copy <base>.{pak,fit,burnin.jpg,foliage.json}
		// that exist into <modsRoot>/<modName>/missions/. Additive + reversible
		// (delete the folder to undo). Never deletes or overwrites outside dest.
		std::string src(missionPath);
		size_t slash = src.find_last_of("/\\");
		std::string srcDir  = (slash != std::string::npos) ? src.substr(0, slash) : std::string(".");
		std::string fileNm  = (slash != std::string::npos) ? src.substr(slash + 1) : src;
		size_t dot = fileNm.find_last_of('.');
		std::string base = (dot != std::string::npos) ? fileNm.substr(0, dot) : fileNm;

		char destDir[900];
		snprintf(destDir, sizeof(destDir), "%s/%s/missions", s_modsRoot, s_modName);

		char lvl[900];
		snprintf(lvl, sizeof(lvl), "%s", s_modsRoot);                         CreateDirectoryA(lvl, nullptr);
		snprintf(lvl, sizeof(lvl), "%s/%s", s_modsRoot, s_modName);           CreateDirectoryA(lvl, nullptr);
		CreateDirectoryA(destDir, nullptr);

		const char* exts[] = { ".pak", ".fit", ".burnin.jpg", ".foliage.json" };
		int copied = 0;
		for (int i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); ++i)
		{
			char srcF[900], dstF[1000];
			snprintf(srcF, sizeof(srcF), "%s/%s%s", srcDir.c_str(), base.c_str(), exts[i]);
			snprintf(dstF, sizeof(dstF), "%s/%s%s", destDir, base.c_str(), exts[i]);
			// CopyFileA(bFailIfExists=FALSE) overwrites only inside our dest dir.
			if (CopyFileA(srcF, dstF, FALSE))
				++copied;
		}
		snprintf(s_pkgStatus, sizeof(s_pkgStatus),
		         "Copied %d file(s) to %s", copied, destDir);
	}
	if (!haveMission) ImGui::EndDisabled();

	if (s_pkgStatus[0])
		ImGui::TextWrapped("%s", s_pkgStatus);

	ImGui::End();
}

#else
void EditorInterface::renderToolbarImGui() {}
void EditorInterface::renderObjectCompanionPanel() {}
void EditorInterface::renderPlacePanelImGui() {}
void EditorInterface::renderMissionToolsImGui() {}
#endif

void EditorInterface::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) 
{
	
	if (pScrollBar != nullptr && pScrollBar->SendChildNotifyLastMsg())
		return;     // eat it

	// ignore scroll bar msgs from other controls
	if (pScrollBar != GetScrollBarCtrl(SB_HORZ))
		return;


	SCROLLINFO sInfo;
	sInfo.cbSize = sizeof ( SCROLLINFO ); 
	sInfo.fMask = SIF_POS | SIF_PAGE;
	sInfo.nMin = 0; 
	sInfo.nMax = 0; 
	sInfo.nPage = 0; 
	sInfo.nPos = 0; 
	sInfo.nTrackPos = 0; 
	
	CPoint pt;

	GetScrollInfo( SB_HORZ, &sInfo );
	pt.x = sInfo.nPos;

	switch ( nSBCode )
	{
		
		case SB_LINELEFT:
			pt.x +=  (-0.01 * HSCROLLBAR_RANGE);
			pt.y = 0;
			break;
		case SB_LINERIGHT:
			pt.x += (0.01 * HSCROLLBAR_RANGE);
			pt.y = 0;
			break;	
		case SB_PAGELEFT:
			pt.x -= sInfo.nPage;
			break;
		case SB_PAGERIGHT:
			pt.x += sInfo.nPage;
			break;
		case SB_THUMBPOSITION:
			pt.x = nPos;
			break;
		case SB_THUMBTRACK:
			pt.x = nPos;
			break;


		default:
			break;
	}

	
	/* this calculation was based in the code for Camera::moveRight(float amount) */
	Stuff::Vector3D direction;
	if (!eye->usePerspective)
	{
		direction.x = 1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = -1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	float worldCameraRotation = eye->getCameraRotation();
	OppRotate(direction,worldCameraRotation);

	Stuff::Vector3D eyeDisplacement = eye->getPosition();

	/* maxVisual was taken from Camera::setPosition(). */
	float maxVisual = (Terrain::worldUnitsMapSide / 2) - Terrain::worldUnitsPerVertex;
	float bound = SQRT_2 * maxVisual;
	float amount = bound * 2.0f * (((float)pt.x) / HSCROLLBAR_RANGE - 0.5f);

	float amountMore = amount - direction * eyeDisplacement;
	
	eye->moveRight(amountMore);

	sInfo.nPos = pt.x;
	sInfo.fMask = SIF_POS; 
	
	this->SetScrollPos( SB_HORZ, pt.x );
	
	CWnd ::OnHScroll(nSBCode, nPos, pScrollBar);

	SafeRunGameOSLogic();
	tacMap.Invalidate(FALSE);  // async/coalesced — avoid synchronous per-event minimap repaint storm
	syncScrollBars();
}
