// Shared surface of the UiDefs translation units (UiDefs*.cpp).
//
// UiDefs.cpp keeps the UiDefs/GameOSPage implementation; the defs-file parser, element builder and draw helpers live in
// UiDefs_parse/element/draw.cpp inside the internal namespace below.
#ifndef UIDEFS_INTERNAL_H
#define UIDEFS_INTERNAL_H

/***************************************************************
 * FILENAME: UiDefs.cpp
 * DESCRIPTION: data/defs UI Editor FIT runtime rendered as an ImGui HUD over GameOS.
 *
 * AUTHOR: Unknown
 * CREATED: Unknown
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-06-10
 *
 * CHANGES:
 * - Parses typed GuiPage / GuiImage / GuiText / GuiRect / GuiButton blocks.
 * - Resolves display text through data/defs StringCatalog keys first.
 * - Renders replacement pages through ImGui draw lists and routes button legacyIds.
 ***************************************************************/

#include "UiDefs.h"
#include "UiDefsLockstep.h"

#include "logisticsscreen.h"
#include "abutton.h"
#include "../GuiRuntime/GuiRuntime.h"
#include "mclib.h"
#include "userinput.h"
// UI-PHASE1-INTEGRATION-GAP: "StringCatalog.h" was #included but nothing in
// this file calls StringCatalog::* — no such header exists anywhere in this
// tree (not in the modder's file drop either), so it was dropped rather than
// invented. If display-text lookup through data/defs StringCatalog keys
// (per this file's own header comment) is actually needed, it needs a real
// implementation, not just this include.

