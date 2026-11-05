// MissionInterfaceManager: UI-command channel: file polling, verb dispatch and the ack epilogue.
// Declared in missiongui.h; shared names in missiongui_internal.h.
#include <atomic>
#include "missiongui_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"


//--------------------------------------------------------------------------
// UI-COMMAND-CHANNEL-1: MC2_UI_COMMAND_FILE=<path> (default unset -> this
// function is one cached-getenv check per frame, zero behavior change).
// When set, the file is size-probed once per frame (_stat64, cheap); when
// non-empty, all its lines are consumed (read-then-truncate) and executed,
// one command per line:
//   toggle_pause          - MissionInterfaceManager::togglePause() (same as
//                           the MC2_BOOT_PAUSED fixture / ESC key)
//   press pause.<BTN>     - PauseWindow::handleClick(BTN), BTN one of
//                           OBJECTIVES|SAVE|LOAD|OPTIONS|RESTART|EXIT|RETURN
//                           (same path the ImGui InvisibleButtons take)
//   press dialog.YES|NO   - pause abort/exit confirm dialog, same
//                           LogisticsOKDialog::handleMessage the ImGui
//                           renderImGuiConfirm() buttons send
//   debugview <n>         - DEBUG-VIEW-SWEEP-1: set the global full-scene
//                           debug render view (0=off 1=shadow 2=depth
//                           3=albedo 4=overdraw 5=normals 6=chunk). Fans out
//                           to every lane's debug channel via
//                           gos_SetGlobalDebugView(); runtime, no restart.
//   screenshot <name>       - TGA of the presented frame via the DEV-SHELL
//                           capture point (gos_dev_shell::scheduleScreenshot,
//                           writes dev_shell_out/<name>.tga). Requires
//                           MC2_DEV_SHELL=1 (the capture hook is gated on the
//                           shell); acked blocked otherwise.
// GAMEPLAY-DRIVER-1 world verbs (route through the mouse/keyboard dispatchers):
//   select <partId|all|lance|class <NAME>|name <substr>>
//   order move <x> <y> [run|walk]  - MOVETO order via handleOrders (doMove path)
//   order attack <targetPartId>    - ATTACK_OBJECT via handleTacticalOrder
//   order stop|hold_fire|free_fire - the corresponding TACTICAL_ORDER_* code
//   camera <x> <y> [rot] [zoomAlt] - eye->setPosition (swoopy goal-seek) + rot/zoom
//   camera_follow <partId|off>     - Camera::setCameraTargetId (frame one unit)
//   time_scale <f>                 - simSpeedSetMultiplier (scenarioTime AND
//                                    frameLength in lockstep), clamp 0.25..4
//   status                         - dump SELECTED units id/name/pos/hp to .ack
// GAMEPLAY-DRIVER-2 readback verbs (write multi-line payloads to .ack):
//   list [all|player|enemy|team <N>|class <NAME>]
//                                  - enumerate EVERY mover incl. enemies; the
//                                    only way to obtain a target partId
//   unit <partId>                  - deep per-unit readback, including which
//                                    draw path it took (appearance type,
//                                    inView, static recipe index, screenPos)
//   mission_status                 - frame / scenarioTime / sim speed / mission
//                                    status / objective status / mover count
// GAMEPLAY-DRIVER-2 lighting verbs (the day AND night verdict rule):
//   time_of_day <0..1>             - full sun+ambient+sky+fog recompute
//                                    (0.25 noon, 0.5 sunset, 0.75 midnight)
//   sun_pitch <deg> [yaw]          - light DIRECTION only, colors unchanged
//   light_state                    - readback pitch/yaw/isNight/nightFactor/
//                                    ambient/light/fog so a driver can ASSERT
//   end_mission win|loss|draw      - force the termination counter (AAR path)
// GAMEPLAY-DRIVER-3 determinism / coverage verbs:
//   freeze_movers on [frame] | off | status
//                                  - pin every mover (rides Mission::update's
//                                    existing updateAppearancesOnly branch, so
//                                    they still DRAW but stop simulating and
//                                    stop re-pushing torso/turret aim). With a
//                                    <frame> the freeze engages at a
//                                    deterministic mission frame, which is what
//                                    makes a pose reproducible across BUILDS;
//                                    needs MC2_SMOKE_FIXED_TIMESTEP=1.
//   set_light <phase|0..1>|alt <deg> [yaw]
//                                  - named front door for the sun
//                                    (dawn/day/noon/dusk/night/midnight...),
//                                    delegating to the same Camera calls
//                                    time_of_day / sun_pitch use. Makes the
//                                    day-AND-night verdict rule expressible in
//                                    a scenario file on ANY mission.
//   list buildings                 - enumerate buildings (NOT movers, so `list`
//                                    can never reach them) with which gestures
//                                    have animation data and whether the object
//                                    is on the static batch or animated path
// RENDERER-INPUT-READBACK-1 verbs. Everything above reads GAME state; these
// five read what the RENDERER was handed, which is the half that was missing:
//   light_row <partId>             - the per-object light row, produced by the
//                                    SAME GatherLightsParameters() the draw
//                                    calls, from the SAME list that object's
//                                    appearance class passes. Per light: type,
//                                    the UPLOADED origin vs the SOURCE origin
//                                    (atOrigin=1 == evaluated at world origin),
//                                    direction, colour, falloff. Plus the BAKED
//                                    row still being replayed for a registered
//                                    static prop, so a stale bake is visible.
//   draw_path <partId>             - recipe_replay | dynamic_submit |
//                                    legacy_cpu, plus recipeDrift = |baked
//                                    matrix - live position|. Non-zero drift
//                                    means the pixels are placed from an old
//                                    pose.
//   shadow_feed <partId>           - membership in the static-building and
//                                    dynamic-prop shadow feeds, the pose each
//                                    SUBMITTED, shadowDrift against the live
//                                    pose, and whether the static pass drew at
//                                    all this mission.
//   ground <partId>|<x> <y>        - terrain elevation / type / normal under a
//                                    point, objAboveGround, and (static props)
//                                    recipeAboveGround + recipeVsObj.
//   pixel <x> <y> [z]|screen <px> <py>
//                                  - one scene-FBO texel at a world point (or
//                                    raw texel). srcFrame is CURRENT-1: this
//                                    channel is polled BEFORE the frame
//                                    renders, so the FBO holds the previous
//                                    frame and a same-line `screenshot` is one
//                                    frame AHEAD.
// GAMEPLAY-DRIVER-1/3 test-only fixtures (gated MC2_TEST_SPAWN=1, else blocked):
//   animate <partId> <gestureId|damaged|normal>
//                                  - force ONE building onto the animated path
//                                    (setGesture) or through the damage shape
//                                    swap (setObjStatus). The never-exercised
//                                    defect class; see terrobj.cpp's own
//                                    "may be zero in smoke missions" comment.
//   spawn <clone|vehicle|infantry|objNum> <x> <y> [team] [variant] - addMover
//   teleport <partId> <x> <y>      - Mover::setTeleportPosition engine hook
// Every consumed line is acknowledged by appending "[f=<frame>] <line> -> ok|
// unknown|blocked (reason)" to <path>.ack, so a driving agent can confirm the
// engine actually saw each command AND gate on an exact frame number. Lets
// agents press ANY pause-menu UI button in a
// running instance headlessly (fix-verification driver). Same fixture family
// as MC2_BOOT_PAUSED above. Registered (Trace).
void MissionInterfaceManager::processUiCommandChannel()
{
	static const std::string s_pathText = mc2::gate::str<"MC2_UI_COMMAND_FILE">();   // "" when unset or empty
	static const char* s_path = s_pathText.c_str();
	if (!s_path || !s_path[0])
		return;

	struct _stat64 st;
	if (_stat64(s_path, &st) != 0 || st.st_size <= 0)
		return;

	// Consume: read everything, then truncate so the driver appends fresh
	// commands and an empty file means "all consumed".
	std::vector<std::string> lines;
	{
		FILE* f = fopen(s_path, "rb");  // path-audit: loose-only (MC2_UI_COMMAND_FILE fixture channel, env-supplied path, not game data)
		if (!f)
			return;
		std::string all;
		char buf[512];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			all.append(buf, n);
		fclose(f);
		f = fopen(s_path, "wb");
		if (f)
			fclose(f);
		size_t pos = 0;
		while (pos < all.size()) {
			size_t nl = all.find('\n', pos);
			if (nl == std::string::npos)
				nl = all.size();
			std::string line = all.substr(pos, nl - pos);
			pos = nl + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.pop_back();
			if (!line.empty())
				lines.push_back(line);
		}
	}
	if (lines.empty())
		return;

	const std::string ackPath = std::string(s_path) + ".ack";
	FILE* ack = fopen(ackPath.c_str(), "ab");

	for (size_t i = 0; i < lines.size(); ++i) {
		const std::string& line = lines[i];

		UiCmdCtx c{line};
		// Groups run in the original verb-chain order: prefix matches are order-sensitive
		// ("order move " before "order ", "list buildings" before "list").
		(void)(uiCmdBasic(c) || uiCmdWorld(c) || uiCmdReadback(c) || uiCmdProbe(c) || uiCmdFixture(c));
		const char* result = c.result;
		const std::string& note = c.note;
		const std::string& extra = c.extra;

		if (ack) {
			// GAMEPLAY-DRIVER-2: echo the render frame number on EVERY ack. The
			// ack previously carried no frame, which is the single reason
			// scenario_runner's at_frame / capture-by-frame gates had to be
			// wall-clock approximations ((now - t_live) * fps). With this a
			// driver can gate exactly.
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			if (note.empty())
				mc2::fmt::print(ack, "[f={:d}] {} -> {}\n", mc2::fmt::as_u32(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed))), line.c_str(), mc2::fmt::safe(result));
			else
				mc2::fmt::print(ack, "[f={:d}] {} -> {} ({})\n", mc2::fmt::as_u32(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed))), line.c_str(), mc2::fmt::safe(result), note.c_str());
			if (!extra.empty())
				mc2::fmt::print(ack, "{}", extra.c_str());
		}
	}
	if (ack) {
		fflush(ack);
		fclose(ack);
	}
}

