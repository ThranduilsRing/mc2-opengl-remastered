/***************************************************************
 * FILENAME: UiDefs.h
 * DESCRIPTION: GameOS renderer for data/defs UI Editor FIT pages.
 *
 * AUTHOR: Unknown
 * CREATED: Unknown
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-06-10
 *
 * CHANGES:
 * - Added runtime loader for typed GuiPage / Gui* blocks under data/defs.
 * - Added GameOS draw/update bridge for replacement logistics UI pages.
 ***************************************************************/

#ifndef MC2R_UI_DEFS_H
#define MC2R_UI_DEFS_H

#include <string>
#include <vector>

class LogisticsScreen;

namespace UiDefs {

bool gameOsUiDefsEnabled();
bool editorUiDefsEnabled();
std::string replacementPathForLegacyFit(const char* legacyFitPath);
std::string replacementPathForEditorShell();

class GameOSPage {
public:
    GameOSPage();
    ~GameOSPage();

    GameOSPage(const GameOSPage&) = delete;
    GameOSPage& operator=(const GameOSPage&) = delete;

    bool load(const char* path);
    void clear();

    bool isLoaded() const;
    const char* key() const;

    // True when the page declares legacyPassthrough = true in its GuiPage block.
    // LogisticsScreen::render() renders the full legacy element set first, then
    // this page as an overlay on top — used for screens where the legacy system
    // draws the real content (options sub-tabs, planet background, etc.).
    bool isLegacyPassthrough() const;

    // Overrides the display text of the element with the given key at runtime.
    // Works for GuiText and GuiButton elements. Returns false if key not found.
    bool setElementText(const std::string& key, const std::string& text);

    // Runtime show/hide of any element by key (e.g. the Mech Lab weapon-info
    // icons that swap with the selected component type). Returns false if the
    // key is not found.
    bool setElementVisible(const std::string& key, bool visible);

    // Sets the texture path on a GuiImage element at runtime.
    // Clears the cached texture handle so it reloads on the next render frame.
    // Returns false if key not found.
    bool setElementTexture(const std::string& key, const std::string& texturePath);

    // Inject a runtime MC_TextureManager texture node (e.g. a mission tac-map
    // built from a .pak by getMissionTGA) into an image element; 0 clears it.
    bool setElementTextureNode(const std::string& key, long textureNode);
    // Raw gos texture handle (e.g. a live video frame); bypasses the texture
    // manager. 0 clears the override so the element falls back to its texture.
    bool setElementGosTexture(const std::string& key, unsigned int gosHandle);
    // On-screen rect of an element (using the current page scale) so game code can
    // overlay dynamic content (e.g. mission-briefing objective markers) aligned to it.
    bool getElementScreenRect(const std::string& key, float& x, float& y, float& w, float& h);
    // UI-LAYER-CONTRACT-2: page mirrors this legacy control section?
    bool coversLegacySection(const char* section) const;
    // kind ("Rect"/"Static"/"Text"/"Button") + index, prefix-agnostic.
    bool coversLegacyControl(const char* kind, int index) const;

    // File-backed image with an explicit texel sub-rect (uvX..uvW) and a
    // destination rect, for runtime-sized art like the encyclopedia weapon icon.
    bool setElementImageRegion(const std::string& key, const std::string& texturePath,
                               int uvX, int uvY, int uvW, int uvH,
                               int dstX, int dstY, int dstW, int dstH,
                               bool legacyUvSpace = false);

    void update(LogisticsScreen* target, int xOffset = 0, int yOffset = 0);
    void render(int xOffset = 0, int yOffset = 0);
    bool inside(int x, int y, int xOffset = 0, int yOffset = 0) const;

    // When the owning screen renders its live legacy animObjects through the
    // aObject GUI bridge, the page's static GuiAnimation snapshots must not
    // also draw (they are the same art without playback).
    void setSuppressAnimationElements(bool suppress);

    // List-element runtime data binding (GuiList elements: e.g. the
    // multiplayer map list, mechbay drop list). The FIT describes the box
    // geometry and one item-row template (font/size/align/spacing); row
    // contents are game data and are owned by the screen, set here each
    // time they change. elementKey is the GuiList element's "key" field
    // (e.g. "game.mcl_mp_loadmap_list0.list.map").
    bool setListItems(const std::string& elementKey, const std::vector<std::string>& items);
    // Optional per-item text colors (ARGB), parallel to the items. Shorter than
    // the item list => trailing rows fall back to the element's text color.
    bool setListItemColors(const std::string& elementKey, const std::vector<unsigned int>& colors);
    int  getListItemCount(const std::string& elementKey) const;

    // GuiSlider value (0..maxValue). setSliderValue clamps; getSliderValue reads
    // the live value the user dragged to.
    bool setSliderValue(const std::string& elementKey, int value);
    int  getSliderValue(const std::string& elementKey) const;
    int  getListSelection(const std::string& elementKey) const;
    void setListSelection(const std::string& elementKey, int index);

    // Edit box runtime data binding (GuiEditBox elements).
    // buf is a persistent caller-owned char[bufSize] modified in place by ImGui.
    // getEditText / setEditText read/write the internal buffer.
    // isEditBoxFocused returns true while the widget has keyboard focus.
    // isAnyEditBoxFocused returns true if any edit box on this page is focused.
    // requestEditFocus causes SetKeyboardFocusHere to be called on next render.
    // True if this page has at least one visible GuiEditBox element.
    // Callers use this to suppress legacy aEdit renders that would otherwise
    // double-draw under the ImGui edit box window.
    bool hasEditBox() const;

    bool getEditText(const std::string& elementKey, std::string& text) const;
    bool setEditText(const std::string& elementKey, const std::string& text);
    bool isEditBoxFocused(const std::string& elementKey) const;
    bool isAnyEditBoxFocused() const;
    void requestEditFocus(const std::string& elementKey);

private:
    struct Impl;
    Impl* impl;
};

} // namespace UiDefs

#endif // MC2R_UI_DEFS_H
