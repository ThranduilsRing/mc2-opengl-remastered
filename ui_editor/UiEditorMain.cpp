/***************************************************************
 * FILENAME: UiEditorMain.cpp
 * DESCRIPTION: Standalone ImGui shell for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added a minimal compile-first UI Editor executable.
 * - Added canvas, project tree, smart tools, hierarchy, and inspector shell.
 * - Updated the window title to use centralized UI Editor version constants.
 * - Added legacy text, scrollbar, and 3D viewport truth-viewer rendering.
 * - Added read-only loading and display of one real UI FIT layout.
 * - Added explicit FIT path controls and clickable Mechlopedia file loading.
 * - Split the right sidebar into a scrollable hierarchy pane and persistent inspector pane.
 * - Added human-readable cell aliases for hierarchy, canvas labels, and inspector display.
 * - Added in-memory move, resize, inspector rect editing, and dirty-state tracking.
 * - Added locked main.fit composite preview behind Mechlopedia detail FITs.
 * - Aligned composite previews by drawing detail pages at their FIT mount offsets.
 * - Added image preview rendering for FIT cells with texture/image references.
 * - Added Viewer-matched legacy atlas UV preview handling for FIT image/button cells.
 * - Reduced canvas label clutter with hover/selected labels and a Labels toggle.
 * - Added source-preserving Save Copy output for edited FIT rectangles.
 * - Moved Save Copy to the File menu and added Quit.
 * - Added Save Copy overwrite confirmation and dirty reload protection.
 * - Split the left panel into resizable Project/Layouts and Smart Tools panes.
 * - Moved Load Path into the File menu and moved Save Copy path into a dialog.
 * - Added smallest-cell canvas hit testing so large layout rects do not block controls.
 * - Moved selected-cell geometry readout to a fixed canvas HUD.
 * - Changed canvas HUD to a compact vertical readout and removed redundant load hint text.
 * - Added canvas size controls, cursor-centered zoom, and edit-history prep.
 * - Added resolution-independent page profile and scaled-preview foundation.
 * - Expanded the Project browser to discover and load all data/defs/ui FIT files.
 * - Added Concept Shell pass framing, top toolbar grouping, View menu controls, and status bar.
 * - Added UI Page creation and editor-side page navigation metadata foundation.
 * - Added edit command history, undo/redo controls, and per-tab command stacks.
 * - Added image preview diagnostics for legacy texture compatibility auditing.
 * - Added text wrap/anchor/font/color and image placement authoring controls.
 * - Loaded font catalog TTFonts from shared/fonts.fit and applied text pulse/flash preview.
 * - Added legacy button state UV/text-color/text-rect preview support.
 * - Added options companion composition and centered button text preview.
 * - Added v0.5.3s UI cleanup/QOL pass for pane stability, help-text preview, and rect clipboard.
 * - Added v0.5.3t selection, group-move, snap default, and button text-rect editing cleanup.
 * - Added v0.5.3u package/defines/modern rect foundation.
 * - Added v0.5.3x Viewer UV restore, text-rect hit priority, and stricter layout browser filtering.
 ***************************************************************/

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include "UiEditorFitLoader.h"
#include "UiEditorImageCache.h"
#include "UiEditorVersion.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	enum UiEditorTransformMode
	{
		TransformNone = 0,
		TransformMove,
		TransformResizeNW,
		TransformResizeNE,
		TransformResizeSW,
		TransformResizeSE,
		TransformTextMove,
		TransformTextResizeNW,
		TransformTextResizeNE,
		TransformTextResizeSW,
		TransformTextResizeSE
	};


	enum UiEditorSmartTool
	{
		SmartToolSelect = 0,
		SmartToolColorBlock,
		SmartToolText,
		SmartToolButton,
		SmartToolPanel,
		SmartToolImage,
		SmartToolList
	};

	enum UiEditorButtonVisualState
	{
		ButtonVisualNormal = 0,
		ButtonVisualHover,
		ButtonVisualPressed,
		ButtonVisualSelected,
		ButtonVisualDisabled
	};


	enum UiEditorEditCommandKind
	{
		EditCommandRect = 0,
		EditCommandTextRect,
		EditCommandGroupRect,
		EditCommandAlias,
		EditCommandVisibility,
		EditCommandLock,
		EditCommandCreate,
		EditCommandPageLink
	};

	struct UiEditorRectSnapshot
	{
		int cellIndex;
		float x;
		float y;
		float width;
		float height;

		UiEditorRectSnapshot()
			: cellIndex(-1)
			, x(0.0f)
			, y(0.0f)
			, width(0.0f)
			, height(0.0f)
		{
		}
	};

	struct UiEditorEditHistoryEntry
	{
		UiEditorEditCommandKind kind;
		std::string label;
		int cellIndex;
		float beforeX;
		float beforeY;
		float beforeWidth;
		float beforeHeight;
		float afterX;
		float afterY;
		float afterWidth;
		float afterHeight;
		std::string beforeText;
		std::string afterText;
		bool beforeBool;
		bool afterBool;
		std::vector<UiEditorRectSnapshot> beforeRects;
		std::vector<UiEditorRectSnapshot> afterRects;
		bool affectsSource;

		UiEditorEditHistoryEntry()
			: kind(EditCommandRect)
			, cellIndex(-1)
			, beforeX(0.0f)
			, beforeY(0.0f)
			, beforeWidth(0.0f)
			, beforeHeight(0.0f)
			, afterX(0.0f)
			, afterY(0.0f)
			, afterWidth(0.0f)
			, afterHeight(0.0f)
			, beforeBool(false)
			, afterBool(false)
			, affectsSource(false)
		{
		}
	};

	struct UiEditorResolutionProfile
	{
		const char* label;
		int width;
		int height;
		const char* category;
	};

	struct UiEditorUiFitEntry
	{
		std::string loadPath;
		std::string relativePath;
		std::string surface;
		std::string folder;
		std::string filename;
		bool manifest;
		bool mechlopedia;
	};

	struct UiEditorFontCatalogEntry
	{
		std::string key;
		std::string displayName;
		std::string path;
		std::string family;
		std::string style;
		float defaultSize;
		int legacyId;
		bool supportsBold;
		bool supportsItalic;
		ImFont* font;

		UiEditorFontCatalogEntry()
			: defaultSize(16.0f)
			, legacyId(-1)
			, supportsBold(false)
			, supportsItalic(false)
			, font(nullptr)
		{
		}
	};


	struct UiEditorCanvasTab
	{
		std::string loadPath;
		std::string title;
		UiEditorFitLayout layout;
		UiEditorFitLayout baseLayout;
		bool hasBaseLayout;
		bool dirty;
		int selectedCell;
		float zoom;
		int canvasWidth;
		int canvasHeight;
		std::vector<float> verticalGuides;
		std::vector<float> horizontalGuides;
		std::vector<UiEditorEditHistoryEntry> editHistory;
		std::vector<UiEditorEditHistoryEntry> editRedoHistory;
		float scrollX;
		float scrollY;

		UiEditorCanvasTab()
			: hasBaseLayout(false)
			, dirty(false)
			, selectedCell(-1)
			, zoom(0.75f)
			, canvasWidth(800)
			, canvasHeight(600)
			, scrollX(0.0f)
			, scrollY(0.0f)
		{
		}
	};


	struct UiEditorPageLink
	{
		std::string fromPageKey;
		std::string widgetKey;
		std::string action;
		std::string targetPageKey;
		std::string missionKey;
	};


	const UiEditorResolutionProfile kResolutionProfiles[] =
	{
		{ "Legacy 640 x 480", 640, 480, "Legacy 4:3" },
		{ "Legacy 800 x 600", 800, 600, "Legacy 4:3" },
		{ "Legacy 1024 x 768", 1024, 768, "Legacy 4:3" },
		{ "Legacy 1280 x 1024", 1280, 1024, "Legacy 5:4" },
		{ "Legacy 1600 x 1200", 1600, 1200, "Legacy 4:3" },
		{ "Modern 1920 x 1080", 1920, 1080, "Modern 16:9" },
		{ "Custom", 0, 0, "Custom" }
	};

	const char* kAnchorModes[] =
	{
		"Top Left",
		"Top",
		"Top Right",
		"Left",
		"Center",
		"Right",
		"Bottom Left",
		"Bottom",
		"Bottom Right"
	};

	const char* kScalePolicies[] =
	{
		"Fixed",
		"Proportional",
		"Anchored",
		"Stretch",
		"Nine Slice",
		"Font Scale",
		"Atlas Scale"
	};

	const char* kTextAnchorLabels[] =
	{
		"Upper Left",
		"Top Middle",
		"Upper Right",
		"Middle Left",
		"Center",
		"Middle Right",
		"Lower Left",
		"Bottom Middle",
		"Lower Right"
	};

	const char* kTextAnchorValues[] =
	{
		"upper_left",
		"top_middle",
		"upper_right",
		"middle_left",
		"center",
		"middle_right",
		"lower_left",
		"bottom_middle",
		"lower_right"
	};

	const char* kFontPresets[] =
	{
		"Agency Regular",
		"Liberation Sans Regular",
		"Liberation Sans",
		"Arial Narrow",
		"Eurostile",
		"Courier New"
	};


	struct UiEditorLegacyActionPreset
	{
		const char* label;
		const char* actionType;
		const char* legacyMessage;
	};

	const UiEditorLegacyActionPreset kLegacyActionPresets[] =
	{
		{ "None", "none", "" },
		{ "Next", "next", "NEXT" },
		{ "Previous", "previous", "PREVIOUS" },
		{ "Done", "done", "DONE" },
		{ "Paused", "paused", "PAUSED" },
		{ "Up", "up", "UP" },
		{ "Down", "down", "DOWN" },
		{ "Yes", "yes", "YES" },
		{ "No", "no", "NO" },
		{ "Main Menu", "mainMenu", "MAINMENU" },
		{ "Restart", "restart", "RESTART" },
		{ "Multiplayer Restart", "multiplayerRestart", "MULTIPLAYERRESTART" },
		{ "Ready To Load", "readyToLoad", "READYTOLOAD" },
		{ "Go To Splash", "goToSplash", "GOTOSPLASH" },
		{ "Open Page", "openPage", "" },
		{ "Launch Mission", "launchMission", "" },
		{ "Play Video", "playVideo", "PLAYVIDEO" },
		{ "Pause Video", "pauseVideo", "PAUSEVIDEO" },
		{ "Stop Video", "stopVideo", "STOPVIDEO" },
		{ "Custom Legacy Message", "customLegacyMessage", "" }
	};

	const char* kSimulationPreviewStates[] =
	{
		"Normal",
		"Hover",
		"Pressed",
		"Selected",
		"Disabled"
	};

	int g_inspectorBufferCell = -1;
	char g_inspectorVisibleTextBuffer[4096] = { 0 };
	char g_inspectorTextKeyBuffer[256] = { 0 };
	char g_inspectorTextAlignBuffer[64] = { 0 };
	char g_inspectorTextAnchorBuffer[64] = { 0 };
	char g_inspectorFontBuffer[128] = { 0 };
	char g_inspectorFontStyleBuffer[64] = { 0 };
	char g_inspectorTextureBuffer[512] = { 0 };

	struct UiEditorState
	{
		bool showGrid;
		bool snapToGrid;
		bool showRulers;
		bool showGuides;
		bool snapToGuides;
		UiEditorSmartTool activeSmartTool;
		bool creatingSmartWidget;
		ImVec2 smartCreateStart;
		ImVec2 smartCreateCurrent;
		int generatedWidgetCounter;
		bool showCanvasHud;
		int canvasHudMode;
		bool handPanningCanvas;
		ImVec2 handPanStartMouse;
		float handPanStartScrollX;
		float handPanStartScrollY;
		int activeGuideAxis;
		int activeGuideIndex;
		std::vector<float> verticalGuides;
		std::vector<float> horizontalGuides;
		bool dirty;
		bool showImages;
		bool showLabels;
		bool showImageDiagnostics;
		bool simulationMode;
		int simulationPreviewState;
		bool rectClipboardValid;
		float rectClipboardX;
		float rectClipboardY;
		float rectClipboardWidth;
		float rectClipboardHeight;
		bool selectionMarqueeActive;
		ImVec2 selectionMarqueeStart;
		ImVec2 selectionMarqueeCurrent;
		std::vector<int> marqueeSelectedCells;
		std::vector<UiEditorRectSnapshot> groupDragStartRects;
		float dragStartTextX;
		float dragStartTextY;
		float dragStartTextWidth;
		float dragStartTextHeight;
		float dragStartArtX;
		float dragStartArtY;
		float dragStartArtWidth;
		float dragStartArtHeight;
		float dragStartBorderX;
		float dragStartBorderY;
		float dragStartBorderWidth;
		float dragStartBorderHeight;
		bool requestQuit;
		bool showSaveResultPopup;
		bool openSaveCopyDialog;
		bool openDiscardEditsDialog;
		bool openRenameAliasDialog;
		bool openNewPageDialog;
		bool openAddPageLinkDialog;
		int renameAliasCell;
		float leftPanelWidth;
		float rightPanelWidth;
		float leftLayoutsHeight;
		float rightHierarchyHeight;
		bool useDockingLayout;
		bool resetDockLayout;
		int canvasClickTargetCell;
		std::string pendingLoadPath;
		std::string pendingSavePath;
		float zoom;
		int canvasWidth;
		int canvasHeight;
		int lastCanvasWidth;
		int lastCanvasHeight;
		int sourceProfileIndex;
		int previewProfileIndex;
		int previewWidth;
		int previewHeight;
		bool preserveLegacyCoordinates;
		bool showSafeArea;
		int selectedAnchorMode;
		int selectedScalePolicy;
		std::vector<UiEditorEditHistoryEntry> editHistory;
		std::vector<UiEditorEditHistoryEntry> editRedoHistory;
		int selectedCell;
		int activeCell;
		UiEditorTransformMode activeTransform;
		ImVec2 dragStartMouse;
		float dragStartX;
		float dragStartY;
		float dragStartWidth;
		float dragStartHeight;
		UiEditorFitLayout layout;
		UiEditorFitLayout baseLayout;
		bool hasBaseLayout;
		std::string saveStatus;
		bool uiFitBrowserScanned;
		std::string uiFitRootPath;
		std::vector<UiEditorUiFitEntry> uiFitEntries;
		std::vector<UiEditorCanvasTab> canvasTabs;
		std::vector<UiEditorPageLink> pageLinks;
		int activeCanvasTab;

		UiEditorState()
			: showGrid(true)
			, snapToGrid(false)
			, showRulers(true)
			, showGuides(true)
			, snapToGuides(false)
			, activeSmartTool(SmartToolSelect)
			, creatingSmartWidget(false)
			, smartCreateStart(0.0f, 0.0f)
			, smartCreateCurrent(0.0f, 0.0f)
			, generatedWidgetCounter(0)
			, showCanvasHud(true)
			, canvasHudMode(0)
			, handPanningCanvas(false)
			, handPanStartMouse(0.0f, 0.0f)
			, handPanStartScrollX(0.0f)
			, handPanStartScrollY(0.0f)
			, activeGuideAxis(0)
			, activeGuideIndex(-1)
			, dirty(false)
			, showImages(true)
			, showLabels(false)
			, showImageDiagnostics(false)
			, simulationMode(false)
			, simulationPreviewState(0)
			, rectClipboardValid(false)
			, rectClipboardX(0.0f)
			, rectClipboardY(0.0f)
			, rectClipboardWidth(0.0f)
			, rectClipboardHeight(0.0f)
			, selectionMarqueeActive(false)
			, selectionMarqueeStart(0.0f, 0.0f)
			, selectionMarqueeCurrent(0.0f, 0.0f)
			, dragStartTextX(0.0f)
			, dragStartTextY(0.0f)
			, dragStartTextWidth(0.0f)
			, dragStartTextHeight(0.0f)
			, dragStartArtX(0.0f)
			, dragStartArtY(0.0f)
			, dragStartArtWidth(0.0f)
			, dragStartArtHeight(0.0f)
			, dragStartBorderX(0.0f)
			, dragStartBorderY(0.0f)
			, dragStartBorderWidth(0.0f)
			, dragStartBorderHeight(0.0f)
			, requestQuit(false)
			, showSaveResultPopup(false)
			, openSaveCopyDialog(false)
			, openDiscardEditsDialog(false)
			, openRenameAliasDialog(false)
			, openNewPageDialog(false)
			, openAddPageLinkDialog(false)
			, renameAliasCell(-1)
			, leftPanelWidth(320.0f)
			, rightPanelWidth(360.0f)
			, leftLayoutsHeight(380.0f)
			, rightHierarchyHeight(330.0f)
			, canvasClickTargetCell(-1)
			, zoom(0.75f)
			, canvasWidth(800)
			, canvasHeight(600)
			, lastCanvasWidth(800)
			, lastCanvasHeight(600)
			, sourceProfileIndex(1)
			, previewProfileIndex(1)
			, previewWidth(800)
			, previewHeight(600)
			, preserveLegacyCoordinates(true)
			, showSafeArea(true)
			, selectedAnchorMode(0)
			, selectedScalePolicy(1)
			, selectedCell(-1)
			, activeCell(-1)
			, activeTransform(TransformNone)
			, useDockingLayout(true)
			, resetDockLayout(false)
			, hasBaseLayout(false)
			, uiFitBrowserScanned(false)
			, dragStartMouse(0.0f, 0.0f)
			, dragStartX(0.0f)
			, dragStartY(0.0f)
			, dragStartWidth(0.0f)
			, dragStartHeight(0.0f)
			, activeCanvasTab(-1)
		{
		}
	};

	// MERGE-CONFLICT-UI-PHASE1: ours keeps the single "legacy_imgui" UI package path
	// scheme (data/defs/ui/packages/legacy_imgui/...) used throughout this file's path
	// literals, IsLegacyImguiGamePath(), IsCampaignMapPage()/IsCampaignMissionPage(),
	// composition-base lookups, font catalog candidates, and hierarchy/browser default
	// paths. Theirs (mc2r fork) reworked this into a renamed "default" package plus a
	// full multi-package system: g_uiPackages/g_activeUiPackageIndex, ActiveUiPackageName(),
	// RefreshUiPackageList()/SelectUiPackageIndex(), a package combo box in the UI FIT
	// browser, IsDefaultUiPackageGamePath() replacing IsLegacyImguiGamePath(), merging
	// IsCampaignMapPage()/IsCampaignMissionPage() into IsCampaignOverlayPage(), a
	// declared-base (FitLayout::baseLayoutPath) composition override, and a
	// NormalizeLegacyUiPackagePath()/ui_editor.cfg "uiPackage" migration path. These are
	// not composable as a small patch (touches load/save default paths, browser root
	// resolution, composition-base file lookups, and page-classification predicates
	// throughout the file) so ours' single-package logic is kept unmodified here. If the
	// multi-package rework is wanted, it should be done as its own reviewed slice.
	UiEditorState g_state;
	char g_loadPath[512] = "data/defs/ui/packages/legacy_imgui/mechlopedia/main.fit";
	char g_savePath[512] = "data/defs/ui/packages/legacy_imgui/mechlopedia/main.ui_editor_copy.fit";
	char g_renameAliasBuffer[256] = "";
	char g_uiFitFilter[128] = "";
	char g_newPageKey[128] = "ui.new_page";
	char g_newPageTitle[128] = "New UI Page";
	char g_newPageOutputPath[512] = "data/defs/ui/custom/new_page.fit";
	bool g_newPageOpenAfterCreate = true;
	int g_newPageSurfaceIndex = 0;
	int g_newPageProfileIndex = 5;
	char g_pageLinkTarget[128] = "";
	char g_pageLinkMission[128] = "";
	int g_pageLinkActionIndex = 0;

	std::vector<UiEditorFontCatalogEntry> g_fontCatalog;
	ImFont* g_defaultEditorFont = nullptr;
	std::string g_fontCatalogStatus = "Font catalog not loaded.";


	const char* kNewPageSurfaces[] =
	{
		"custom",
		"game",
		"editor",
		"shared",
		"mechlopedia"
	};

	const char* kPageLinkActions[] =
	{
		"openPage",
		"launchMission"
	};

	const char* SmartToolLabel(UiEditorSmartTool tool);
	const char* SmartToolTypeName(UiEditorSmartTool tool);
	const UiEditorFitCell* SelectedCell();
	UiEditorFitCell* SelectedCellMutable();
	bool CopySelectedRect();
	bool CutSelectedRect();
	bool PasteRectToSelected();
	void ClampCellRect(UiEditorFitCell& cell);
	void ClampTextRect(UiEditorFitCell& cell);
	bool IsTextRectTransform(UiEditorTransformMode mode);
	float SnapDesignX(float value);
	float SnapDesignY(float value);
	float SnapDesignValue(float value);
	bool IsButtonLikeCell(const UiEditorFitCell& cell);

	// Additive from theirs (mc2r fork): propagate a button parent's move/resize to its
	// child text/art/border rects proportionally, so linked child geometry does not
	// desync when the parent cell is dragged on the canvas.
	void ApplyButtonChildRectsFromParentTransform(UiEditorFitCell& cell)
	{
		const float startW = std::max(1.0f, g_state.dragStartWidth);
		const float startH = std::max(1.0f, g_state.dragStartHeight);
		const float scaleX = cell.width / startW;
		const float scaleY = cell.height / startH;

		auto transformRect = [&](float startX, float startY, float startWidth, float startHeight, float& outX, float& outY, float& outWidth, float& outHeight)
		{
			const float relX = startX - g_state.dragStartX;
			const float relY = startY - g_state.dragStartY;
			outX = SnapDesignX(cell.x + relX * scaleX);
			outY = SnapDesignY(cell.y + relY * scaleY);
			outWidth = SnapDesignValue(std::max(1.0f, startWidth * scaleX));
			outHeight = SnapDesignValue(std::max(1.0f, startHeight * scaleY));
		};

		if (cell.hasTextRect)
			transformRect(g_state.dragStartTextX, g_state.dragStartTextY, g_state.dragStartTextWidth, g_state.dragStartTextHeight, cell.textX, cell.textY, cell.textWidth, cell.textHeight);

		if (cell.hasArtRect)
			transformRect(g_state.dragStartArtX, g_state.dragStartArtY, g_state.dragStartArtWidth, g_state.dragStartArtHeight, cell.artX, cell.artY, cell.artWidth, cell.artHeight);

		if (cell.hasBorderRect)
			transformRect(g_state.dragStartBorderX, g_state.dragStartBorderY, g_state.dragStartBorderWidth, g_state.dragStartBorderHeight, cell.borderX, cell.borderY, cell.borderWidthRect, cell.borderHeightRect);

		ClampTextRect(cell);
	}

	void PushEditCommand(const UiEditorEditHistoryEntry& entry)
	{
		g_state.editHistory.push_back(entry);
		g_state.editRedoHistory.clear();

		if (g_state.editHistory.size() > 128)
			g_state.editHistory.erase(g_state.editHistory.begin());
	}

	void RecordEditHistory(
		const char* label,
		int cellIndex,
		float beforeX,
		float beforeY,
		float beforeWidth,
		float beforeHeight,
		float afterX,
		float afterY,
		float afterWidth,
		float afterHeight)
	{
		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandRect;
		entry.label = label != nullptr ? label : "Rect edit";
		entry.cellIndex = cellIndex;
		entry.beforeX = beforeX;
		entry.beforeY = beforeY;
		entry.beforeWidth = beforeWidth;
		entry.beforeHeight = beforeHeight;
		entry.afterX = afterX;
		entry.afterY = afterY;
		entry.afterWidth = afterWidth;
		entry.afterHeight = afterHeight;
		entry.affectsSource = true;
		PushEditCommand(entry);
	}

	void RecordTextRectEditHistory(
		const char* label,
		int cellIndex,
		float beforeX,
		float beforeY,
		float beforeWidth,
		float beforeHeight,
		float afterX,
		float afterY,
		float afterWidth,
		float afterHeight)
	{
		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandTextRect;
		entry.label = label != nullptr ? label : "Text rect edit";
		entry.cellIndex = cellIndex;
		entry.beforeX = beforeX;
		entry.beforeY = beforeY;
		entry.beforeWidth = beforeWidth;
		entry.beforeHeight = beforeHeight;
		entry.afterX = afterX;
		entry.afterY = afterY;
		entry.afterWidth = afterWidth;
		entry.afterHeight = afterHeight;
		entry.affectsSource = true;
		PushEditCommand(entry);
	}

	void RecordGroupRectEditHistory(
		const char* label,
		const std::vector<UiEditorRectSnapshot>& beforeRects,
		const std::vector<UiEditorRectSnapshot>& afterRects)
	{
		if (beforeRects.empty() || afterRects.empty())
			return;

		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandGroupRect;
		entry.label = label != nullptr ? label : "Move selected rects";
		entry.cellIndex = afterRects.front().cellIndex;
		entry.beforeRects = beforeRects;
		entry.afterRects = afterRects;
		entry.affectsSource = true;
		PushEditCommand(entry);
	}

	void RecordAliasEditHistory(int cellIndex, const std::string& beforeAlias, const std::string& afterAlias)
	{
		if (beforeAlias == afterAlias)
			return;

		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandAlias;
		entry.label = "Rename alias";
		entry.cellIndex = cellIndex;
		entry.beforeText = beforeAlias;
		entry.afterText = afterAlias;
		entry.affectsSource = false;
		PushEditCommand(entry);
	}

	void RecordVisibilityEditHistory(int cellIndex, bool beforeVisible, bool afterVisible)
	{
		if (beforeVisible == afterVisible)
			return;

		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandVisibility;
		entry.label = afterVisible ? "Show cell" : "Hide cell";
		entry.cellIndex = cellIndex;
		entry.beforeBool = beforeVisible;
		entry.afterBool = afterVisible;
		entry.affectsSource = false;
		PushEditCommand(entry);
	}

	void RecordLockEditHistory(int cellIndex, bool beforeLocked, bool afterLocked)
	{
		if (beforeLocked == afterLocked)
			return;

		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandLock;
		entry.label = afterLocked ? "Lock cell" : "Unlock cell";
		entry.cellIndex = cellIndex;
		entry.beforeBool = beforeLocked;
		entry.afterBool = afterLocked;
		entry.affectsSource = false;
		PushEditCommand(entry);
	}

	void RecordCreateEditHistory(int cellIndex)
	{
		UiEditorEditHistoryEntry entry;
		entry.kind = EditCommandCreate;
		entry.label = "Create smart widget";
		entry.cellIndex = cellIndex;
		entry.beforeBool = false;
		entry.afterBool = true;
		entry.affectsSource = true;
		PushEditCommand(entry);
	}

	bool ApplyEditCommand(const UiEditorEditHistoryEntry& entry, bool undo)
	{
		if (entry.kind == EditCommandPageLink)
			return false;

		if (entry.kind == EditCommandGroupRect)
		{
			const std::vector<UiEditorRectSnapshot>& rects = undo ? entry.beforeRects : entry.afterRects;
			bool applied = false;
			for (std::size_t i = 0; i < rects.size(); ++i)
			{
				const UiEditorRectSnapshot& snapshot = rects[i];
				if (snapshot.cellIndex < 0 || snapshot.cellIndex >= static_cast<int>(g_state.layout.cells.size()))
					continue;

				UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(snapshot.cellIndex)];
				cell.x = snapshot.x;
				cell.y = snapshot.y;
				cell.width = snapshot.width;
				cell.height = snapshot.height;
				ClampCellRect(cell);
				applied = true;
			}

			if (!applied)
				return false;

			if (entry.affectsSource)
				g_state.dirty = true;

			return true;
		}

		if (entry.cellIndex < 0 || entry.cellIndex >= static_cast<int>(g_state.layout.cells.size()))
			return false;

		UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(entry.cellIndex)];

		switch (entry.kind)
		{
		case EditCommandRect:
			cell.x = undo ? entry.beforeX : entry.afterX;
			cell.y = undo ? entry.beforeY : entry.afterY;
			cell.width = undo ? entry.beforeWidth : entry.afterWidth;
			cell.height = undo ? entry.beforeHeight : entry.afterHeight;
			ClampCellRect(cell);
			break;

		case EditCommandTextRect:
			cell.hasTextRect = true;
			cell.textX = undo ? entry.beforeX : entry.afterX;
			cell.textY = undo ? entry.beforeY : entry.afterY;
			cell.textWidth = undo ? entry.beforeWidth : entry.afterWidth;
			cell.textHeight = undo ? entry.beforeHeight : entry.afterHeight;
			ClampTextRect(cell);
			break;

		case EditCommandAlias:
			cell.aliasOverride = undo ? entry.beforeText : entry.afterText;
			break;

		case EditCommandVisibility:
			cell.visible = undo ? entry.beforeBool : entry.afterBool;
			if (!cell.visible && g_state.selectedCell == entry.cellIndex)
			{
				g_state.selectedCell = -1;
				g_state.activeCell = -1;
				g_state.activeTransform = TransformNone;
			}
			break;

		case EditCommandLock:
			cell.locked = undo ? entry.beforeBool : entry.afterBool;
			if (cell.locked && g_state.activeCell == entry.cellIndex)
			{
				g_state.activeCell = -1;
				g_state.activeTransform = TransformNone;
			}
			break;

		case EditCommandCreate:
			cell.visible = undo ? false : true;
			cell.locked = false;
			if (undo && g_state.selectedCell == entry.cellIndex)
			{
				g_state.selectedCell = -1;
				g_state.activeCell = -1;
				g_state.activeTransform = TransformNone;
			}
			else if (!undo)
			{
				g_state.selectedCell = entry.cellIndex;
			}
			break;

		case EditCommandGroupRect:
		case EditCommandPageLink:
		default:
			return false;
		}

		if (entry.affectsSource)
			g_state.dirty = true;

		return true;
	}

	bool UndoLastEditCommand()
	{
		if (g_state.editHistory.empty())
			return false;

		const UiEditorEditHistoryEntry entry = g_state.editHistory.back();
		g_state.editHistory.pop_back();

		if (!ApplyEditCommand(entry, true))
			return false;

		g_state.editRedoHistory.push_back(entry);
		return true;
	}

	bool RedoLastEditCommand()
	{
		if (g_state.editRedoHistory.empty())
			return false;

		const UiEditorEditHistoryEntry entry = g_state.editRedoHistory.back();
		g_state.editRedoHistory.pop_back();

		if (!ApplyEditCommand(entry, false))
			return false;

		g_state.editHistory.push_back(entry);
		return true;
	}

	bool RectChanged(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
	{
		return ax != bx || ay != by || aw != bw || ah != bh;
	}

	const UiEditorResolutionProfile& ResolutionProfileAt(int index)
	{
		const int count = static_cast<int>(sizeof(kResolutionProfiles) / sizeof(kResolutionProfiles[0]));

		if (index < 0 || index >= count)
			return kResolutionProfiles[1];

		return kResolutionProfiles[index];
	}

	int ResolutionProfileCount()
	{
		return static_cast<int>(sizeof(kResolutionProfiles) / sizeof(kResolutionProfiles[0]));
	}

	float AspectRatioFor(int width, int height)
	{
		return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
	}

	const char* AspectCategoryFor(int width, int height)
	{
		const float aspect = AspectRatioFor(width, height);

		if (std::abs(aspect - (16.0f / 9.0f)) < 0.03f)
			return "Modern 16:9";

		if (std::abs(aspect - (4.0f / 3.0f)) < 0.03f)
			return "Legacy 4:3";

		if (std::abs(aspect - (5.0f / 4.0f)) < 0.03f)
			return "Legacy 5:4";

		return "Custom";
	}

	void ApplySourceProfile(int index)
	{
		const UiEditorResolutionProfile& profile = ResolutionProfileAt(index);
		g_state.sourceProfileIndex = index;

		if (profile.width > 0 && profile.height > 0)
		{
			g_state.canvasWidth = profile.width;
			g_state.canvasHeight = profile.height;
			g_state.lastCanvasWidth = g_state.canvasWidth;
			g_state.lastCanvasHeight = g_state.canvasHeight;
		}
	}

	void ApplyPreviewProfile(int index)
	{
		const UiEditorResolutionProfile& profile = ResolutionProfileAt(index);
		g_state.previewProfileIndex = index;

		if (profile.width > 0 && profile.height > 0)
		{
			g_state.previewWidth = profile.width;
			g_state.previewHeight = profile.height;
		}
	}

	float CurrentProfileScale()
	{
		const float sx = g_state.canvasWidth > 0 ? static_cast<float>(g_state.previewWidth) / static_cast<float>(g_state.canvasWidth) : 1.0f;
		const float sy = g_state.canvasHeight > 0 ? static_cast<float>(g_state.previewHeight) / static_cast<float>(g_state.canvasHeight) : 1.0f;

		return std::max(0.05f, std::min(sx, sy));
	}

	float CurrentCanvasDrawScale()
	{
		return std::max(0.05f, g_state.zoom * CurrentProfileScale());
	}

	bool CurrentProfileIsAspectMatched()
	{
		return std::abs(AspectRatioFor(g_state.canvasWidth, g_state.canvasHeight) - AspectRatioFor(g_state.previewWidth, g_state.previewHeight)) < 0.03f;
	}

	ImVec4 AccentOrange()
	{
		return ImVec4(0.95f, 0.55f, 0.12f, 1.0f);
	}

	ImVec4 AccentCyan()
	{
		return ImVec4(0.16f, 0.75f, 0.95f, 1.0f);
	}

	ImVec4 PanelBlue()
	{
		return ImVec4(0.13f, 0.28f, 0.42f, 1.0f);
	}

	ImU32 ColorU32(const ImVec4& color)
	{
		return ImGui::ColorConvertFloat4ToU32(color);
	}

	void ApplyEditorStyle()
	{
		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 3.0f;
		style.ChildRounding = 3.0f;
		style.FrameRounding = 3.0f;
		style.GrabRounding = 3.0f;
		style.ScrollbarRounding = 3.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;
		style.ItemSpacing = ImVec2(7.0f, 5.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
		style.WindowPadding = ImVec2(9.0f, 8.0f);
		style.FramePadding = ImVec2(7.0f, 4.0f);

		ImVec4* colors = style.Colors;
		colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.095f, 1.0f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.095f, 0.105f, 0.122f, 1.0f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.082f, 0.095f, 0.98f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.06f, 0.065f, 0.075f, 1.0f);
		colors[ImGuiCol_Border] = ImVec4(0.28f, 0.30f, 0.34f, 1.0f);
		colors[ImGuiCol_Header] = ImVec4(0.35f, 0.19f, 0.05f, 1.0f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.58f, 0.32f, 0.08f, 1.0f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.75f, 0.42f, 0.10f, 1.0f);
		colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.21f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.25f, 0.08f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.38f, 0.08f, 1.0f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.23f, 1.0f);
		colors[ImGuiCol_CheckMark] = AccentCyan();
		colors[ImGuiCol_SliderGrab] = AccentOrange();
		colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.15f, 0.18f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.36f, 0.48f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(0.15f, 0.30f, 0.42f, 1.0f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.18f, 0.34f, 0.42f, 0.35f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.22f, 0.62f, 0.78f, 0.75f);
	}

	void DrawSectionTitle(const char* text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, AccentOrange());
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImGui::Separator();
	}

	void DrawToolbarGroupLabel(const char* text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.66f, 0.72f, 1.0f));
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImGui::SameLine();
	}

	void DrawToolbarDivider()
	{
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
	}

	bool RequestLoadLayoutPath(const char* path);

	void DrawTopToolbar()
	{
		ImGui::BeginChild("toolbar", ImVec2(0.0f, 44.0f), true);

		DrawToolbarGroupLabel("Mode");
		ImGui::Button("Select");
		ImGui::SameLine();
		ImGui::Button("Move");

		DrawToolbarDivider();

		DrawToolbarGroupLabel("Create");
		ImGui::Button("Cell");
		ImGui::SameLine();
		ImGui::Button("Text");
		ImGui::SameLine();
		ImGui::Button("Image");
		ImGui::SameLine();
		ImGui::Button("Button");

		DrawToolbarDivider();

		DrawToolbarGroupLabel("File");
		if (ImGui::Button("New Page"))
			g_state.openNewPageDialog = true;
		ImGui::SameLine();
		if (ImGui::Button("Load Path"))
			RequestLoadLayoutPath(g_loadPath);
		ImGui::SameLine();
		if (ImGui::Button("Reload"))
			RequestLoadLayoutPath(g_loadPath);
		ImGui::SameLine();
		if (ImGui::Button("Save Copy"))
			g_state.openSaveCopyDialog = true;

		DrawToolbarDivider();

		DrawToolbarGroupLabel("Edit");
		if (!g_state.editHistory.empty())
		{
			if (ImGui::Button("Undo"))
				UndoLastEditCommand();
		}
		else
		{
			ImGui::TextDisabled("Undo");
		}
		ImGui::SameLine();
		if (!g_state.editRedoHistory.empty())
		{
			if (ImGui::Button("Redo"))
				RedoLastEditCommand();
		}
		else
		{
			ImGui::TextDisabled("Redo");
		}

		ImGui::SameLine();
		if (SelectedCell() != nullptr)
		{
			if (ImGui::Button("Copy Rect"))
				CopySelectedRect();
			ImGui::SameLine();
			if (g_state.rectClipboardValid)
			{
				if (ImGui::Button("Paste Rect"))
					PasteRectToSelected();
			}
			else
			{
				ImGui::TextDisabled("Paste Rect");
			}
		}
		else
		{
			ImGui::TextDisabled("Copy Rect");
			ImGui::SameLine();
			ImGui::TextDisabled("Paste Rect");
		}

		ImGui::SameLine();
		ImGui::TextDisabled("  |  %s", UiEditorVersion_GetSemVer());

		ImGui::EndChild();
	}

	void DrawStatusBar()
	{
		ImGui::BeginChild("status_bar", ImVec2(0.0f, 24.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		const int activeCount = static_cast<int>(g_state.layout.cells.size());
		const int baseCount = static_cast<int>(g_state.baseLayout.cells.size());
		const int tabCount = static_cast<int>(g_state.canvasTabs.size());

		ImGui::Text("Status: %s", g_state.layout.statusMessage.empty() ? "Ready" : g_state.layout.statusMessage.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("| Cells: %d", activeCount);
		if (g_state.hasBaseLayout)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("+ %d locked base", baseCount);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("| Tabs: %d", tabCount);
		ImGui::SameLine();
		ImGui::TextDisabled("| Zoom %.2fx", g_state.zoom);
		ImGui::SameLine();
		ImGui::TextDisabled("| Canvas %dx%d", g_state.canvasWidth, g_state.canvasHeight);
		ImGui::SameLine();
		ImGui::TextDisabled("| Cmds %d/%d", static_cast<int>(g_state.editHistory.size()), static_cast<int>(g_state.editRedoHistory.size()));
		if (!g_state.marqueeSelectedCells.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("| Selected %d", static_cast<int>(g_state.marqueeSelectedCells.size()));
		}
		if (g_state.rectClipboardValid)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("| Rect clipboard %.0f,%.0f %.0fx%.0f", g_state.rectClipboardX, g_state.rectClipboardY, g_state.rectClipboardWidth, g_state.rectClipboardHeight);
		}
		if (g_state.dirty)
		{
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, AccentOrange());
			ImGui::TextUnformatted("| modified");
			ImGui::PopStyleColor();
		}

		ImGui::EndChild();
	}

	void SetLoadPath(const char* path)
	{
		if (path == nullptr)
			return;

		std::snprintf(g_loadPath, sizeof(g_loadPath), "%s", path);
	}

	std::string ToLowerCopy(const std::string& value);
	std::string FileNameFromPath(const std::string& value);
	std::string DirectoryFromPath(const std::string& value);
	void RefreshCompositeBase();
	void UpdateSavePathFromLayout();
	bool FileExists(const char* path);
	bool LoadLayoutPathNow(const char* path);
	bool RequestLoadLayoutPath(const char* path);
	bool PerformSaveLayoutCopy();
	bool SaveLayoutCopy();
	bool CreateNewUiPageFromDialog();
	void RefreshUiFitBrowser(bool force);
	bool IsTextRenderableCell(const UiEditorFitCell& cell);
	void SyncActiveCanvasTabFromState();
	void AddOrUpdateActiveCanvasTabFromState(const std::string& loadPath);
	void ActivateCanvasTab(int index);

	std::string SanitizePageKeyForPath(const std::string& value)
	{
		std::string result;
		result.reserve(value.size());

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const char c = value[i];
			if ((c >= 'a' && c <= 'z')
				|| (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9')
				|| c == '_'
				|| c == '-')
			{
				result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			else if (c == '.' || c == ' ' || c == '/')
			{
				result.push_back('_');
			}
		}

		if (result.empty())
			result = "new_page";

		return result;
	}

	void UpdateNewPageOutputPath()
	{
		const int surfaceCount = static_cast<int>(sizeof(kNewPageSurfaces) / sizeof(kNewPageSurfaces[0]));
		const int surfaceIndex = (g_newPageSurfaceIndex >= 0 && g_newPageSurfaceIndex < surfaceCount) ? g_newPageSurfaceIndex : 0;
		const std::string safeName = SanitizePageKeyForPath(g_newPageKey);
		std::snprintf(
			g_newPageOutputPath,
			sizeof(g_newPageOutputPath),
			"data/defs/ui/%s/%s.fit",
			kNewPageSurfaces[surfaceIndex],
			safeName.c_str());
	}

	bool CreateNewUiPageFromDialog()
	{
		const std::string pageKey = g_newPageKey;
		const std::string pageTitle = g_newPageTitle;
		const std::string outputPath = g_newPageOutputPath;

		if (pageKey.empty() || outputPath.empty())
		{
			g_state.saveStatus = "New UI Page failed: page key and output path are required.";
			g_state.showSaveResultPopup = true;
			ImGui::OpenPopup("Save Copy Result");
			return false;
		}

		if (FileExists(outputPath.c_str()))
		{
			g_state.saveStatus = std::string("New UI Page refused: output already exists: ") + outputPath;
			g_state.showSaveResultPopup = true;
			ImGui::OpenPopup("Save Copy Result");
			return false;
		}

		std::error_code ec;
		const std::filesystem::path outFsPath(outputPath);
		const std::filesystem::path parentPath = outFsPath.parent_path();
		if (!parentPath.empty())
			std::filesystem::create_directories(parentPath, ec);

		const UiEditorResolutionProfile& profile = ResolutionProfileAt(g_newPageProfileIndex);
		const int width = profile.width > 0 ? profile.width : 1920;
		const int height = profile.height > 0 ? profile.height : 1080;

		std::ofstream out(outputPath.c_str(), std::ios::out | std::ios::trunc);
		if (!out)
		{
			g_state.saveStatus = std::string("New UI Page failed: could not write ") + outputPath;
			g_state.showSaveResultPopup = true;
			ImGui::OpenPopup("Save Copy Result");
			return false;
		}

		out << "// MC2R UI Editor generated page shell\n";
		out << "// Safe starter page: no runtime hookup implied.\n\n";
		out << "GuiPage {\n";
		out << "    key = \"" << pageKey << "\"\n";
		out << "    title = \"" << pageTitle << "\"\n";
		out << "    rect = 0,0," << width << "," << height << "\n";
		out << "    coordinateSpace = \"screen_" << width << "x" << height << "\"\n";
		out << "    sourceProfile = \"" << profile.label << "\"\n";
		out << "}\n";

		g_state.saveStatus = std::string("New UI Page created: ") + outputPath;
		g_state.showSaveResultPopup = true;
		ImGui::OpenPopup("Save Copy Result");
		RefreshUiFitBrowser(true);

		if (g_newPageOpenAfterCreate)
			RequestLoadLayoutPath(outputPath.c_str());

		return true;
	}


	bool TryLoadFitLayout(const std::string& path)
	{
		UiEditorFitLayout loadedLayout;
		if (!UiEditorFitLoadLayout(path.c_str(), loadedLayout))
		{
			g_state.layout = loadedLayout;
			g_state.baseLayout = UiEditorFitLayout();
			g_state.hasBaseLayout = false;
			g_state.selectedCell = -1;
			g_state.activeCell = -1;
			g_state.activeTransform = TransformNone;
			g_state.dirty = false;
			return false;
		}

		g_state.layout = loadedLayout;
		if (g_state.layout.overlaysGameOS)
		{
			if (!g_state.layout.statusMessage.empty())
				g_state.layout.statusMessage += " ";
			g_state.layout.statusMessage += "[GameOS overlay]";
		}
		g_state.canvasWidth = loadedLayout.designWidth;
		g_state.canvasHeight = loadedLayout.designHeight;
		g_state.lastCanvasWidth = g_state.canvasWidth;
		g_state.lastCanvasHeight = g_state.canvasHeight;
		g_state.editHistory.clear();
		g_state.editRedoHistory.clear();
		g_state.selectedCell = loadedLayout.cells.empty() ? -1 : 0;
		g_state.activeCell = -1;
		g_state.activeTransform = TransformNone;
		g_state.dirty = false;
		RefreshCompositeBase();
		UpdateSavePathFromLayout();
		return true;
	}

	bool LoadLayoutPathNow(const char* path)
	{
		if (path == nullptr || path[0] == '\0')
			return false;

		std::vector<std::string> candidates;
		const std::string requestedPath(path);

		candidates.push_back(requestedPath);
		candidates.push_back(std::string("../") + requestedPath);
		candidates.push_back(std::string("../../") + requestedPath);
		candidates.push_back(std::string("../../../") + requestedPath);

		char* basePath = SDL_GetBasePath();
		if (basePath != nullptr)
		{
			const std::string base(basePath);
			candidates.push_back(base + requestedPath);
			candidates.push_back(base + "../" + requestedPath);
			candidates.push_back(base + "../../" + requestedPath);
			SDL_free(basePath);
		}

		for (std::size_t i = 0; i < candidates.size(); ++i)
		{
			if (TryLoadFitLayout(candidates[i]))
				return true;
		}

		return false;
	}

	bool RequestLoadLayoutPath(const char* path)
	{
		if (path == nullptr || path[0] == '\0')
			return false;

		const std::string requestedPath(path);

		if (g_state.dirty)
		{
			g_state.pendingLoadPath = requestedPath;
			g_state.openDiscardEditsDialog = true;
			return false;
		}

		SyncActiveCanvasTabFromState();
		SetLoadPath(requestedPath.c_str());
		const bool loaded = LoadLayoutPathNow(g_loadPath);
		if (loaded)
			AddOrUpdateActiveCanvasTabFromState(requestedPath);
		return loaded;
	}

	bool LoadDefaultLayout()
	{
		SetLoadPath("data/defs/ui/packages/legacy_imgui/mechlopedia/main.fit");
		const bool loaded = LoadLayoutPathNow(g_loadPath);
		if (loaded)
			AddOrUpdateActiveCanvasTabFromState(g_loadPath);
		return loaded;
	}

	bool LoadMechlopediaFile(const char* filename)
	{
		char path[512];
		std::snprintf(path, sizeof(path), "data/defs/ui/packages/legacy_imgui/mechlopedia/%s", filename);
		return RequestLoadLayoutPath(path);
	}

	void SyncActiveCanvasTabFromState()
	{
		if (g_state.activeCanvasTab < 0 || g_state.activeCanvasTab >= static_cast<int>(g_state.canvasTabs.size()))
			return;

		UiEditorCanvasTab& tab = g_state.canvasTabs[static_cast<std::size_t>(g_state.activeCanvasTab)];
		tab.layout = g_state.layout;
		tab.baseLayout = g_state.baseLayout;
		tab.hasBaseLayout = g_state.hasBaseLayout;
		tab.dirty = g_state.dirty;
		tab.selectedCell = g_state.selectedCell;
		tab.zoom = g_state.zoom;
		tab.canvasWidth = g_state.canvasWidth;
		tab.canvasHeight = g_state.canvasHeight;
		tab.verticalGuides = g_state.verticalGuides;
		tab.horizontalGuides = g_state.horizontalGuides;
		tab.editHistory = g_state.editHistory;
		tab.editRedoHistory = g_state.editRedoHistory;
		tab.title = g_state.layout.loaded ? FileNameFromPath(g_state.layout.sourcePath) : tab.title;
	}

	void AddOrUpdateActiveCanvasTabFromState(const std::string& loadPath)
	{
		for (int i = 0; i < static_cast<int>(g_state.canvasTabs.size()); ++i)
		{
			if (g_state.canvasTabs[static_cast<std::size_t>(i)].loadPath == loadPath)
			{
				g_state.activeCanvasTab = i;
				SyncActiveCanvasTabFromState();
				return;
			}
		}

		UiEditorCanvasTab tab;
		tab.loadPath = loadPath;
		tab.title = g_state.layout.loaded ? FileNameFromPath(g_state.layout.sourcePath) : FileNameFromPath(loadPath);
		tab.layout = g_state.layout;
		tab.baseLayout = g_state.baseLayout;
		tab.hasBaseLayout = g_state.hasBaseLayout;
		tab.dirty = g_state.dirty;
		tab.selectedCell = g_state.selectedCell;
		tab.zoom = g_state.zoom;
		tab.canvasWidth = g_state.canvasWidth;
		tab.canvasHeight = g_state.canvasHeight;
		tab.verticalGuides = g_state.verticalGuides;
		tab.horizontalGuides = g_state.horizontalGuides;
		tab.editHistory = g_state.editHistory;
		tab.editRedoHistory = g_state.editRedoHistory;
		g_state.canvasTabs.push_back(tab);
		g_state.activeCanvasTab = static_cast<int>(g_state.canvasTabs.size()) - 1;
	}

	void ActivateCanvasTab(int index)
	{
		if (index < 0 || index >= static_cast<int>(g_state.canvasTabs.size()))
			return;

		SyncActiveCanvasTabFromState();

		g_state.activeCanvasTab = index;
		const UiEditorCanvasTab& tab = g_state.canvasTabs[static_cast<std::size_t>(index)];
		g_state.layout = tab.layout;
		g_state.baseLayout = tab.baseLayout;
		g_state.hasBaseLayout = tab.hasBaseLayout;
		g_state.dirty = tab.dirty;
		g_state.selectedCell = tab.selectedCell;
		g_state.activeCell = -1;
		g_state.activeTransform = TransformNone;
		g_state.zoom = tab.zoom;
		g_state.canvasWidth = tab.canvasWidth;
		g_state.canvasHeight = tab.canvasHeight;
		g_state.verticalGuides = tab.verticalGuides;
		g_state.horizontalGuides = tab.horizontalGuides;
		g_state.editHistory = tab.editHistory;
		g_state.editRedoHistory = tab.editRedoHistory;

		if (!tab.loadPath.empty())
			SetLoadPath(tab.loadPath.c_str());

		UpdateSavePathFromLayout();
	}

	void CloseCanvasTab(int index)
	{
		if (index < 0 || index >= static_cast<int>(g_state.canvasTabs.size()))
			return;

		const int nextIndex = std::max(0, std::min(index, static_cast<int>(g_state.canvasTabs.size()) - 2));
		g_state.canvasTabs.erase(g_state.canvasTabs.begin() + index);

		if (g_state.canvasTabs.empty())
		{
			g_state.activeCanvasTab = -1;
			return;
		}

		g_state.activeCanvasTab = -1;
		ActivateCanvasTab(nextIndex);
	}

	const UiEditorFitCell* SelectedCell()
	{
		return UiEditorFitGetCell(g_state.layout, g_state.selectedCell);
	}

	UiEditorFitCell* SelectedCellMutable()
	{
		if (g_state.selectedCell < 0 || g_state.selectedCell >= static_cast<int>(g_state.layout.cells.size()))
			return nullptr;

		return &g_state.layout.cells[static_cast<std::size_t>(g_state.selectedCell)];
	}

	float SnapGridValue(float value)
	{
		if (!g_state.snapToGrid)
			return value;

		const float grid = 20.0f;
		return std::round(value / grid) * grid;
	}

	float SnapGuideValue(float value, const std::vector<float>& guides)
	{
		if (!g_state.snapToGuides || guides.empty())
			return value;

		const float threshold = 6.0f;
		float best = value;
		float bestDistance = threshold;

		for (float guide : guides)
		{
			const float distance = std::fabs(value - guide);
			if (distance <= bestDistance)
			{
				best = guide;
				bestDistance = distance;
			}
		}

		return best;
	}

	float SnapDesignValue(float value)
	{
		return SnapGridValue(value);
	}

	float SnapDesignX(float value)
	{
		const float guided = SnapGuideValue(value, g_state.verticalGuides);
		return guided != value ? guided : SnapGridValue(value);
	}

	float SnapDesignY(float value)
	{
		const float guided = SnapGuideValue(value, g_state.horizontalGuides);
		return guided != value ? guided : SnapGridValue(value);
	}

	float ScreenToDesignX(float screenX, const ImVec2& artMin, float scale)
	{
		const float safeScale = scale > 0.001f ? scale : 1.0f;
		return (screenX - artMin.x) / safeScale;
	}

	float ScreenToDesignY(float screenY, const ImVec2& artMin, float scale)
	{
		const float safeScale = scale > 0.001f ? scale : 1.0f;
		return (screenY - artMin.y) / safeScale;
	}

	void RemoveGuide(std::vector<float>& guides, int index)
	{
		if (index < 0 || index >= static_cast<int>(guides.size()))
			return;

		guides.erase(guides.begin() + index);
	}

	// Additive from theirs (mc2r fork): persist a handful of editor preferences
	// (panel sizes, view toggles, docking mode, last load/save paths) across
	// sessions in a plain-text ui_editor.cfg beside the executable.
	const char* UiEditorConfigPath()
	{
		return "ui_editor.cfg";
	}

	void SaveEditorConfig()
	{
		std::ofstream output(UiEditorConfigPath(), std::ios::trunc);
		if (!output.is_open())
			return;

		output << "loadPath=" << g_loadPath << "\n";
		output << "savePath=" << g_savePath << "\n";
		output << "showGrid=" << (g_state.showGrid ? 1 : 0) << "\n";
		output << "snapToGrid=" << (g_state.snapToGrid ? 1 : 0) << "\n";
		output << "showGuides=" << (g_state.showGuides ? 1 : 0) << "\n";
		output << "snapToGuides=" << (g_state.snapToGuides ? 1 : 0) << "\n";
		output << "showImages=" << (g_state.showImages ? 1 : 0) << "\n";
		output << "showLabels=" << (g_state.showLabels ? 1 : 0) << "\n";
		output << "showCanvasHud=" << (g_state.showCanvasHud ? 1 : 0) << "\n";
		output << "useDockingLayout=" << (g_state.useDockingLayout ? 1 : 0) << "\n";
		output << "leftPanelWidth=" << g_state.leftPanelWidth << "\n";
		output << "rightPanelWidth=" << g_state.rightPanelWidth << "\n";
		output << "leftLayoutsHeight=" << g_state.leftLayoutsHeight << "\n";
		output << "rightHierarchyHeight=" << g_state.rightHierarchyHeight << "\n";
		output << "zoom=" << g_state.zoom << "\n";
	}

	void LoadEditorConfig()
	{
		std::ifstream input(UiEditorConfigPath());
		if (!input.is_open())
			return;

		std::string line;
		while (std::getline(input, line))
		{
			const std::string::size_type equals = line.find('=');
			if (equals == std::string::npos)
				continue;

			const std::string key = line.substr(0, equals);
			const std::string value = line.substr(equals + 1);
			const bool boolValue = value == "1" || value == "true" || value == "TRUE";

			if (key == "loadPath")
				std::snprintf(g_loadPath, sizeof(g_loadPath), "%s", value.c_str());
			else if (key == "savePath")
				std::snprintf(g_savePath, sizeof(g_savePath), "%s", value.c_str());
			else if (key == "showGrid")
				g_state.showGrid = boolValue;
			else if (key == "snapToGrid")
				g_state.snapToGrid = boolValue;
			else if (key == "showGuides")
				g_state.showGuides = boolValue;
			else if (key == "snapToGuides")
				g_state.snapToGuides = boolValue;
			else if (key == "showImages")
				g_state.showImages = boolValue;
			else if (key == "showLabels")
				g_state.showLabels = boolValue;
			else if (key == "showCanvasHud")
				g_state.showCanvasHud = boolValue;
			else if (key == "useDockingLayout")
				g_state.useDockingLayout = boolValue;
			else if (key == "leftPanelWidth")
				g_state.leftPanelWidth = std::max(180.0f, std::strtof(value.c_str(), nullptr));
			else if (key == "rightPanelWidth")
				g_state.rightPanelWidth = std::max(220.0f, std::strtof(value.c_str(), nullptr));
			else if (key == "leftLayoutsHeight")
				g_state.leftLayoutsHeight = std::max(120.0f, std::strtof(value.c_str(), nullptr));
			else if (key == "rightHierarchyHeight")
				g_state.rightHierarchyHeight = std::max(120.0f, std::strtof(value.c_str(), nullptr));
			else if (key == "zoom")
				g_state.zoom = std::max(0.1f, std::strtof(value.c_str(), nullptr));
		}
	}

	void MarkDirty()
	{
		g_state.dirty = true;
	}

	std::string MakeGeneratedWidgetKey(UiEditorSmartTool tool)
	{
		++g_state.generatedWidgetCounter;
		char buffer[128];
		std::snprintf(
			buffer,
			sizeof(buffer),
			"ui_editor.generated.%s_%03d",
			SmartToolTypeName(tool),
			g_state.generatedWidgetCounter);
		return ToLowerCopy(buffer);
	}

	UiEditorFitCell BuildSmartWidgetCell(UiEditorSmartTool tool, float x, float y, float width, float height)
	{
		UiEditorFitCell cell;
		cell.key = MakeGeneratedWidgetKey(tool);
		cell.type = SmartToolTypeName(tool);
		cell.pageKey = g_state.layout.key;
		cell.role = "smart_tool";
		cell.layer = "smart_tools";
		cell.rectRaw.clear();
		cell.rectFieldName = "rect";
		cell.x = SnapDesignX(x);
		cell.y = SnapDesignY(y);
		cell.width = SnapDesignValue(std::max(width, 8.0f));
		cell.height = SnapDesignValue(std::max(height, 8.0f));
		cell.hasUvPixels = false;
		cell.uvX = 0.0f;
		cell.uvY = 0.0f;
		cell.uvWidth = 0.0f;
		cell.uvHeight = 0.0f;
		cell.visible = true;
		cell.locked = false;
		cell.actionType = "none";
		cell.legacyMessage.clear();
		cell.targetPageKey.clear();
		cell.targetMissionKey.clear();
		cell.transitionName.clear();
		cell.toggleButton = false;
		cell.checked = false;
		cell.selectedState = false;
		cell.disabledState = false;
		cell.pulseEnabled = false;
		cell.pulseSpeed = 1.5f;
		cell.aliasOverride = SmartToolLabel(tool);
		if (tool == SmartToolColorBlock)
		{
			cell.fillColor[0] = 0.18f; cell.fillColor[1] = 0.20f; cell.fillColor[2] = 0.24f; cell.fillColor[3] = 1.0f;
			cell.borderColor[0] = 0.55f; cell.borderColor[1] = 0.62f; cell.borderColor[2] = 0.70f; cell.borderColor[3] = 1.0f;
			cell.borderWidth = 1.0f;
			cell.opacity = 1.0f;
			cell.actionType = "none";
		}
		else if (tool == SmartToolPanel)
		{
			cell.fillColor[0] = 0.08f; cell.fillColor[1] = 0.10f; cell.fillColor[2] = 0.13f; cell.fillColor[3] = 0.92f;
			cell.borderColor[0] = 0.32f; cell.borderColor[1] = 0.38f; cell.borderColor[2] = 0.46f; cell.borderColor[3] = 1.0f;
			cell.borderWidth = 1.0f;
			cell.opacity = 1.0f;
		}
		else if (tool == SmartToolButton)
		{
			cell.textKey = "ui.new_button";
			cell.visibleText = "Button";
			cell.textAlign = "center";
			cell.textAnchor = "center";
			cell.wrapText = false;
			cell.fillColor[0] = 0.14f; cell.fillColor[1] = 0.20f; cell.fillColor[2] = 0.28f; cell.fillColor[3] = 1.0f;
			cell.borderColor[0] = 0.40f; cell.borderColor[1] = 0.67f; cell.borderColor[2] = 0.88f; cell.borderColor[3] = 1.0f;
			cell.borderWidth = 1.0f;
			cell.opacity = 1.0f;
			cell.actionType = "none";
			cell.legacyMessage.clear();
			cell.toggleButton = false;
			cell.pulseEnabled = false;
		}
		else if (tool == SmartToolText)
		{
			cell.textKey = "ui.new_text";
			cell.visibleText = "Text";
			cell.textAlign = "left";
			cell.textAnchor = "upper_left";
			cell.wrapText = true;
			cell.fillColor[0] = 0.0f; cell.fillColor[1] = 0.0f; cell.fillColor[2] = 0.0f; cell.fillColor[3] = 0.0f;
			cell.borderColor[0] = 0.0f; cell.borderColor[1] = 0.0f; cell.borderColor[2] = 0.0f; cell.borderColor[3] = 0.0f;
			cell.opacity = 1.0f;
		}
		else
		{
			cell.textKey.clear();
		}
		if (tool == SmartToolImage)
			cell.texture = "";
		if (tool == SmartToolList)
		{
			cell.fillColor[0] = 0.10f; cell.fillColor[1] = 0.11f; cell.fillColor[2] = 0.13f; cell.fillColor[3] = 0.88f;
			cell.borderColor[0] = 0.22f; cell.borderColor[1] = 0.32f; cell.borderColor[2] = 0.42f; cell.borderColor[3] = 1.0f;
			cell.borderWidth = 1.0f;
			cell.opacity = 1.0f;
			cell.isListCell = true;
			cell.itemHeight = 0.0f;
			cell.itemOffsetX = 2.0f;
			cell.itemOffsetY = 0.0f;
			cell.highlightColor[0] = 0.95f;
			cell.highlightColor[1] = 0.55f;
			cell.highlightColor[2] = 0.12f;
			cell.highlightColor[3] = 0.50f;
			cell.listPreviewItemCount = 3;
			cell.font = "Agency Regular";
			cell.fontSize = 14.0f;
			cell.textAlign = "left";
			cell.textAnchor = "upper_left";
			cell.role = "list";
			cell.layer = "control";
		}
		return cell;
	}

	int AddSmartWidget(UiEditorSmartTool tool, float x, float y, float width, float height)
	{
		if (!g_state.layout.loaded)
		{
			g_state.layout.loaded = true;
			g_state.layout.key = "ui.untitled";
			g_state.layout.title = "Untitled UI Page";
			g_state.layout.designWidth = g_state.canvasWidth;
			g_state.layout.designHeight = g_state.canvasHeight;
			g_state.layout.statusMessage = "Untitled page created in memory.";
		}

		UiEditorFitCell cell = BuildSmartWidgetCell(tool, x, y, width, height);
		ClampCellRect(cell);
		g_state.layout.cells.push_back(cell);
		const int index = static_cast<int>(g_state.layout.cells.size()) - 1;
		g_state.selectedCell = index;
		RecordCreateEditHistory(index);
		MarkDirty();
		return index;
	}

	ImVec2 ScreenToDesignPoint(const ImVec2& artMin, float scale, const ImVec2& screenPoint)
	{
		const float safeScale = scale > 0.001f ? scale : 1.0f;
		return ImVec2(
			(screenPoint.x - artMin.x) / safeScale,
			(screenPoint.y - artMin.y) / safeScale);
	}

	void HandleSmartToolCreation(ImDrawList* drawList, const ImVec2& artMin, const ImVec2& artMax, float scale)
	{
		if (g_state.activeSmartTool == SmartToolSelect)
			return;

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool hoveringArt = ImGui::IsMouseHoveringRect(artMin, artMax, true);

		if (!g_state.creatingSmartWidget && hoveringArt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			g_state.creatingSmartWidget = true;
			g_state.smartCreateStart = ScreenToDesignPoint(artMin, scale, mouse);
			g_state.smartCreateCurrent = g_state.smartCreateStart;
		}

		if (!g_state.creatingSmartWidget)
			return;

		g_state.smartCreateCurrent = ScreenToDesignPoint(artMin, scale, mouse);

		const float x0 = std::min(g_state.smartCreateStart.x, g_state.smartCreateCurrent.x);
		const float y0 = std::min(g_state.smartCreateStart.y, g_state.smartCreateCurrent.y);
		const float x1 = std::max(g_state.smartCreateStart.x, g_state.smartCreateCurrent.x);
		const float y1 = std::max(g_state.smartCreateStart.y, g_state.smartCreateCurrent.y);

		const ImVec2 previewMin(artMin.x + x0 * scale, artMin.y + y0 * scale);
		const ImVec2 previewMax(artMin.x + x1 * scale, artMin.y + y1 * scale);
		drawList->AddRectFilled(previewMin, previewMax, IM_COL32(70, 180, 230, 45));
		drawList->AddRect(previewMin, previewMax, ColorU32(AccentCyan()), 0.0f, 0, 2.0f);
		drawList->AddText(ImVec2(previewMin.x + 6.0f, previewMin.y + 5.0f), ColorU32(AccentCyan()), SmartToolLabel(g_state.activeSmartTool));

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const float width = std::max(x1 - x0, 32.0f);
			const float height = std::max(y1 - y0, 24.0f);
			AddSmartWidget(g_state.activeSmartTool, x0, y0, width, height);
			g_state.creatingSmartWidget = false;
			g_state.activeSmartTool = SmartToolSelect;
		}
	}

	void ClampCellRect(UiEditorFitCell& cell)
	{
		cell.width = std::max(cell.width, 4.0f);
		cell.height = std::max(cell.height, 4.0f);
	}

	void ClampTextRect(UiEditorFitCell& cell)
	{
		cell.textWidth = std::max(cell.textWidth, 1.0f);
		cell.textHeight = std::max(cell.textHeight, 1.0f);
	}

	bool IsTextRectTransform(UiEditorTransformMode mode)
	{
		return mode == TransformTextMove
			|| mode == TransformTextResizeNW
			|| mode == TransformTextResizeNE
			|| mode == TransformTextResizeSW
			|| mode == TransformTextResizeSE;
	}


	bool IsUndefinedLegacyStringKey(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);
		return lower.find("ids_undefined_string") != std::string::npos
			|| lower.find("legacy.strings.ids_undefined") != std::string::npos;
	}

	bool IsRuntimeHelpTextCell(const UiEditorFitCell& cell)
	{
		return ToLowerCopy(cell.role) == "help"
			|| ToLowerCopy(cell.layer) == "help";
	}

	bool IsMarqueeSelectedCell(int cellIndex)
	{
		for (std::size_t i = 0; i < g_state.marqueeSelectedCells.size(); ++i)
		{
			if (g_state.marqueeSelectedCells[i] == cellIndex)
				return true;
		}
		return false;
	}

	bool IsCellSelectedForPreview(int cellIndex)
	{
		return cellIndex >= 0 && (cellIndex == g_state.selectedCell || IsMarqueeSelectedCell(cellIndex));
	}

	void ClearMarqueeSelection()
	{
		g_state.marqueeSelectedCells.clear();
		g_state.selectionMarqueeActive = false;
	}

	void SetSingleSelectedCell(int cellIndex)
	{
		g_state.selectedCell = cellIndex;
		g_state.marqueeSelectedCells.clear();
	}

	bool ContainsCellIndex(const std::vector<int>& values, int cellIndex)
	{
		for (std::size_t i = 0; i < values.size(); ++i)
		{
			if (values[i] == cellIndex)
				return true;
		}
		return false;
	}

	std::vector<int> SelectedEditableCellIndices()
	{
		std::vector<int> indices;

		if (g_state.selectedCell >= 0)
			indices.push_back(g_state.selectedCell);

		for (std::size_t i = 0; i < g_state.marqueeSelectedCells.size(); ++i)
		{
			const int cellIndex = g_state.marqueeSelectedCells[i];
			if (!ContainsCellIndex(indices, cellIndex))
				indices.push_back(cellIndex);
		}

		std::vector<int> editable;
		for (std::size_t i = 0; i < indices.size(); ++i)
		{
			const int cellIndex = indices[i];
			if (cellIndex < 0 || cellIndex >= static_cast<int>(g_state.layout.cells.size()))
				continue;

			const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(cellIndex)];
			if (cell.visible && !cell.locked)
				editable.push_back(cellIndex);
		}

		return editable;
	}

	UiEditorRectSnapshot SnapshotCellRect(int cellIndex, const UiEditorFitCell& cell)
	{
		UiEditorRectSnapshot snapshot;
		snapshot.cellIndex = cellIndex;
		snapshot.x = cell.x;
		snapshot.y = cell.y;
		snapshot.width = cell.width;
		snapshot.height = cell.height;
		return snapshot;
	}

	bool CopySelectedRect()
	{
		const UiEditorFitCell* cell = SelectedCell();
		if (cell == nullptr)
			return false;

		g_state.rectClipboardX = cell->x;
		g_state.rectClipboardY = cell->y;
		g_state.rectClipboardWidth = cell->width;
		g_state.rectClipboardHeight = cell->height;
		g_state.rectClipboardValid = true;
		g_state.layout.statusMessage = "Copied selected rect.";
		return true;
	}

	bool PasteRectToSelected()
	{
		UiEditorFitCell* cell = SelectedCellMutable();
		if (cell == nullptr || !g_state.rectClipboardValid || cell->locked)
			return false;

		const float beforeX = cell->x;
		const float beforeY = cell->y;
		const float beforeW = cell->width;
		const float beforeH = cell->height;

		cell->x = SnapDesignX(g_state.rectClipboardX);
		cell->y = SnapDesignY(g_state.rectClipboardY);
		cell->width = SnapDesignValue(std::max(g_state.rectClipboardWidth, 4.0f));
		cell->height = SnapDesignValue(std::max(g_state.rectClipboardHeight, 4.0f));
		ClampCellRect(*cell);

		if (RectChanged(beforeX, beforeY, beforeW, beforeH, cell->x, cell->y, cell->width, cell->height))
		{
			RecordEditHistory("Paste rect", g_state.selectedCell, beforeX, beforeY, beforeW, beforeH, cell->x, cell->y, cell->width, cell->height);
			MarkDirty();
		}

		g_state.layout.statusMessage = "Pasted rect onto selected cell.";
		return true;
	}

	bool CutSelectedRect()
	{
		UiEditorFitCell* cell = SelectedCellMutable();
		if (cell == nullptr || cell->locked)
			return false;

		if (!CopySelectedRect())
			return false;

		const float beforeX = cell->x;
		const float beforeY = cell->y;
		const float beforeW = cell->width;
		const float beforeH = cell->height;
		cell->visible = false;
		RecordEditHistory("Cut rect / hide cell", g_state.selectedCell, beforeX, beforeY, beforeW, beforeH, beforeX, beforeY, beforeW, beforeH);
		MarkDirty();
		g_state.layout.statusMessage = "Cut selected rect into clipboard and hid the source cell.";
		return true;
	}

	bool RectsIntersect(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
	{
		return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
	}

	void SelectCellsInDesignRect(float x, float y, float width, float height)
	{
		g_state.marqueeSelectedCells.clear();
		g_state.selectedCell = -1;

		const float left = std::min(x, x + width);
		const float top = std::min(y, y + height);
		const float right = std::max(x, x + width);
		const float bottom = std::max(y, y + height);

		for (int i = 0; i < static_cast<int>(g_state.layout.cells.size()); ++i)
		{
			const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(i)];
			if (!cell.visible || cell.locked)
				continue;

			if (RectsIntersect(left, top, right - left, bottom - top, cell.x, cell.y, cell.width, cell.height))
			{
				g_state.marqueeSelectedCells.push_back(i);
				g_state.selectedCell = i;
			}
		}

		if (g_state.marqueeSelectedCells.empty())
			g_state.layout.statusMessage = "Marquee selected no rects.";
		else
			g_state.layout.statusMessage = "Marquee selected " + std::to_string(g_state.marqueeSelectedCells.size()) + " rects.";
	}


	void BeginTransform(int cellIndex, UiEditorTransformMode mode)
	{
		if (cellIndex < 0 || cellIndex >= static_cast<int>(g_state.layout.cells.size()))
			return;

		const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(cellIndex)];
		if (!cell.visible || cell.locked)
			return;

		if (!IsCellSelectedForPreview(cellIndex))
			SetSingleSelectedCell(cellIndex);
		else
			g_state.selectedCell = cellIndex;

		g_state.groupDragStartRects.clear();
		if (mode == TransformMove)
		{
			const std::vector<int> selectedIndices = SelectedEditableCellIndices();
			if (selectedIndices.size() > 1)
			{
				for (std::size_t i = 0; i < selectedIndices.size(); ++i)
				{
					const int selectedIndex = selectedIndices[i];
					const UiEditorFitCell& selectedCell = g_state.layout.cells[static_cast<std::size_t>(selectedIndex)];
					g_state.groupDragStartRects.push_back(SnapshotCellRect(selectedIndex, selectedCell));
				}
			}
		}

		g_state.activeCell = cellIndex;
		g_state.activeTransform = mode;
		g_state.dragStartMouse = ImGui::GetIO().MousePos;
		g_state.dragStartX = cell.x;
		g_state.dragStartY = cell.y;
		g_state.dragStartWidth = cell.width;
		g_state.dragStartHeight = cell.height;
		g_state.dragStartTextX = cell.textX;
		g_state.dragStartTextY = cell.textY;
		g_state.dragStartTextWidth = cell.textWidth;
		g_state.dragStartTextHeight = cell.textHeight;
		g_state.dragStartArtX = cell.artX;
		g_state.dragStartArtY = cell.artY;
		g_state.dragStartArtWidth = cell.artWidth;
		g_state.dragStartArtHeight = cell.artHeight;
		g_state.dragStartBorderX = cell.borderX;
		g_state.dragStartBorderY = cell.borderY;
		g_state.dragStartBorderWidth = cell.borderWidthRect;
		g_state.dragStartBorderHeight = cell.borderHeightRect;
	}

	void BeginTextRectTransform(int cellIndex, UiEditorTransformMode mode)
	{
		if (cellIndex < 0 || cellIndex >= static_cast<int>(g_state.layout.cells.size()))
			return;

		UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(cellIndex)];
		if (!cell.visible || cell.locked || !cell.hasTextRect)
			return;

		g_state.selectedCell = cellIndex;
		g_state.activeCell = cellIndex;
		g_state.activeTransform = mode;
		g_state.groupDragStartRects.clear();
		g_state.dragStartMouse = ImGui::GetIO().MousePos;
		g_state.dragStartX = cell.x;
		g_state.dragStartY = cell.y;
		g_state.dragStartWidth = cell.width;
		g_state.dragStartHeight = cell.height;
		g_state.dragStartTextX = cell.textX;
		g_state.dragStartTextY = cell.textY;
		g_state.dragStartTextWidth = cell.textWidth;
		g_state.dragStartTextHeight = cell.textHeight;
	}

	void UpdateActiveTransform(float scale)
	{
		if (g_state.activeCell < 0 || g_state.activeTransform == TransformNone)
			return;

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (g_state.activeCell >= 0 && g_state.activeCell < static_cast<int>(g_state.layout.cells.size()))
			{
				const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(g_state.activeCell)];

				if (g_state.activeTransform == TransformMove && g_state.groupDragStartRects.size() > 1)
				{
					std::vector<UiEditorRectSnapshot> afterRects;
					for (std::size_t i = 0; i < g_state.groupDragStartRects.size(); ++i)
					{
						const int cellIndex = g_state.groupDragStartRects[i].cellIndex;
						if (cellIndex < 0 || cellIndex >= static_cast<int>(g_state.layout.cells.size()))
							continue;

						const UiEditorFitCell& movedCell = g_state.layout.cells[static_cast<std::size_t>(cellIndex)];
						const UiEditorRectSnapshot afterSnapshot = SnapshotCellRect(cellIndex, movedCell);
						if (RectChanged(
							g_state.groupDragStartRects[i].x,
							g_state.groupDragStartRects[i].y,
							g_state.groupDragStartRects[i].width,
							g_state.groupDragStartRects[i].height,
							afterSnapshot.x,
							afterSnapshot.y,
							afterSnapshot.width,
							afterSnapshot.height))
						{
							afterRects.push_back(afterSnapshot);
						}
					}

					if (!afterRects.empty())
						RecordGroupRectEditHistory("Move selected rects", g_state.groupDragStartRects, afterRects);
				}
				else if (IsTextRectTransform(g_state.activeTransform))
				{
					if (RectChanged(
						g_state.dragStartTextX,
						g_state.dragStartTextY,
						g_state.dragStartTextWidth,
						g_state.dragStartTextHeight,
						cell.textX,
						cell.textY,
						cell.textWidth,
						cell.textHeight))
					{
						RecordTextRectEditHistory(
							g_state.activeTransform == TransformTextMove ? "Move text rect" : "Resize text rect",
							g_state.activeCell,
							g_state.dragStartTextX,
							g_state.dragStartTextY,
							g_state.dragStartTextWidth,
							g_state.dragStartTextHeight,
							cell.textX,
							cell.textY,
							cell.textWidth,
							cell.textHeight);
					}
				}
				else if (RectChanged(g_state.dragStartX, g_state.dragStartY, g_state.dragStartWidth, g_state.dragStartHeight, cell.x, cell.y, cell.width, cell.height))
				{
					RecordEditHistory(
						g_state.activeTransform == TransformMove ? "Move cell" : "Resize cell",
						g_state.activeCell,
						g_state.dragStartX,
						g_state.dragStartY,
						g_state.dragStartWidth,
						g_state.dragStartHeight,
						cell.x,
						cell.y,
						cell.width,
						cell.height);
				}
			}

			g_state.groupDragStartRects.clear();
			g_state.activeCell = -1;
			g_state.activeTransform = TransformNone;
			return;
		}

		if (g_state.activeCell >= static_cast<int>(g_state.layout.cells.size()))
		{
			g_state.groupDragStartRects.clear();
			g_state.activeCell = -1;
			g_state.activeTransform = TransformNone;
			return;
		}

		UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(g_state.activeCell)];
		if (!cell.visible || cell.locked)
		{
			g_state.groupDragStartRects.clear();
			g_state.activeCell = -1;
			g_state.activeTransform = TransformNone;
			return;
		}

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const float safeScale = scale > 0.001f ? scale : 1.0f;
		const float dx = (mouse.x - g_state.dragStartMouse.x) / safeScale;
		const float dy = (mouse.y - g_state.dragStartMouse.y) / safeScale;

		if (g_state.activeTransform == TransformMove && g_state.groupDragStartRects.size() > 1)
		{
			for (std::size_t i = 0; i < g_state.groupDragStartRects.size(); ++i)
			{
				const UiEditorRectSnapshot& start = g_state.groupDragStartRects[i];
				if (start.cellIndex < 0 || start.cellIndex >= static_cast<int>(g_state.layout.cells.size()))
					continue;

				UiEditorFitCell& movedCell = g_state.layout.cells[static_cast<std::size_t>(start.cellIndex)];
				if (!movedCell.visible || movedCell.locked)
					continue;

				const float newX = SnapDesignX(start.x + dx);
				const float newY = SnapDesignY(start.y + dy);
				const float childDx = newX - movedCell.x;
				const float childDy = newY - movedCell.y;
				movedCell.x = newX;
				movedCell.y = newY;
				movedCell.width = start.width;
				movedCell.height = start.height;
				if (movedCell.hasTextRect)
				{
					movedCell.textX = SnapDesignX(movedCell.textX + childDx);
					movedCell.textY = SnapDesignY(movedCell.textY + childDy);
				}
				if (movedCell.hasArtRect)
				{
					movedCell.artX = SnapDesignX(movedCell.artX + childDx);
					movedCell.artY = SnapDesignY(movedCell.artY + childDy);
				}
				if (movedCell.hasBorderRect)
				{
					movedCell.borderX = SnapDesignX(movedCell.borderX + childDx);
					movedCell.borderY = SnapDesignY(movedCell.borderY + childDy);
				}
				ClampCellRect(movedCell);
			}
			MarkDirty();
			return;
		}

		if (IsTextRectTransform(g_state.activeTransform))
		{
			float x = g_state.dragStartTextX;
			float y = g_state.dragStartTextY;
			float w = g_state.dragStartTextWidth;
			float h = g_state.dragStartTextHeight;

			switch (g_state.activeTransform)
			{
			case TransformTextMove:
				x = g_state.dragStartTextX + dx;
				y = g_state.dragStartTextY + dy;
				break;

			case TransformTextResizeNW:
				x = g_state.dragStartTextX + dx;
				y = g_state.dragStartTextY + dy;
				w = g_state.dragStartTextWidth - dx;
				h = g_state.dragStartTextHeight - dy;
				break;

			case TransformTextResizeNE:
				y = g_state.dragStartTextY + dy;
				w = g_state.dragStartTextWidth + dx;
				h = g_state.dragStartTextHeight - dy;
				break;

			case TransformTextResizeSW:
				x = g_state.dragStartTextX + dx;
				w = g_state.dragStartTextWidth - dx;
				h = g_state.dragStartTextHeight + dy;
				break;

			case TransformTextResizeSE:
				w = g_state.dragStartTextWidth + dx;
				h = g_state.dragStartTextHeight + dy;
				break;

			default:
				break;
			}

			if (w < 1.0f)
			{
				if (g_state.activeTransform == TransformTextResizeNW || g_state.activeTransform == TransformTextResizeSW)
					x += w - 1.0f;
				w = 1.0f;
			}

			if (h < 1.0f)
			{
				if (g_state.activeTransform == TransformTextResizeNW || g_state.activeTransform == TransformTextResizeNE)
					y += h - 1.0f;
				h = 1.0f;
			}

			cell.textX = SnapDesignX(x);
			cell.textY = SnapDesignY(y);
			cell.textWidth = SnapDesignValue(std::max(w, 1.0f));
			cell.textHeight = SnapDesignValue(std::max(h, 1.0f));
			ClampTextRect(cell);
			MarkDirty();
			return;
		}

		float x = g_state.dragStartX;
		float y = g_state.dragStartY;
		float w = g_state.dragStartWidth;
		float h = g_state.dragStartHeight;

		switch (g_state.activeTransform)
		{
		case TransformMove:
			x = g_state.dragStartX + dx;
			y = g_state.dragStartY + dy;
			break;

		case TransformResizeNW:
			x = g_state.dragStartX + dx;
			y = g_state.dragStartY + dy;
			w = g_state.dragStartWidth - dx;
			h = g_state.dragStartHeight - dy;
			break;

		case TransformResizeNE:
			y = g_state.dragStartY + dy;
			w = g_state.dragStartWidth + dx;
			h = g_state.dragStartHeight - dy;
			break;

		case TransformResizeSW:
			x = g_state.dragStartX + dx;
			w = g_state.dragStartWidth - dx;
			h = g_state.dragStartHeight + dy;
			break;

		case TransformResizeSE:
			w = g_state.dragStartWidth + dx;
			h = g_state.dragStartHeight + dy;
			break;

		case TransformNone:
		default:
			break;
		}

		if (w < 4.0f)
		{
			if (g_state.activeTransform == TransformResizeNW || g_state.activeTransform == TransformResizeSW)
				x += w - 4.0f;

			w = 4.0f;
		}

		if (h < 4.0f)
		{
			if (g_state.activeTransform == TransformResizeNW || g_state.activeTransform == TransformResizeNE)
				y += h - 4.0f;

			h = 4.0f;
		}

		cell.x = SnapDesignX(x);
		cell.y = SnapDesignY(y);
		cell.width = SnapDesignValue(std::max(w, 4.0f));
		cell.height = SnapDesignValue(std::max(h, 4.0f));
		ClampCellRect(cell);
		if (IsButtonLikeCell(cell))
			ApplyButtonChildRectsFromParentTransform(cell);
		MarkDirty();
	}

	bool IsPointInRect(const ImVec2& point, const ImVec2& min, const ImVec2& max)
	{
		return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
	}

	int FindEditableCellAt(const ImVec2& artboard, float scale, const ImVec2& displayOffset, const ImVec2& mouse)
	{
		if (g_state.selectedCell >= 0 && g_state.selectedCell < static_cast<int>(g_state.layout.cells.size()))
		{
			const UiEditorFitCell& selected = g_state.layout.cells[static_cast<std::size_t>(g_state.selectedCell)];

			if (selected.visible && !selected.locked)
			{
				const float displayX = selected.x + displayOffset.x;
				const float displayY = selected.y + displayOffset.y;
				const ImVec2 min(artboard.x + displayX * scale, artboard.y + displayY * scale);
				const ImVec2 max(min.x + selected.width * scale, min.y + selected.height * scale);
				const float handle = 5.0f;

				if (selected.hasTextRect)
				{
					const ImVec2 textMin(
						artboard.x + (selected.textX + displayOffset.x) * scale,
						artboard.y + (selected.textY + displayOffset.y) * scale);
					const ImVec2 textMax(
						textMin.x + selected.textWidth * scale,
						textMin.y + selected.textHeight * scale);

					if (IsPointInRect(mouse, textMin, textMax)
						|| IsPointInRect(mouse, ImVec2(textMin.x - handle, textMin.y - handle), ImVec2(textMin.x + handle, textMin.y + handle))
						|| IsPointInRect(mouse, ImVec2(textMax.x - handle, textMin.y - handle), ImVec2(textMax.x + handle, textMin.y + handle))
						|| IsPointInRect(mouse, ImVec2(textMin.x - handle, textMax.y - handle), ImVec2(textMin.x + handle, textMax.y + handle))
						|| IsPointInRect(mouse, ImVec2(textMax.x - handle, textMax.y - handle), ImVec2(textMax.x + handle, textMax.y + handle)))
					{
						return g_state.selectedCell;
					}
				}

				if (IsPointInRect(mouse, ImVec2(min.x - handle, min.y - handle), ImVec2(min.x + handle, min.y + handle))
					|| IsPointInRect(mouse, ImVec2(max.x - handle, min.y - handle), ImVec2(max.x + handle, min.y + handle))
					|| IsPointInRect(mouse, ImVec2(min.x - handle, max.y - handle), ImVec2(min.x + handle, max.y + handle))
					|| IsPointInRect(mouse, ImVec2(max.x - handle, max.y - handle), ImVec2(max.x + handle, max.y + handle)))
				{
					return g_state.selectedCell;
				}
			}
		}

		// Prefer editable button text rectangles before generic art slices.
		// Legacy chrome often has decorative image strips layered over/around a
		// button, so smallest-rect hit testing can select the art strip instead
		// of the button whose text the user is trying to fix.
		int bestTextCell = -1;
		float bestTextArea = 340282346638528859811704183484516925440.0f;
		for (int i = 0; i < static_cast<int>(g_state.layout.cells.size()); ++i)
		{
			const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(i)];
			if (!cell.visible || cell.locked || !cell.hasTextRect || !IsTextRenderableCell(cell))
				continue;

			const ImVec2 textMin(
				artboard.x + (cell.textX + displayOffset.x) * scale,
				artboard.y + (cell.textY + displayOffset.y) * scale);
			const ImVec2 textMax(
				textMin.x + cell.textWidth * scale,
				textMin.y + cell.textHeight * scale);

			if (!IsPointInRect(mouse, textMin, textMax))
				continue;

			const float area = std::max(cell.textWidth, 1.0f) * std::max(cell.textHeight, 1.0f);
			if (area < bestTextArea)
			{
				bestTextArea = area;
				bestTextCell = i;
			}
		}

		if (bestTextCell >= 0)
			return bestTextCell;

		int bestCell = -1;
		float bestArea = 340282346638528859811704183484516925440.0f;

		for (int i = 0; i < static_cast<int>(g_state.layout.cells.size()); ++i)
		{
			const UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(i)];
			if (!cell.visible || cell.locked)
				continue;

			const float displayX = cell.x + displayOffset.x;
			const float displayY = cell.y + displayOffset.y;
			const ImVec2 min(artboard.x + displayX * scale, artboard.y + displayY * scale);
			const ImVec2 max(min.x + cell.width * scale, min.y + cell.height * scale);

			if (!IsPointInRect(mouse, min, max))
				continue;

			const float area = std::max(cell.width, 1.0f) * std::max(cell.height, 1.0f);
			if (area < bestArea)
			{
				bestArea = area;
				bestCell = i;
			}
		}

		return bestCell;
	}

	std::string ToLowerCopy(const std::string& value)
	{
		std::string result = value;

		for (std::size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));

		return result;
	}

	bool ContainsInsensitive(const std::string& value, const char* needle)
	{
		return ToLowerCopy(value).find(needle) != std::string::npos;
	}

	std::string FileNameFromPath(const std::string& value)
	{
		const std::string::size_type slash = value.find_last_of("/\\");
		if (slash == std::string::npos)
			return value;

		return value.substr(slash + 1);
	}

	std::string DirectoryFromPath(const std::string& value)
	{
		const std::string::size_type slash = value.find_last_of("/\\");
		if (slash == std::string::npos)
			return std::string();

		return value.substr(0, slash + 1);
	}


	std::string RemoveExtension(const std::string& filename)
	{
		const std::string::size_type dot = filename.find_last_of('.');
		if (dot == std::string::npos)
			return filename;

		return filename.substr(0, dot);
	}

	void UpdateSavePathFromLayout()
	{
		if (g_state.layout.sourcePath.empty())
			return;

		const std::string directory = DirectoryFromPath(g_state.layout.sourcePath);
		const std::string filename = FileNameFromPath(g_state.layout.sourcePath);
		const std::string basename = RemoveExtension(filename);
		const std::string savePath = directory + basename + ".ui_editor_copy.fit";

		std::snprintf(g_savePath, sizeof(g_savePath), "%s", savePath.c_str());
		g_state.saveStatus.clear();
		g_state.pendingSavePath.clear();
	}

	bool FileExists(const char* path)
	{
		if (path == nullptr || path[0] == '\0')
			return false;

		std::ifstream file(path);
		return file.good();
	}


	std::string TrimWhitespace(const std::string& value)
	{
		std::string::size_type begin = 0;
		while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
			++begin;

		std::string::size_type end = value.size();
		while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
			--end;

		return value.substr(begin, end - begin);
	}

	std::string StripFitQuotes(const std::string& value)
	{
		std::string result = TrimWhitespace(value);
		if (result.size() >= 2 && result.front() == '"' && result.back() == '"')
			result = result.substr(1, result.size() - 2);
		return result;
	}

	bool ParseFitAssignmentLine(const std::string& line, std::string& key, std::string& value)
	{
		std::string trimmed = TrimWhitespace(line);
		if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
			return false;

		const std::string::size_type comment = trimmed.find("//");
		if (comment != std::string::npos)
			trimmed = TrimWhitespace(trimmed.substr(0, comment));

		const std::string::size_type equals = trimmed.find('=');
		if (equals == std::string::npos)
			return false;

		key = TrimWhitespace(trimmed.substr(0, equals));
		value = StripFitQuotes(trimmed.substr(equals + 1));
		return !key.empty();
	}

	bool ParseCatalogBool(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);
		return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
	}

	std::string NormalizeAssetPath(std::string value)
	{
		std::replace(value.begin(), value.end(), '\\', '/');
		return value;
	}

	void AddReadablePathCandidate(std::vector<std::string>& candidates, const std::string& value)
	{
		if (value.empty())
			return;

		const std::string normalized = NormalizeAssetPath(value);
		if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end())
			candidates.push_back(normalized);
	}

	std::vector<std::string> BuildReadablePathCandidates(const std::string& path)
	{
		std::vector<std::string> candidates;
		if (path.empty())
			return candidates;

		const std::string normalized = NormalizeAssetPath(path);
		AddReadablePathCandidate(candidates, normalized);
		AddReadablePathCandidate(candidates, (std::filesystem::current_path() / normalized).string());
		AddReadablePathCandidate(candidates, std::string("../") + normalized);
		AddReadablePathCandidate(candidates, std::string("../../") + normalized);
		AddReadablePathCandidate(candidates, std::string("../../../") + normalized);

		char* basePath = SDL_GetBasePath();
		if (basePath != nullptr)
		{
			const std::string base = NormalizeAssetPath(basePath);
			AddReadablePathCandidate(candidates, base + normalized);
			AddReadablePathCandidate(candidates, base + "../" + normalized);
			AddReadablePathCandidate(candidates, base + "../../" + normalized);
			AddReadablePathCandidate(candidates, base + "../../../" + normalized);
			SDL_free(basePath);
		}

		return candidates;
	}

	std::string ResolveReadablePath(const std::string& path)
	{
		const std::vector<std::string> candidates = BuildReadablePathCandidates(path);
		for (std::size_t i = 0; i < candidates.size(); ++i)
		{
			std::error_code ec;
			if (std::filesystem::exists(std::filesystem::path(candidates[i]), ec))
				return candidates[i];
		}

		return path;
	}

	void LoadFontCatalog()
	{
		g_fontCatalog.clear();

		ImGuiIO& io = ImGui::GetIO();
		g_defaultEditorFont = io.Fonts->AddFontDefault();

		const char* catalogCandidates[] =
		{
			"data/defs/ui/packages/legacy_imgui/shared/fonts.fit",
			"data/defs/ui/shared/fonts.fit",
			nullptr
		};

		std::string catalogPath;
		std::ifstream input;
		for (const char** candidate = catalogCandidates; *candidate != nullptr; ++candidate)
		{
			catalogPath = ResolveReadablePath(*candidate);
			input.open(catalogPath.c_str());
			if (input.is_open())
				break;
		}

		if (!input.is_open())
		{
			g_fontCatalogStatus = "Missing legacy_imgui package font catalog; using ImGui default font.";
			return;
		}

		bool inFontAsset = false;
		UiEditorFontCatalogEntry current;
		std::string line;
		while (std::getline(input, line))
		{
			const std::string trimmed = TrimWhitespace(line);
			if (trimmed.rfind("FontAsset", 0) == 0 && trimmed.find('{') != std::string::npos)
			{
				inFontAsset = true;
				current = UiEditorFontCatalogEntry();
				continue;
			}

			if (!inFontAsset)
				continue;

			if (trimmed == "}")
			{
				if (!current.key.empty() && !current.path.empty())
				{
					std::string readablePath = ResolveReadablePath(current.path);
					std::vector<std::string> fontCandidates;
					fontCandidates.push_back(readablePath);
					fontCandidates.push_back(ResolveReadablePath(std::string("assets/graphics/fonts/ui/") + std::filesystem::path(current.path).filename().string()));
					fontCandidates.push_back(ResolveReadablePath(std::string("assets/graphics/fonts/") + std::filesystem::path(current.path).filename().string()));
					fontCandidates.push_back(ResolveReadablePath(std::string("data/defs/ui/shared/fonts/") + std::filesystem::path(current.path).filename().string()));

					for (std::size_t candidateIndex = 0; candidateIndex < fontCandidates.size(); ++candidateIndex)
					{
						std::error_code ec;
						if (!std::filesystem::exists(std::filesystem::path(fontCandidates[candidateIndex]), ec))
							continue;

						ImFont* loadedFont = io.Fonts->AddFontFromFileTTF(fontCandidates[candidateIndex].c_str(), std::max(1.0f, current.defaultSize));
						if (loadedFont != nullptr)
						{
							current.font = loadedFont;
							current.path = fontCandidates[candidateIndex];
							break;
						}
					}

					g_fontCatalog.push_back(current);
				}

				inFontAsset = false;
				continue;
			}

			std::string key;
			std::string value;
			if (!ParseFitAssignmentLine(line, key, value))
				continue;

			if (key == "key")
				current.key = value;
			else if (key == "displayName")
				current.displayName = value;
			else if (key == "path")
				current.path = value;
			else if (key == "family")
				current.family = value;
			else if (key == "style")
				current.style = value;
			else if (key == "defaultSize")
				current.defaultSize = static_cast<float>(std::atof(value.c_str()));
			else if (key == "legacyId")
				current.legacyId = std::atoi(value.c_str());
			else if (key == "supportsBold")
				current.supportsBold = ParseCatalogBool(value);
			else if (key == "supportsItalic")
				current.supportsItalic = ParseCatalogBool(value);
		}

		int loadedCount = 0;
		for (std::size_t i = 0; i < g_fontCatalog.size(); ++i)
		{
			if (g_fontCatalog[i].font != nullptr)
				++loadedCount;
		}

		char status[160];
		std::snprintf(status, sizeof(status), "Loaded %d/%d TTFonts from %s.", loadedCount, static_cast<int>(g_fontCatalog.size()), catalogPath.c_str());
		g_fontCatalogStatus = status;
	}

	const UiEditorFontCatalogEntry* FindFontCatalogEntryByName(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);
		if (lower.empty())
			return nullptr;

		for (std::size_t i = 0; i < g_fontCatalog.size(); ++i)
		{
			const UiEditorFontCatalogEntry& entry = g_fontCatalog[i];
			if (lower == ToLowerCopy(entry.key)
				|| lower == ToLowerCopy(entry.displayName)
				|| lower == ToLowerCopy(entry.path)
				|| lower == ToLowerCopy(std::filesystem::path(entry.path).filename().string()))
			{
				return &entry;
			}
		}

		if (lower == "liberation sans regular")
			return FindFontCatalogEntryByName("Liberation Sans");

		if (lower == "arial narrow" || lower == "arialnarrow" || lower == "agencyfb")
			return FindFontCatalogEntryByName("Agency Regular");

		return nullptr;
	}

	const UiEditorFontCatalogEntry* FindFontCatalogEntryByFamilyStyle(const std::string& family, const std::string& style)
	{
		const std::string lowerFamily = ToLowerCopy(family);
		const std::string lowerStyle = ToLowerCopy(style);
		if (lowerFamily.empty() || lowerStyle.empty())
			return nullptr;

		for (std::size_t i = 0; i < g_fontCatalog.size(); ++i)
		{
			const UiEditorFontCatalogEntry& candidate = g_fontCatalog[i];
			if (ToLowerCopy(candidate.family) == lowerFamily
				&& ToLowerCopy(candidate.style) == lowerStyle)
			{
				return &candidate;
			}
		}

		return nullptr;
	}

	const UiEditorFontCatalogEntry* FindStyledFontCatalogEntry(const UiEditorFitCell& cell)
	{
		const UiEditorFontCatalogEntry* base = FindFontCatalogEntryByName(cell.font);
		if (base == nullptr)
			base = FindFontCatalogEntryByName("Agency Regular");
		if (base == nullptr)
			base = FindFontCatalogEntryByName("Liberation Sans");

		if (base == nullptr)
			return nullptr;

		std::string requestedStyle = cell.fontStyle.empty() ? base->style : cell.fontStyle;
		if (cell.bold && cell.italic)
			requestedStyle = "BoldItalic";
		else if (cell.bold)
			requestedStyle = "Bold";
		else if (cell.italic)
			requestedStyle = "Italic";

		const UiEditorFontCatalogEntry* styled = FindFontCatalogEntryByFamilyStyle(base->family, requestedStyle);
		if (styled != nullptr)
			return styled;

		if (!cell.fontStyle.empty())
		{
			// Font Type is an explicit authoring field. If a family does not have
			// the requested style, try a catalog key/display-name lookup before
			// falling back to the base face.
			styled = FindFontCatalogEntryByName(base->family + " " + cell.fontStyle);
			if (styled != nullptr)
				return styled;
		}

		if (cell.bold || cell.italic)
		{
			for (std::size_t i = 0; i < g_fontCatalog.size(); ++i)
			{
				const UiEditorFontCatalogEntry& candidate = g_fontCatalog[i];
				if (ToLowerCopy(candidate.family) != ToLowerCopy(base->family))
					continue;

				const std::string style = ToLowerCopy(candidate.style);
				if (cell.bold && cell.italic && style.find("bold") != std::string::npos && style.find("italic") != std::string::npos)
					return &candidate;
				if (cell.bold && !cell.italic && style.find("bold") != std::string::npos && style.find("italic") == std::string::npos)
					return &candidate;
				if (!cell.bold && cell.italic && style.find("italic") != std::string::npos && style.find("bold") == std::string::npos)
					return &candidate;
			}
		}

		return base;
	}

	ImFont* FontForCell(const UiEditorFitCell& cell)
	{
		const UiEditorFontCatalogEntry* entry = FindStyledFontCatalogEntry(cell);
		if (entry != nullptr && entry->font != nullptr)
			return entry->font;

		return g_defaultEditorFont != nullptr ? g_defaultEditorFont : ImGui::GetFont();
	}

	std::string NormalizePathForDisplay(const std::string& value)
	{
		std::string result = value;
		std::replace(result.begin(), result.end(), '\\', '/');
		return result;
	}

	std::string SurfaceFromUiRelativePath(const std::string& relativePath)
	{
		const std::string normalized = NormalizePathForDisplay(relativePath);
		const std::string packagePrefix = "packages/legacy_imgui/";
		if (normalized.rfind(packagePrefix, 0) == 0)
		{
			const std::string remainder = normalized.substr(packagePrefix.size());
			const std::string::size_type slash = remainder.find('/');
			if (slash == std::string::npos)
				return "legacy_imgui";
			return std::string("legacy_imgui/") + remainder.substr(0, slash);
		}

		const std::string::size_type slash = normalized.find('/');
		if (slash == std::string::npos)
			return "root";

		return normalized.substr(0, slash);
	}

	std::string FolderFromUiRelativePath(const std::string& relativePath)
	{
		const std::string normalized = NormalizePathForDisplay(relativePath);
		const std::string::size_type slash = normalized.find_last_of('/');
		if (slash == std::string::npos)
			return std::string();

		return normalized.substr(0, slash);
	}

	bool DirectoryExists(const std::string& path)
	{
		std::error_code ec;
		return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
	}

	std::string ResolveUiRootPath()
	{
		std::vector<std::string> candidates;
		candidates.push_back("data/defs/ui");
		candidates.push_back("../data/defs/ui");
		candidates.push_back("../../data/defs/ui");
		candidates.push_back("../../../data/defs/ui");

		char* basePath = SDL_GetBasePath();
		if (basePath != nullptr)
		{
			const std::string base(basePath);
			candidates.push_back(base + "data/defs/ui");
			candidates.push_back(base + "../data/defs/ui");
			candidates.push_back(base + "../../data/defs/ui");
			candidates.push_back(base + "../../../data/defs/ui");
			SDL_free(basePath);
		}

		for (const std::string& candidate : candidates)
		{
			if (DirectoryExists(candidate))
				return candidate;
		}

		return std::string();
	}

	// Additive from theirs (mc2r fork): recognize GuiRedirect indirection (follow to the
	// resolved target instead of unconditionally rejecting) and the newer UiV2* block
	// vocabulary, so the layout browser also surfaces UiV2-authored FITs. Package-path
	// literals below are left as ours' single "legacy_imgui" scheme (see the
	// MERGE-CONFLICT-UI-PHASE1 note near g_loadPath).
	bool TryReadGuiRedirectTargetForBrowser(const std::filesystem::path& filePath, std::string& outTarget)
	{
		outTarget.clear();

		std::ifstream input(filePath);
		if (!input.is_open())
			return false;

		bool inRedirect = false;
		std::string line;
		while (std::getline(input, line))
		{
			const std::string trimmed = TrimWhitespace(line);
			if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
				continue;

			if (!inRedirect)
			{
				const std::string::size_type brace = trimmed.find('{');
				if (brace == std::string::npos)
					continue;

				inRedirect = TrimWhitespace(trimmed.substr(0, brace)) == "GuiRedirect";
				continue;
			}

			if (trimmed == "}")
				return !outTarget.empty();

			std::string key;
			std::string value;
			if (ParseFitAssignmentLine(trimmed, key, value)
				&& (key == "target" || key == "targetFile" || key == "targetPath"))
			{
				outTarget = value;
			}
		}

		return !outTarget.empty();
	}

	bool IsDataStyleUiPath(const std::string& path)
	{
		const std::string lower = ToLowerCopy(NormalizePathForDisplay(path));
		return lower.rfind("data/", 0) == 0
			|| lower.rfind("../data/", 0) == 0
			|| lower.rfind("../../data/", 0) == 0
			|| lower.rfind("../../../data/", 0) == 0;
	}

	std::filesystem::path ResolveBrowserRedirectTargetPath(const std::filesystem::path& filePath, const std::string& target)
	{
		if (target.empty())
			return std::filesystem::path();

		std::filesystem::path targetPath(target);
		if (targetPath.is_absolute())
			return targetPath;

		if (IsDataStyleUiPath(target))
			return std::filesystem::path(target).lexically_normal();

		return (filePath.parent_path() / targetPath).lexically_normal();
	}

	bool UiFileHasGuiPageAndDrawableBlocks(const std::filesystem::path& filePath)
	{
		std::ifstream input(filePath);
		if (!input.is_open())
			return false;

		bool hasGuiPage = false;
		bool hasDrawableBlock = false;
		std::string line;
		while (std::getline(input, line))
		{
			const std::string trimmed = TrimWhitespace(line);
			if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
				continue;

			if (trimmed.rfind("GuiRedirect", 0) == 0)
				return false;

			if (trimmed.rfind("GuiPage", 0) == 0 && trimmed.rfind("GuiPageRef", 0) != 0)
				hasGuiPage = true;
			if (trimmed.rfind("UiV2Page", 0) == 0)
				hasGuiPage = true;

			if (trimmed.rfind("GuiImage", 0) == 0
				|| trimmed.rfind("GuiButton", 0) == 0
				|| trimmed.rfind("GuiText", 0) == 0
				|| trimmed.rfind("GuiRect", 0) == 0
				|| trimmed.rfind("GuiPanel", 0) == 0
				|| trimmed.rfind("GuiMount", 0) == 0
				|| trimmed.rfind("GuiScrollbar", 0) == 0
				|| trimmed.rfind("UiV2Static", 0) == 0
				|| trimmed.rfind("UiV2Button", 0) == 0
				|| trimmed.rfind("UiV2Text", 0) == 0
				|| trimmed.rfind("UiV2Rect", 0) == 0
				|| trimmed.rfind("UiV2AnimObject", 0) == 0
				|| trimmed.rfind("UiV2Edit", 0) == 0)
			{
				hasDrawableBlock = true;
			}
		}

		return hasGuiPage && hasDrawableBlock;
	}

	bool IsActualUiLayoutFitResolved(const std::filesystem::path& filePath, const std::string& relativePath, int redirectDepth)
	{
		std::string relative = ToLowerCopy(NormalizePathForDisplay(relativePath));
		if (relative.rfind("legacy/", 0) == 0 || relative.find("/legacy/") != std::string::npos
			|| relative.rfind("_audit/", 0) == 0 || relative.find("/_audit/") != std::string::npos
			|| relative.rfind("compatibility/", 0) == 0 || relative.find("/compatibility/") != std::string::npos
			|| relative.rfind("defines/", 0) == 0 || relative.find("/defines/") != std::string::npos
			|| relative.rfind("index/", 0) == 0 || relative.find("/index/") != std::string::npos)
		{
			return false;
		}

		const std::string filename = ToLowerCopy(filePath.filename().string());
		if (filename == "package.fit"
			|| filename == "default_ui_package.fit"
			|| filename == "manifest.fit"
			|| filename.find("manifest") != std::string::npos
			|| filename.find("font") != std::string::npos
			|| filename.find("content_styles") != std::string::npos
			|| filename.find("ui_rects") != std::string::npos
			|| filename.find("ui_elements") != std::string::npos
			|| filename.find("ui_actions") != std::string::npos
			|| filename.find("ui_fonts") != std::string::npos)
		{
			return false;
		}

		std::string redirectTarget;
		if (TryReadGuiRedirectTargetForBrowser(filePath, redirectTarget))
		{
			if (redirectDepth >= 8)
				return false;

			const std::filesystem::path resolvedTarget = ResolveBrowserRedirectTargetPath(filePath, redirectTarget);
			std::error_code ec;
			if (!std::filesystem::exists(resolvedTarget, ec) || !std::filesystem::is_regular_file(resolvedTarget, ec))
				return false;

			return IsActualUiLayoutFitResolved(resolvedTarget, std::string(), redirectDepth + 1);
		}

		return UiFileHasGuiPageAndDrawableBlocks(filePath);
	}

	bool IsActualUiLayoutFit(const std::filesystem::path& filePath, const std::string& relativePath)
	{
		return IsActualUiLayoutFitResolved(filePath, relativePath, 0);
	}

	void RefreshUiFitBrowser(bool force)
	{
		if (g_state.uiFitBrowserScanned && !force)
			return;

		g_state.uiFitEntries.clear();
		g_state.uiFitRootPath = ResolveUiRootPath();
		g_state.uiFitBrowserScanned = true;

		if (g_state.uiFitRootPath.empty())
			return;

		std::error_code ec;
		const std::filesystem::path root(g_state.uiFitRootPath);

		for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		{
			if (ec)
				break;

			if (!it->is_regular_file(ec))
				continue;

			const std::filesystem::path filePath = it->path();
			if (ToLowerCopy(filePath.extension().string()) != ".fit")
				continue;

			const std::string relative = NormalizePathForDisplay(std::filesystem::relative(filePath, root, ec).generic_string());
			if (relative.empty())
				continue;

			if (!IsActualUiLayoutFit(filePath, relative))
				continue;

			UiEditorUiFitEntry entry;
			entry.relativePath = relative;
			entry.loadPath = std::string("data/defs/ui/") + relative;
			entry.surface = SurfaceFromUiRelativePath(relative);
			entry.folder = FolderFromUiRelativePath(relative);
			entry.filename = filePath.filename().string();
			entry.manifest = ContainsInsensitive(entry.filename, "manifest");
			entry.mechlopedia = ContainsInsensitive(entry.loadPath, "mechlopedia");

			g_state.uiFitEntries.push_back(entry);
		}

		std::sort(g_state.uiFitEntries.begin(), g_state.uiFitEntries.end(),
			[](const UiEditorUiFitEntry& a, const UiEditorUiFitEntry& b)
			{
				if (a.surface != b.surface)
					return a.surface < b.surface;

				if (a.folder != b.folder)
					return a.folder < b.folder;

				return a.filename < b.filename;
			});
	}

	bool UiFitEntryMatchesFilter(const UiEditorUiFitEntry& entry)
	{
		if (g_uiFitFilter[0] == '\0')
			return true;

		const std::string filter = ToLowerCopy(g_uiFitFilter);
		const std::string haystack = ToLowerCopy(entry.loadPath + " " + entry.surface + " " + entry.folder + " " + entry.filename);
		return haystack.find(filter) != std::string::npos;
	}

	bool PerformSaveLayoutCopy()
	{
		std::string status;
		const bool saved = UiEditorFitSaveLayoutCopy(g_state.layout, g_savePath, status);
		g_state.saveStatus = status;

		if (saved)
			g_state.dirty = false;

		g_state.showSaveResultPopup = true;
		ImGui::OpenPopup("Save Copy Result");
		return saved;
	}

	bool SaveLayoutCopy()
	{
		if (g_savePath[0] != '\0'
			&& !g_state.layout.sourcePath.empty()
			&& ToLowerCopy(g_state.layout.sourcePath) == ToLowerCopy(g_savePath))
		{
			g_state.saveStatus = "Save Copy refused: output path matches the loaded source FIT.";
			g_state.showSaveResultPopup = true;
			ImGui::OpenPopup("Save Copy Result");
			return false;
		}

		if (g_savePath[0] != '\0' && FileExists(g_savePath))
		{
			g_state.pendingSavePath = g_savePath;
			g_state.saveStatus = std::string("Save Copy output already exists: ") + g_savePath;
			ImGui::OpenPopup("Overwrite Save Copy?");
			return false;
		}

		return PerformSaveLayoutCopy();
	}

	bool IsMechlopediaCompositePage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));
		const std::string path = ToLowerCopy(layout.sourcePath);

		return path.find("mechlopedia") != std::string::npos
			&& filename.find(".fit") != std::string::npos
			&& filename != "main.fit"
			&& filename != "manifest.fit"
			&& filename != "content_styles.fit"
			&& filename != "list_entry.fit";
	}

	bool IsLegacyImguiGamePath(std::string path)
	{
		path = ToLowerCopy(path);
		std::replace(path.begin(), path.end(), '\\', '/');

		return path.find("/packages/legacy_imgui/game/") != std::string::npos
			|| path.find("/game/legacy_imgui/") != std::string::npos;
	}

	bool IsCampaignMapPage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));
		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');

		return IsLegacyImguiGamePath(path)
			&& filename.find("mcl_cm_op") == 0
			&& filename.find("sample") == std::string::npos
			&& filename.find(".fit") != std::string::npos;
	}

	bool IsCampaignMissionPage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));
		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');

		return IsLegacyImguiGamePath(path)
			&& filename.find("mcl_cm_op") == 0
			&& filename.find("mission") != std::string::npos
			&& filename.find(".fit") != std::string::npos;
	}

	bool IsLegacyGameOsMechlopediaBase(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');
		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));

		return IsLegacyImguiGamePath(path)
			&& (filename == "mcl_en.fit" || filename == "mcl_en_800.fit");
	}

	bool IsLegacyGameOsMechlopediaDetailPage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');
		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));

		return IsLegacyImguiGamePath(path)
			&& filename.find("mcl_en") == 0
			&& filename.find(".fit") != std::string::npos
			&& !IsLegacyGameOsMechlopediaBase(layout);
	}

	bool IsLegacyOptionsCompanionPage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');
		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));

		return IsLegacyImguiGamePath(path)
			&& (filename == "mcl_optionsaudio.fit"
				|| filename == "mcl_optionsgameplay.fit"
				|| filename == "mcl_optionsgraphics.fit"
				|| filename == "mcl_optionshotkeys.fit");
	}

	bool IsLegacyMechPurchaseInfoPage(const UiEditorFitLayout& layout)
	{
		if (!layout.loaded)
			return false;

		std::string path = ToLowerCopy(layout.sourcePath);
		std::replace(path.begin(), path.end(), '\\', '/');
		const std::string filename = ToLowerCopy(FileNameFromPath(layout.sourcePath));

		return IsLegacyImguiGamePath(path)
			&& filename == "mcl_mechinfo.fit";
	}


	bool CellLooksLikeMissionVideoMount(const UiEditorFitCell& cell)
	{
		const std::string combined = ToLowerCopy(
			cell.key + " " + cell.role + " " + cell.textKey + " " + cell.texture + " "
			+ cell.widgetType + " " + cell.sourceControlType + " " + cell.visibleText + " "
			+ cell.controlId);

		return combined.find("video_mount") != std::string::npos
			|| combined.find("video window") != std::string::npos
			|| combined.find("video is placed here") != std::string::npos
			|| combined.find("movie") != std::string::npos
			|| combined.find("tacscreen") != std::string::npos
			|| combined.find("tac_screen") != std::string::npos;
	}

	bool CellLooksLikeMissionMapMount(const UiEditorFitCell& cell)
	{
		const std::string combined = ToLowerCopy(
			cell.key + " " + cell.role + " " + cell.textKey + " " + cell.texture + " "
			+ cell.widgetType + " " + cell.sourceControlType + " " + cell.visibleText + " "
			+ cell.controlId);

		return combined.find("planet_map_mount") != std::string::npos
			|| combined.find("map is placed here") != std::string::npos
			|| combined.find("planet") != std::string::npos
			|| combined.find("map") != std::string::npos
			|| combined.find("operation") != std::string::npos;
	}

	bool FindCompositionMountRect(const UiEditorFitLayout& baseLayout, bool (*predicate)(const UiEditorFitCell&), float& outX, float& outY)
	{
		if (!baseLayout.loaded || predicate == nullptr)
			return false;

		for (std::size_t i = 0; i < baseLayout.cells.size(); ++i)
		{
			const UiEditorFitCell& cell = baseLayout.cells[i];
			if (cell.width <= 0.0f || cell.height <= 0.0f)
				continue;

			if (!predicate(cell))
				continue;

			outX = cell.x;
			outY = cell.y;
			return true;
		}

		return false;
	}

	void ApplyLegacyActiveMount(UiEditorFitLayout& layout, const UiEditorFitLayout* compositionBase)
	{
		if (!layout.loaded)
			return;

		if (IsLegacyGameOsMechlopediaDetailPage(layout))
		{
			// Legacy LogisticsMechDisplay mounts detail pages into the shared
			// encyclopedia frame at runtime with render(285, 58).  Keep the
			// active detail FIT editable in its own local coordinates while
			// previewing it where the game actually draws it.
			layout.mountOffsetX = 285.0f;
			layout.mountOffsetY = 58.0f;
			return;
		}

		if (IsLegacyOptionsCompanionPage(layout))
		{
			// Options tab pages are child screens mounted inside mcl_options.fit
			// at the tab content rect.  Keep source-local coordinates editable
			// while previewing against the runtime parent page.
			layout.mountOffsetX = 95.0f;
			layout.mountOffsetY = 156.0f;
			return;
		}

		if (IsLegacyMechPurchaseInfoPage(layout))
		{
			layout.mountOffsetX = 0.0f;
			layout.mountOffsetY = 0.0f;
			return;
		}

		if (IsCampaignMapPage(layout) || IsCampaignMissionPage(layout))
		{
			float mountX = 0.0f;
			float mountY = 0.0f;

			// Operation/planet/mission companion pages mount into MissionSelectionScreen
			// MAP_RECT (CMRect3).  Do not let the left VID COM/video rectangle win
			// just because the base page also contains video/tacscreen wording.
			if (compositionBase != nullptr
				&& FindCompositionMountRect(*compositionBase, CellLooksLikeMissionMapMount, mountX, mountY))
			{
				layout.mountOffsetX = mountX;
				layout.mountOffsetY = mountY;
				return;
			}

			if (compositionBase != nullptr
				&& FindCompositionMountRect(*compositionBase, CellLooksLikeMissionVideoMount, mountX, mountY))
			{
				layout.mountOffsetX = mountX;
				layout.mountOffsetY = mountY;
				return;
			}

			// Last-resort legacy fallback from MissionSelectionScreen MAP_RECT.
			layout.mountOffsetX = 439.0f;
			layout.mountOffsetY = 92.0f;
		}
	}


	void MarkCompositionLayer(UiEditorFitLayout& layout, const char* layerName)
	{
		for (std::size_t i = 0; i < layout.cells.size(); ++i)
		{
			UiEditorFitCell& cell = layout.cells[i];
			cell.locked = true;
			cell.layer = layerName != nullptr ? layerName : "composition";
			if (cell.role.empty())
				cell.role = "composition";
		}
	}

	bool TryLoadCompositionLayer(const std::string& path, UiEditorFitLayout& outLayer, const char* layerName)
	{
		if (path.empty())
			return false;

		UiEditorFitLayout layer;
		if (!UiEditorFitLoadLayout(path.c_str(), layer))
			return false;

		MarkCompositionLayer(layer, layerName);
		outLayer = layer;
		return true;
	}

	void AppendCompositionLayer(UiEditorFitLayout& composed, const UiEditorFitLayout& layer)
	{
		if (!layer.loaded)
			return;

		if (!composed.loaded)
		{
			composed = layer;
			return;
		}

		composed.cells.insert(composed.cells.end(), layer.cells.begin(), layer.cells.end());
		if (layer.designWidth > composed.designWidth)
			composed.designWidth = layer.designWidth;
		if (layer.designHeight > composed.designHeight)
			composed.designHeight = layer.designHeight;
	}

	bool BuildLegacyCompositionBase(const UiEditorFitLayout& activeLayout, UiEditorFitLayout& outBase)
	{
		outBase = UiEditorFitLayout();

		if (!activeLayout.loaded)
			return false;

		const std::string directory = DirectoryFromPath(activeLayout.sourcePath);

		if (IsMechlopediaCompositePage(activeLayout))
		{
			UiEditorFitLayout mainLayer;
			if (!TryLoadCompositionLayer(directory + "main.fit", mainLayer, "mechlopedia-main"))
				return false;

			outBase = mainLayer;
			return true;
		}

		if (IsLegacyGameOsMechlopediaDetailPage(activeLayout))
		{
			UiEditorFitLayout mainLayer;
			if (!TryLoadCompositionLayer(directory + "mcl_en.fit", mainLayer, "legacy-mechlopedia-main"))
				return false;

			outBase = mainLayer;
			return true;
		}

		if (IsLegacyOptionsCompanionPage(activeLayout))
		{
			UiEditorFitLayout optionsLayer;
			if (!TryLoadCompositionLayer(directory + "mcl_options.fit", optionsLayer, "legacy-options-main"))
				return false;

			outBase = optionsLayer;
			return true;
		}

		if (IsLegacyMechPurchaseInfoPage(activeLayout))
		{
			UiEditorFitLayout mechPurchaseLayer;
			if (!TryLoadCompositionLayer(directory + "mcl_mdollar.fit", mechPurchaseLayer, "legacy-mech-purchase-main"))
				return false;

			outBase = mechPurchaseLayer;
			return true;
		}

		if (IsCampaignMapPage(activeLayout) || IsCampaignMissionPage(activeLayout))
		{
			UiEditorFitLayout campaignFrame;
			if (TryLoadCompositionLayer(directory + "mcl_cm_layout.fit", campaignFrame, "campaign-frame"))
				AppendCompositionLayer(outBase, campaignFrame);

			return outBase.loaded;
		}

		return false;
	}

	bool ShouldUseCompositeBase(const UiEditorFitLayout& layout)
	{
		UiEditorFitLayout ignored;
		return BuildLegacyCompositionBase(layout, ignored);
	}

	void RefreshCompositeBase()
	{
		g_state.baseLayout = UiEditorFitLayout();
		g_state.hasBaseLayout = false;

		UiEditorFitLayout composedBase;
		if (!BuildLegacyCompositionBase(g_state.layout, composedBase))
			return;

		ApplyLegacyActiveMount(g_state.layout, &composedBase);

		g_state.baseLayout = composedBase;
		g_state.hasBaseLayout = true;

		if (composedBase.designWidth > 0 && composedBase.designHeight > 0)
		{
			g_state.canvasWidth = std::max(g_state.canvasWidth, composedBase.designWidth);
			g_state.canvasHeight = std::max(g_state.canvasHeight, composedBase.designHeight);
			g_state.lastCanvasWidth = g_state.canvasWidth;
			g_state.lastCanvasHeight = g_state.canvasHeight;
		}
	}

	int ExtractTrailingNumber(const std::string& value)
	{
		if (value.empty())
			return -1;

		int end = static_cast<int>(value.size()) - 1;
		while (end >= 0 && !std::isdigit(static_cast<unsigned char>(value[static_cast<std::size_t>(end)])))
			--end;

		if (end < 0)
			return -1;

		int begin = end;
		while (begin >= 0 && std::isdigit(static_cast<unsigned char>(value[static_cast<std::size_t>(begin)])))
			--begin;

		return std::atoi(value.substr(static_cast<std::size_t>(begin + 1), static_cast<std::size_t>(end - begin)).c_str());
	}

	bool RectNear(const UiEditorFitCell& cell, float x, float y, float width, float height)
	{
		const float tolerance = 3.0f;

		return std::abs(cell.x - x) <= tolerance
			&& std::abs(cell.y - y) <= tolerance
			&& std::abs(cell.width - width) <= tolerance
			&& std::abs(cell.height - height) <= tolerance;
	}

	std::string NumberedAlias(const char* baseName, const UiEditorFitCell& cell, bool oneBased)
	{
		const int number = ExtractTrailingNumber(cell.key);

		if (number < 0)
			return baseName;

		char label[128];
		std::snprintf(label, sizeof(label), "%s %d", baseName, oneBased ? number + 1 : number);
		return label;
	}

	std::string DisplayAliasForCell(const UiEditorFitCell& cell)
	{
		if (!cell.aliasOverride.empty())
			return cell.aliasOverride;

		if (!cell.visibleText.empty())
			return cell.visibleText;

		if (!cell.controlId.empty())
			return cell.controlId;

		const std::string combined = ToLowerCopy(cell.key + " " + cell.role + " " + cell.textKey + " " + cell.texture + " " + cell.widgetType + " " + cell.sourceControlType);

		if (combined.find("close") != std::string::npos)
			return "Close Button";

		if (combined.find("description") != std::string::npos || combined.find("desc") != std::string::npos)
			return "Description Body";

		if (combined.find("portrait") != std::string::npos || RectNear(cell, 268.0f, 100.0f, 178.0f, 240.0f))
			return "Portrait";

		if (combined.find("stats") != std::string::npos || RectNear(cell, 470.0f, 314.0f, 144.0f, 146.0f))
			return "Stats Panel";

		if (combined.find("loadout") != std::string::npos || RectNear(cell, 628.0f, 314.0f, 143.0f, 146.0f))
			return "Loadout Panel";

		if (combined.find("entry") != std::string::npos || combined.find("list") != std::string::npos || RectNear(cell, 36.0f, 220.0f, 206.0f, 306.0f))
			return "Entry List";

		if (combined.find("category") != std::string::npos || RectNear(cell, 34.0f, 96.0f, 138.0f, 104.0f))
			return "Category Buttons";

		if (ContainsInsensitive(cell.type, "Gui3DView") || ContainsInsensitive(cell.role, "3d_view") || ContainsInsensitive(cell.role, "viewport"))
			return "3D View";

		if (ContainsInsensitive(cell.type, "GuiScrollbar") || ContainsInsensitive(cell.role, "scrollbar"))
			return "Scrollbar";

		if (ContainsInsensitive(cell.type, "GuiButton"))
			return NumberedAlias("Category Button", cell, true);

		if (ContainsInsensitive(cell.type, "GuiText"))
		{
			if (combined.find("title") != std::string::npos)
				return "Title Text";

			return NumberedAlias("Text", cell, false);
		}

		if (ContainsInsensitive(cell.type, "GuiImage") || ContainsInsensitive(cell.type, "GuiStatic"))
		{
			const std::string textureName = FileNameFromPath(cell.texture);

			if (ContainsInsensitive(textureName, "mcl_enc"))
				return NumberedAlias("Background Slice", cell, false);

			if (!textureName.empty())
				return std::string("Image: ") + textureName;

			return NumberedAlias("Image", cell, false);
		}

		if (ContainsInsensitive(cell.type, "GuiRect"))
			return NumberedAlias("Panel", cell, false);

		if (!cell.role.empty())
			return cell.role;

		if (!cell.key.empty())
			return cell.key;

		return cell.type;
	}

	std::string DisplayNameForCell(const UiEditorFitCell& cell)
	{
		if (!cell.aliasOverride.empty())
			return cell.aliasOverride;

		return DisplayAliasForCell(cell);
	}

	std::string HierarchyLabelForCell(const UiEditorFitCell& cell)
	{
		return DisplayNameForCell(cell) + "##" + cell.key;
	}

	void BeginRenameAlias(int cellIndex)
	{
		UiEditorFitCell* cell = UiEditorFitGetCell(g_state.layout, cellIndex) == nullptr
			? nullptr
			: &g_state.layout.cells[static_cast<std::size_t>(cellIndex)];

		if (cell == nullptr)
			return;

		g_state.selectedCell = cellIndex;
		g_state.renameAliasCell = cellIndex;

		const std::string currentName = DisplayNameForCell(*cell);
		std::snprintf(g_renameAliasBuffer, sizeof(g_renameAliasBuffer), "%s", currentName.c_str());
		g_state.openRenameAliasDialog = true;
	}

	void DrawUiFitEntryRow(const UiEditorUiFitEntry& entry)
	{
		const bool selected = ToLowerCopy(g_state.layout.sourcePath) == ToLowerCopy(entry.loadPath)
			|| ToLowerCopy(g_loadPath) == ToLowerCopy(entry.loadPath);

		std::string label = entry.relativePath;
		if (entry.manifest)
			label += "  [manifest]";

		if (ImGui::Selectable(label.c_str(), selected))
			RequestLoadLayoutPath(entry.loadPath.c_str());

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", entry.loadPath.c_str());
	}

	void DrawUiFitBrowser()
	{
		RefreshUiFitBrowser(false);

		ImGui::TextUnformatted("UI FIT Browser");
		ImGui::SameLine();

		if (ImGui::SmallButton("Refresh"))
			RefreshUiFitBrowser(true);

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##ui_fit_filter", "filter UI FITs", g_uiFitFilter, sizeof(g_uiFitFilter));

		if (g_state.uiFitRootPath.empty())
		{
			ImGui::TextWrapped("data/defs/ui was not found beside the executable or nearby repo paths.");
			return;
		}

		int visibleCount = 0;
		for (const UiEditorUiFitEntry& entry : g_state.uiFitEntries)
		{
			if (UiFitEntryMatchesFilter(entry))
				++visibleCount;
		}

		ImGui::TextDisabled("%d / %d UI FITs", visibleCount, static_cast<int>(g_state.uiFitEntries.size()));

		std::string currentSurface;
		bool surfaceOpen = false;

		for (const UiEditorUiFitEntry& entry : g_state.uiFitEntries)
		{
			if (!UiFitEntryMatchesFilter(entry))
				continue;

			if (entry.surface != currentSurface)
			{
				if (surfaceOpen)
					ImGui::TreePop();

				currentSurface = entry.surface;
				const bool defaultOpen = currentSurface == "legacy_imgui/mechlopedia" || currentSurface == "legacy_imgui/editor" || currentSurface == "legacy_imgui/game";
				ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
				surfaceOpen = ImGui::TreeNodeEx(currentSurface.c_str(), flags);
			}

			if (surfaceOpen)
			{
				ImGui::PushID(entry.loadPath.c_str());
				DrawUiFitEntryRow(entry);
				ImGui::PopID();
			}
		}

		if (surfaceOpen)
			ImGui::TreePop();
	}

	void DrawProjectTree()
	{
		DrawSectionTitle("PROJECT / LAYOUTS");

		ImGui::TextUnformatted("Load path");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText("##fit_load_path", g_loadPath, sizeof(g_loadPath));

		ImGui::Spacing();

		DrawUiFitBrowser();

		ImGui::Spacing();
		ImGui::TextWrapped("%s", g_state.layout.statusMessage.c_str());

		if (!g_state.layout.sourcePath.empty())
			ImGui::TextWrapped("Loaded: %s", g_state.layout.sourcePath.c_str());
	}

	const char* SmartToolLabel(UiEditorSmartTool tool)
	{
		switch (tool)
		{
		case SmartToolColorBlock: return "Color Block";
		case SmartToolText: return "Text";
		case SmartToolButton: return "Button";
		case SmartToolPanel: return "Panel";
		case SmartToolImage: return "Image";
		case SmartToolList: return "List";
		case SmartToolSelect:
		default: return "Select";
		}
	}

	const char* SmartToolTypeName(UiEditorSmartTool tool)
	{
		switch (tool)
		{
		case SmartToolColorBlock: return "GuiColorBlock";
		case SmartToolText: return "GuiText";
		case SmartToolButton: return "GuiButton";
		case SmartToolPanel: return "GuiPanel";
		case SmartToolImage: return "GuiImage";
		case SmartToolList: return "GuiList";
		case SmartToolSelect:
		default: return "GuiCell";
		}
	}

		ImU32 CellColorU32(const float color[4], float opacityScale = 1.0f)
	{
		const float alpha = std::max(0.0f, std::min(1.0f, color[3] * opacityScale));
		return IM_COL32(
			static_cast<int>(std::max(0.0f, std::min(1.0f, color[0])) * 255.0f),
			static_cast<int>(std::max(0.0f, std::min(1.0f, color[1])) * 255.0f),
			static_cast<int>(std::max(0.0f, std::min(1.0f, color[2])) * 255.0f),
			static_cast<int>(alpha * 255.0f));
	}

	bool IsSmartProductionCell(const UiEditorFitCell& cell)
	{
		return cell.type == "GuiColorBlock"
			|| cell.type == "GuiText"
			|| cell.type == "GuiButton"
			|| cell.type == "GuiPanel";
	}


	ImGuiID g_directNumericInputId = 0;
	bool g_directNumericInputFocus = false;

	float ClampFloatValue(float value, float minValue, float maxValue)
	{
		if (minValue < maxValue)
			return std::max(minValue, std::min(maxValue, value));
		return value;
	}

	bool DragFloatWithDoubleClickInput(const char* label, float* value, float speed, float minValue, float maxValue, const char* format)
	{
		const ImGuiID id = ImGui::GetID(label);
		bool changed = false;

		if (g_directNumericInputId == id)
		{
			if (g_directNumericInputFocus)
			{
				ImGui::SetKeyboardFocusHere();
				g_directNumericInputFocus = false;
			}

			changed = ImGui::InputFloat(label, value, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
			if (changed || ImGui::IsItemDeactivated())
				g_directNumericInputId = 0;
		}
		else
		{
			changed = ImGui::DragFloat(label, value, speed, minValue, maxValue, format);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				g_directNumericInputId = id;
				g_directNumericInputFocus = true;
			}
		}

		if (changed)
			*value = ClampFloatValue(*value, minValue, maxValue);

		return changed;
	}

	bool SliderFloatWithDoubleClickInput(const char* label, float* value, float minValue, float maxValue, const char* format)
	{
		const ImGuiID id = ImGui::GetID(label);
		bool changed = false;

		if (g_directNumericInputId == id)
		{
			if (g_directNumericInputFocus)
			{
				ImGui::SetKeyboardFocusHere();
				g_directNumericInputFocus = false;
			}

			changed = ImGui::InputFloat(label, value, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
			if (changed || ImGui::IsItemDeactivated())
				g_directNumericInputId = 0;
		}
		else
		{
			changed = ImGui::SliderFloat(label, value, minValue, maxValue, format);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				g_directNumericInputId = id;
				g_directNumericInputFocus = true;
			}
		}

		if (changed)
			*value = ClampFloatValue(*value, minValue, maxValue);

		return changed;
	}

	void SyncInspectorTextBuffers(int cellIndex, const UiEditorFitCell& cell)
	{
		if (g_inspectorBufferCell == cellIndex)
			return;

		std::snprintf(g_inspectorVisibleTextBuffer, sizeof(g_inspectorVisibleTextBuffer), "%s", cell.visibleText.c_str());
		std::snprintf(g_inspectorTextKeyBuffer, sizeof(g_inspectorTextKeyBuffer), "%s", cell.textKey.c_str());
		std::snprintf(g_inspectorTextAlignBuffer, sizeof(g_inspectorTextAlignBuffer), "%s", cell.textAlign.empty() ? "left" : cell.textAlign.c_str());
		std::snprintf(g_inspectorTextAnchorBuffer, sizeof(g_inspectorTextAnchorBuffer), "%s", cell.textAnchor.empty() ? "upper_left" : cell.textAnchor.c_str());
		std::snprintf(g_inspectorFontBuffer, sizeof(g_inspectorFontBuffer), "%s", cell.font.empty() ? "Liberation Sans Regular" : cell.font.c_str());
		std::snprintf(g_inspectorFontStyleBuffer, sizeof(g_inspectorFontStyleBuffer), "%s", cell.fontStyle.empty() ? "Regular" : cell.fontStyle.c_str());
		std::snprintf(g_inspectorTextureBuffer, sizeof(g_inspectorTextureBuffer), "%s", cell.texture.c_str());
		g_inspectorBufferCell = cellIndex;
	}