// Pause menu, dialog and capture verbs (toggle_pause, press, debugview, screenshot). Returns true when a verb of this group matched.
bool MissionInterfaceManager::uiCmdBasic(UiCmdCtx& c)
{
	const std::string& line = c.line;
	const char*& result = c.result;
	std::string& note = c.note;

	if (line == "toggle_pause") {
		if (eye && eye->inMovieMode) {
			result = "blocked";
			note = "movie mode";
		} else {
			togglePause();
			result = "ok";
		}
	}
	else if (line.compare(0, 12, "press pause.") == 0) {
		static const struct { const char* name; int id; } kBtns[] = {
			{ "OBJECTIVES", PauseWindow::OBJECTIVES },
			{ "SAVE",       PauseWindow::SAVE },
			{ "LOAD",       PauseWindow::LOAD },
			{ "OPTIONS",    PauseWindow::OPTIONS },
			{ "RESTART",    PauseWindow::RESTART },
			{ "EXIT",       PauseWindow::EXIT },
			{ "RETURN",     PauseWindow::RETURN },
		};
		const std::string btn = line.substr(12);
		int id = -1;
		for (const auto & btnElem : kBtns) {
			if (btn == btnElem.name) {
				id = btnElem.id;
				break;
			}
		}
		PauseWindow* pw = controlGui.getPauseWnd();
		if (id < 0) {
			note = "no such pause button";
		} else if (!pw || !isPaused() || isPausedWithoutMenu()) {
			result = "blocked";
			note = "pause menu not up";
		} else if (pw->confirmDialogUp()) {
			result = "blocked";
			note = "confirm dialog up (use press dialog.YES/NO)";
		} else {
			pw->handleClick(id);
			result = "ok";
		}
	}
	else if (line == "press dialog.YES" || line == "press dialog.NO") {
		PauseWindow* pw = controlGui.getPauseWnd();
		if (!pw || !pw->confirmDialogUp()) {
			result = "blocked";
			note = "no confirm dialog up";
		} else {
			LogisticsOKDialog::instance()->handleMessage(aMSG_LEFTMOUSEDOWN,
				line == "press dialog.YES" ? static_cast<unsigned long>(LogisticsScreen::YES)
										   : static_cast<unsigned long>(LogisticsScreen::NO));
			result = "ok";
		}
	}
	else if (line.compare(0, 10, "debugview ") == 0) {
		// DEBUG-VIEW-SWEEP-1: runtime global debug-view switch.
		static const char* kViewNames[7] = {
			"off", "shadow-factor", "normalized-depth", "unlit-albedo",
			"overdraw-proxy", "world-normals", "chunk-tint"
		};
		const std::string arg = line.substr(10);
		char* end = nullptr;
		const long v = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0' || v < 0 || v > 6) {
			note = "debugview N, N in 0..6";
		} else {
			gos_SetGlobalDebugView(static_cast<int>(v));
			result = "ok";
			note = kViewNames[v];
		}
	}
	else if (line.compare(0, 11, "screenshot ") == 0) {
		std::string name = line.substr(11);
		const size_t sp = name.find(' ');
		if (sp != std::string::npos)
			name = name.substr(0, sp);
		if (!mc2::gate::get<"MC2_DEV_SHELL">()) {
			result = "blocked";
			note = "needs MC2_DEV_SHELL=1 (capture hook is shell-gated)";
		} else if (name.empty()) {
			note = "missing name";
		} else {
			_mkdir("dev_shell_out");  // capture hook does not create it
			gos_dev_shell::scheduleScreenshot(name.c_str());
			result = "ok";
			note = "dev_shell_out/" + name + ".tga at end of frame";
		}
	}
	else
		return false;
	return true;
}

