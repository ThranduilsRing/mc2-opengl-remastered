/***************************************************************
 * FILENAME: UiEditorFitLoader.h
 * DESCRIPTION: Minimal read-only FIT layout loader for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added read-only structures for loading typed UI FIT layout blocks.
 * - Added page mount offsets for composite preview alignment.
 * - Added legacy image atlas UV fields for Viewer-matched previews.
 * - Added source-preserving Save Copy support for edited cell rectangles.
 * - Added legacy selected/checked state fields for truth-viewer previews.
 * - Added runtime text binding/source fields for dynamic legacy text slots.
 * - Added editable text font, color, anchor, and image source fields.
 * - Added text effect/source fields for legacy pulse/flash preview.
 * - Added legacy button state atlas/text rect/color fields for truth-viewer previews.
 ***************************************************************/

#pragma once

#include <string>
#include <vector>

struct UiEditorFitCell
{
	std::string key;
	std::string type;
	std::string pageKey;
	std::string role;
	std::string layer;
	std::string texture;
	std::string textKey;
	std::string visibleText;
	std::string textAlign;
	std::string textAnchor;
	std::string font;
	std::string fontStyle;
	float fontSize;
	std::string textEffect;
	std::string legacyTextEffectSource;
	bool bold;
	bool italic;
	float textColor[4];
	bool hasNormalTextColor;
	float normalTextColor[4];
	bool hasPressedTextColor;
	float pressedTextColor[4];
	bool hasHighlightTextColor;
	float highlightTextColor[4];
	bool hasDisabledTextColor;
	float disabledTextColor[4];
	bool buttonOverlayEnabled;
	float buttonOverlayAlpha;
	bool hasNormalOverlayColor;
	float normalOverlayColor[4];
	bool hasPressedOverlayColor;
	float pressedOverlayColor[4];
	bool hasHighlightOverlayColor;
	float highlightOverlayColor[4];
	bool hasDisabledOverlayColor;
	float disabledOverlayColor[4];
	bool hasTextRect;
	float textX;
	float textY;
	float textWidth;
	float textHeight;
	std::string runtimeTextBinding;
	std::string runtimeTextSource;
	std::string legacyTemplateTextKey;
	std::string legacyTemplateVisibleText;
	std::string widgetType;
	std::string sourceControlType;
	std::string sourceStyle;
	std::string sourceLine;
	std::string legacySection;
	std::string legacyComment;
	std::string legacyContext;
	std::string legacyRectSource;
	std::string helpCaptionKey;
	std::string helpCaptionText;
	std::string helpDescKey;
	std::string helpDescText;
	bool textMissing;
	std::string controlId;
	std::string actionType;
	std::string targetPageKey;
	std::string targetMissionKey;
	std::string transitionName;
	std::string legacyMessage;
	std::string aliasOverride;

	bool visible;
	bool locked;
	bool wrapText;
	bool toggleButton;
	bool checked;
	bool selectedState;
	bool disabledState;
	bool pulseEnabled;
	float pulseSpeed;
	float fillColor[4];
	float borderColor[4];
	float borderWidth;
	bool borderEnabled;
	bool hasBorderRect;
	float borderX;
	float borderY;
	float borderWidthRect;
	float borderHeightRect;
	bool hasArtRect;
	float artX;
	float artY;
	float artWidth;
	float artHeight;
	std::string buttonTemplate;
	std::string linkedImageKey;
	std::string linkedButtonKey;
	std::string buttonTemplatePart;
	float opacity;

	std::string rectRaw;
	std::string rectFieldName;

	float x;
	float y;
	float width;
	float height;

	bool hasUvPixels;
	float uvX;
	float uvY;
	float uvWidth;
	float uvHeight;
	bool hasPressedUvPixels;
	float uvPressedX;
	float uvPressedY;
	bool hasHighlightUvPixels;
	float uvHighlightX;
	float uvHighlightY;
	bool hasDisabledUvPixels;
	float uvDisabledX;
	float uvDisabledY;

	// GuiList authoring fields. Item row contents are always runtime-pushed via
	// setListItems(); only geometry and style are authored in the FIT.
	bool isListCell;
	float itemHeight;         // row height; 0 = auto (fontSize + 4)
	float itemOffsetX;        // text indent from list left edge
	float itemOffsetY;        // text nudge from row top
	float highlightColor[4];  // RGBA selection-highlight tint
	int listPreviewItemCount; // placeholder rows shown in the editor canvas

	UiEditorFitCell();
};

struct UiEditorFitLayout
{
	bool loaded;
	std::string sourcePath;
	std::string statusMessage;
	std::string key;
	std::string title;
	int designWidth;
	int designHeight;
	float mountOffsetX;
	float mountOffsetY;
	bool overlaysGameOS;
	std::string renderMode;
	std::string backgroundPolicy;
	std::string baseLayoutPath;
	std::string compositionMode;
	std::vector<UiEditorFitCell> cells;

	UiEditorFitLayout();
};

bool UiEditorFitLoadLayout(const char* path, UiEditorFitLayout& outLayout);
bool UiEditorFitSaveLayoutCopy(const UiEditorFitLayout& layout, const char* path, std::string& outStatus);
const UiEditorFitCell* UiEditorFitGetCell(const UiEditorFitLayout& layout, int index);