bool ToolButton(const char* label, UiEditorSmartTool tool)
	{
		const bool active = (g_state.activeSmartTool == tool);
		const ImVec2 size(112.0f, 34.0f);
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ColorU32(PanelBlue()));
		const bool clicked = ImGui::Button(label, size);
		if (active)
			ImGui::PopStyleColor();
		if (clicked)
			g_state.activeSmartTool = tool;
		return clicked;
	}


	int TextAnchorIndexForValue(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);
		for (int i = 0; i < static_cast<int>(sizeof(kTextAnchorValues) / sizeof(kTextAnchorValues[0])); ++i)
		{
			if (lower == kTextAnchorValues[i])
				return i;
		}

		if (lower == "left")
			return 0;
		if (lower == "top" || lower == "top_middle")
			return 1;
		if (lower == "right")
			return 2;
		if (lower == "middle" || lower == "center")
			return 4;
		if (lower == "bottom")
			return 7;

		return 0;
	}

	int FontPresetIndexForValue(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);

		for (int i = 0; i < static_cast<int>(g_fontCatalog.size()); ++i)
		{
			const UiEditorFontCatalogEntry& entry = g_fontCatalog[static_cast<std::size_t>(i)];
			if (lower == ToLowerCopy(entry.key)
				|| lower == ToLowerCopy(entry.displayName)
				|| lower == ToLowerCopy(entry.path))
			{
				return i;
			}
		}

		for (int i = 0; i < static_cast<int>(sizeof(kFontPresets) / sizeof(kFontPresets[0])); ++i)
		{
			if (lower == ToLowerCopy(kFontPresets[i]))
				return i;
		}

		return -1;
	}

	int LegacyActionPresetIndexFor(const UiEditorFitCell& cell)
	{
		for (int i = 0; i < static_cast<int>(sizeof(kLegacyActionPresets) / sizeof(kLegacyActionPresets[0])); ++i)
		{
			if (cell.actionType == kLegacyActionPresets[i].actionType)
				return i;
		}

		return 0;
	}

	bool IsButtonLikeCell(const UiEditorFitCell& cell)
	{
		if (cell.type == "GuiButton")
			return true;

		const std::string loweredType = ToLowerCopy(cell.type);
		const std::string loweredWidget = ToLowerCopy(cell.widgetType);
		const std::string loweredControl = ToLowerCopy(cell.sourceControlType);
		return loweredType.find("button") != std::string::npos
			|| loweredWidget.find("button") != std::string::npos
			|| loweredControl.find("button") != std::string::npos
			|| loweredControl.find("check") != std::string::npos;
	}

	void DrawSmartTools()
	{
		DrawSectionTitle("SMART TOOLS");

		ToolButton("Select", SmartToolSelect);
		ImGui::SameLine();
		ToolButton("Color Block", SmartToolColorBlock);

		ToolButton("Text", SmartToolText);
		ImGui::SameLine();
		ToolButton("Button", SmartToolButton);

		ToolButton("Panel", SmartToolPanel);
		ImGui::SameLine();
		ToolButton("Image", SmartToolImage);
		ImGui::SameLine();
		ToolButton("List", SmartToolList);

		ImGui::Spacing();
		ImGui::TextWrapped("Active: %s", SmartToolLabel(g_state.activeSmartTool));
		if (g_state.activeSmartTool != SmartToolSelect)
			ImGui::TextDisabled("Drag on the canvas to create.");

		ImGui::Spacing();
		ImGui::TextDisabled("Select + drag to move/resize. Shift-drag the canvas to marquee-select rects.");
	}

	float ClampedSplitHeightValue(float topHeight, float totalHeight, float minTop, float minBottom)
	{
		const float splitterHeight = 7.0f;
		if (totalHeight <= minTop + minBottom + splitterHeight)
			return std::max(40.0f, totalHeight * 0.50f);

		const float maxTop = std::max(minTop, totalHeight - minBottom - splitterHeight);
		return std::max(minTop, std::min(topHeight, maxTop));
	}

	void ClampSplitHeight(float& topHeight, float totalHeight, float minTop, float minBottom)
	{
		topHeight = ClampedSplitHeightValue(topHeight, totalHeight, minTop, minBottom);
	}

	void DrawHorizontalSplitter(const char* id, float& topHeight, float totalHeight, float minTop, float minBottom)
	{
		const float splitterHeight = 7.0f;

		ImGui::InvisibleButton(id, ImVec2(-1.0f, splitterHeight));

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 lineColor = ImGui::IsItemHovered() || ImGui::IsItemActive()
			? ColorU32(AccentCyan())
			: IM_COL32(96, 100, 108, 180);

		drawList->AddLine(
			ImVec2(min.x + 4.0f, (min.y + max.y) * 0.5f),
			ImVec2(max.x - 4.0f, (min.y + max.y) * 0.5f),
			lineColor,
			1.0f);

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

		if (ImGui::IsItemActive())
		{
			topHeight += ImGui::GetIO().MouseDelta.y;
			ClampSplitHeight(topHeight, totalHeight, minTop, minBottom);
		}
	}

	