// GAMEPLAY-DRIVER-1 world verbs (select, order, camera, time_scale, status). Returns true when a verb of this group matched.
bool MissionInterfaceManager::uiCmdWorld(UiCmdCtx& c)
{
	const std::string& line = c.line;
	const char*& result = c.result;
	std::string& note = c.note;
	std::string& extra = c.extra;

	//---------------------------------------------------------------------
	// GAMEPLAY-DRIVER-1 world verbs. Every one routes through the SAME
	// dispatcher the mouse/keyboard uses (handleOrders / Mover::setSelected
	// / Camera::setPosition) so game rules apply. The homeCmdr / homeTeam
	// gate matches the roster walks in doMove/doAttack.
	if (line.compare(0, 7, "select ") == 0) {
		const std::string arg = line.substr(7);
		Team* pTeam = Team::home;
		const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
		if (!pTeam) { result = "blocked"; note = "no home team"; }
		else if (arg == "all" || arg == "lance") {
			long n = 0;
			for (long i = 0; i < pTeam->getRosterSize(); i++) {
				Mover* m = static_cast<Mover*>(pTeam->getMover(i));
				if (!m) continue;
				const bool mine = m->isOnGUI() && !m->isDisabled() &&
					commanderIdIs(m->getCommander(), homeCmdr);
				m->setSelected(mine);
				if (mine) n++;
			}
			result = "ok";
			char b[48]; mc2::fmt::to_buf(b, "{:d} selected", mc2::fmt::as_i32(n)); note = b;
		}
		// GAMEPLAY-DRIVER-2: predicate selection. `select <partId>` alone
		// forces an agent to hardcode ids that change per mission AND per
		// run (spawned ids are allocated in a different range), which is
		// exactly what made the shipped night_light_test scenario aim at
		// the wrong units.
		else if (arg.compare(0, 6, "class ") == 0 || arg.compare(0, 5, "name ") == 0) {
			const bool byClass = (arg.compare(0, 6, "class ") == 0);
			std::string needle = arg.substr(byClass ? 6 : 5);
			for (auto & needleElem : needle)
				needleElem = static_cast<char>(toupper(static_cast<unsigned char>(needleElem)));
			long n = 0;
			for (long i = 0; i < pTeam->getRosterSize(); i++) {
				Mover* m = static_cast<Mover*>(pTeam->getMover(i));
				if (!m) continue;
				bool hit = false;
				if (m->isOnGUI() && !m->isDisabled() &&
					commanderIdIs(m->getCommander(), homeCmdr)) {
					std::string hay = byClass
						? std::string(getObjectClassName(m->getObjectClass()))
						: std::string(m->getName() ? m->getName() : "");
					for (auto & hayElem : hay)
						hayElem = static_cast<char>(toupper(static_cast<unsigned char>(hayElem)));
					hit = (hay.find(needle) != std::string::npos);
				}
				m->setSelected(hit);
				if (hit) n++;
			}
			result = n ? "ok" : "blocked";
			char b[96];
			mc2::fmt::to_buf(b, "{:d} matched {} '{}'", mc2::fmt::as_i32(n), mc2::fmt::safe(byClass ? "class" : "name"), needle.c_str());
			note = b;
		}
		else {
			char* end = nullptr;
			const long pid = strtol(arg.c_str(), &end, 10);
			if (arg.empty() || !end || *end != '\0') {
				note = "select <partId|all|lance|class <NAME>|name <substr>>";
			} else {
				GameObjectPtr obj = ObjectManager->findByPartId(pid);
				if (!obj || !obj->isMover()) {
					result = "blocked"; note = "no such mover";
				} else if (!commanderIdIs((static_cast<Mover*>(obj))->getCommander(), homeCmdr)) {
					result = "blocked"; note = "not a player unit";
				} else {
					for (long i = 0; i < pTeam->getRosterSize(); i++) {
						Mover* m = static_cast<Mover*>(pTeam->getMover(i));
						if (m) m->setSelected(false);
					}
					(static_cast<Mover*>(obj))->setSelected(true);
					result = "ok";
				}
			}
		}
	}
	else if (line == "skip_intro") {
		// Same as pressing ESC during a cinematic: ends movie mode so the HUD returns.
		if (eye && eye->inMovieMode) eye->forceMovieToEnd();
		result = "ok";
	}
	else if (line.compare(0, 9, "pickgrid ") == 0) {
		int cols = 0, rows = 0;
		std::sscanf(line.c_str() + 9, "%d %d", &cols, &rows);
		extern std::string mc2PickGridReport(int, int);
		note = std::string(1, static_cast<char>(10)) + mc2PickGridReport(cols > 0 ? cols : 8, rows > 0 ? rows : 5);
		result = "ok";
	}
	else if (line.compare(0, 6, "mouse ") == 0) {
		// mouse <x> <y>: park the raw cursor on a logical screen point (pick proof on empty ground).
		float x = 0, y = 0;
		std::sscanf(line.c_str() + 6, "%f %f", &x, &y);
		gos_TestSetRawMouse(x * Environment.drawableWidth / Environment.screenWidth, y * Environment.drawableHeight / Environment.screenHeight);
		result = "ok";
	}
	else if (line.compare(0, 6, "hover ") == 0) {
		// hover <partId>: park the raw cursor on that object (hover tooltip proof).
		GameObjectPtr obj = ObjectManager->findByPartId(strtol(line.c_str() + 6, nullptr, 10));
		Stuff::Vector4D sp;
		if (!obj || !eye) { result = "blocked"; note = "no such object"; }
		else {
			glm::vec3 pos = obj->getPosition();
			eye->projectForScreenXY(pos, sp);
			gos_TestSetRawMouse(sp.x * Environment.drawableWidth / Environment.screenWidth, sp.y * Environment.drawableHeight / Environment.screenHeight);	// sp is logical; the raw mouse is in window points
			result = "ok";
		}
	}
	else if (line.compare(0, 11, "order move ") == 0) {
		// order move <x> <y> [run|walk]  (default run)
		float x = 0, y = 0; char mode[16] = {0};
		const int nf = sscanf(line.c_str() + 11, "%f %f %15s", &x, &y, mode);
		if (nf < 2) {
			note = "order move <x> <y> [run|walk]";
		} else {
			const bool walk = (nf >= 3 && strcmp(mode, "walk") == 0);
			glm::vec3 pos; pos.x = x; pos.y = y; pos.z = 0.0f;
			if (land) pos.z = land->getTerrainElevation(pos);
			Team* pTeam = Team::home;
			const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
			long sel = 0;
			for (long i = 0; pTeam && i < pTeam->getRosterSize(); i++) {
				Mover* m = static_cast<Mover*>(pTeam->getMover(i));
				if (m && m->isSelected() && commanderIdIs(m->getCommander(), homeCmdr)) sel++;
			}
			if (!sel) {
				result = "blocked"; note = "no player unit selected (use select)";
			} else {
				LocationNode path; path.location = pos; path.run = !walk; path.next = nullptr;
				TacticalOrder tacOrder;
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
				tacOrder.initWayPath(&path);
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] = walk ? TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				tacOrder.pack(nullptr, nullptr);
				handleOrders(tacOrder);   // same dispatcher doMove() calls
				result = "ok";
				char b[96];
				mc2::fmt::to_buf(b, "{:d} unit(s) -> ({:.1f},{:.1f}) {}", mc2::fmt::as_i32(sel), x, y, mc2::fmt::safe(walk ? "walk" : "run"));
				note = b;
			}
		}
	}
	else if (line.compare(0, 13, "order attack ") == 0) {
		const std::string arg = line.substr(13);
		char* end = nullptr;
		const long tid = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0') {
			note = "order attack <targetPartId>";
		} else {
			GameObjectPtr tobj = ObjectManager->findByPartId(tid);
			if (!tobj) { result = "blocked"; note = "no such target"; }
			else {
				Team* pTeam = Team::home;
				const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
				long sel = 0;
				for (long i = 0; pTeam && i < pTeam->getRosterSize(); i++) {
					Mover* m = static_cast<Mover*>(pTeam->getMover(i));
					if (!(m && m->isSelected() && commanderIdIs(m->getCommander(), homeCmdr)))
						continue;
					TacticalOrder tacOrder;
					tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
					tacOrder.targetWID = tobj->getWatchID();
					tacOrder.attackParams.type = ATTACK_TO_DESTROY;
					tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
					tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
					tacOrder.attackParams.pursue = true;
					tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
					tacOrder.pack(nullptr, nullptr);
					m->handleTacticalOrder(tacOrder);
					sel++;
				}
				if (!sel) { result = "blocked"; note = "no player unit selected"; }
				else {
					result = "ok";
					char b[64]; mc2::fmt::to_buf(b, "{:d} unit(s) attacking {:d}", mc2::fmt::as_i32(sel), mc2::fmt::as_i32(tid)); note = b;
				}
			}
		}
	}
	// GAMEPLAY-DRIVER-2: the no-argument TacticalOrder codes. Without at
	// least a stop an agent can start a move but never cancel it.
	// (There is no FREE_FIRE counterpart to HOLD_FIRE in the enum --
	// tacordr.h -- so hold_fire is cleared by issuing a new order.)
	else if (line.compare(0, 6, "order ") == 0 &&
			 (line.substr(6) == "stop" || line.substr(6) == "hold_fire" ||
			  line.substr(6) == "powerup" || line.substr(6) == "powerdown" ||
			  line.substr(6) == "withdraw")) {
		const std::string sub = line.substr(6);
		TacticalOrderCode code = TACTICAL_ORDER_STOP;
		if      (sub == "hold_fire") code = TACTICAL_ORDER_HOLD_FIRE;
		else if (sub == "powerup")   code = TACTICAL_ORDER_POWERUP;
		else if (sub == "powerdown") code = TACTICAL_ORDER_POWERDOWN;
		else if (sub == "withdraw")  code = TACTICAL_ORDER_WITHDRAW;
		Team* pTeam = Team::home;
		const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
		long sel = 0;
		for (long i = 0; pTeam && i < pTeam->getRosterSize(); i++) {
			Mover* m = static_cast<Mover*>(pTeam->getMover(i));
			if (!(m && m->isSelected() && commanderIdIs(m->getCommander(), homeCmdr)))
				continue;
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, code);
			tacOrder.pack(nullptr, nullptr);
			m->handleTacticalOrder(tacOrder);
			sel++;
		}
		if (!sel) { result = "blocked"; note = "no player unit selected"; }
		else {
			result = "ok";
			char b[64]; mc2::fmt::to_buf(b, "{:d} unit(s) {}", mc2::fmt::as_i32(sel), sub.c_str()); note = b;
		}
	}
	else if (line.compare(0, 7, "camera ") == 0) {
		// camera <x> <y> [rotDeg] [zoomAltitude]
		float x = 0, y = 0, rot = 0, zoom = 0;
		const int nf = sscanf(line.c_str() + 7, "%f %f %f %f", &x, &y, &rot, &zoom);
		if (nf < 2) { note = "camera <x> <y> [rotDeg] [zoomAlt]"; }
		else if (!eye) { result = "blocked"; note = "no camera"; }
		else {
			glm::vec3 pos; pos.x = x; pos.y = y; pos.z = 0.0f;
			if (land) pos.z = land->getTerrainElevation(pos);
			eye->setPosition(pos, true);              // swoopy = goal-seek smoothing
			if (nf >= 3) eye->setCameraRotation(rot, rot);
			if (nf >= 4) eye->setCameraAltitudeDesired(zoom);
			result = "ok";
			char b[96];
			mc2::fmt::to_buf(b, "cam ({:.1f},{:.1f}){}{}", x, y, mc2::fmt::safe(nf >= 3 ? " +rot" : ""), mc2::fmt::safe(nf >= 4 ? " +zoom" : ""));
			note = b;
		}
	}
	// GAMEPLAY-DRIVER-2: frame a SPECIFIC unit instead of guessing world
	// coordinates. Camera::setCameraTargetId sets lookTargetObject, which
	// GameCamera::update consumes every frame via getCamObject(partId,true).
	// NOTE: mutually exclusive with MC2_CAMERA_PARK -- freezeScriptedCamera
	// ForCapture() deliberately clears lookTargetObject.
	else if (line.compare(0, 14, "camera_follow ") == 0) {
		const std::string arg = line.substr(14);
		if (!eye) { result = "blocked"; note = "no camera"; }
		else if (arg == "off") {
			eye->setCameraTargetId(-1);
			result = "ok"; note = "follow cleared";
		} else {
			char* end = nullptr;
			const long pid = strtol(arg.c_str(), &end, 10);
			if (arg.empty() || !end || *end != '\0') {
				note = "camera_follow <partId|off>";
			} else {
				GameObjectPtr obj = ObjectManager->findByPartId(pid);
				if (!obj) { result = "blocked"; note = "no such object"; }
				else {
					eye->setCameraTargetId(pid);
					glm::vec3 p = obj->getPosition();
					result = "ok";
					char b[96];
					mc2::fmt::to_buf(b, "following {:d} at ({:.1f},{:.1f})", mc2::fmt::as_i32(pid), p.x, p.y);
					note = b;
				}
			}
		}
	}
	else if (line.compare(0, 11, "time_scale ") == 0) {
		// GAMEPLAY-DRIVER-2 CORRECTION: the ui-phase1 prototype wrote its own
		// g_uiTimeScale, which multiplied frameLength ONLY -- so motion sped
		// up while scenarioTime (objective timers) did not, and the two
		// clocks silently diverged. Canonical has since grown
		// SIM-SPEED-BUTTON-1, which applies one multiplier at BOTH drivers
		// in lockstep (scenarioTime in mission.cpp, frameLength in
		// mechcmd2.cpp). Route the verb through that instead of
		// reintroducing the divergence. Clamp is the shared [0.25, 4.0].
		const std::string arg = line.substr(11);
		char* end = nullptr;
		const float f = strtof(arg.c_str(), &end);
		if (arg.empty() || !end || *end != '\0') {
			note = "time_scale <factor 0.25..4>";
		} else {
			simSpeedSetMultiplier(f);
			result = "ok";
			char b[128];
			mc2::fmt::to_buf(b, "sim_speed={:.2f}{}", static_cast<double>(simSpeedGetMultiplier()), mc2::fmt::safe(SmokeMode::fixedTimestepEnabled()
					? " (NOTE: MC2_SMOKE_FIXED_TIMESTEP pins both clocks to 1x)"
					: ""));
			note = b;
		}
	}
	else if (line == "status") {
		// dump every selected player unit's id/name/pos/health to the ack file
		Team* pTeam = Team::home;
		const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
		long n = 0;
		for (long i = 0; pTeam && i < pTeam->getRosterSize(); i++) {
			Mover* m = static_cast<Mover*>(pTeam->getMover(i));
			if (!(m && m->isSelected() && commanderIdIs(m->getCommander(), homeCmdr)))
				continue;
			glm::vec3 p = m->getPosition();
			char b[224];
			mc2::fmt::to_buf(b, "  [id={:d} name={} pos=({:.1f},{:.1f},{:.1f}) hp={:.3f}{}]\n", mc2::fmt::as_i32(m->getPartId()), mc2::fmt::safe((m->getName() ? m->getName() : "?")), p.x, p.y, p.z, m->getStatusRating(), mc2::fmt::safe(m->isDestroyed() ? " DESTROYED" : ""));
			extra += b;
			n++;
		}
		result = "ok";
		char h[48]; mc2::fmt::to_buf(h, "{:d} selected", mc2::fmt::as_i32(n)); note = h;
	}
	else
		return false;
	return true;
}