// Public GameOS bridge: converts a gos texture handle to the OpenGL texture
// name that ImGui's OpenGL backend can draw.  This keeps art loading under
// the existing GameOS/mcTextureManager path while making the new UI layer a
// proper ImGui HUD.
// Legacy global help-caption id (gui/asystem.cpp). A hovered defs-page element
// publishes its help id here; LogisticsScreen::update() consumes and clears it.
extern long helpTextID;

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cmath>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace uidefs_detail {


// Scrollbar width in local (UI-space) units. Used in both the draw function and
// click handler so clicks land exactly on what the user sees.
constexpr int kScrollbarLocalW = 8;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Block {
    std::string type;
    std::unordered_map<std::string, std::string> fields;
};

struct UiElement {
    enum Kind { Image, RectElement, Text, Button, Placeholder, List, EditBox, Combo, Slider } kind = Placeholder;
    std::string key;
    std::string widgetType;
    Rect rect;
    Rect textRect;
    bool hasTextRect = false;
    bool visible = true;
    bool fromAnimation = false;
    bool outline = false;
    bool textOutline = false;   // legacy aButton data.outlineText: draw a hollow
                                // rect around the button's text rect in the text
                                // colour (the blue/orange box around the label).
    int legacyId = 0;
    int helpId = 0;             // helpDescLegacyId; hovered element sets ::helpTextID
    int legacyButtonIndex = -1;
    std::string legacySection;   // e.g. "Static4" / "Rect0" / "Text2" / "Button3"
    std::string texturePath;
    std::string legacyTexture;
    int textureNode = 0;
    bool textureNodeAssigned = false;
    bool flipV = false;
    bool pixelPerfect = false;  // nearest-filter this sprite (crisp; matches OG gos_FilterNone)
    bool alphaTest = false;     // binary alpha-test this sprite (solid/crisp; matches OG gos_AlphaTest)
    bool fitContain = false;    // image: letterbox-fit (preserve aspect, centered) vs stretch
    unsigned int gosTexture = 0;
    unsigned int gosTextureOverride = 0; // raw gos handle (e.g. live video frame); bypasses the texture manager
    int textureWidth = 0;
    int textureHeight = 0;
    int uvX = 0;
    int uvY = 0;
    int uvW = 0;
    int uvH = 0;
    bool uvLegacySpace = false; // uvXYWH addressed in legacy size/uvScale space
    unsigned int fillColor = 0x00000000;
    unsigned int borderColor = 0xff505050;
    unsigned int textColor = 0xffffffff;
    unsigned int pressedTextColor = 0xffffffff;
    unsigned int highlightTextColor = 0xffffffff;
    unsigned int disabledTextColor = 0xff6b6b6b;
    std::string textKey;
    std::string text;
    std::string fontPath;
    int fontId = 0;
    int fontSize = 12;
    int textAlign = 0;
    bool pressed = false;
    // Render a toggle/option button as a modern checkbox or radio ("circle check")
    // glyph driven by `pressed`, instead of the legacy art+animated label. Keeps
    // all button interaction (click->message->press sync).
    enum class ButtonStyle { Normal, Checkbox, Radio } buttonStyle = ButtonStyle::Normal;

    struct AnimKeyframe { float time = 0.0f; unsigned int color = 0xffffffff; };
    static constexpr int kMaxAnimKF = 8;

    int  legacyNormalAnimKFCount = 0;
    bool legacyNormalAnimLoops   = false;
    AnimKeyframe legacyNormalAnimKF[kMaxAnimKF];

    int  legacyPressedAnimKFCount = 0;
    bool legacyPressedAnimLoops   = false;
    AnimKeyframe legacyPressedAnimKF[kMaxAnimKF];

    int  legacyHighlightAnimKFCount = 0;
    bool legacyHighlightAnimLoops   = false;
    AnimKeyframe legacyHighlightAnimKF[kMaxAnimKF];

    int  legacyHighlightPressedAnimKFCount = 0;
    bool legacyHighlightPressedAnimLoops   = false;
    AnimKeyframe legacyHighlightPressedAnimKF[kMaxAnimKF];

    int  legacyDisabledAnimKFCount = 0;
    bool legacyDisabledAnimLoops   = false;
    AnimKeyframe legacyDisabledAnimKF[kMaxAnimKF];

    float animTimeNormal           = 0.0f;
    float animTimeHover            = 0.0f;
    float animTimePressed          = 0.0f;
    float animTimeHighlightPressed = 0.0f;
    float animTimeDisabled         = 0.0f;

    // Per-state UV overrides (-1 = use base uvX/uvY)
    int uvPressedX  = -1, uvPressedY  = -1;
    int uvHoverX    = -1, uvHoverY    = -1;
    int uvDisabledX = -1, uvDisabledY = -1;

    // Animation routing: legacyAnimateBmp → tint texture; legacyAnimateText → tint text
    bool legacyAnimateBmp  = true;
    bool legacyAnimateText = false;

    bool isHovered = false;
    enum class BtnState { Normal, Hover, Pressed, HighlightPressed, Disabled }
        btnState = BtnState::Normal;

    std::vector<std::string> items;
    // Optional per-item text color (ARGB). When an entry exists for a row it
    // overrides textColor; empty => every row uses textColor. Populated via
    // GameOSPage::setListItemColors (e.g. the encyclopedia weapon loadout,
    // which colors items by weapon range).
    std::vector<unsigned int> itemColors;
    int selectedIndex = -1;
    int scrollOffset = 0;
    unsigned int highlightColor = 0xff3a5f8a;
    unsigned int selectedTextColor = 0xffffffff;  // selected-row text color (orange list -> white when selected)
    int itemHeight = 0;     // 0 => derive from fontSize at render time
    int itemOffsetX = 4;
    int itemOffsetY = 2;
    int secondColumnX = 0;  // if >0, split item text on '\t': left part in [0..secondColumnX), right part starting at secondColumnX

    bool expanded = false;
    int popupHeight = 75;
    int hoverIndex = -1;   // row under the mouse while popup is open

    // Slider (GuiSlider) runtime state. Value in [0, sliderMax]; the owning screen
    // seeds it via setSliderValue and reads it back via getSliderValue each frame.
    int  sliderValue  = 0;
    int  sliderMax    = 255;
    bool sliderActive = false;   // true while the thumb is being dragged

    // EditBox (GuiEditBox) runtime state.
    // editBuffer is owned here and handed directly to ImGui::InputText each frame.
    static constexpr int kEditBufSize = 256;
    char editBuffer[kEditBufSize] = {};
    int  maxLength     = kEditBufSize - 1;
    bool focusRequested = false;    // consumed on first render after set
    bool editBoxActive  = false;    // true while widget has keyboard focus
};

// FIT pages are authored in a local coordinate space (GuiPage localWidth/
// localHeight, typically 800x600).  ImGui draws in window-logical pixels
// (io.DisplaySize), so every rect, font size, and mouse coordinate must be
// transformed between the two spaces.  Without this the page renders at
// authored size in the upper-left of the window.
struct PageScale {
    float sx = 1.0f;
    float sy = 1.0f;
    // display-pixel origin of the 16:9 UI canvas
    // (pillarbox/letterbox pads); 0,0 when the canvas fills the display.
    float ox = 0.0f;
    float oy = 0.0f;
};

struct FRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

std::string toLower(std::string v);
bool uiDefsTraceEnabled();
void uiDefsTrace(const char* fmt, ...);
void uiDefsLog(const char* fmt, ...);
bool parseInt(const std::string& s, int& out);
unsigned int parseColor(const std::unordered_map<std::string, std::string>& f, const char* name, unsigned int fallback);
std::string field(const std::unordered_map<std::string, std::string>& f, const char* name, const char* fallback = "");
Rect parseRect(const std::string& value);
bool pointInRect(const Rect& r, int x, int y);
std::vector<Block> parseTypedFit(const char* path);
std::filesystem::path defsRoot();
bool fileExistsPath(const std::filesystem::path& p);
bool uiPackageVersionCheckPassed();
std::filesystem::path resolveAssetPath(const std::string& value);
void traceMissingTextureOnce(const std::string& texturePath, const std::filesystem::path& resolved);
bool readUiTextureSize(const std::filesystem::path& path, int& width, int& height);
std::string baseNameNoExt(const char* path);
int alignmentFromString(const std::string& value);
int parseLegacyButtonIndex(const std::unordered_map<std::string, std::string>& f);
bool parseBool(const std::unordered_map<std::string, std::string>& f, const char* name, bool fallback = false);
UiElement makeElement(const Block& b);
PageScale currentPageScale(int localWidth, int localHeight);
FRect scaledRect(const Rect& r, int xOffset, int yOffset, const PageScale& s);
int scaledFontSize(int fontSize, const PageScale& s);
int messageIdForElement(LogisticsScreen* screen, const UiElement& e);
bool isButtonEnabled(LogisticsScreen* screen, const UiElement& e);
void drawUiRectElement(const FRect& r, unsigned int color, bool filled);
unsigned int sampleLegacyAnim(const UiElement::AnimKeyframe* kf, int count, bool loops, float t);
bool drawUiImageElement(UiElement& e, int xOffset, int yOffset, const PageScale& s, unsigned int tint = 0xffffffff);
void renderTextElement(UiElement& e, int xOffset, int yOffset, unsigned int color, const PageScale& s);
void drawUiListElement(UiElement& e, int xOffset, int yOffset, const PageScale& s);
void drawUiSliderElement(UiElement& e, int xOffset, int yOffset, const PageScale& s);
void drawUiCheckRadioElement(UiElement& e, int xOffset, int yOffset, const PageScale& s);
float getFrameDelta();

} // namespace uidefs_detail

using namespace uidefs_detail;

#endif // UIDEFS_INTERNAL_H