float ClampedPanelWidthValue(float width, float minWidth, float maxWidth)
	{
		return std::max(minWidth, std::min(width, maxWidth));
	}

void ClampPanelWidth(float& width, float minWidth, float maxWidth)
	{
		width = ClampedPanelWidthValue(width, minWidth, maxWidth);
	}

	void DrawVerticalSplitter(const char* id, float& width, float minWidth, float maxWidth, bool invertDelta)
	{
		const float splitterWidth = 7.0f;

		ImGui::InvisibleButton(id, ImVec2(splitterWidth, -1.0f));

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 lineColor = ImGui::IsItemHovered() || ImGui::IsItemActive()
			? ColorU32(AccentCyan())
			: IM_COL32(96, 100, 108, 180);

		drawList->AddLine(
			ImVec2((min.x + max.x) * 0.5f, min.y + 4.0f),
			ImVec2((min.x + max.x) * 0.5f, max.y - 4.0f),
			lineColor,
			1.0f);

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (ImGui::IsItemActive())
		{
			const float delta = ImGui::GetIO().MouseDelta.x;
			width += invertDelta ? -delta : delta;
			ClampPanelWidth(width, minWidth, maxWidth);
		}
	}


	void DrawLeftPanel(float width)
	{
		ImGui::BeginChild("left_panel", ImVec2(width, 0.0f), false);

		const float totalHeight = ImGui::GetContentRegionAvail().y;
		const float displayLayoutsHeight = ClampedSplitHeightValue(g_state.leftLayoutsHeight, totalHeight, 160.0f, 170.0f);

		ImGui::BeginChild("project_layouts_panel", ImVec2(0.0f, displayLayoutsHeight), true);
		DrawProjectTree();
		ImGui::EndChild();

		DrawHorizontalSplitter("##left_panel_splitter", g_state.leftLayoutsHeight, totalHeight, 160.0f, 170.0f);

		ImGui::BeginChild("smart_tools_panel", ImVec2(0.0f, 0.0f), true);
		DrawSmartTools();
		ImGui::EndChild();

		ImGui::EndChild();
	}

	void DrawGrid(ImDrawList* drawList, const ImVec2& origin, const ImVec2& size, float spacing)
	{
		const ImU32 gridColor = IM_COL32(80, 86, 92, 70);

		for (float x = origin.x; x <= origin.x + size.x; x += spacing)
			drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), gridColor);

		for (float y = origin.y; y <= origin.y + size.y; y += spacing)
			drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), gridColor);
	}

	void DrawRulersAndGuides(
		ImDrawList* drawList,
		const ImVec2& viewportMin,
		const ImVec2& viewportMax,
		const ImVec2& artMin,
		const ImVec2& artMax,
		float scale)
	{
		const float ruler = 24.0f;
		const ImU32 rulerBg = IM_COL32(28, 31, 35, 245);
		const ImU32 rulerBorder = IM_COL32(86, 94, 104, 210);
		const ImU32 tickColor = IM_COL32(150, 160, 172, 180);
		const ImU32 guideColor = IM_COL32(70, 190, 255, 185);
		const ImU32 guideActiveColor = IM_COL32(120, 225, 255, 235);

		if (g_state.showRulers)
		{
			drawList->AddRectFilled(viewportMin, ImVec2(viewportMax.x, viewportMin.y + ruler), rulerBg);
			drawList->AddRectFilled(viewportMin, ImVec2(viewportMin.x + ruler, viewportMax.y), rulerBg);
			drawList->AddLine(ImVec2(viewportMin.x, viewportMin.y + ruler), ImVec2(viewportMax.x, viewportMin.y + ruler), rulerBorder);
			drawList->AddLine(ImVec2(viewportMin.x + ruler, viewportMin.y), ImVec2(viewportMin.x + ruler, viewportMax.y), rulerBorder);

			const float major = 100.0f;
			const float minor = 20.0f;

			const float rulerMaxX = static_cast<float>(std::max(std::max(g_state.canvasWidth, g_state.previewWidth), 1920) + 1000);
			const float rulerMaxY = static_cast<float>(std::max(std::max(g_state.canvasHeight, g_state.previewHeight), 1080) + 1000);

			for (float designX = 0.0f; designX <= rulerMaxX; designX += minor)
			{
				const float sx = artMin.x + designX * scale;
				if (sx < viewportMin.x + ruler || sx > viewportMax.x)
					continue;

				const bool isMajor = std::fmod(designX, major) == 0.0f;
				const float tickHeight = isMajor ? 12.0f : 6.0f;
				drawList->AddLine(ImVec2(sx, viewportMin.y + ruler), ImVec2(sx, viewportMin.y + ruler - tickHeight), tickColor);

				if (isMajor)
				{
					char label[32];
					std::snprintf(label, sizeof(label), "%.0f", designX);
					drawList->AddText(ImVec2(sx + 3.0f, viewportMin.y + 3.0f), tickColor, label);
				}
			}

			for (float designY = 0.0f; designY <= rulerMaxY; designY += minor)
			{
				const float sy = artMin.y + designY * scale;
				if (sy < viewportMin.y + ruler || sy > viewportMax.y)
					continue;

				const bool isMajor = std::fmod(designY, major) == 0.0f;
				const float tickWidth = isMajor ? 12.0f : 6.0f;
				drawList->AddLine(ImVec2(viewportMin.x + ruler, sy), ImVec2(viewportMin.x + ruler - tickWidth, sy), tickColor);

				if (isMajor)
				{
					char label[32];
					std::snprintf(label, sizeof(label), "%.0f", designY);
					drawList->AddText(ImVec2(viewportMin.x + 3.0f, sy + 3.0f), tickColor, label);
				}
			}
		}

		if (g_state.showGuides)
		{
			for (int i = 0; i < static_cast<int>(g_state.verticalGuides.size()); ++i)
			{
				const float sx = artMin.x + g_state.verticalGuides[static_cast<std::size_t>(i)] * scale;
				if (sx < viewportMin.x || sx > viewportMax.x)
					continue;

				drawList->AddLine(
					ImVec2(sx, viewportMin.y),
					ImVec2(sx, viewportMax.y),
					(g_state.activeGuideAxis == 1 && g_state.activeGuideIndex == i) ? guideActiveColor : guideColor,
					1.0f);
			}

			for (int i = 0; i < static_cast<int>(g_state.horizontalGuides.size()); ++i)
			{
				const float sy = artMin.y + g_state.horizontalGuides[static_cast<std::size_t>(i)] * scale;
				if (sy < viewportMin.y || sy > viewportMax.y)
					continue;

				drawList->AddLine(
					ImVec2(viewportMin.x, sy),
					ImVec2(viewportMax.x, sy),
					(g_state.activeGuideAxis == 2 && g_state.activeGuideIndex == i) ? guideActiveColor : guideColor,
					1.0f);
			}
		}
	}


	void HandleGuideInteraction(
		const ImVec2& viewportMin,
		const ImVec2& viewportMax,
		const ImVec2& artMin,
		const ImVec2& artMax,
		float scale)
	{
		if (!g_state.showRulers)
			return;

		const float ruler = 24.0f;
		const float hitSlop = 6.0f;
		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyShift)
		{
			// Shift + left drag is reserved for rect marquee selection.  Do not create,
			// move, or delete guides while that selection gesture is active.
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				g_state.activeGuideAxis = 0;
				g_state.activeGuideIndex = -1;
			}
			return;
		}

		const ImVec2 mouse = io.MousePos;
		const bool topRuler = IsPointInRect(mouse, ImVec2(viewportMin.x + ruler, viewportMin.y), ImVec2(viewportMax.x, viewportMin.y + ruler));
		const bool leftRuler = IsPointInRect(mouse, ImVec2(viewportMin.x, viewportMin.y + ruler), ImVec2(viewportMin.x + ruler, viewportMax.y));
		const bool inViewport = IsPointInRect(mouse, viewportMin, viewportMax);

		int hoverAxis = 0;
		int hoverIndex = -1;

		if (g_state.showGuides && inViewport)
		{
			for (int i = 0; i < static_cast<int>(g_state.verticalGuides.size()); ++i)
			{
				const float sx = artMin.x + g_state.verticalGuides[static_cast<std::size_t>(i)] * scale;
				if (std::fabs(mouse.x - sx) <= hitSlop)
				{
					hoverAxis = 1;
					hoverIndex = i;
					break;
				}
			}

			if (hoverAxis == 0)
			{
				for (int i = 0; i < static_cast<int>(g_state.horizontalGuides.size()); ++i)
				{
					const float sy = artMin.y + g_state.horizontalGuides[static_cast<std::size_t>(i)] * scale;
					if (std::fabs(mouse.y - sy) <= hitSlop)
					{
						hoverAxis = 2;
						hoverIndex = i;
						break;
					}
				}
			}
		}

		if (g_state.activeGuideAxis == 0)
		{
			g_state.activeGuideAxis = hoverAxis;
			g_state.activeGuideIndex = hoverIndex;
		}

		if (hoverAxis != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			if (hoverAxis == 1)
				RemoveGuide(g_state.verticalGuides, hoverIndex);
			else if (hoverAxis == 2)
				RemoveGuide(g_state.horizontalGuides, hoverIndex);

			g_state.activeGuideAxis = 0;
			g_state.activeGuideIndex = -1;
			return;
		}

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (hoverAxis != 0)
			{
				g_state.activeGuideAxis = hoverAxis;
				g_state.activeGuideIndex = hoverIndex;
			}
			else if (topRuler)
			{
				g_state.verticalGuides.push_back(ScreenToDesignX(mouse.x, artMin, scale));
				g_state.activeGuideAxis = 1;
				g_state.activeGuideIndex = static_cast<int>(g_state.verticalGuides.size()) - 1;
			}
			else if (leftRuler)
			{
				g_state.horizontalGuides.push_back(ScreenToDesignY(mouse.y, artMin, scale));
				g_state.activeGuideAxis = 2;
				g_state.activeGuideIndex = static_cast<int>(g_state.horizontalGuides.size()) - 1;
			}
		}

		if (g_state.activeGuideAxis != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (g_state.activeGuideAxis == 1 && g_state.activeGuideIndex >= 0 && g_state.activeGuideIndex < static_cast<int>(g_state.verticalGuides.size()))
				g_state.verticalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)] = ScreenToDesignX(mouse.x, artMin, scale);
			else if (g_state.activeGuideAxis == 2 && g_state.activeGuideIndex >= 0 && g_state.activeGuideIndex < static_cast<int>(g_state.horizontalGuides.size()))
				g_state.horizontalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)] = ScreenToDesignY(mouse.y, artMin, scale);
		}

		if (g_state.activeGuideAxis != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			if (g_state.activeGuideAxis == 1 && g_state.activeGuideIndex >= 0 && g_state.activeGuideIndex < static_cast<int>(g_state.verticalGuides.size()))
			{
				const float value = g_state.verticalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)];
				if (topRuler || value < 0.0f || value > static_cast<float>(g_state.canvasWidth))
					RemoveGuide(g_state.verticalGuides, g_state.activeGuideIndex);
				else
					g_state.verticalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)] = std::max(0.0f, std::min(static_cast<float>(g_state.canvasWidth), value));
			}
			else if (g_state.activeGuideAxis == 2 && g_state.activeGuideIndex >= 0 && g_state.activeGuideIndex < static_cast<int>(g_state.horizontalGuides.size()))
			{
				const float value = g_state.horizontalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)];
				if (leftRuler || value < 0.0f || value > static_cast<float>(g_state.canvasHeight))
					RemoveGuide(g_state.horizontalGuides, g_state.activeGuideIndex);
				else
					g_state.horizontalGuides[static_cast<std::size_t>(g_state.activeGuideIndex)] = std::max(0.0f, std::min(static_cast<float>(g_state.canvasHeight), value));
			}

			g_state.activeGuideAxis = 0;
			g_state.activeGuideIndex = -1;
		}

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && hoverAxis == 0)
		{
			g_state.activeGuideAxis = 0;
			g_state.activeGuideIndex = -1;
		}
	}


	void DrawCell(ImDrawList* drawList, const ImVec2& artboard, float scale, float x, float y, float w, float h, const char* label, bool selected)
	{
		const ImVec2 min(artboard.x + x * scale, artboard.y + y * scale);
		const ImVec2 max(min.x + w * scale, min.y + h * scale);
		const ImU32 border = selected ? ColorU32(AccentCyan()) : IM_COL32(210, 130, 45, 180);
		const ImU32 fill = selected ? IM_COL32(30, 90, 110, 70) : IM_COL32(70, 52, 30, 55);

		drawList->AddRectFilled(min, max, fill);
		drawList->AddRect(min, max, border, 0.0f, 0, selected ? 2.0f : 1.0f);
		drawList->AddText(ImVec2(min.x + 6.0f, min.y + 5.0f), border, label);

		if (!selected)
			return;

		const float handle = 5.0f;
		const ImU32 handleColor = IM_COL32(230, 245, 255, 255);
		drawList->AddRectFilled(ImVec2(min.x - handle, min.y - handle), ImVec2(min.x + handle, min.y + handle), handleColor);
		drawList->AddRectFilled(ImVec2(max.x - handle, min.y - handle), ImVec2(max.x + handle, min.y + handle), handleColor);
		drawList->AddRectFilled(ImVec2(min.x - handle, max.y - handle), ImVec2(min.x + handle, max.y + handle), handleColor);
		drawList->AddRectFilled(ImVec2(max.x - handle, max.y - handle), ImVec2(max.x + handle, max.y + handle), handleColor);
	}


	ImVec2 ClampUv(const ImVec2& uv)
	{
		return ImVec2(
			std::max(0.0f, std::min(1.0f, uv.x)),
			std::max(0.0f, std::min(1.0f, uv.y)));
	}

	void ResolveLegacyImageUvsFromPixels(
		const UiEditorFitCell& cell,
		const UiEditorImageTexture& image,
		float atlasX,
		float atlasY,
		float atlasWidth,
		float atlasHeight,
		ImVec2& uv0,
		ImVec2& uv1)
	{
		uv0 = ImVec2(0.0f, 0.0f);
		uv1 = ImVec2(1.0f, 1.0f);

		if (!cell.hasUvPixels || image.width <= 0 || image.height <= 0 || atlasWidth <= 0.0f || atlasHeight <= 0.0f)
			return;

		const float fileWidth = static_cast<float>(image.width);
		const float fileHeight = static_cast<float>(image.height);
		const float biasX = 0.1f / fileWidth;
		const float biasY = 0.1f / fileHeight;

		// First try the coordinates as pixels in the actual converted PNG.  This is
		// required for generated per-piece legacy art such as loading-screen tiles
		// where uvWidth/uvHeight match the file itself.
		if (atlasX >= 0.0f
			&& atlasY >= 0.0f
			&& atlasX + atlasWidth <= fileWidth + 0.5f
			&& atlasY + atlasHeight <= fileHeight + 0.5f)
		{
			uv0 = ClampUv(ImVec2((atlasX / fileWidth) + biasX, (atlasY / fileHeight) + biasY));
			uv1 = ClampUv(ImVec2(((atlasX + atlasWidth) / fileWidth) + biasX, ((atlasY + atlasHeight) / fileHeight) + biasY));
			return;
		}

		// Fallback for legacy 256-era atlas coordinates sampled from a scaled atlas.
		const float logicalAtlasSize = 256.0f;
		const float scaleX = fileWidth / logicalAtlasSize;
		const float scaleY = fileHeight / logicalAtlasSize;

		uv0 = ClampUv(ImVec2(
			((atlasX * scaleX) / fileWidth) + biasX,
			((atlasY * scaleY) / fileHeight) + biasY));

		uv1 = ClampUv(ImVec2(
			(((atlasX + atlasWidth) * scaleX) / fileWidth) + biasX,
			(((atlasY + atlasHeight) * scaleY) / fileHeight) + biasY));
	}

	void ResolveLegacyImageUvs(
		const UiEditorFitCell& cell,
		const UiEditorImageTexture& image,
		UiEditorButtonVisualState buttonState,
		ImVec2& uv0,
		ImVec2& uv1)
	{
		float atlasX = cell.uvX;
		float atlasY = cell.uvY;

		if (buttonState == ButtonVisualDisabled && cell.hasDisabledUvPixels)
		{
			atlasX = cell.uvDisabledX;
			atlasY = cell.uvDisabledY;
		}
		else if (buttonState == ButtonVisualPressed && cell.hasPressedUvPixels)
		{
			atlasX = cell.uvPressedX;
			atlasY = cell.uvPressedY;
		}
		else if ((buttonState == ButtonVisualHover || buttonState == ButtonVisualSelected) && cell.hasHighlightUvPixels)
		{
			atlasX = cell.uvHighlightX;
			atlasY = cell.uvHighlightY;
		}

		ResolveLegacyImageUvsFromPixels(cell, image, atlasX, atlasY, cell.uvWidth, cell.uvHeight, uv0, uv1);
	}

	void DrawCellLabel(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const char* label, ImU32 color, bool selected);

	void DrawCellImage(
		ImDrawList* drawList,
		const UiEditorFitCell& cell,
		const ImVec2& min,
		const ImVec2& max,
		bool editable,
		UiEditorButtonVisualState buttonState)
	{
		if (!g_state.showImages || cell.texture.empty())
			return;

		const UiEditorImageTexture* image = UiEditorImageCache_Get(cell.texture.c_str());
		if (image == nullptr || !image->loaded || !image->textureId)
		{
			if (g_state.showImageDiagnostics)
			{
				drawList->PushClipRect(min, max, true);
				drawList->AddRectFilled(min, max, IM_COL32(90, 25, 25, 70));
				drawList->AddRect(min, max, IM_COL32(255, 90, 90, 190), 0.0f, 0, 1.5f);
				DrawCellLabel(drawList, min, max, cell.texture.c_str(), IM_COL32(255, 180, 180, 255), true);
				drawList->PopClipRect();
			}
			return;
		}

		ImVec2 uv0;
		ImVec2 uv1;
		ResolveLegacyImageUvs(cell, *image, buttonState, uv0, uv1);

		const ImU32 tint = editable ? IM_COL32(255, 255, 255, 240) : IM_COL32(160, 170, 180, 130);

		drawList->PushClipRect(min, max, true);
		drawList->AddImage(
			image->textureId,
			min,
			max,
			uv0,
			uv1,
			tint);

		if (g_state.showImageDiagnostics)
		{
			const ImU32 diagColor = cell.hasUvPixels ? IM_COL32(80, 210, 255, 190) : IM_COL32(255, 210, 80, 180);
			drawList->AddRect(min, max, diagColor, 0.0f, 0, 1.0f);
			const char* mode = cell.hasUvPixels ? "atlas uvPixels" : "whole image";
			DrawCellLabel(drawList, min, max, mode, diagColor, false);
		}

		drawList->PopClipRect();
	}

	const float* ButtonOverlayColorForState(const UiEditorFitCell& cell, UiEditorButtonVisualState buttonState, bool& hasColor)
	{
		hasColor = false;
		if (buttonState == ButtonVisualDisabled && cell.hasDisabledOverlayColor)
		{
			hasColor = true;
			return cell.disabledOverlayColor;
		}
		if (buttonState == ButtonVisualPressed && cell.hasPressedOverlayColor)
		{
			hasColor = true;
			return cell.pressedOverlayColor;
		}
		if ((buttonState == ButtonVisualHover || buttonState == ButtonVisualSelected) && cell.hasHighlightOverlayColor)
		{
			hasColor = true;
			return cell.highlightOverlayColor;
		}
		if (cell.hasNormalOverlayColor)
		{
			hasColor = true;
			return cell.normalOverlayColor;
		}
		return cell.textColor;
	}

	void DrawButtonColorOverlay(ImDrawList* drawList, const UiEditorFitCell& cell, const ImVec2& min, const ImVec2& max, UiEditorButtonVisualState buttonState)
	{
		if (!cell.buttonOverlayEnabled)
			return;

		bool hasOverlayColor = false;
		const float* overlayColor = ButtonOverlayColorForState(cell, buttonState, hasOverlayColor);
		if (!hasOverlayColor)
			return;

		float alpha = std::max(0.0f, std::min(1.0f, cell.buttonOverlayAlpha));
		if (cell.pulseEnabled && buttonState != ButtonVisualDisabled)
		{
			const float speed = std::max(0.1f, cell.pulseSpeed);
			const float wave = static_cast<float>((std::sin(ImGui::GetTime() * speed * 6.2831853) + 1.0) * 0.5);
			alpha *= (0.40f + 0.60f * wave);
		}

		drawList->PushClipRect(min, max, true);
		drawList->AddRectFilled(min, max, CellColorU32(overlayColor, alpha));
		drawList->PopClipRect();
	}


	void DrawCellLabel(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const char* label, ImU32 color, bool selected);

	bool IsTextRenderableCell(const UiEditorFitCell& cell)
	{
		if (cell.type == "GuiText" || cell.type == "GuiButton")
			return true;

		if (!cell.visibleText.empty() || !cell.textKey.empty() || cell.textMissing)
			return true;

		return ContainsInsensitive(cell.type, "Text");
	}

	bool IsListCell(const UiEditorFitCell& cell)
	{
		return cell.type == "GuiList" || cell.isListCell;
	}

	void DrawListPreview(ImDrawList* drawList, const UiEditorFitCell& cell, const ImVec2& min, const ImVec2& max, bool selected)
	{
		const float height = max.y - min.y;
		const float width  = max.x - min.x;

		const ImU32 bg       = IM_COL32(26, 26, 26, 204);
		const ImU32 border   = selected ? ColorU32(AccentCyan()) : IM_COL32(0, 110, 180, 200);
		const float cR = std::max(0.0f, std::min(1.0f, cell.highlightColor[0]));
		const float cG = std::max(0.0f, std::min(1.0f, cell.highlightColor[1]));
		const float cB = std::max(0.0f, std::min(1.0f, cell.highlightColor[2]));
		const float cA = std::max(0.0f, std::min(1.0f, cell.highlightColor[3]));
		const ImU32 highlight = IM_COL32(
			static_cast<int>(cR * 255),
			static_cast<int>(cG * 255),
			static_cast<int>(cB * 255),
			static_cast<int>(cA * 255));
		const ImU32 rowText = IM_COL32(190, 200, 210, 160);
		const float borderW = selected ? 2.0f : 1.0f;

		drawList->AddRectFilled(min, max, bg);
		drawList->AddRect(min, max, border, 0.0f, 0, borderW);

		const int rowCount = std::max(1, cell.listPreviewItemCount);
		const float rowH = (height - 2.0f) / static_cast<float>(rowCount);
		const float indent = 4.0f;

		drawList->PushClipRect(min, max, true);

		// Row 0 is drawn as "selected"
		drawList->AddRectFilled(
			ImVec2(min.x + 1.0f, min.y + 1.0f),
			ImVec2(max.x - 1.0f, min.y + 1.0f + rowH),
			highlight);

		// Remaining rows: dashed text-stub lines
		for (int r = 1; r < rowCount; ++r)
		{
			const float rowTop = min.y + 1.0f + r * rowH;
			const float lineY  = rowTop + rowH * 0.45f;
			if (lineY + 2.0f > max.y - 1.0f)
				break;
			const float lineLen = width - indent * 2.0f - (r & 1 ? 30.0f : 10.0f);
			if (lineLen > 4.0f)
				drawList->AddLine(ImVec2(min.x + indent, lineY), ImVec2(min.x + indent + lineLen, lineY), rowText, 1.0f);
		}

		drawList->PopClipRect();
	}

	bool IsScrollbarCell(const UiEditorFitCell& cell)
	{
		const bool scrollbarTyped = cell.type == "GuiScrollbar" || ContainsInsensitive(cell.role, "scrollbar");
		if (!scrollbarTyped)
			return false;

		// ScrollBar.fit exposes the up/down/thumb sprites as individual atlas-backed
		// parts. Those must render as images; only runtime-created parent scrollbars
		// should use the synthesized whole-scrollbar preview.
		if (!cell.texture.empty()
			&& (ContainsInsensitive(cell.role, "up_button")
				|| ContainsInsensitive(cell.role, "down_button")
				|| ContainsInsensitive(cell.role, "thumb")))
		{
			return false;
		}

		return true;
	}

	bool Is3DViewCell(const UiEditorFitCell& cell)
	{
		return cell.type == "Gui3DView"
			|| ContainsInsensitive(cell.role, "3d_view")
			|| ContainsInsensitive(cell.role, "viewport");
	}

	void DrawLegacyScrollbarPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool selected)
	{
		const float width = std::max(6.0f, max.x - min.x);
		const float height = std::max(12.0f, max.y - min.y);
		const float buttonHeight = std::min(12.0f, std::max(6.0f, height * 0.18f));
		const ImU32 track = IM_COL32(0, 47, 85, 150);
		const ImU32 border = selected ? ColorU32(AccentCyan()) : IM_COL32(0, 120, 190, 210);
		const ImU32 button = IM_COL32(170, 105, 20, 220);
		const ImU32 thumb = IM_COL32(205, 125, 25, 220);

		drawList->AddRectFilled(min, max, track);
		drawList->AddRect(min, max, border, 0.0f, 0, selected ? 2.0f : 1.0f);

		const ImVec2 topMin(min.x + 1.0f, min.y + 1.0f);
		const ImVec2 topMax(max.x - 1.0f, min.y + buttonHeight);
		const ImVec2 bottomMin(min.x + 1.0f, max.y - buttonHeight);
		const ImVec2 bottomMax(max.x - 1.0f, max.y - 1.0f);
		drawList->AddRectFilled(topMin, topMax, button);
		drawList->AddRectFilled(bottomMin, bottomMax, button);

		const float thumbHeight = std::min(22.0f, std::max(10.0f, height * 0.20f));
		const float thumbY = std::min(bottomMin.y - thumbHeight - 2.0f, topMax.y + 2.0f);
		drawList->AddRectFilled(
			ImVec2(min.x + 2.0f, thumbY),
			ImVec2(max.x - 2.0f, thumbY + thumbHeight),
			thumb);

		const float cx = min.x + width * 0.5f;
		drawList->AddTriangleFilled(
			ImVec2(cx, topMin.y + 2.0f),
			ImVec2(topMin.x + 3.0f, topMax.y - 3.0f),
			ImVec2(topMax.x - 3.0f, topMax.y - 3.0f),
			IM_COL32(20, 22, 24, 230));
		drawList->AddTriangleFilled(
			ImVec2(cx, bottomMax.y - 2.0f),
			ImVec2(bottomMin.x + 3.0f, bottomMin.y + 3.0f),
			ImVec2(bottomMax.x - 3.0f, bottomMin.y + 3.0f),
			IM_COL32(20, 22, 24, 230));
	}

	void DrawLegacy3DViewPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, bool selected)
	{
		const ImU32 border = selected ? ColorU32(AccentCyan()) : IM_COL32(0, 92, 150, 190);
		drawList->AddRectFilled(min, max, IM_COL32(2, 8, 12, 210));
		drawList->AddRect(min, max, border, 0.0f, 0, selected ? 2.0f : 1.0f);

		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float radius = std::max(8.0f, std::min(max.x - min.x, max.y - min.y) * 0.22f);
		drawList->AddCircle(center, radius, IM_COL32(0, 120, 210, 120), 24, 1.0f);
		drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(0, 120, 210, 90), 1.0f);
		drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(0, 120, 210, 90), 1.0f);
		DrawCellLabel(drawList, min, max, "3D View", border, selected);
	}

	enum UiEditorTextAnchorAxis
	{
		TextAnchorAxisStart,
		TextAnchorAxisCenter,
		TextAnchorAxisEnd
	};

	UiEditorTextAnchorAxis HorizontalTextAnchorAxis(const std::string& anchor)
	{
		const std::string lower = ToLowerCopy(anchor);
		if (lower.find("right") != std::string::npos)
			return TextAnchorAxisEnd;
		if (lower.find("left") != std::string::npos)
			return TextAnchorAxisStart;
		return TextAnchorAxisCenter;
	}

	UiEditorTextAnchorAxis VerticalTextAnchorAxis(const std::string& anchor)
	{
		const std::string lower = ToLowerCopy(anchor);
		if (lower.find("bottom") != std::string::npos || lower.find("lower") != std::string::npos)
			return TextAnchorAxisEnd;
		if (lower.find("top") != std::string::npos || lower.find("upper") != std::string::npos || lower.empty())
			return TextAnchorAxisStart;
		return TextAnchorAxisCenter;
	}

	std::string EffectiveTextAnchor(const UiEditorFitCell& cell)
	{
		if (!cell.textAnchor.empty())
			return cell.textAnchor;

		const std::string align = ToLowerCopy(cell.textAlign);
		if (align == "center" || align == "middle")
			return "center";
		if (align == "right")
			return "upper_right";

		return "upper_left";
	}

	ImVec2 AnchoredTextPosition(const ImVec2& min, const ImVec2& max, const ImVec2& textSize, const std::string& anchor)
	{
		const float padding = 6.0f;
		ImVec2 pos(min.x + padding, min.y + 5.0f);

		switch (HorizontalTextAnchorAxis(anchor))
		{
		case TextAnchorAxisEnd:
			pos.x = max.x - textSize.x - padding;
			break;
		case TextAnchorAxisCenter:
			pos.x = min.x + std::max(padding, ((max.x - min.x) - textSize.x) * 0.5f);
			break;
		case TextAnchorAxisStart:
		default:
			pos.x = min.x + padding;
			break;
		}

		switch (VerticalTextAnchorAxis(anchor))
		{
		case TextAnchorAxisEnd:
			pos.y = max.y - textSize.y - padding;
			break;
		case TextAnchorAxisCenter:
			pos.y = min.y + std::max(padding, ((max.y - min.y) - textSize.y) * 0.5f);
			break;
		case TextAnchorAxisStart:
		default:
			pos.y = min.y + padding;
			break;
		}

		pos.x = std::max(min.x + padding, std::min(pos.x, max.x - padding));
		pos.y = std::max(min.y + padding, std::min(pos.y, max.y - padding));
		return pos;
	}

	void DrawCellTextPreview(
		ImDrawList* drawList,
		const UiEditorFitCell& cell,
		const ImVec2& min,
		const ImVec2& max,
		float scale,
		UiEditorButtonVisualState buttonState)
	{
		if (IsRuntimeHelpTextCell(cell))
			return;

		std::string textValue = !cell.visibleText.empty() ? cell.visibleText : cell.textKey;
		if (IsUndefinedLegacyStringKey(textValue))
			textValue.clear();
		if (textValue.empty() && cell.textMissing)
			textValue = "<missing text>";

		if (textValue.empty())
			return;

		const float textPadding = 4.0f;
		const float boxWidth = std::max(1.0f, max.x - min.x);
		const float wrapWidth = cell.wrapText ? std::max(1.0f, boxWidth - textPadding * 2.0f) : 0.0f;
		const float requestedFontSize = (cell.fontSize > 0.0f) ? cell.fontSize * scale : ImGui::GetFontSize();
		const float drawFontSize = std::max(1.0f, requestedFontSize);
		ImFont* font = FontForCell(cell);

		const char* text = textValue.c_str();
		const float calcWrapWidth = cell.wrapText ? wrapWidth : 0.0f;
		ImVec2 textSize = font->CalcTextSizeA(drawFontSize, FLT_MAX, calcWrapWidth, text, nullptr);
		if (cell.wrapText)
			textSize.x = std::min(textSize.x, wrapWidth);

		const bool centerButtonText = IsButtonLikeCell(cell) && !cell.hasTextRect;
		const std::string anchor = centerButtonText ? "center" : EffectiveTextAnchor(cell);
		ImVec2 textPos = AnchoredTextPosition(min, max, textSize, anchor);
		if (!centerButtonText && cell.textAnchor.empty())
		{
			if (cell.textAlign == "center")
				textPos.x = min.x + std::max(textPadding, ((max.x - min.x) - textSize.x) * 0.5f);
			else if (cell.textAlign == "right")
				textPos.x = max.x - textSize.x - textPadding;
		}

		const float* stateColor = cell.textColor;
		if (buttonState == ButtonVisualDisabled && cell.hasDisabledTextColor)
			stateColor = cell.disabledTextColor;
		else if (buttonState == ButtonVisualPressed && cell.hasPressedTextColor)
			stateColor = cell.pressedTextColor;
		else if ((buttonState == ButtonVisualHover || buttonState == ButtonVisualSelected) && cell.hasHighlightTextColor)
			stateColor = cell.highlightTextColor;
		else if (cell.hasNormalTextColor)
			stateColor = cell.normalTextColor;

		float textAlpha = 1.0f;
		float blendedColor[4] = { stateColor[0], stateColor[1], stateColor[2], stateColor[3] };
		const bool pulseOrFlash = cell.pulseEnabled
			|| ToLowerCopy(cell.textEffect).find("pulse") != std::string::npos
			|| ToLowerCopy(cell.textEffect).find("flash") != std::string::npos;
		if (pulseOrFlash && buttonState != ButtonVisualDisabled)
		{
			const float speed = std::max(0.1f, cell.pulseSpeed);
			const float wave = static_cast<float>((std::sin(ImGui::GetTime() * speed * 6.2831853) + 1.0) * 0.5);
			if (cell.hasNormalTextColor && cell.hasHighlightTextColor)
			{
				for (int i = 0; i < 4; ++i)
					blendedColor[i] = cell.normalTextColor[i] * (1.0f - wave) + cell.highlightTextColor[i] * wave;
			}
			else
			{
				textAlpha = 0.45f + 0.55f * wave;
			}
		}

		const ImU32 textColor = cell.textMissing ? IM_COL32(255, 130, 95, static_cast<int>(255.0f * textAlpha)) : CellColorU32(blendedColor, textAlpha);
		drawList->PushClipRect(min, max, true);
		drawList->AddText(font, drawFontSize, textPos, textColor, text, nullptr, wrapWidth);
		drawList->PopClipRect();
	}

	void DrawCellLabel(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const char* label, ImU32 color, bool selected)
	{
		if (label == nullptr || label[0] == '\0')
			return;

		const ImVec2 textPos(min.x + 5.0f, min.y + 4.0f);
		const ImVec2 textSize = ImGui::CalcTextSize(label);
		const ImVec2 bgMax(
			std::min(max.x, textPos.x + textSize.x + 8.0f),
			std::min(max.y, textPos.y + textSize.y + 6.0f));

		if (bgMax.x > textPos.x && bgMax.y > textPos.y)
		{
			drawList->AddRectFilled(
				ImVec2(textPos.x - 3.0f, textPos.y - 2.0f),
				bgMax,
				selected ? IM_COL32(5, 18, 24, 220) : IM_COL32(8, 10, 12, 185));
		}

		drawList->PushClipRect(min, max, true);
		drawList->AddText(textPos, color, label);
		drawList->PopClipRect();
	}

	bool ShouldDrawCellLabel(bool selected, bool hovered, bool editable)
	{
		if (g_state.showLabels)
			return true;

		if (selected || hovered)
			return true;

		// When images are hidden, labels are the main visual identity for cells.
		if (!g_state.showImages && editable)
			return true;

		return false;
	}

	UiEditorFitCell* FindMutableCellByKey(const std::string& key)
	{
		if (key.empty())
			return nullptr;

		for (std::size_t i = 0; i < g_state.layout.cells.size(); ++i)
		{
			if (g_state.layout.cells[i].key == key)
				return &g_state.layout.cells[i];
		}

		return nullptr;
	}

	void DrawLoadedCell(ImDrawList* drawList, const ImVec2& artboard, float scale, const ImVec2& displayOffset, UiEditorFitCell& cell, int cellIndex, bool editable)
	{
		if (editable && !cell.visible)
			return;

		if (editable && !cell.linkedButtonKey.empty() && !cell.buttonTemplatePart.empty())
			return;

		const bool locked = editable && cell.locked;
		const bool transformable = editable && !locked;
		const bool selected = editable && IsCellSelectedForPreview(cellIndex);
		const float displayX = cell.x + displayOffset.x;
		const float displayY = cell.y + displayOffset.y;
		const ImVec2 min(artboard.x + displayX * scale, artboard.y + displayY * scale);
		const ImVec2 max(min.x + cell.width * scale, min.y + cell.height * scale);
		const bool productionCell = IsSmartProductionCell(cell);
		const bool buttonLike = IsButtonLikeCell(cell);
		const float handle = 5.0f;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool hovered = ImGui::IsMouseHoveringRect(min, max, true);

		bool previewSelected = cell.selectedState || cell.checked;
		bool previewDisabled = cell.disabledState;
		bool previewPressed = false;
		bool previewHover = buttonLike && hovered;
		if (g_state.simulationMode && buttonLike)
		{
			previewHover = previewHover || (g_state.simulationPreviewState == 1);
			previewPressed = (g_state.simulationPreviewState == 2);
			previewSelected = previewSelected || (g_state.simulationPreviewState == 3);
			previewDisabled = previewDisabled || (g_state.simulationPreviewState == 4);
		}

		UiEditorButtonVisualState buttonState = ButtonVisualNormal;
		if (buttonLike)
		{
			if (previewDisabled)
				buttonState = ButtonVisualDisabled;
			else if (previewPressed)
				buttonState = ButtonVisualPressed;
			else if (previewHover)
				buttonState = ButtonVisualHover;
			else if (previewSelected)
				buttonState = ButtonVisualSelected;
		}

		ImU32 border = selected ? ColorU32(AccentCyan()) : (productionCell ? CellColorU32(cell.borderColor, cell.opacity) : (transformable ? IM_COL32(210, 130, 45, 170) : IM_COL32(110, 118, 128, 110)));
		ImU32 fill = productionCell ? CellColorU32(cell.fillColor, cell.opacity) : (selected ? IM_COL32(30, 90, 110, 70) : (transformable ? IM_COL32(70, 52, 30, 42) : IM_COL32(35, 42, 48, 42)));

		if (buttonLike && (previewHover || previewPressed || previewSelected || previewDisabled))
		{
			if (previewDisabled)
			{
				fill = IM_COL32(58, 62, 66, 90);
				border = IM_COL32(110, 114, 120, 160);
			}
			else if (previewPressed)
			{
				fill = IM_COL32(20, 75, 105, 90);
				border = IM_COL32(90, 210, 255, 230);
			}
			else if (previewHover || previewSelected)
			{
				fill = IM_COL32(38, 78, 100, 72);
				border = IM_COL32(255, 255, 255, 220);
			}
		}

		if (buttonLike && cell.pulseEnabled && !previewDisabled && (previewHover || previewPressed || previewSelected))
		{
			const float t = static_cast<float>((std::sin(ImGui::GetTime() * std::max(0.1f, cell.pulseSpeed) * 6.2831853) + 1.0) * 0.5);
			const int c = static_cast<int>(150.0f + 105.0f * t);
			border = IM_COL32(c, c, 255, 235);
		}

		const ImVec2 nwMin(min.x - handle, min.y - handle);
		const ImVec2 nwMax(min.x + handle, min.y + handle);
		const ImVec2 neMin(max.x - handle, min.y - handle);
		const ImVec2 neMax(max.x + handle, min.y + handle);
		const ImVec2 swMin(min.x - handle, max.y - handle);
		const ImVec2 swMax(min.x + handle, max.y + handle);
		const ImVec2 seMin(max.x - handle, max.y - handle);
		const ImVec2 seMax(max.x + handle, max.y + handle);

		ImVec2 textRectMin(0.0f, 0.0f);
		ImVec2 textRectMax(0.0f, 0.0f);
		const bool hasEditableTextRect = transformable && selected && cell.hasTextRect && IsTextRenderableCell(cell);
		if (hasEditableTextRect)
		{
			textRectMin = ImVec2(
				artboard.x + (cell.textX + displayOffset.x) * scale,
				artboard.y + (cell.textY + displayOffset.y) * scale);
			textRectMax = ImVec2(
				textRectMin.x + cell.textWidth * scale,
				textRectMin.y + cell.textHeight * scale);
		}

		bool textRectTransformStarted = false;
		const bool leftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		if (transformable && selected && hasEditableTextRect && leftClicked)
		{
			const ImVec2 textNwMin(textRectMin.x - handle, textRectMin.y - handle);
			const ImVec2 textNwMax(textRectMin.x + handle, textRectMin.y + handle);
			const ImVec2 textNeMin(textRectMax.x - handle, textRectMin.y - handle);
			const ImVec2 textNeMax(textRectMax.x + handle, textRectMin.y + handle);
			const ImVec2 textSwMin(textRectMin.x - handle, textRectMax.y - handle);
			const ImVec2 textSwMax(textRectMin.x + handle, textRectMax.y + handle);
			const ImVec2 textSeMin(textRectMax.x - handle, textRectMax.y - handle);
			const ImVec2 textSeMax(textRectMax.x + handle, textRectMax.y + handle);

			if (IsPointInRect(mouse, textNwMin, textNwMax))
				BeginTextRectTransform(cellIndex, TransformTextResizeNW);
			else if (IsPointInRect(mouse, textNeMin, textNeMax))
				BeginTextRectTransform(cellIndex, TransformTextResizeNE);
			else if (IsPointInRect(mouse, textSwMin, textSwMax))
				BeginTextRectTransform(cellIndex, TransformTextResizeSW);
			else if (IsPointInRect(mouse, textSeMin, textSeMax))
				BeginTextRectTransform(cellIndex, TransformTextResizeSE);
			else if (IsPointInRect(mouse, textRectMin, textRectMax))
				BeginTextRectTransform(cellIndex, TransformTextMove);

			textRectTransformStarted = g_state.activeTransform != TransformNone && IsTextRectTransform(g_state.activeTransform);
		}

		if (!textRectTransformStarted && transformable && cellIndex == g_state.canvasClickTargetCell && leftClicked)
		{
			if (hasEditableTextRect)
			{
				const ImVec2 textNwMin(textRectMin.x - handle, textRectMin.y - handle);
				const ImVec2 textNwMax(textRectMin.x + handle, textRectMin.y + handle);
				const ImVec2 textNeMin(textRectMax.x - handle, textRectMin.y - handle);
				const ImVec2 textNeMax(textRectMax.x + handle, textRectMin.y + handle);
				const ImVec2 textSwMin(textRectMin.x - handle, textRectMax.y - handle);
				const ImVec2 textSwMax(textRectMin.x + handle, textRectMax.y + handle);
				const ImVec2 textSeMin(textRectMax.x - handle, textRectMax.y - handle);
				const ImVec2 textSeMax(textRectMax.x + handle, textRectMax.y + handle);

				if (IsPointInRect(mouse, textNwMin, textNwMax))
					BeginTextRectTransform(cellIndex, TransformTextResizeNW);
				else if (IsPointInRect(mouse, textNeMin, textNeMax))
					BeginTextRectTransform(cellIndex, TransformTextResizeNE);
				else if (IsPointInRect(mouse, textSwMin, textSwMax))
					BeginTextRectTransform(cellIndex, TransformTextResizeSW);
				else if (IsPointInRect(mouse, textSeMin, textSeMax))
					BeginTextRectTransform(cellIndex, TransformTextResizeSE);
				else if (IsPointInRect(mouse, textRectMin, textRectMax))
					BeginTextRectTransform(cellIndex, TransformTextMove);
				else if (IsPointInRect(mouse, nwMin, nwMax))
					BeginTransform(cellIndex, TransformResizeNW);
				else if (IsPointInRect(mouse, neMin, neMax))
					BeginTransform(cellIndex, TransformResizeNE);
				else if (IsPointInRect(mouse, swMin, swMax))
					BeginTransform(cellIndex, TransformResizeSW);
				else if (IsPointInRect(mouse, seMin, seMax))
					BeginTransform(cellIndex, TransformResizeSE);
				else if (hovered)
					BeginTransform(cellIndex, TransformMove);
			}
			else if (selected && IsPointInRect(mouse, nwMin, nwMax))
				BeginTransform(cellIndex, TransformResizeNW);
			else if (selected && IsPointInRect(mouse, neMin, neMax))
				BeginTransform(cellIndex, TransformResizeNE);
			else if (selected && IsPointInRect(mouse, swMin, swMax))
				BeginTransform(cellIndex, TransformResizeSW);
			else if (selected && IsPointInRect(mouse, seMin, seMax))
				BeginTransform(cellIndex, TransformResizeSE);
			else if (hovered)
				BeginTransform(cellIndex, TransformMove);
		}

		const bool scrollbarCell = IsScrollbarCell(cell);
		const bool view3DCell = Is3DViewCell(cell);
		const bool listCell = IsListCell(cell);

		if (scrollbarCell)
		{
			DrawLegacyScrollbarPreview(drawList, min, max, selected);
		}
		else if (view3DCell)
		{
			DrawLegacy3DViewPreview(drawList, min, max, selected);
		}
		else if (listCell)
		{
			DrawListPreview(drawList, cell, min, max, selected);
		}
		else
		{
			ImVec2 artMin = min;
			ImVec2 artMax = max;
			if (buttonLike && cell.hasArtRect)
			{
				artMin = ImVec2(artboard.x + (cell.artX + displayOffset.x) * scale, artboard.y + (cell.artY + displayOffset.y) * scale);
				artMax = ImVec2(artMin.x + cell.artWidth * scale, artMin.y + cell.artHeight * scale);
			}

			drawList->AddRectFilled(min, max, fill);
			DrawCellImage(drawList, cell, artMin, artMax, transformable, buttonState);
			if (buttonLike)
				DrawButtonColorOverlay(drawList, cell, artMin, artMax, buttonState);

			if (buttonLike && !cell.linkedImageKey.empty())
			{
				UiEditorFitCell* linkedImage = FindMutableCellByKey(cell.linkedImageKey);
				if (linkedImage != nullptr)
				{
					const ImVec2 linkedMin(artboard.x + (linkedImage->x + displayOffset.x) * scale, artboard.y + (linkedImage->y + displayOffset.y) * scale);
					const ImVec2 linkedMax(linkedMin.x + linkedImage->width * scale, linkedMin.y + linkedImage->height * scale);
					DrawCellImage(drawList, *linkedImage, linkedMin, linkedMax, false, buttonState);
				}
			}

			if (!buttonLike || cell.borderEnabled)
			{
				ImVec2 borderMin = min;
				ImVec2 borderMax = max;
				if (buttonLike && cell.hasBorderRect)
				{
					borderMin = ImVec2(artboard.x + (cell.borderX + displayOffset.x) * scale, artboard.y + (cell.borderY + displayOffset.y) * scale);
					borderMax = ImVec2(borderMin.x + cell.borderWidthRect * scale, borderMin.y + cell.borderHeightRect * scale);
				}
				drawList->AddRect(borderMin, borderMax, border, 0.0f, 0, selected ? 2.0f : std::max(cell.borderWidth, 1.0f));
			}
		}

		const std::string label = DisplayNameForCell(cell);
		if (IsTextRenderableCell(cell)
			&& !IsRuntimeHelpTextCell(cell)
			&& (!cell.visibleText.empty() || (!cell.textKey.empty() && !IsUndefinedLegacyStringKey(cell.textKey)) || cell.textMissing))
		{
			ImVec2 textMin = min;
			ImVec2 textMax = max;
			if (cell.hasTextRect)
			{
				textMin = ImVec2(
					artboard.x + (cell.textX + displayOffset.x) * scale,
					artboard.y + (cell.textY + displayOffset.y) * scale);
				textMax = ImVec2(
					textMin.x + cell.textWidth * scale,
					textMin.y + cell.textHeight * scale);

				if (buttonLike && (selected || hovered || cell.pulseEnabled))
				{
					float glowAlpha = selected || hovered ? 170.0f : 95.0f;
					if (cell.pulseEnabled)
					{
						const float wave = static_cast<float>((std::sin(ImGui::GetTime() * std::max(0.1f, cell.pulseSpeed) * 6.2831853) + 1.0) * 0.5);
						glowAlpha = 70.0f + 150.0f * wave;
					}
					drawList->AddRect(textMin, textMax, IM_COL32(255, 190, 50, static_cast<int>(glowAlpha)), 0.0f, 0, selected ? 2.0f : 1.0f);
					if (selected && transformable)
					{
						const ImU32 textHandleColor = IM_COL32(255, 220, 90, 255);
						drawList->AddRectFilled(ImVec2(textMin.x - handle, textMin.y - handle), ImVec2(textMin.x + handle, textMin.y + handle), textHandleColor);
						drawList->AddRectFilled(ImVec2(textMax.x - handle, textMin.y - handle), ImVec2(textMax.x + handle, textMin.y + handle), textHandleColor);
						drawList->AddRectFilled(ImVec2(textMin.x - handle, textMax.y - handle), ImVec2(textMin.x + handle, textMax.y + handle), textHandleColor);
						drawList->AddRectFilled(ImVec2(textMax.x - handle, textMax.y - handle), ImVec2(textMax.x + handle, textMax.y + handle), textHandleColor);
					}
				}
			}
			DrawCellTextPreview(drawList, cell, textMin, textMax, scale, buttonState);
		}
		else if (!scrollbarCell && !view3DCell && !IsRuntimeHelpTextCell(cell) && ShouldDrawCellLabel(selected, hovered, editable))
		{
			DrawCellLabel(drawList, min, max, label.c_str(), border, selected);
		}

		if (!selected || locked)
			return;

		const ImU32 handleColor = IM_COL32(230, 245, 255, 255);
		drawList->AddRectFilled(nwMin, nwMax, handleColor);
		drawList->AddRectFilled(neMin, neMax, handleColor);
		drawList->AddRectFilled(swMin, swMax, handleColor);
		drawList->AddRectFilled(seMin, seMax, handleColor);

	}
	void DrawCanvasHud(ImDrawList* drawList, const ImVec2& canvasMin)
	{
		if (!g_state.showCanvasHud)
			return;

		const UiEditorFitCell* cell = SelectedCell();
		if (cell == nullptr)
			return;

		char nameLine[256];
		char xLine[64];
		char yLine[64];
		char wLine[64];
		char hLine[64];

		std::snprintf(nameLine, sizeof(nameLine), "%s", DisplayNameForCell(*cell).c_str());
		std::snprintf(xLine, sizeof(xLine), "X %.0f", cell->x);
		std::snprintf(yLine, sizeof(yLine), "Y %.0f", cell->y);
		std::snprintf(wLine, sizeof(wLine), "W %.0f", cell->width);
		std::snprintf(hLine, sizeof(hLine), "H %.0f", cell->height);

		const char* lines[] = { nameLine, xLine, yLine, wLine, hLine };
		const float lineHeight = ImGui::GetTextLineHeight();
		float maxWidth = 0.0f;

		for (int i = 0; i < 5; ++i)
			maxWidth = std::max(maxWidth, ImGui::CalcTextSize(lines[i]).x);

		const ImVec2 pos(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
		const ImVec2 max(pos.x + maxWidth + 14.0f, pos.y + lineHeight * 5.0f + 10.0f);

		drawList->AddRectFilled(pos, max, IM_COL32(8, 12, 16, 220));
		drawList->AddRect(pos, max, ColorU32(AccentCyan()));

		for (int i = 0; i < 5; ++i)
		{
			drawList->AddText(
				ImVec2(pos.x + 7.0f, pos.y + 5.0f + lineHeight * static_cast<float>(i)),
				IM_COL32(220, 245, 255, 255),
				lines[i]);
		}
	}

	// Additive from theirs (mc2r fork): stable draw-order sort so background/map art,
	// frames, controls, and text layer sensibly regardless of source-file cell order.
	int CanvasLayerDrawRank(const UiEditorFitCell& cell)
	{
		const std::string combined = ToLowerCopy(
			cell.type + " " + cell.role + " " + cell.layer + " " + cell.key + " "
			+ cell.texture + " " + cell.sourceControlType + " " + cell.aliasOverride);

		if (!cell.visible)
			return -100;

		if (combined.find("metadata") != std::string::npos)
			return -90;

		if (combined.find("background") != std::string::npos)
			return 0;

		// Campaign operation FITs carry the planet/map bitmap as a normal image, but
		// the legacy runtime treats it as a map background.  Draw it before mission
		// controls so play/pause/mission buttons are not buried under the planet art.
		if (combined.find("mcl_cm_op") != std::string::npos
			|| combined.find("planet") != std::string::npos
			|| combined.find("map") != std::string::npos)
			return 10;

		if (combined.find("art") != std::string::npos || combined.find("guiimage") != std::string::npos)
			return 20;

		if (combined.find("frame") != std::string::npos || combined.find("guirect") != std::string::npos)
			return 30;

		if (combined.find("button") != std::string::npos
			|| combined.find("control") != std::string::npos
			|| combined.find("scroll") != std::string::npos)
			return 60;

		if (combined.find("text") != std::string::npos || IsTextRenderableCell(cell))
			return 80;

		return 50;
	}

	template <typename LayoutT>
	void DrawLayoutCellsInCanvasOrder(
		ImDrawList* drawList,
		const ImVec2& artMin,
		float scale,
		const ImVec2& offset,
		LayoutT& layout,
		bool editable)
	{
		std::vector<int> drawOrder;
		drawOrder.reserve(layout.cells.size());
		for (int i = 0; i < static_cast<int>(layout.cells.size()); ++i)
			drawOrder.push_back(i);

		std::stable_sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b)
		{
			const int rankA = CanvasLayerDrawRank(layout.cells[static_cast<std::size_t>(a)]);
			const int rankB = CanvasLayerDrawRank(layout.cells[static_cast<std::size_t>(b)]);
			if (rankA != rankB)
				return rankA < rankB;
			return a < b;
		});

		for (std::size_t orderIndex = 0; orderIndex < drawOrder.size(); ++orderIndex)
		{
			const int cellIndex = drawOrder[orderIndex];
			DrawLoadedCell(
				drawList,
				artMin,
				scale,
				offset,
				layout.cells[static_cast<std::size_t>(cellIndex)],
				editable ? cellIndex : -1,
				editable);
		}
	}

	void DrawCanvas(const ImVec2& panelSize)
	{
		ImGui::BeginChild("canvas_panel", panelSize, true);

		if (!g_state.canvasTabs.empty())
		{
			for (int i = 0; i < static_cast<int>(g_state.canvasTabs.size()); ++i)
			{
				UiEditorCanvasTab& tab = g_state.canvasTabs[static_cast<std::size_t>(i)];
				ImGui::PushID(i);
				const bool activeTab = (i == g_state.activeCanvasTab);
				std::string tabLabel = tab.title.empty() ? "Untitled" : tab.title;
				if (tab.dirty || (activeTab && g_state.dirty))
					tabLabel += " *";
				if (activeTab)
					ImGui::PushStyleColor(ImGuiCol_Button, ColorU32(PanelBlue()));
				if (ImGui::SmallButton(tabLabel.c_str()))
					ActivateCanvasTab(i);
				if (activeTab)
					ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 2.0f);
				if (ImGui::SmallButton("x"))
				{
					CloseCanvasTab(i);
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
				ImGui::SameLine();
			}
			ImGui::NewLine();
			ImGui::Separator();
		}

		const char* layoutTitle = g_state.layout.loaded ? g_state.layout.title.c_str() : "mechlopedia/main.fit";
		ImGui::Text("%s%s", layoutTitle, g_state.dirty ? " *" : "");
		ImGui::SameLine();
		if (g_state.hasBaseLayout)
			ImGui::TextDisabled("%d active cells + %d locked base%s", static_cast<int>(g_state.layout.cells.size()), static_cast<int>(g_state.baseLayout.cells.size()), g_state.dirty ? " | modified in memory" : "");
		else
			ImGui::TextDisabled("%d cells%s", static_cast<int>(g_state.layout.cells.size()), g_state.dirty ? " | modified in memory" : "");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		SliderFloatWithDoubleClickInput("Zoom", &g_state.zoom, 0.25f, 3.0f, "%.2fx");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(72.0f);
		if (ImGui::InputInt("W", &g_state.canvasWidth, 0, 0))
		{
			g_state.canvasWidth = std::max(g_state.canvasWidth, 64);
			g_state.lastCanvasWidth = g_state.canvasWidth;
			MarkDirty();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(72.0f);
		if (ImGui::InputInt("H", &g_state.canvasHeight, 0, 0))
		{
			g_state.canvasHeight = std::max(g_state.canvasHeight, 64);
			g_state.lastCanvasHeight = g_state.canvasHeight;
			MarkDirty();
		}
		ImGui::SameLine();
		if (ImGui::Button("Canvas Options..."))
			ImGui::OpenPopup("canvas_options_popup");
		if (ImGui::BeginPopup("canvas_options_popup"))
		{
			ImGui::TextDisabled("Canvas Display");
			ImGui::Checkbox("Grid", &g_state.showGrid);
			ImGui::Checkbox("Snap to Grid", &g_state.snapToGrid);
			ImGui::Checkbox("Rulers", &g_state.showRulers);
			ImGui::Checkbox("Guides", &g_state.showGuides);
			ImGui::Checkbox("Snap to Guides", &g_state.snapToGuides);
			ImGui::Separator();
			ImGui::Checkbox("Images", &g_state.showImages);
			ImGui::Checkbox("Labels", &g_state.showLabels);
			ImGui::Checkbox("Image Diagnostics", &g_state.showImageDiagnostics);
			ImGui::Separator();
			ImGui::Checkbox("HUD", &g_state.showCanvasHud);
			const char* hudModes[] = { "Overlay", "Off-canvas" };
			ImGui::SetNextItemWidth(120.0f);
			ImGui::Combo("HUD Mode", &g_state.canvasHudMode, hudModes, IM_ARRAYSIZE(hudModes));
			ImGui::EndPopup();
		}

		ImGui::Separator();

		ImGui::TextDisabled("Profile");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(158.0f);
		if (ImGui::BeginCombo("Source", ResolutionProfileAt(g_state.sourceProfileIndex).label))
		{
			for (int i = 0; i < ResolutionProfileCount(); ++i)
			{
				const bool selected = (g_state.sourceProfileIndex == i);
				if (ImGui::Selectable(ResolutionProfileAt(i).label, selected))
				{
					ApplySourceProfile(i);
					MarkDirty();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(158.0f);
		if (ImGui::BeginCombo("Preview", ResolutionProfileAt(g_state.previewProfileIndex).label))
		{
			for (int i = 0; i < ResolutionProfileCount(); ++i)
			{
				const bool selected = (g_state.previewProfileIndex == i);
				if (ImGui::Selectable(ResolutionProfileAt(i).label, selected))
					ApplyPreviewProfile(i);
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::Checkbox("Safe", &g_state.showSafeArea);
		ImGui::SameLine();
		ImGui::TextDisabled("Scale %.2fx | %s%s",
			CurrentProfileScale(),
			AspectCategoryFor(g_state.canvasWidth, g_state.canvasHeight),
			CurrentProfileIsAspectMatched() ? "" : " -> aspect mismatch");

		if (g_state.showCanvasHud && g_state.canvasHudMode == 1)
		{
			const UiEditorFitCell* hudCell = SelectedCell();
			if (hudCell != nullptr)
				ImGui::TextDisabled("HUD: %s | X %.0f | Y %.0f | W %.0f | H %.0f",
					DisplayNameForCell(*hudCell).c_str(),
					hudCell->x,
					hudCell->y,
					hudCell->width,
					hudCell->height);
		}

		ImGuiWindowFlags canvasFlags =
			ImGuiWindowFlags_HorizontalScrollbar
			| ImGuiWindowFlags_AlwaysHorizontalScrollbar
			| ImGuiWindowFlags_AlwaysVerticalScrollbar;

		ImGui::BeginChild("canvas_viewport", ImVec2(0.0f, 0.0f), true, canvasFlags);

		if (g_state.activeCanvasTab >= 0 && g_state.activeCanvasTab < static_cast<int>(g_state.canvasTabs.size()) && !g_state.handPanningCanvas)
		{
			const UiEditorCanvasTab& activeTab = g_state.canvasTabs[static_cast<std::size_t>(g_state.activeCanvasTab)];
			if (activeTab.scrollX > 0.0f)
				ImGui::SetScrollX(activeTab.scrollX);
			if (activeTab.scrollY > 0.0f)
				ImGui::SetScrollY(activeTab.scrollY);
		}

		const ImVec2 zoomViewportMin = ImGui::GetWindowPos();
		const ImVec2 zoomViewportSize = ImGui::GetWindowSize();
		const ImVec2 zoomAnchor(
			zoomViewportMin.x + zoomViewportSize.x * 0.5f,
			zoomViewportMin.y + zoomViewportSize.y * 0.5f);
		const float oldScale = CurrentCanvasDrawScale();
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
		{
			const ImVec2 artPaddingForZoom(20.0f, 20.0f);
			const float anchorDesignX = (ImGui::GetScrollX() + zoomAnchor.x - zoomViewportMin.x - artPaddingForZoom.x) / oldScale;
			const float anchorDesignY = (ImGui::GetScrollY() + zoomAnchor.y - zoomViewportMin.y - artPaddingForZoom.y) / oldScale;

			g_state.zoom = std::max(0.25f, std::min(3.0f, g_state.zoom + wheel * 0.10f));

			const float newScale = CurrentCanvasDrawScale();
			ImGui::SetScrollX(artPaddingForZoom.x + anchorDesignX * newScale - (zoomAnchor.x - zoomViewportMin.x));
			ImGui::SetScrollY(artPaddingForZoom.y + anchorDesignY * newScale - (zoomAnchor.y - zoomViewportMin.y));
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 viewportMin = ImGui::GetWindowPos();
		const ImVec2 viewportSize = ImGui::GetWindowSize();
		const ImVec2 viewportMax(viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y);
		const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();

		drawList->AddRectFilled(viewportMin, viewportMax, IM_COL32(16, 18, 21, 255));

		const float scale = CurrentCanvasDrawScale();
		const ImVec2 artSize(g_state.canvasWidth * scale, g_state.canvasHeight * scale);
		const float rulerReserve = g_state.showRulers ? 32.0f : 0.0f;
		const float workspaceMargin = 220.0f;
		const float minWorkspaceWidth = std::max(viewportSize.x + 600.0f, static_cast<float>(std::max(g_state.previewWidth, 1920)) * scale + workspaceMargin * 2.0f);
		const float minWorkspaceHeight = std::max(viewportSize.y + 420.0f, static_cast<float>(std::max(g_state.previewHeight, 1080)) * scale + workspaceMargin * 2.0f);
		const ImVec2 virtualCanvasSize(
			std::max(artSize.x + workspaceMargin * 2.0f + rulerReserve, minWorkspaceWidth),
			std::max(artSize.y + workspaceMargin * 2.0f + rulerReserve, minWorkspaceHeight));

		const ImGuiIO& io = ImGui::GetIO();
		if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			const float framedScrollX = std::max(0.0f, (virtualCanvasSize.x - viewportSize.x) * 0.5f);
			const float framedScrollY = std::max(0.0f, (virtualCanvasSize.y - viewportSize.y) * 0.5f);
			ImGui::SetScrollX(framedScrollX);
			ImGui::SetScrollY(framedScrollY);
		}

		const ImVec2 artMin(
			contentOrigin.x + rulerReserve + std::max(workspaceMargin, (virtualCanvasSize.x - rulerReserve - artSize.x) * 0.5f),
			contentOrigin.y + rulerReserve + std::max(workspaceMargin, (virtualCanvasSize.y - rulerReserve - artSize.y) * 0.5f));
		const ImVec2 artMax(artMin.x + artSize.x, artMin.y + artSize.y);

		drawList->AddRectFilled(artMin, artMax, IM_COL32(20, 24, 28, 255));
		drawList->AddRect(artMin, artMax, IM_COL32(235, 145, 45, 220), 0.0f, 0, 2.0f);

		if (g_state.showSafeArea)
		{
			const float safePadX = artSize.x * 0.05f;
			const float safePadY = artSize.y * 0.05f;
			drawList->AddRect(
				ImVec2(artMin.x + safePadX, artMin.y + safePadY),
				ImVec2(artMax.x - safePadX, artMax.y - safePadY),
				IM_COL32(90, 170, 220, 120),
				0.0f,
				0,
				1.0f);
		}

		if (g_state.showGrid)
			DrawGrid(drawList, artMin, artSize, 20.0f * scale);

		HandleGuideInteraction(viewportMin, viewportMax, artMin, artMax, scale);
		DrawRulersAndGuides(drawList, viewportMin, viewportMax, artMin, artMax, scale);
		HandleSmartToolCreation(drawList, artMin, artMax, scale);

		const ImVec2 activeOffsetForHitTest(
			g_state.hasBaseLayout ? g_state.layout.mountOffsetX : 0.0f,
			g_state.hasBaseLayout ? g_state.layout.mountOffsetY : 0.0f);

		const bool hoveringArtForSelect = ImGui::IsMouseHoveringRect(artMin, artMax, true);
		if (g_state.activeSmartTool == SmartToolSelect
			&& !g_state.creatingSmartWidget
			&& g_state.layout.loaded
			&& ImGui::GetIO().KeyShift
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left)
			&& hoveringArtForSelect)
		{
			g_state.selectionMarqueeActive = true;
			g_state.selectionMarqueeStart = ScreenToDesignPoint(artMin, scale, ImGui::GetIO().MousePos);
			g_state.selectionMarqueeStart.x -= activeOffsetForHitTest.x;
			g_state.selectionMarqueeStart.y -= activeOffsetForHitTest.y;
			g_state.selectionMarqueeCurrent = g_state.selectionMarqueeStart;
			g_state.activeTransform = TransformNone;
			g_state.activeCell = -1;
		}

		if (g_state.selectionMarqueeActive)
		{
			g_state.selectionMarqueeCurrent = ScreenToDesignPoint(artMin, scale, ImGui::GetIO().MousePos);
			g_state.selectionMarqueeCurrent.x -= activeOffsetForHitTest.x;
			g_state.selectionMarqueeCurrent.y -= activeOffsetForHitTest.y;

			const float mx0 = std::min(g_state.selectionMarqueeStart.x, g_state.selectionMarqueeCurrent.x);
			const float my0 = std::min(g_state.selectionMarqueeStart.y, g_state.selectionMarqueeCurrent.y);
			const float mx1 = std::max(g_state.selectionMarqueeStart.x, g_state.selectionMarqueeCurrent.x);
			const float my1 = std::max(g_state.selectionMarqueeStart.y, g_state.selectionMarqueeCurrent.y);
			const ImVec2 marqueeMin(artMin.x + (mx0 + activeOffsetForHitTest.x) * scale, artMin.y + (my0 + activeOffsetForHitTest.y) * scale);
			const ImVec2 marqueeMax(artMin.x + (mx1 + activeOffsetForHitTest.x) * scale, artMin.y + (my1 + activeOffsetForHitTest.y) * scale);
			drawList->AddRectFilled(marqueeMin, marqueeMax, IM_COL32(60, 180, 230, 32));
			drawList->AddRect(marqueeMin, marqueeMax, ColorU32(AccentCyan()), 0.0f, 0, 1.0f);

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				SelectCellsInDesignRect(mx0, my0, mx1 - mx0, my1 - my0);
				g_state.selectionMarqueeActive = false;
			}
		}

		g_state.canvasClickTargetCell = -1;
		if (!g_state.selectionMarqueeActive
			&& g_state.activeSmartTool == SmartToolSelect
			&& !g_state.creatingSmartWidget
			&& g_state.layout.loaded
			&& !g_state.layout.cells.empty()
			&& !ImGui::GetIO().KeyShift
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left)
			&& hoveringArtForSelect)
		{
			g_state.canvasClickTargetCell = FindEditableCellAt(artMin, scale, activeOffsetForHitTest, ImGui::GetIO().MousePos);
			if (g_state.canvasClickTargetCell >= 0 && !IsCellSelectedForPreview(g_state.canvasClickTargetCell))
				SetSingleSelectedCell(g_state.canvasClickTargetCell);
		}

		// v0.4.17b: empty-canvas left-drag panning was removed because it
		// intercepted normal scrollbar interaction. Dedicated pan mode can be
		// added later without stealing the primary mouse button.

		UpdateActiveTransform(scale);

		if (g_state.layout.loaded && !g_state.layout.cells.empty())
		{
			if (g_state.hasBaseLayout)
			{
				const ImVec2 baseOffset(0.0f, 0.0f);
				DrawLayoutCellsInCanvasOrder(drawList, artMin, scale, baseOffset, g_state.baseLayout, false);
			}

			const ImVec2 activeOffset(
				g_state.hasBaseLayout ? g_state.layout.mountOffsetX : 0.0f,
				g_state.hasBaseLayout ? g_state.layout.mountOffsetY : 0.0f);

			DrawLayoutCellsInCanvasOrder(drawList, artMin, scale, activeOffset, g_state.layout, true);
		}
		else
		{
			DrawCell(drawList, artMin, scale, 28.0f, 26.0f, 744.0f, 46.0f, "Title", false);
			DrawCell(drawList, artMin, scale, 34.0f, 96.0f, 138.0f, 104.0f, "Category Buttons", false);
			DrawCell(drawList, artMin, scale, 36.0f, 220.0f, 206.0f, 306.0f, "List", false);
			DrawCell(drawList, artMin, scale, 268.0f, 100.0f, 178.0f, 240.0f, "Portrait", false);
			DrawCell(drawList, artMin, scale, 470.0f, 100.0f, 301.0f, 192.0f, "Description Body", true);
			DrawCell(drawList, artMin, scale, 470.0f, 314.0f, 144.0f, 146.0f, "Stats Panel", false);
			DrawCell(drawList, artMin, scale, 628.0f, 314.0f, 143.0f, 146.0f, "Loadout", false);
			DrawCell(drawList, artMin, scale, 692.0f, 538.0f, 82.0f, 34.0f, "Close Button", false);
		}

		drawList->PushClipRect(viewportMin, viewportMax, true);
		if (g_state.showCanvasHud && g_state.canvasHudMode == 0)
			DrawCanvasHud(drawList, viewportMin);
		drawList->PopClipRect();

		ImGui::Dummy(virtualCanvasSize);


		ImGui::EndChild();

		if (g_state.activeCanvasTab >= 0 && g_state.activeCanvasTab < static_cast<int>(g_state.canvasTabs.size()))
		{
			UiEditorCanvasTab& activeTab = g_state.canvasTabs[static_cast<std::size_t>(g_state.activeCanvasTab)];
			activeTab.scrollX = ImGui::GetScrollX();
			activeTab.scrollY = ImGui::GetScrollY();
			activeTab.dirty = g_state.dirty;
		}

		ImGui::EndChild();
	}

	void DrawHierarchy()
	{
		DrawSectionTitle("HIERARCHY / LAYERS");

		if (!g_state.layout.loaded || g_state.layout.cells.empty())
		{
			ImGui::TextDisabled("No loaded cells.");
			return;
		}

		if (g_state.hasBaseLayout && !g_state.baseLayout.cells.empty())
		{
			ImGui::SeparatorText("locked base: main.fit");

			for (int i = 0; i < static_cast<int>(g_state.baseLayout.cells.size()); ++i)
			{
				const UiEditorFitCell& baseCell = g_state.baseLayout.cells[static_cast<std::size_t>(i)];
				const std::string label = DisplayAliasForCell(baseCell);
				ImGui::TextDisabled("[locked] %s", label.c_str());
			}
		}

		const char* currentLayer = nullptr;

		for (int i = 0; i < static_cast<int>(g_state.layout.cells.size()); ++i)
		{
			UiEditorFitCell& cell = g_state.layout.cells[static_cast<std::size_t>(i)];

			if (currentLayer == nullptr || cell.layer != currentLayer)
			{
				currentLayer = cell.layer.c_str();
				ImGui::SeparatorText(currentLayer);
			}

			ImGui::PushID(i);

			if (ImGui::SmallButton(cell.visible ? "eye" : "---"))
			{
				cell.visible = !cell.visible;
				if (!cell.visible && g_state.selectedCell == i)
				{
					g_state.selectedCell = -1;
					g_state.activeCell = -1;
					g_state.activeTransform = TransformNone;
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(cell.visible ? "Hide this cell in the editor canvas" : "Show this cell in the editor canvas");

			ImGui::SameLine();

			if (ImGui::SmallButton(cell.locked ? "lock" : "edit"))
			{
				cell.locked = !cell.locked;
				if (cell.locked && g_state.activeCell == i)
				{
					g_state.activeCell = -1;
					g_state.activeTransform = TransformNone;
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(cell.locked ? "Unlock this cell for editing" : "Lock this cell against canvas transforms");

			ImGui::SameLine();

			const bool selected = (i == g_state.selectedCell);
			std::string label = HierarchyLabelForCell(cell);
			if (!cell.visible)
				label = "[hidden] " + label;
			else if (cell.locked)
				label = "[locked] " + label;

			if (ImGui::Selectable(label.c_str(), selected))
				g_state.selectedCell = cell.visible ? i : -1;

			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Rename Alias"))
					BeginRenameAlias(i);

				if (ImGui::MenuItem(cell.visible ? "Hide Cell" : "Show Cell"))
				{
					const bool beforeVisible = cell.visible;
					cell.visible = !cell.visible;
					RecordVisibilityEditHistory(i, beforeVisible, cell.visible);
					if (!cell.visible && g_state.selectedCell == i)
					{
						g_state.selectedCell = -1;
						g_state.activeCell = -1;
						g_state.activeTransform = TransformNone;
					}
				}

				if (ImGui::MenuItem(cell.locked ? "Unlock Cell" : "Lock Cell"))
				{
					const bool beforeLocked = cell.locked;
					cell.locked = !cell.locked;
					RecordLockEditHistory(i, beforeLocked, cell.locked);
					if (cell.locked && g_state.activeCell == i)
					{
						g_state.activeCell = -1;
						g_state.activeTransform = TransformNone;
					}
				}

				ImGui::TextDisabled("Raw FIT id: %s", cell.key.c_str());
				ImGui::EndPopup();
			}

			ImGui::PopID();
		}
	}

	void DrawInspector()
	{
		DrawSectionTitle("INSPECTOR");

		if (!g_state.layout.loaded)
		{
			ImGui::TextWrapped("Load data/defs/ui/packages/legacy_imgui/mechlopedia/main.fit to inspect cells.");
			return;
		}

		UiEditorFitCell* cell = SelectedCellMutable();

		ImGui::SeparatorText("Layout");
		ImGui::Text("key: %s", g_state.layout.key.c_str());
		ImGui::Text("title: %s", g_state.layout.title.c_str());
		ImGui::Text("source: %s", g_state.layout.sourcePath.c_str());
		ImGui::Text("source design: %d x %d", g_state.layout.designWidth, g_state.layout.designHeight);
		if (ToLowerCopy(g_state.layout.sourcePath).find("/editor/") != std::string::npos || ToLowerCopy(g_state.layout.sourcePath).find("\\editor\\") != std::string::npos)
			ImGui::TextDisabled("editor replacement source: FIT/ImGui target; MFC is extraction source only.");
		ImGui::Text("canvas/source: %d x %d", g_state.canvasWidth, g_state.canvasHeight);
		ImGui::Text("preview: %d x %d", g_state.previewWidth, g_state.previewHeight);
		ImGui::Text("profile scale: %.3fx", CurrentProfileScale());
		ImGui::Text("aspect: %s", AspectCategoryFor(g_state.canvasWidth, g_state.canvasHeight));
		ImGui::Checkbox("preserve legacy coordinates", &g_state.preserveLegacyCoordinates);
		ImGui::SetNextItemWidth(160.0f);
		ImGui::Combo("anchor prep", &g_state.selectedAnchorMode, kAnchorModes, static_cast<int>(sizeof(kAnchorModes) / sizeof(kAnchorModes[0])));
		ImGui::SetNextItemWidth(160.0f);
		ImGui::Combo("scale policy prep", &g_state.selectedScalePolicy, kScalePolicies, static_cast<int>(sizeof(kScalePolicies) / sizeof(kScalePolicies[0])));
		if (g_state.hasBaseLayout)
			ImGui::Text("mount: %.0f, %.0f", g_state.layout.mountOffsetX, g_state.layout.mountOffsetY);
		ImGui::Text("cells: %d", static_cast<int>(g_state.layout.cells.size()));
		ImGui::TextDisabled("edit history entries: %d", static_cast<int>(g_state.editHistory.size()));

		if (g_state.hasBaseLayout)
			ImGui::TextDisabled("composition base: %d locked cells", static_cast<int>(g_state.baseLayout.cells.size()));

		ImGui::SeparatorText("Page Flow");
		ImGui::TextDisabled("Editor-side navigation metadata. Runtime hookup/export comes later.");
		if (ImGui::Button("Add Link From Selection"))
		{
			g_pageLinkTarget[0] = '\0';
			g_pageLinkMission[0] = '\0';
			g_pageLinkActionIndex = 0;
			g_state.openAddPageLinkDialog = true;
		}

		if (g_state.pageLinks.empty())
		{
			ImGui::TextDisabled("No page links staged.");
		}
		else
		{
			for (int i = 0; i < static_cast<int>(g_state.pageLinks.size()); ++i)
			{
				UiEditorPageLink& link = g_state.pageLinks[static_cast<std::size_t>(i)];
				ImGui::PushID(i);
				ImGui::TextWrapped("%s -> %s%s%s",
					link.widgetKey.empty() ? "<page>" : link.widgetKey.c_str(),
					link.action.c_str(),
					link.targetPageKey.empty() ? "" : " : ",
					link.targetPageKey.empty() ? "" : link.targetPageKey.c_str());
				if (!link.missionKey.empty())
					ImGui::TextDisabled("mission: %s", link.missionKey.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("remove"))
				{
					g_state.pageLinks.erase(g_state.pageLinks.begin() + i);
					g_state.dirty = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
		}

		if (cell == nullptr)
		{
			ImGui::Spacing();
			ImGui::TextDisabled("No selected cell.");
			return;
		}

		ImGui::SeparatorText("Selected Cell");
		const std::string alias = DisplayNameForCell(*cell);
		ImGui::Text("alias: %s", alias.c_str());
		if (!cell->aliasOverride.empty())
			ImGui::TextDisabled("alias override is in-memory only");
		ImGui::Text("id: %s", cell->key.c_str());
		ImGui::Text("type: %s", cell->type.c_str());
		ImGui::Text("layer: %s", cell->layer.c_str());

		if (!cell->widgetType.empty())
			ImGui::Text("widgetType: %s", cell->widgetType.c_str());

		if (!cell->sourceControlType.empty())
			ImGui::Text("sourceControlType: %s", cell->sourceControlType.c_str());

		if (!cell->controlId.empty())
			ImGui::Text("controlId: %s", cell->controlId.c_str());

		if (!cell->visibleText.empty())
			ImGui::TextWrapped("visibleText: %s", cell->visibleText.c_str());

		if (!cell->runtimeTextBinding.empty())
		{
			ImGui::TextWrapped("runtimeTextBinding: %s", cell->runtimeTextBinding.c_str());
			if (!cell->runtimeTextSource.empty())
				ImGui::TextWrapped("runtimeTextSource: %s", cell->runtimeTextSource.c_str());
			if (!cell->legacyTemplateVisibleText.empty())
				ImGui::TextWrapped("legacy template text: %s", cell->legacyTemplateVisibleText.c_str());
		}

		if (!cell->sourceStyle.empty())
			ImGui::TextWrapped("sourceStyle: %s", cell->sourceStyle.c_str());

		if (!cell->sourceLine.empty())
			ImGui::TextWrapped("sourceLine: %s", cell->sourceLine.c_str());

		if (!cell->role.empty())
			ImGui::Text("role: %s", cell->role.c_str());

		if (!cell->pageKey.empty())
			ImGui::Text("pageKey: %s", cell->pageKey.c_str());

		if (!cell->texture.empty())
		{
			ImGui::TextWrapped("texture: %s", cell->texture.c_str());

			const UiEditorImageTexture* image = UiEditorImageCache_Get(cell->texture.c_str());
			if (image != nullptr && image->loaded)
			{
				ImGui::Text("image: %d x %d", image->width, image->height);
				if (image->resolvedPath != nullptr && image->resolvedPath[0] != '\0')
					ImGui::TextWrapped("resolved: %s", image->resolvedPath);
			}
			else
			{
				ImGui::TextDisabled("%s", UiEditorImageCache_GetStatus());
			}
		}

		if (cell->hasUvPixels)
			ImGui::Text("uvPixels: %.0f, %.0f, %.0f, %.0f", cell->uvX, cell->uvY, cell->uvWidth, cell->uvHeight);

		if (!cell->textKey.empty())
			ImGui::TextWrapped("textKey: %s", cell->textKey.c_str());

		if (cell->type == "GuiColorBlock" || cell->type == "GuiPanel" || cell->type == "GuiButton" || cell->type == "GuiText")
		{
			ImGui::SeparatorText("Smart Tool Properties");
			bool styleChanged = false;
			styleChanged |= ImGui::ColorEdit4("fill", cell->fillColor);
			styleChanged |= ImGui::ColorEdit4("border", cell->borderColor);
			styleChanged |= DragFloatWithDoubleClickInput("border width", &cell->borderWidth, 0.1f, 0.0f, 32.0f, "%.1f");
			styleChanged |= SliderFloatWithDoubleClickInput("opacity", &cell->opacity, 0.0f, 1.0f, "%.2f");
			if (styleChanged)
			{
				cell->borderWidth = std::max(0.0f, cell->borderWidth);
				cell->opacity = std::max(0.0f, std::min(1.0f, cell->opacity));
				MarkDirty();
			}
		}

		if (cell->type == "GuiList" || cell->isListCell)
		{
			ImGui::SeparatorText("List Widget");
			ImGui::TextDisabled("Row contents are pushed at runtime via setListItems(). Only geometry and style are authored here.");
			bool listChanged = false;
			listChanged |= DragFloatWithDoubleClickInput("item height", &cell->itemHeight, 1.0f, 0.0f, 200.0f, "%.1f px (0=auto)");
			listChanged |= DragFloatWithDoubleClickInput("item offset X", &cell->itemOffsetX, 0.5f, 0.0f, 100.0f, "%.1f");
			listChanged |= DragFloatWithDoubleClickInput("item offset Y", &cell->itemOffsetY, 0.5f, 0.0f, 100.0f, "%.1f");
			listChanged |= ImGui::ColorEdit4("highlight color", cell->highlightColor);
			int previewCount = cell->listPreviewItemCount;
			if (ImGui::SliderInt("preview rows", &previewCount, 1, 10))
			{
				cell->listPreviewItemCount = previewCount;
				listChanged = true;
			}
			if (listChanged)
				MarkDirty();
		}

		const bool textEditable = IsTextRenderableCell(*cell) || cell->type == "GuiText" || cell->type == "GuiButton";
		const bool imageEditable = ContainsInsensitive(cell->type, "GuiImage") || ContainsInsensitive(cell->type, "GuiAnimation") || !cell->texture.empty();

		if (textEditable || imageEditable)
			SyncInspectorTextBuffers(g_state.selectedCell, *cell);

		if (textEditable)
		{
			ImGui::SeparatorText("Text Authoring");
			ImGui::TextDisabled("Edits apply to the generated ImGui FIT copy, not the legacy source FIT.");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputTextMultiline("visible text", g_inspectorVisibleTextBuffer, sizeof(g_inspectorVisibleTextBuffer), ImVec2(-1.0f, 92.0f)))
			{
				cell->visibleText = g_inspectorVisibleTextBuffer;
				if (!cell->visibleText.empty())
					cell->aliasOverride = cell->visibleText;
				MarkDirty();
			}

			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputText("text key", g_inspectorTextKeyBuffer, sizeof(g_inspectorTextKeyBuffer)))
			{
				cell->textKey = g_inspectorTextKeyBuffer;
				MarkDirty();
			}

			ImGui::SetNextItemWidth(150.0f);
			if (ImGui::BeginCombo("align", cell->textAlign.empty() ? "left" : cell->textAlign.c_str()))
			{
				const char* alignValues[] = { "left", "center", "right" };
				for (int i = 0; i < static_cast<int>(sizeof(alignValues) / sizeof(alignValues[0])); ++i)
				{
					const bool selectedAlign = ToLowerCopy(cell->textAlign) == alignValues[i];
					if (ImGui::Selectable(alignValues[i], selectedAlign))
					{
						cell->textAlign = alignValues[i];
						std::snprintf(g_inspectorTextAlignBuffer, sizeof(g_inspectorTextAlignBuffer), "%s", cell->textAlign.c_str());
						MarkDirty();
					}
					if (selectedAlign)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			int anchorIndex = TextAnchorIndexForValue(cell->textAnchor);
			ImGui::SetNextItemWidth(170.0f);
			if (ImGui::Combo("text anchor", &anchorIndex, kTextAnchorLabels, static_cast<int>(sizeof(kTextAnchorLabels) / sizeof(kTextAnchorLabels[0]))))
			{
				cell->textAnchor = kTextAnchorValues[anchorIndex];
				std::snprintf(g_inspectorTextAnchorBuffer, sizeof(g_inspectorTextAnchorBuffer), "%s", cell->textAnchor.c_str());
				MarkDirty();
			}

			if (ImGui::InputText("anchor raw", g_inspectorTextAnchorBuffer, sizeof(g_inspectorTextAnchorBuffer)))
			{
				cell->textAnchor = g_inspectorTextAnchorBuffer;
				MarkDirty();
			}

			if (ImGui::Checkbox("word wrap", &cell->wrapText))
				MarkDirty();

			ImGui::SeparatorText("Font");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputText("font", g_inspectorFontBuffer, sizeof(g_inspectorFontBuffer)))
			{
				cell->font = g_inspectorFontBuffer;
				MarkDirty();
			}

			int fontIndex = FontPresetIndexForValue(cell->font);
			const UiEditorFontCatalogEntry* activeFontEntry = FindStyledFontCatalogEntry(*cell);
			const char* fontPreview = activeFontEntry != nullptr ? activeFontEntry->displayName.c_str() : (fontIndex >= 0 ? kFontPresets[fontIndex] : "Custom");
			ImGui::SetNextItemWidth(190.0f);
			if (ImGui::BeginCombo("font preset", fontPreview))
			{
				if (!g_fontCatalog.empty())
				{
					for (int i = 0; i < static_cast<int>(g_fontCatalog.size()); ++i)
					{
						const UiEditorFontCatalogEntry& entry = g_fontCatalog[static_cast<std::size_t>(i)];
						const bool selectedFont = activeFontEntry == &entry;
						const std::string label = entry.displayName.empty() ? entry.key : entry.displayName;
						if (ImGui::Selectable(label.c_str(), selectedFont))
						{
							cell->font = entry.key;
							if (cell->fontSize <= 0.0f)
								cell->fontSize = entry.defaultSize;
							std::snprintf(g_inspectorFontBuffer, sizeof(g_inspectorFontBuffer), "%s", cell->font.c_str());
							MarkDirty();
						}
						if (selectedFont)
							ImGui::SetItemDefaultFocus();
					}
				}
				else
				{
					for (int i = 0; i < static_cast<int>(sizeof(kFontPresets) / sizeof(kFontPresets[0])); ++i)
					{
						const bool selectedFont = (i == fontIndex);
						if (ImGui::Selectable(kFontPresets[i], selectedFont))
						{
							fontIndex = i;
							cell->font = kFontPresets[i];
							std::snprintf(g_inspectorFontBuffer, sizeof(g_inspectorFontBuffer), "%s", cell->font.c_str());
							MarkDirty();
						}
						if (selectedFont)
							ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			if (activeFontEntry != nullptr)
			{
				std::vector<std::string> styleOptions;
				for (std::size_t i = 0; i < g_fontCatalog.size(); ++i)
				{
					const UiEditorFontCatalogEntry& entry = g_fontCatalog[i];
					if (ToLowerCopy(entry.family) != ToLowerCopy(activeFontEntry->family))
						continue;
					if (std::find(styleOptions.begin(), styleOptions.end(), entry.style) == styleOptions.end())
						styleOptions.push_back(entry.style);
				}

				if (!styleOptions.empty())
				{
					const char* stylePreview = cell->fontStyle.empty() ? activeFontEntry->style.c_str() : cell->fontStyle.c_str();
					ImGui::SetNextItemWidth(190.0f);
					if (ImGui::BeginCombo("font type", stylePreview))
					{
						for (std::size_t styleIndex = 0; styleIndex < styleOptions.size(); ++styleIndex)
						{
							const bool selectedStyle = ToLowerCopy(styleOptions[styleIndex]) == ToLowerCopy(cell->fontStyle.empty() ? activeFontEntry->style : cell->fontStyle);
							if (ImGui::Selectable(styleOptions[styleIndex].c_str(), selectedStyle))
							{
								cell->fontStyle = styleOptions[styleIndex];
								cell->bold = ToLowerCopy(cell->fontStyle).find("bold") != std::string::npos;
								cell->italic = ToLowerCopy(cell->fontStyle).find("italic") != std::string::npos;
								std::snprintf(g_inspectorFontStyleBuffer, sizeof(g_inspectorFontStyleBuffer), "%s", cell->fontStyle.c_str());
								MarkDirty();
							}
							if (selectedStyle)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				}
			}

			if (ImGui::InputText("font type raw", g_inspectorFontStyleBuffer, sizeof(g_inspectorFontStyleBuffer)))
			{
				cell->fontStyle = g_inspectorFontStyleBuffer;
				MarkDirty();
			}

			ImGui::TextDisabled("%s", g_fontCatalogStatus.c_str());

			if (DragFloatWithDoubleClickInput("font size", &cell->fontSize, 0.2f, 1.0f, 96.0f, "%.1f"))
				MarkDirty();

			bool fontStyleChanged = false;
			fontStyleChanged |= ImGui::Checkbox("bold", &cell->bold);
			ImGui::SameLine();
			fontStyleChanged |= ImGui::Checkbox("italic", &cell->italic);
			if (fontStyleChanged)
			{
				if (cell->bold && cell->italic)
					cell->fontStyle = "BoldItalic";
				else if (cell->bold)
					cell->fontStyle = "Bold";
				else if (cell->italic)
					cell->fontStyle = "Italic";
				else
					cell->fontStyle = "Regular";
				std::snprintf(g_inspectorFontStyleBuffer, sizeof(g_inspectorFontStyleBuffer), "%s", cell->fontStyle.c_str());
				MarkDirty();
			}

			if (ImGui::ColorEdit4("font color", cell->textColor))
				MarkDirty();

			bool textPulseChanged = ImGui::Checkbox("text pulse / flash", &cell->pulseEnabled);
			if (textPulseChanged)
			{
				if (cell->pulseEnabled && cell->textEffect.empty())
					cell->textEffect = "pulse";
				else if (!cell->pulseEnabled && cell->textEffect == "pulse")
					cell->textEffect.clear();
				MarkDirty();
			}
			if (cell->pulseEnabled)
			{
				if (DragFloatWithDoubleClickInput("text pulse speed", &cell->pulseSpeed, 0.05f, 0.1f, 10.0f, "%.2f"))
				{
					cell->pulseSpeed = std::max(0.1f, cell->pulseSpeed);
					MarkDirty();
				}
			}
			if (!cell->legacyTextEffectSource.empty())
				ImGui::TextDisabled("effect source: %s", cell->legacyTextEffectSource.c_str());
		}

		if (imageEditable)
		{
			ImGui::SeparatorText("Image Authoring");
			ImGui::TextDisabled("Set the generated ImGui texture path. Legacy atlas UVs remain visible for truth checks.");
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputText("texture", g_inspectorTextureBuffer, sizeof(g_inspectorTextureBuffer)))
			{
				cell->texture = g_inspectorTextureBuffer;
				MarkDirty();
			}

			char linkedButtonBuffer[192];
			std::snprintf(linkedButtonBuffer, sizeof(linkedButtonBuffer), "%s", cell->linkedButtonKey.c_str());
			if (ImGui::InputText("linked button key", linkedButtonBuffer, sizeof(linkedButtonBuffer)))
			{
				cell->linkedButtonKey = linkedButtonBuffer;
				MarkDirty();
			}

			char templatePartBuffer[96];
			std::snprintf(templatePartBuffer, sizeof(templatePartBuffer), "%s", cell->buttonTemplatePart.c_str());
			if (ImGui::InputText("template part", templatePartBuffer, sizeof(templatePartBuffer)))
			{
				cell->buttonTemplatePart = templatePartBuffer;
				MarkDirty();
			}
		}


		if (IsButtonLikeCell(*cell))
		{
			ImGui::SeparatorText("Button Compatibility");
			if (cell->buttonTemplate.empty())
				cell->buttonTemplate = "composite_hit_art_border_text_v1";

			char buttonTemplateBuffer[128];
			std::snprintf(buttonTemplateBuffer, sizeof(buttonTemplateBuffer), "%s", cell->buttonTemplate.c_str());
			if (ImGui::InputText("template", buttonTemplateBuffer, sizeof(buttonTemplateBuffer)))
			{
				cell->buttonTemplate = buttonTemplateBuffer;
				MarkDirty();
			}

			char linkedImageBuffer[192];
			std::snprintf(linkedImageBuffer, sizeof(linkedImageBuffer), "%s", cell->linkedImageKey.c_str());
			if (ImGui::InputText("linked image key", linkedImageBuffer, sizeof(linkedImageBuffer)))
			{
				cell->linkedImageKey = linkedImageBuffer;
				MarkDirty();
			}

			int actionIndex = LegacyActionPresetIndexFor(*cell);
			ImGui::SetNextItemWidth(190.0f);
			const int actionCount = static_cast<int>(sizeof(kLegacyActionPresets) / sizeof(kLegacyActionPresets[0]));
			const char* actionPreview = (actionIndex >= 0 && actionIndex < actionCount) ? kLegacyActionPresets[actionIndex].label : "None";
			ImGui::SetNextItemWidth(190.0f);
			if (ImGui::BeginCombo("action preset", actionPreview))
			{
				for (int i = 0; i < actionCount; ++i)
				{
					const bool selectedAction = (i == actionIndex);
					if (ImGui::Selectable(kLegacyActionPresets[i].label, selectedAction))
					{
						actionIndex = i;
						cell->actionType = kLegacyActionPresets[i].actionType;
						if (cell->legacyMessage.empty() || std::strcmp(kLegacyActionPresets[i].legacyMessage, "") != 0)
							cell->legacyMessage = kLegacyActionPresets[i].legacyMessage;
						MarkDirty();
					}
					if (selectedAction)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			char legacyMessageBuffer[128];
			std::snprintf(legacyMessageBuffer, sizeof(legacyMessageBuffer), "%s", cell->legacyMessage.c_str());
			if (ImGui::InputText("legacy message", legacyMessageBuffer, sizeof(legacyMessageBuffer)))
			{
				cell->legacyMessage = legacyMessageBuffer;
				MarkDirty();
			}

			char targetPageBuffer[160];
			std::snprintf(targetPageBuffer, sizeof(targetPageBuffer), "%s", cell->targetPageKey.c_str());
			if (ImGui::InputText("target page", targetPageBuffer, sizeof(targetPageBuffer)))
			{
				cell->targetPageKey = targetPageBuffer;
				MarkDirty();
			}

			char targetMissionBuffer[160];
			std::snprintf(targetMissionBuffer, sizeof(targetMissionBuffer), "%s", cell->targetMissionKey.c_str());
			if (ImGui::InputText("target mission", targetMissionBuffer, sizeof(targetMissionBuffer)))
			{
				cell->targetMissionKey = targetMissionBuffer;
				MarkDirty();
			}

			char transitionBuffer[128];
			std::snprintf(transitionBuffer, sizeof(transitionBuffer), "%s", cell->transitionName.c_str());
			if (ImGui::InputText("transition", transitionBuffer, sizeof(transitionBuffer)))
			{
				cell->transitionName = transitionBuffer;
				MarkDirty();
			}

			if (ImGui::Checkbox("toggle / checkbox", &cell->toggleButton))
				MarkDirty();
			ImGui::SameLine();
			if (ImGui::Checkbox("checked", &cell->checked))
				MarkDirty();

			if (ImGui::Checkbox("selected state", &cell->selectedState))
				MarkDirty();
			ImGui::SameLine();
			if (ImGui::Checkbox("disabled", &cell->disabledState))
				MarkDirty();

			if (ImGui::Checkbox("pulse / flash", &cell->pulseEnabled))
				MarkDirty();
			if (cell->pulseEnabled)
			{
				if (DragFloatWithDoubleClickInput("pulse speed", &cell->pulseSpeed, 0.05f, 0.1f, 10.0f, "%.2f"))
				{
					cell->pulseSpeed = std::max(0.1f, cell->pulseSpeed);
					MarkDirty();
				}
			}

			ImGui::SeparatorText("Button Overlay");
			if (ImGui::Checkbox("button color overlay", &cell->buttonOverlayEnabled))
				MarkDirty();
			if (cell->buttonOverlayEnabled)
			{
				if (DragFloatWithDoubleClickInput("overlay alpha", &cell->buttonOverlayAlpha, 0.02f, 0.0f, 1.0f, "%.2f"))
					MarkDirty();

				if (ImGui::ColorEdit4("normal overlay", cell->normalOverlayColor))
				{
					cell->hasNormalOverlayColor = true;
					MarkDirty();
				}
				if (ImGui::ColorEdit4("pressed overlay", cell->pressedOverlayColor))
				{
					cell->hasPressedOverlayColor = true;
					MarkDirty();
				}
				if (ImGui::ColorEdit4("hover/selected overlay", cell->highlightOverlayColor))
				{
					cell->hasHighlightOverlayColor = true;
					MarkDirty();
				}
				if (ImGui::ColorEdit4("disabled overlay", cell->disabledOverlayColor))
				{
					cell->hasDisabledOverlayColor = true;
					MarkDirty();
				}
			}

			ImGui::SeparatorText("Button Image Rect");
			if (!cell->hasArtRect && ImGui::Button("Create image rect from button"))
			{
				cell->hasArtRect = true;
				cell->artX = cell->x;
				cell->artY = cell->y;
				cell->artWidth = cell->width;
				cell->artHeight = cell->height;
				MarkDirty();
			}
			if (cell->hasArtRect)
			{
				bool artRectChanged = false;
				artRectChanged |= DragFloatWithDoubleClickInput("image x", &cell->artX, 1.0f, -10000.0f, 10000.0f, "%.0f");
				artRectChanged |= DragFloatWithDoubleClickInput("image y", &cell->artY, 1.0f, -10000.0f, 10000.0f, "%.0f");
				artRectChanged |= DragFloatWithDoubleClickInput("image w", &cell->artWidth, 1.0f, 1.0f, 10000.0f, "%.0f");
				artRectChanged |= DragFloatWithDoubleClickInput("image h", &cell->artHeight, 1.0f, 1.0f, 10000.0f, "%.0f");
				if (artRectChanged)
				{
					cell->artWidth = std::max(1.0f, cell->artWidth);
					cell->artHeight = std::max(1.0f, cell->artHeight);
					MarkDirty();
				}
			}

			ImGui::SeparatorText("Button Border Rect");
			if (ImGui::Checkbox("border enabled", &cell->borderEnabled))
				MarkDirty();
			if (!cell->hasBorderRect && ImGui::Button("Create border rect from button"))
			{
				cell->hasBorderRect = true;
				cell->borderX = cell->x;
				cell->borderY = cell->y;
				cell->borderWidthRect = cell->width;
				cell->borderHeightRect = cell->height;
				MarkDirty();
			}
			if (cell->hasBorderRect)
			{
				bool borderRectChanged = false;
				borderRectChanged |= DragFloatWithDoubleClickInput("border x", &cell->borderX, 1.0f, -10000.0f, 10000.0f, "%.0f");
				borderRectChanged |= DragFloatWithDoubleClickInput("border y", &cell->borderY, 1.0f, -10000.0f, 10000.0f, "%.0f");
				borderRectChanged |= DragFloatWithDoubleClickInput("border w", &cell->borderWidthRect, 1.0f, 1.0f, 10000.0f, "%.0f");
				borderRectChanged |= DragFloatWithDoubleClickInput("border h", &cell->borderHeightRect, 1.0f, 1.0f, 10000.0f, "%.0f");
				if (borderRectChanged)
				{
					cell->borderWidthRect = std::max(1.0f, cell->borderWidthRect);
					cell->borderHeightRect = std::max(1.0f, cell->borderHeightRect);
					MarkDirty();
				}
			}

			ImGui::SeparatorText("Button Text Rect");
			ImGui::TextDisabled("Legacy buttons have a separate text rectangle inside the button sprite.");
			if (!cell->hasTextRect && ImGui::Button("Create text rect from button"))
			{
				cell->hasTextRect = true;
				cell->textX = cell->x;
				cell->textY = cell->y;
				cell->textWidth = cell->width;
				cell->textHeight = cell->height;
				MarkDirty();
			}
			if (cell->hasTextRect)
			{
				bool textRectChanged = false;
				textRectChanged |= DragFloatWithDoubleClickInput("text rect x", &cell->textX, 1.0f, -10000.0f, 10000.0f, "%.0f");
				textRectChanged |= DragFloatWithDoubleClickInput("text rect y", &cell->textY, 1.0f, -10000.0f, 10000.0f, "%.0f");
				textRectChanged |= DragFloatWithDoubleClickInput("text rect w", &cell->textWidth, 1.0f, 1.0f, 10000.0f, "%.0f");
				textRectChanged |= DragFloatWithDoubleClickInput("text rect h", &cell->textHeight, 1.0f, 1.0f, 10000.0f, "%.0f");
				if (textRectChanged)
				{
					cell->textWidth = std::max(1.0f, cell->textWidth);
					cell->textHeight = std::max(1.0f, cell->textHeight);
					MarkDirty();
				}

				ImGui::TextDisabled("Drag the yellow text rect on the canvas; it no longer requires being the top click target.");
				if (ImGui::SmallButton("text -1 x"))
				{
					cell->textX -= 1.0f;
					MarkDirty();
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("text +1 x"))
				{
					cell->textX += 1.0f;
					MarkDirty();
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("text -1 y"))
				{
					cell->textY -= 1.0f;
					MarkDirty();
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("text +1 y"))
				{
					cell->textY += 1.0f;
					MarkDirty();
				}
			}

			ImGui::TextDisabled("Legacy actions are editor metadata for ImGui/runtime hookup.");
		}

		ImGui::SeparatorText("Editor State");
		ImGui::Text("visible: %s", cell->visible ? "yes" : "no");
		ImGui::Text("locked: %s", cell->locked ? "yes" : "no");
		if (!cell->visible)
			ImGui::TextDisabled("Hidden cells are editor-only hidden and are not drawn on the canvas.");
		if (cell->locked)
			ImGui::TextDisabled("Locked cells can be inspected but cannot be moved or resized on the canvas.");

		ImGui::SeparatorText("Rect");
		if (!cell->visible || cell->locked)
		{
			ImGui::Text("x: %.0f", cell->x);
			ImGui::Text("y: %.0f", cell->y);
			ImGui::Text("w: %.0f", cell->width);
			ImGui::Text("h: %.0f", cell->height);
		}
		else
		{
			const float beforeX = cell->x;
			const float beforeY = cell->y;
			const float beforeW = cell->width;
			const float beforeH = cell->height;

			bool rectChanged = false;
			rectChanged |= DragFloatWithDoubleClickInput("x", &cell->x, 1.0f, -10000.0f, 10000.0f, "%.0f");
			rectChanged |= DragFloatWithDoubleClickInput("y", &cell->y, 1.0f, -10000.0f, 10000.0f, "%.0f");
			rectChanged |= DragFloatWithDoubleClickInput("w", &cell->width, 1.0f, 4.0f, 10000.0f, "%.0f");
			rectChanged |= DragFloatWithDoubleClickInput("h", &cell->height, 1.0f, 4.0f, 10000.0f, "%.0f");

			if (rectChanged)
			{
				ClampCellRect(*cell);
				RecordEditHistory("Inspector rect edit", g_state.selectedCell, beforeX, beforeY, beforeW, beforeH, cell->x, cell->y, cell->width, cell->height);
				MarkDirty();
			}
		}

		ImGui::Spacing();
	}
	void DrawRightPanel(float width)
	{
		ImGui::BeginChild("right_panel", ImVec2(width, 0.0f), false);

		const float totalHeight = ImGui::GetContentRegionAvail().y;
		const float displayHierarchyHeight = ClampedSplitHeightValue(g_state.rightHierarchyHeight, totalHeight, 150.0f, 190.0f);

		ImGui::BeginChild("hierarchy_panel", ImVec2(0.0f, displayHierarchyHeight), true);
		DrawHierarchy();
		ImGui::EndChild();

		DrawHorizontalSplitter("##right_panel_splitter", g_state.rightHierarchyHeight, totalHeight, 150.0f, 190.0f);

		ImGui::BeginChild("inspector_panel", ImVec2(0.0f, 0.0f), true);
		DrawInspector();
		ImGui::EndChild();

		ImGui::EndChild();
	}


	void CenterNextPopupModal()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (!viewport)
			return;

		ImGui::SetNextWindowPos(
			ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
			ImGuiCond_Always,
			ImVec2(0.5f, 0.5f));
	}


	void DrawSafetyPopups()
	{
		if (g_state.openSaveCopyDialog)
		{
			ImGui::OpenPopup("Save Copy");
			g_state.openSaveCopyDialog = false;
		}

		if (g_state.openDiscardEditsDialog)
		{
			ImGui::OpenPopup("Discard in-memory edits?");
			g_state.openDiscardEditsDialog = false;
		}

		if (g_state.openRenameAliasDialog)
		{
			ImGui::OpenPopup("Rename Alias");
			g_state.openRenameAliasDialog = false;
		}

		if (g_state.openNewPageDialog)
		{
			ImGui::OpenPopup("New UI Page");
			g_state.openNewPageDialog = false;
		}

		if (g_state.openAddPageLinkDialog)
		{
			ImGui::OpenPopup("Add Page Link");
			g_state.openAddPageLinkDialog = false;
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("New UI Page", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("Create a new FIT-backed UI page shell. This is data-only and does not imply runtime hookup.");
			ImGui::Spacing();

			ImGui::SetNextItemWidth(420.0f);
			if (ImGui::InputText("Page key", g_newPageKey, sizeof(g_newPageKey)))
				UpdateNewPageOutputPath();

			ImGui::SetNextItemWidth(420.0f);
			ImGui::InputText("Title", g_newPageTitle, sizeof(g_newPageTitle));

			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::Combo("Surface", &g_newPageSurfaceIndex, kNewPageSurfaces, static_cast<int>(sizeof(kNewPageSurfaces) / sizeof(kNewPageSurfaces[0]))))
				UpdateNewPageOutputPath();

			ImGui::SetNextItemWidth(260.0f);
			if (ImGui::BeginCombo("Source profile", ResolutionProfileAt(g_newPageProfileIndex).label))
			{
				for (int i = 0; i < ResolutionProfileCount(); ++i)
				{
					const bool selected = (g_newPageProfileIndex == i);
					if (ImGui::Selectable(ResolutionProfileAt(i).label, selected))
					{
						g_newPageProfileIndex = i;
						UpdateNewPageOutputPath();
					}

					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			const UiEditorResolutionProfile& profile = ResolutionProfileAt(g_newPageProfileIndex);
			const int previewW = profile.width > 0 ? profile.width : 1920;
			const int previewH = profile.height > 0 ? profile.height : 1080;
			ImGui::TextDisabled("Initial canvas: %d x %d", previewW, previewH);

			ImGui::SetNextItemWidth(520.0f);
			ImGui::InputText("Output FIT", g_newPageOutputPath, sizeof(g_newPageOutputPath));
			ImGui::Checkbox("Open after create", &g_newPageOpenAfterCreate);

			ImGui::Spacing();

			if (ImGui::Button("Create Page", ImVec2(130.0f, 0.0f)))
			{
				CreateNewUiPageFromDialog();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("Add Page Link", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			UiEditorFitCell* cell = SelectedCellMutable();
			ImGui::TextWrapped("Add editor-side navigation metadata for the selected widget. Runtime hookup comes later.");
			if (cell != nullptr)
			{
				ImGui::TextDisabled("Widget: %s", cell->key.c_str());
				std::string linkLabel = DisplayAliasForCell(*cell);
				if (!linkLabel.empty())
					ImGui::TextDisabled("Alias: %s", linkLabel.c_str());
			}
			else
			{
				ImGui::TextDisabled("No selected widget. Link will be page-level metadata.");
			}

			ImGui::Spacing();
			ImGui::SetNextItemWidth(200.0f);
			ImGui::Combo("Action", &g_pageLinkActionIndex, kPageLinkActions, static_cast<int>(sizeof(kPageLinkActions) / sizeof(kPageLinkActions[0])));

			ImGui::SetNextItemWidth(360.0f);
			ImGui::InputText("Target page key", g_pageLinkTarget, sizeof(g_pageLinkTarget));

			ImGui::SetNextItemWidth(360.0f);
			ImGui::InputText("Mission key", g_pageLinkMission, sizeof(g_pageLinkMission));

			if (ImGui::Button("Add Link", ImVec2(120.0f, 0.0f)))
			{
				UiEditorPageLink link;
				link.fromPageKey = g_state.layout.key;
				link.widgetKey = cell != nullptr ? cell->key : std::string();
				link.action = kPageLinkActions[g_pageLinkActionIndex];
				link.targetPageKey = g_pageLinkTarget;
				link.missionKey = g_pageLinkMission;
				g_state.pageLinks.push_back(link);
				g_state.layout.statusMessage = "Added editor-side page link metadata.";
				g_state.dirty = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("Save Copy", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("Save a source-preserving copy of the active FIT. The loaded source file will not be overwritten.");
			ImGui::Spacing();
			ImGui::SetNextItemWidth(520.0f);
			ImGui::InputText("Output", g_savePath, sizeof(g_savePath));

			if (ImGui::Button("Save Copy", ImVec2(120.0f, 0.0f)))
			{
				SaveLayoutCopy();
				if (g_state.pendingSavePath.empty())
					ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("Discard in-memory edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("The current layout has unsaved in-memory edits. Reloading or loading another FIT will discard them.");
			ImGui::Spacing();

			if (ImGui::Button("Discard and Load", ImVec2(150.0f, 0.0f)))
			{
				const std::string path = g_state.pendingLoadPath;
				g_state.pendingLoadPath.clear();
				g_state.dirty = false;
				SetLoadPath(path.c_str());
				const bool loaded = LoadLayoutPathNow(g_loadPath);
				if (loaded)
					AddOrUpdateActiveCanvasTabFromState(path);
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
			{
				g_state.pendingLoadPath.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("Rename Alias", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			UiEditorFitCell* cell = SelectedCellMutable();

			if (g_state.renameAliasCell < 0
				|| g_state.renameAliasCell >= static_cast<int>(g_state.layout.cells.size()))
				cell = nullptr;
			else
				cell = &g_state.layout.cells[static_cast<std::size_t>(g_state.renameAliasCell)];

			if (cell == nullptr)
			{
				ImGui::TextDisabled("No editable cell selected.");
				if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
				{
					g_state.renameAliasCell = -1;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			else
			{
				ImGui::TextWrapped("Rename the editor alias for this cell. The raw FIT id remains unchanged.");
				ImGui::TextDisabled("Raw FIT id: %s", cell->key.c_str());
				ImGui::Spacing();
				ImGui::SetNextItemWidth(360.0f);
				ImGui::InputText("Alias", g_renameAliasBuffer, sizeof(g_renameAliasBuffer));

				if (ImGui::Button("Apply", ImVec2(100.0f, 0.0f)))
				{
					const std::string beforeAlias = cell->aliasOverride;
					cell->aliasOverride = g_renameAliasBuffer;
					RecordAliasEditHistory(g_state.renameAliasCell, beforeAlias, cell->aliasOverride);
					g_state.selectedCell = g_state.renameAliasCell;
					g_state.renameAliasCell = -1;
					ImGui::CloseCurrentPopup();
				}

				ImGui::SameLine();

				if (ImGui::Button("Reset Alias", ImVec2(110.0f, 0.0f)))
				{
					const std::string beforeAlias = cell->aliasOverride;
					cell->aliasOverride.clear();
					RecordAliasEditHistory(g_state.renameAliasCell, beforeAlias, cell->aliasOverride);
					std::snprintf(g_renameAliasBuffer, sizeof(g_renameAliasBuffer), "%s", DisplayAliasForCell(*cell).c_str());
				}

				ImGui::SameLine();

				if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
				{
					g_state.renameAliasCell = -1;
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}
		}

		CenterNextPopupModal();
		if (ImGui::BeginPopupModal("Overwrite Save Copy?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("The Save Copy output already exists:");
			ImGui::TextWrapped("%s", g_state.pendingSavePath.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("Overwrite this copy? The loaded source FIT is still protected.");

			if (ImGui::Button("Overwrite Copy", ImVec2(140.0f, 0.0f)))
			{
				PerformSaveLayoutCopy();
				g_state.pendingSavePath.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
			{
				g_state.pendingSavePath.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		if (g_state.showSaveResultPopup && ImGui::BeginPopupModal("Save Copy Result", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", g_state.saveStatus.empty() ? "Save Copy completed." : g_state.saveStatus.c_str());

			if (ImGui::Button("OK", ImVec2(100.0f, 0.0f)))
			{
				g_state.showSaveResultPopup = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	// Additive from theirs (mc2r fork): optional ImGui docking-based layout, toggled
	// via View > Window > Floating Panels. Off by default behavior is unchanged
	// (g_state.useDockingLayout defaults true here to match the fork's own default;
	// the fixed-splitter layout below remains fully intact as the else-branch).
	void ApplyDefaultDockingLayout(ImGuiID dockspaceId, const ImGuiViewport* viewport)
	{
		static bool s_checkedExistingDockLayout = false;

		const bool missingDockNode = ImGui::DockBuilderGetNode(dockspaceId) == nullptr;
		if (!g_state.resetDockLayout && s_checkedExistingDockLayout && !missingDockNode)
			return;

		if (!g_state.resetDockLayout && !missingDockNode)
		{
			s_checkedExistingDockLayout = true;
			return;
		}

		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
		ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

		ImGuiID dockMain = dockspaceId;
		ImGuiID dockLeft = 0;
		ImGuiID dockRight = 0;

		ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.24f, &dockLeft, &dockMain);
		ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.26f, &dockRight, &dockMain);

		ImGuiID dockLeftTop = 0;
		ImGuiID dockLeftBottom = 0;
		ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.45f, &dockLeftBottom, &dockLeftTop);

		ImGuiID dockRightTop = 0;
		ImGuiID dockRightBottom = 0;
		ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.50f, &dockRightBottom, &dockRightTop);

		ImGui::DockBuilderDockWindow("Project / Layouts", dockLeftTop);
		ImGui::DockBuilderDockWindow("Smart Tools", dockLeftBottom);
		ImGui::DockBuilderDockWindow("Canvas", dockMain);
		ImGui::DockBuilderDockWindow("Hierarchy / Layers", dockRightTop);
		ImGui::DockBuilderDockWindow("Inspector", dockRightBottom);

		ImGui::DockBuilderFinish(dockspaceId);

		g_state.resetDockLayout = false;
		s_checkedExistingDockLayout = true;
	}

	void DrawMainUi()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();

		ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
		ImGui::Begin("MC2R UI Editor", nullptr,
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse);

		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New UI Page..."))
					g_state.openNewPageDialog = true;

				ImGui::Separator();

				if (ImGui::MenuItem("Load Path"))
					RequestLoadLayoutPath(g_loadPath);

				if (ImGui::MenuItem("Reload"))
					RequestLoadLayoutPath(g_loadPath);

				ImGui::Separator();

				if (ImGui::MenuItem("Save Copy"))
					g_state.openSaveCopyDialog = true;

				ImGui::Separator();

				if (ImGui::MenuItem("Save Configuration"))
					SaveEditorConfig();

				if (ImGui::MenuItem("Quit"))
				{
					SaveEditorConfig();
					g_state.requestQuit = true;
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Edit"))
			{
				const bool canUndo = !g_state.editHistory.empty();
				const bool canRedo = !g_state.editRedoHistory.empty();
				if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo))
					UndoLastEditCommand();
				if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo))
					RedoLastEditCommand();

				ImGui::Separator();
				const bool hasSelection = SelectedCell() != nullptr;
				if (ImGui::MenuItem("Copy Rect", "Ctrl+C", false, hasSelection))
					CopySelectedRect();
				if (ImGui::MenuItem("Cut Rect / Hide", "Ctrl+X", false, hasSelection))
					CutSelectedRect();
				if (ImGui::MenuItem("Paste Rect", "Ctrl+V", false, hasSelection && g_state.rectClipboardValid))
					PasteRectToSelected();

				ImGui::Separator();
				if (canUndo)
					ImGui::TextDisabled("Last: %s", g_state.editHistory.back().label.c_str());
				else
					ImGui::TextDisabled("No edit commands recorded.");
				ImGui::TextDisabled("History: %d  Redo: %d", static_cast<int>(g_state.editHistory.size()), static_cast<int>(g_state.editRedoHistory.size()));
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("View"))
			{
				ImGui::MenuItem("Grid", nullptr, &g_state.showGrid);
				ImGui::MenuItem("Snap to Grid", nullptr, &g_state.snapToGrid);
				ImGui::MenuItem("Rulers", nullptr, &g_state.showRulers);
				ImGui::MenuItem("Guides", nullptr, &g_state.showGuides);
				ImGui::MenuItem("Snap to Guides", nullptr, &g_state.snapToGuides);
				ImGui::Separator();
				ImGui::MenuItem("Images", nullptr, &g_state.showImages);
				ImGui::MenuItem("Labels", nullptr, &g_state.showLabels);
				ImGui::MenuItem("Image Diagnostics", nullptr, &g_state.showImageDiagnostics);
				ImGui::MenuItem("HUD", nullptr, &g_state.showCanvasHud);
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Tools"))
			{
				ImGui::TextDisabled("Smart Tools are staged for v0.5.x.");
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Window"))
			{
				if (ImGui::MenuItem("Floating Panels", nullptr, g_state.useDockingLayout))
				{
					g_state.useDockingLayout = !g_state.useDockingLayout;
					SaveEditorConfig();
				}
				if (ImGui::MenuItem("Reset Default Dock Layout"))
					g_state.resetDockLayout = true;
				if (ImGui::MenuItem("Save Configuration"))
					SaveEditorConfig();
				ImGui::TextDisabled("Default dock layout: Project top-left, Smart Tools bottom-left, Canvas center, Hierarchy top-right, Inspector bottom-right.");
				ImGui::TextDisabled("Preferences save to ui_editor.cfg; window placement saves to ui_editor_imgui.ini.");
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Help"))
			{
				ImGui::TextDisabled("%s", UiEditorVersion_GetWindowTitle());
				ImGui::TextDisabled("Source-safe FIT editing shell.");
				ImGui::EndMenu();
			}

			ImGui::EndMainMenuBar();
		}

		if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Z) && !g_state.editHistory.empty())
				UndoLastEditCommand();
			if (ImGui::IsKeyPressed(ImGuiKey_Y) && !g_state.editRedoHistory.empty())
				RedoLastEditCommand();
			if (ImGui::IsKeyPressed(ImGuiKey_C) && SelectedCell() != nullptr)
				CopySelectedRect();
			if (ImGui::IsKeyPressed(ImGuiKey_X) && SelectedCell() != nullptr)
				CutSelectedRect();
			if (ImGui::IsKeyPressed(ImGuiKey_V) && SelectedCell() != nullptr && g_state.rectClipboardValid)
				PasteRectToSelected();
		}

		DrawTopToolbar();

		if (g_state.useDockingLayout)
		{
			ImGuiID dockspaceId = ImGui::GetID("MC2R_UI_EDITOR_DOCKSPACE");
			ApplyDefaultDockingLayout(dockspaceId, viewport);
			ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -28.0f), ImGuiDockNodeFlags_PassthruCentralNode);

			ImGui::Begin("Project / Layouts");
			DrawProjectTree();
			ImGui::End();

			ImGui::Begin("Smart Tools");
			DrawSmartTools();
			ImGui::End();

			ImGui::Begin("Canvas");
			DrawCanvas(ImGui::GetContentRegionAvail());
			ImGui::End();

			ImGui::Begin("Hierarchy / Layers");
			DrawHierarchy();
			ImGui::End();

			ImGui::Begin("Inspector");
			DrawInspector();
			ImGui::End();
		}
		else
		{
			ImGui::BeginChild("main_dock_area", ImVec2(0.0f, -28.0f), false);

			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float availableWidth = ImGui::GetContentRegionAvail().x;
			const float minLeftWidth = 220.0f;
			const float minCenterWidth = 280.0f;
			const float minRightWidth = 260.0f;
			const float splitterWidth = 7.0f;

			float displayLeftPanelWidth = ClampedPanelWidthValue(g_state.leftPanelWidth, minLeftWidth, std::max(minLeftWidth, availableWidth - minCenterWidth - minRightWidth - splitterWidth * 2.0f - spacing * 4.0f));
			float displayRightPanelWidth = ClampedPanelWidthValue(g_state.rightPanelWidth, minRightWidth, std::max(minRightWidth, availableWidth - minCenterWidth - displayLeftPanelWidth - splitterWidth * 2.0f - spacing * 4.0f));

			float centerWidth = availableWidth
				- displayLeftPanelWidth
				- displayRightPanelWidth
				- splitterWidth * 2.0f
				- spacing * 4.0f;

			if (centerWidth < minCenterWidth)
			{
				const float deficit = minCenterWidth - centerWidth;
				const float rightTake = std::min(deficit * 0.5f, std::max(0.0f, displayRightPanelWidth - minRightWidth));
				displayRightPanelWidth -= rightTake;
				const float leftTake = std::min(deficit - rightTake, std::max(0.0f, displayLeftPanelWidth - minLeftWidth));
				displayLeftPanelWidth -= leftTake;

				centerWidth = availableWidth
					- displayLeftPanelWidth
					- displayRightPanelWidth
					- splitterWidth * 2.0f
					- spacing * 4.0f;
			}

			DrawLeftPanel(displayLeftPanelWidth);
			ImGui::SameLine();
			DrawVerticalSplitter("##left_right_sizing_splitter", g_state.leftPanelWidth, minLeftWidth, std::max(minLeftWidth, availableWidth - minCenterWidth - minRightWidth), false);
			ImGui::SameLine();
			DrawCanvas(ImVec2(centerWidth > minCenterWidth ? centerWidth : minCenterWidth, 0.0f));
			ImGui::SameLine();
			DrawVerticalSplitter("##right_left_sizing_splitter", g_state.rightPanelWidth, minRightWidth, std::max(minRightWidth, availableWidth - minCenterWidth - minLeftWidth), true);
			ImGui::SameLine();
			DrawRightPanel(displayRightPanelWidth);

			ImGui::EndChild();
		}

		DrawStatusBar();

		DrawSafetyPopups();

		ImGui::End();
	}
}

int main(int, char**)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	SDL_Window* window = SDL_CreateWindow(
		UiEditorVersion_GetWindowTitle(),
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		1280,
		720,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

	if (!window)
	{
		std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_GLContext glContext = SDL_GL_CreateContext(window);
	if (!glContext)
	{
		std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	SDL_GL_MakeCurrent(window, glContext);
	SDL_GL_SetSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = "ui_editor_imgui.ini";

	ApplyEditorStyle();
	LoadFontCatalog();
	UiEditorImageCache_Initialize();
	LoadEditorConfig();
	LoadDefaultLayout();

	ImGui_ImplSDL2_InitForOpenGL(window, glContext);
	ImGui_ImplOpenGL3_Init("#version 130");

	bool running = true;
	while (running)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			ImGui_ImplSDL2_ProcessEvent(&event);

			if (event.type == SDL_QUIT)
				running = false;

			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)
				running = false;
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		DrawMainUi();

		if (g_state.requestQuit)
			running = false;

		ImGui::Render();

		int displayW = 0;
		int displayH = 0;
		SDL_GL_GetDrawableSize(window, &displayW, &displayH);

		glViewport(0, 0, displayW, displayH);
		glClearColor(0.06f, 0.065f, 0.075f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(window);
	}

	SaveEditorConfig();

	UiEditorImageCache_Shutdown();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	SDL_GL_DeleteContext(glContext);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}