// GAMEPLAY-DRIVER-2 game-state readback and lighting verbs (list, unit, mission_status, end_mission, time_of_day, sun_pitch, light_state). Returns true when a verb of this group matched.
bool MissionInterfaceManager::uiCmdReadback(UiCmdCtx& c)
{
	const std::string& line = c.line;
	const char*& result = c.result;
	std::string& note = c.note;
	std::string& extra = c.extra;

	//---------------------------------------------------------------------
	// GAMEPLAY-DRIVER-2 readback verbs. `status` only reports units that are
	// ALREADY selected, and `select` only reaches home-commander units, so
	// nothing in the channel could ever produce an ENEMY partId -- which is
	// why `order attack` was undrivable end-to-end and why no shipped
	// scenario exercised it.
	// ANIMATE-BUILDING-1: buildings are NOT movers, so the mover enumeration
	// in the generic `list` branch below can never reach one -- which is a
	// large part of why the animated-building defect class was never driven.
	// MUST stay ahead of the `list ` prefix test or that branch swallows it.
	// The fields printed are the ones that actually discriminate the class:
	// WHICH gestures have animation data, and whether the object is sitting
	// on the GPU static-prop batch or on the per-frame animated path.
	if (line == "list buildings") {
		if (!ObjectManager) {
			result = "blocked"; note = "no object manager";
		} else {
			std::vector<long> seen;
			long n = 0, animatable = 0, nTurret = 0;
			// Buildings, terrain objects and TURRETS live in THREE SEPARATE
			// arrays -- objmgr.cpp increments numTerrainObjects /
			// numTurrets / numGates on independent branches of one switch,
			// and objmgr.h exposes getNumBuildings /
			// getNumTerrainObjects / getNumTurrets as separate accessors.
			// Walk all three and de-dup by partId.
			//
			// TURRET-LIST-ENUM-1: the turret pass was missing. Every pop-up
			// turret (151 of them across 11 stock maps) carries a
			// BldgAppearance with real gesture data, and the `animate` verb
			// below tells the operator to take ids + animGestures "from
			// `list buildings`" -- which never listed a single turret.
			// findByPartId (objmgr.cpp) walks the master objList, so
			// `animate` could always DRIVE a turret; only the enumeration
			// that hands you its partId was absent.
			for (int pass = 0; pass < 3; ++pass) {
				const long cnt = (pass == 0) ? ObjectManager->getNumBuildings()
							   : (pass == 1) ? ObjectManager->getNumTerrainObjects()
											 : ObjectManager->getNumTurrets();
				for (long i = 0; i < cnt; ++i) {
					GameObjectPtr o = (pass == 0)
						? static_cast<GameObjectPtr>(ObjectManager->getBuilding(i))
						: (pass == 1)
							? static_cast<GameObjectPtr>(ObjectManager->getTerrainObject(i))
							: static_cast<GameObjectPtr>(ObjectManager->getTurret(i));
					if (!o || !o->getExists()) continue;
					BldgAppearance* ba =
						dynamic_cast<BldgAppearance*>(o->getAppearance());
					if (!ba || !ba->appearType) continue;
					const long pid = o->getPartId();
					bool dup = false;
					for (size_t s = 0; s < seen.size(); ++s)
						if (seen[s] == pid) { dup = true; break; }
					if (dup) continue;
					seen.push_back(pid);
					char mask[MAX_BD_ANIMATIONS + 1];
					int mi = 0;
					for (long g = 0; g < MAX_BD_ANIMATIONS; ++g)
						if (ba->hasAnimationData(g))
							mask[mi++] = static_cast<char>(('0' + g));
					mask[mi] = '\0';
					if (mi > 0) ++animatable;
					glm::vec3 p = o->getPosition();
					const char* cls = getObjectClassName(o->getObjectClass());
					char b[352];
					mc2::fmt::to_buf(b, "  [id={:d} type={} class={} pos=({:.1f},{:.1f},{:.1f}) gesture={:d} animGestures={} staticReg={:d} recipe={:d} staticNow={:d} eligible={:d}]\n", mc2::fmt::as_i32(pid), mc2::fmt::safe((ba->appearType->name ? ba->appearType->name : "?")), mc2::fmt::safe(cls ? cls : "?"), p.x, p.y, p.z, mc2::fmt::as_i32(ba->getCurrentGestureId()), mc2::fmt::safe(mi > 0 ? mask : "-"), mc2::fmt::as_i32(ba->isStaticRegistered() ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(ba->getStaticRecipeIndex())), mc2::fmt::as_i32(ba->IsStaticNow() ? 1 : 0), mc2::fmt::as_i32(ba->isStaticEligible() ? 1 : 0));
					extra += b;
					++n;
					if (pass == 2) ++nTurret;
				}
			}
			char b[192];
			mc2::fmt::to_buf(b, "  buildings={:d} turrets={:d} animatable={:d} animStartInvalidated={:d}\n", mc2::fmt::as_i32(n - nTurret), mc2::fmt::as_i32(nTurret), mc2::fmt::as_i32(animatable), mc2::fmt::as_u32(static_cast<unsigned long>(g_bldgAnimGate_animStartInvalidated())));
			extra += b;
			mc2::fmt::to_buf(b, "{:d} object(s) ({:d} turret(s)), {:d} with animation data", mc2::fmt::as_i32(n), mc2::fmt::as_i32(nTurret), mc2::fmt::as_i32(animatable));
			note = b;
			result = "ok";
		}
	}
	else if (line == "list" || line.compare(0, 5, "list ") == 0) {
		const std::string arg = (line.size() > 5) ? line.substr(5) : std::string("all");
		const long homeCmdr = Commander::home ? Commander::home->getId() : -1;
		const long homeTeam = Team::home ? Team::home->getId() : -1;
		long wantTeam = -2, n = 0;
		std::string wantClass;
		bool bad = false, playerOnly = false, enemyOnly = false;
		if (arg == "all" || arg.empty()) { /* default */ }
		else if (arg == "player") playerOnly = true;
		else if (arg == "enemy")  enemyOnly = true;
		else if (arg.compare(0, 5, "team ") == 0) {
			char* end = nullptr;
			wantTeam = strtol(arg.c_str() + 5, &end, 10);
			if (!end || *end != '\0') bad = true;
		}
		else if (arg.compare(0, 6, "class ") == 0) {
			wantClass = arg.substr(6);
			for (size_t c = 0; c < wantClass.size(); ++c)
				wantClass[c] = static_cast<char>(toupper(static_cast<unsigned char>(wantClass[c])));
		}
		else bad = true;

		if (bad) {
			note = "list [all|player|enemy|team <N>|class <NAME>]";
		} else if (!ObjectManager) {
			result = "blocked"; note = "no object manager";
		} else {
			for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
				MoverPtr m = ObjectManager->getMover(i);
				if (!m) continue;
				const bool mine = (m->getCommanderId() == homeCmdr);
				if (playerOnly && !mine) continue;
				if (enemyOnly && mine) continue;
				if (wantTeam != -2 && m->getTeamId() != wantTeam) continue;
				const char* cls = getObjectClassName(m->getObjectClass());
				if (!wantClass.empty()) {
					std::string hay(cls ? cls : "");
					for (size_t c = 0; c < hay.size(); ++c)
						hay[c] = static_cast<char>(toupper(static_cast<unsigned char>(hay[c])));
					if (hay.find(wantClass) == std::string::npos) continue;
				}
				glm::vec3 p = m->getPosition();
				AppearancePtr ap = m->getAppearance();
				char b[288];
				mc2::fmt::to_buf(b, "  [id={:d} name={} class={} team={:d} cmdr={:d} pos=({:.1f},{:.1f},{:.1f}) hp={:.3f} sel={:d} gui={:d} inView={:d}{}{}]\n", mc2::fmt::as_i32(m->getPartId()), mc2::fmt::safe((m->getName() ? m->getName() : "?")), mc2::fmt::safe(cls ? cls : "?"), mc2::fmt::as_i32(m->getTeamId()), mc2::fmt::as_i32(m->getCommanderId()), p.x, p.y, p.z, m->getStatusRating(), mc2::fmt::as_i32(m->isSelected() ? 1 : 0), mc2::fmt::as_i32(m->isOnGUI() ? 1 : 0), mc2::fmt::as_i32((ap && ap->canBeSeen()) ? 1 : 0), mc2::fmt::safe(m->isDisabled()  ? " DISABLED"  : ""), mc2::fmt::safe(m->isDestroyed() ? " DESTROYED" : ""));
				extra += b;
				n++;
			}
			result = "ok";
			char h[80];
			mc2::fmt::to_buf(h, "{:d} mover(s) of {:d}", mc2::fmt::as_i32(n), mc2::fmt::as_i32(ObjectManager->getNumMovers()));
			note = h;
		}
	}
	// GAMEPLAY-DRIVER-2: per-unit deep readback, INCLUDING which draw path
	// the object took. getStatusRating() collapses armor x weapons x pilot
	// into one float, so `status` can report hp=0.000 without telling you
	// WHY -- which is precisely how the shipped night_light_test scenario
	// passed while every unit it spawned was inert.
	else if (line.compare(0, 5, "unit ") == 0) {
		const std::string arg = line.substr(5);
		char* end = nullptr;
		const long pid = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0') {
			note = "unit <partId>";
		} else {
			GameObjectPtr obj = ObjectManager ? ObjectManager->findByPartId(pid) : nullptr;
			if (!obj) { result = "blocked"; note = "no such object"; }
			else {
				glm::vec3 p = obj->getPosition();
				char b[512];
				mc2::fmt::to_buf(b, "  id={:d} name={} class={} pos=({:.2f},{:.2f},{:.2f}) hp={:.3f} exists={:d} disabled={:d} destroyed={:d}\n", mc2::fmt::as_i32(pid), mc2::fmt::safe((obj->getName() ? obj->getName() : "?")), mc2::fmt::safe(getObjectClassName(obj->getObjectClass())), p.x, p.y, p.z, obj->getStatusRating(), mc2::fmt::as_i32(obj->getExists() ? 1 : 0), mc2::fmt::as_i32(obj->isDisabled() ? 1 : 0), mc2::fmt::as_i32(obj->isDestroyed() ? 1 : 0));
				extra += b;

				if (obj->isMover()) {
					MoverPtr m = static_cast<MoverPtr>(obj);
					MechWarriorPtr pilot = m->getPilot();
					mc2::fmt::to_buf(b, "  mover: team={:d} cmdr={:d} gui={:d} selected={:d} typeHandle={:d} pilot={}\n", mc2::fmt::as_i32(m->getTeamId()), mc2::fmt::as_i32(m->getCommanderId()), mc2::fmt::as_i32(m->isOnGUI() ? 1 : 0), mc2::fmt::as_i32(m->isSelected() ? 1 : 0), mc2::fmt::as_i32(m->getTypeHandle()), mc2::fmt::safe((pilot && pilot->getName()) ? pilot->getName() : "<none>"));
					extra += b;
				}

				// --- draw-path readback -------------------------------------
				if (obj->getObjectClass() == GROUNDVEHICLE) {
					const auto* vehicle = static_cast<GroundVehicle*>(obj);
					mc2::fmt::to_buf(b, "  vehicle: speed_mps={:.3f} turret_deg={:.3f}\n", vehicle->velocityMag, vehicle->turretRotation);
					extra += b;
				}
				// Which renderer lane did this object actually take this
				// frame? inView is the cull verdict, staticRecipe >= 0 means
				// it went through the StaticSceneProxy / GPU-direct static
				// recipe path (BldgAppearance / TreeAppearance register
				// there), -1 means the per-frame dynamic walk. screenPos is
				// the post-transform position the HUD/pick code uses, so a
				// unit that renders off-frustum shows here as w<=0.
				AppearancePtr ap = obj->getAppearance();
				if (!ap) {
					extra += "  draw: <no appearance>\n";
				} else {
					AppearanceTypePtr at = ap->getAppearanceType();
					Stuff::Vector4D sp = ap->getScreenPos();
					mc2::fmt::to_buf(b, "  draw: appearType={} appearNum={:d} inView={:d} staticRecipe={:d} screenPos=({:.1f},{:.1f},{:.3f},{:.3f})\n", mc2::fmt::safe((at && at->name) ? at->name : "?"), mc2::fmt::as_u32(at ? at->appearanceNum : 0UL), mc2::fmt::as_i32(ap->canBeSeen() ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(ap->getStaticRecipeIndex())), sp.x, sp.y, sp.z, sp.w);
					extra += b;
				}
				result = "ok";
			}
		}
	}
	// GAMEPLAY-DRIVER-2: mission-level readback. Lets a driver gate on real
	// mission state instead of wall-clock guesses.
	else if (line == "mission_status") {
		extern std::atomic<uint32_t> g_mc2FrameCounter;
		char b[320];
		// FREEZE-MOVERS-1: frozen/frozenAt ride the EXISTING readback rather
		// than adding a verb, so assert_mission can gate an A/B arm on "the
		// movers really were pinned, at the frame I asked for".
		mc2::fmt::to_buf(b, "  frame={:d} scenarioTime={:.2f} simSpeed={:.2f} fixedTimestep={:d} status={:d} paused={:d} movers={:d} frozen={:d} frozenAt={:d} missionFrame={:d}\n", mc2::fmt::as_u32(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed))), static_cast<double>(scenarioTime), static_cast<double>(simSpeedGetMultiplier()), mc2::fmt::as_i32(SmokeMode::fixedTimestepEnabled() ? 1 : 0), mc2::fmt::as_i32(mission ? mission->getStatus() : -1L), mc2::fmt::as_i32(isPaused() ? 1 : 0), mc2::fmt::as_i32(ObjectManager ? ObjectManager->getNumMovers() : -1L), mc2::fmt::as_i32(mc2FreezeMoversActive()), mc2::fmt::as_i32(mc2FreezeMoversEngagedAt()), mc2::fmt::as_i32(mc2FreezeMoversMissionFrame()));
		extra += b;
		if (Team::home) {
			mc2::fmt::to_buf(b, "  objectives: status={:d}\n", mc2::fmt::as_i32(static_cast<int>(Team::home->objectives.Status())));
			extra += b;
		}
		result = "ok";
	}
	// GAMEPLAY-DRIVER-2: force a mission outcome. Without this a driver has
	// to satisfy real objectives or lose every unit, so the AAR / salvage /
	// campaign-progression paths were untestable headlessly.
	else if (line.compare(0, 12, "end_mission ") == 0) {
		const std::string arg = line.substr(12);
		unsigned long res = 0;
		bool ok = true;
		if      (arg == "win")  res = mis_PLAYER_WIN_BIG;
		else if (arg == "loss") res = mis_PLAYER_LOST_BIG;
		else if (arg == "draw") res = mis_PLAYER_DRAW;
		else ok = false;
		if (!ok) { note = "end_mission win|loss|draw"; }
		else if (!mission) { result = "blocked"; note = "no mission"; }
		else {
			mission->forceTerminate(res);
			result = "ok";
			char b[96];
			mc2::fmt::to_buf(b, "{} (result={:d}); channel goes dead once logistics takes over", arg.c_str(), mc2::fmt::as_u32(res));
			note = b;
		}
	}
	//---------------------------------------------------------------------
	// GAMEPLAY-DRIVER-2 lighting verbs. The day AND night verdict rule was
	// unenforceable from the channel: the ONLY way to see night light was to
	// pick a mission that ships as night, which is why night_light_test.json
	// hardcodes mc2_10. See the contract comment in mclib/camera.h.
	else if (line.compare(0, 12, "time_of_day ") == 0) {
		const std::string arg = line.substr(12);
		char* end = nullptr;
		const float f = strtof(arg.c_str(), &end);
		if (arg.empty() || !end || *end != '\0') {
			note = "time_of_day <0..1>  (0.25=noon, 0.5=sunset, 0.75=midnight)";
		} else if (!eye) {
			result = "blocked"; note = "no camera";
		} else {
			const float pitch = eye->setDaylightFraction(f);
			result = "ok";
			char b[128];
			mc2::fmt::to_buf(b, "lightPitch={:.1f} isNight={:d} nightFactor={:.2f} (full recompute)", static_cast<double>(pitch), mc2::fmt::as_i32(eye->getIsNight() ? 1 : 0), static_cast<double>(eye->getNightFactor()));
			note = b;
		}
	}
	else if (line.compare(0, 10, "sun_pitch ") == 0) {
		// sun_pitch <deg> [yawDeg]   -- direction only, colors unchanged
		float pitch = 0, yaw = 0;
		const int nf = sscanf(line.c_str() + 10, "%f %f", &pitch, &yaw);
		if (nf < 1) { note = "sun_pitch <deg 0..360> [yawDeg]"; }
		else if (!eye) { result = "blocked"; note = "no camera"; }
		else {
			const float p = eye->setSunPitch(pitch, nf >= 2, yaw);
			result = "ok";
			char b[128];
			mc2::fmt::to_buf(b, "lightPitch={:.1f}{} (direction only; ambient/fog unchanged)", static_cast<double>(p), mc2::fmt::safe(nf >= 2 ? " +yaw" : ""));
			note = b;
		}
	}
	else if (line == "light_state") {
		// Readback so a driver can ASSERT the light condition rather than
		// eyeball a screenshot. Pair with time_of_day for a day/night A/B.
		if (!eye) { result = "blocked"; note = "no camera"; }
		else {
			char b[384];
			mc2::fmt::to_buf(b, "  lightPitch={:.2f} lightYaw={:.2f} isNight={:d} nightFactor={:.3f}\n  ambient=({:d},{:d},{:d}) light=({:d},{:d},{:d}) fog=0x{:06x} day2NightTransitionTime={:.1f} dayLightTime={:.1f}\n", static_cast<double>(eye->lightPitch), static_cast<double>(eye->lightYaw), mc2::fmt::as_i32(eye->getIsNight() ? 1 : 0), static_cast<double>(eye->getNightFactor()), mc2::fmt::as_u32(static_cast<unsigned>(eye->ambientRed)), mc2::fmt::as_u32(static_cast<unsigned>(eye->ambientGreen)), mc2::fmt::as_u32(static_cast<unsigned>(eye->ambientBlue)), mc2::fmt::as_u32(static_cast<unsigned>(eye->lightRed)), mc2::fmt::as_u32(static_cast<unsigned>(eye->lightGreen)), mc2::fmt::as_u32(static_cast<unsigned>(eye->lightBlue)), mc2::fmt::as_u32((eye->fogColor & 0xffffffUL)), static_cast<double>(eye->day2NightTransitionTime), static_cast<double>(eye->dayLightTime));
			extra += b;
			result = "ok";
		}
	}
	else
		return false;
	return true;
}
