#include "UiDefs_internal.h"
#include "Gates/gate.h"
#include "../mclib/texture_assets.h"
#include <tracy/Tracy.hpp>
#include <set>
#include <string>


struct UiDefs::GameOSPage::Impl {
    bool loaded = false;
    std::string key;
    int localWidth = 800;
    int localHeight = 600;
    bool renderScaleTraced = false;
    // page-local -> screen transform needs the same offsets
    // render() was last called with; getElementScreenRect used (0,0) and
    // returned page-local coords, so the mech-preview composite landed at the
    // wrong screen position (e.g. mechlopedia's (285,58) page offset lost).
    int lastRenderXOffset = 0;
    int lastRenderYOffset = 0;
    bool suppressAnimationElements = false;
    bool legacyPassthrough = false;
    // draw-output telemetry for the zero-draw
    // fail-soft (LogisticsScreen falls back to legacy when a rendered
    // non-passthrough page emitted no draw commands).
    bool hasRendered = false;
    int  lastRenderDrawCount = 0;
    bool zeroDrawLogged = false;
    std::vector<UiElement> elements;
};

bool UiDefs::gameOsUiDefsEnabled()
{
    // The data/defs GameOS UI path is the replacement path, not an opt-in demo.
    // Set MC2_GAMEOS_UI_DEFS=0/false/off/no to force the legacy GameOS FIT path
    // when comparing behavior or bisecting UI regressions.
    return true;   // Std row, default on
}

bool UiDefs::editorUiDefsEnabled()
{
    // Mission Editor shell overlay is data/defs-first by default.  Force it off
    // with MC2_EDITOR_UI_DEFS=0 when comparing against the old pure-MFC shell.
    return mc2::gate::get<"MC2_EDITOR_UI_DEFS">();   // Std row, default on
}

std::string UiDefs::replacementPathForEditorShell()
{
    const std::filesystem::path path = defsRoot() / "ui" / "packages" / "default" / "editor" / "missionscrn.fit";
    if (fileExistsPath(path)) {
        uiDefsTrace("editor shell FIT=%s", path.string().c_str());
        return path.string();
    }
    uiDefsTrace("editor shell FIT missing at %s", path.string().c_str());
    return std::string();
}

std::string UiDefs::replacementPathForLegacyFit(const char* legacyFitPath)
{
    const std::string stem = baseNameNoExt(legacyFitPath);
    if (stem.empty())
        return std::string();

    // the mission-results defs page
    // (mcui_mr_layout: salvage / pilot review / promotion) is UNFINISHED
    // modder content — the game-side screens were explicitly not done in the
    // ImGui-port handoff, and the generated page breaks the AAR while pure
    // legacy renders it correctly. Keep the AAR on the legacy path until the
    // page is actually authored. MC2_UI_DEFS_RESULTS=1 opts the page back in
    // for iterating on it.
    // The generated
    // loading-screen pages (mcl_loadingscreen*) regressed the load sequence
    // (door art shifted to the top-left instead of centered, residual
    // logo/hourglass banner art over pilot-ready and mission start) while
    // pure legacy renders it correctly. Pin them to legacy until authored.
    // MC2_UI_DEFS_LOADSCREEN=1 opts back in for iteration.
    if (stem.rfind("mcl_loadingscreen", 0) == 0) {
        static const bool s_loadDefs = false;
        if (!s_loadDefs) {
            uiDefsTrace("legacy FIT %s -> LEGACY (loadscreen page opt-out)", legacyFitPath ? legacyFitPath : "<null>");
            return std::string();
        }
    }

    if (stem == "mcui_mr_layout") {
        static const bool s_resultsDefs = false;
        if (!s_resultsDefs) {
            uiDefsTrace("legacy FIT %s -> LEGACY (results page opt-out)", legacyFitPath ? legacyFitPath : "<null>");
            return std::string();
        }
    }

    const std::filesystem::path root = defsRoot() / "ui" / "packages" / "default";
    const std::filesystem::path gamePath = root / "game" / (stem + ".fit");
    if (fileExistsPath(gamePath)) {
        uiDefsTrace("legacy FIT %s -> %s", legacyFitPath ? legacyFitPath : "<null>", gamePath.string().c_str());
        return gamePath.string();
    }

    if (stem == "mcl_m$") {
        const std::filesystem::path dollarPath = root / "game" / "mcl_mdollar.fit";
        if (fileExistsPath(dollarPath)) {
            uiDefsTrace("legacy FIT %s -> %s", legacyFitPath ? legacyFitPath : "<null>", dollarPath.string().c_str());
            return dollarPath.string();
        }
    }

    const std::filesystem::path editorPath = root / "editor" / (stem + ".fit");
    if (fileExistsPath(editorPath)) {
        uiDefsTrace("legacy FIT %s -> %s", legacyFitPath ? legacyFitPath : "<null>", editorPath.string().c_str());
        return editorPath.string();
    }

    uiDefsTrace("legacy FIT %s has no data/defs replacement under %s", legacyFitPath ? legacyFitPath : "<null>", root.string().c_str());
    return std::string();
}

UiDefs::GameOSPage::GameOSPage()
    : impl(new Impl())
{
}

UiDefs::GameOSPage::~GameOSPage()
{
    clear();
    delete impl;
    impl = nullptr;
}

bool UiDefs::GameOSPage::load(const char* path)
{
    clear();
    if (!path || !path[0])
        return false;

    // version-skewed package => refuse every page
    // (loud, once) so the caller falls back to the legacy screen path.
    if (!uiPackageVersionCheckPassed()) {
        uiDefsTrace("page load refused (package version gate): %s", path);
        return false;
    }

    if (!fileExistsPath(std::filesystem::path(path))) {
        uiDefsLog("page load FAILED (file missing): %s -- falling back to legacy screen", path);
        return false;
    }

    const std::vector<Block> blocks = parseTypedFit(path);
    if (blocks.empty()) {
        uiDefsLog("page load FAILED (parse failure / no typed blocks): %s -- falling back to legacy screen", path);
        return false;
    }

    std::string pageKey;
    for (const Block& b : blocks) {
        if (b.type == "GuiPage" || b.type == "UiV2Page") {
            pageKey = field(b.fields, "key");
            impl->key = pageKey;
            parseInt(field(b.fields, "localWidth", "800"), impl->localWidth);
            parseInt(field(b.fields, "localHeight", "600"), impl->localHeight);
            impl->legacyPassthrough = parseBool(b.fields, "legacyPassthrough", false);
            break;
        }
    }

    for (const Block& b : blocks) {
        if (b.type == "GuiPage" || b.type == "UiV2Page" || b.type == "GuiMetadata")
            continue;
        // V2 metadata/unrenderable blocks.
        if (b.type == "UiV2FamilyCount" || b.type == "UiV2Cursor" ||
            b.type == "UiV2SourceBlock" || b.type == "UiV2Animation")
            continue;
        // Shadowed duplicate blocks are unreachable by the engine (seekBlock
        // is first-match-wins); skip them.
        if (field(b.fields, "shadowedDuplicate") == "true")
            continue;
        const std::string page = field(b.fields, "pageKey");
        if (!pageKey.empty() && !page.empty() && page != pageKey)
            continue;
        UiElement e = makeElement(b);
        // Load runtime-populated elements too (authored visible=false with no static
        // string, e.g. the cbills readout / help text area); the render + hit-test
        // passes already skip !visible, and setElementText reveals them once real text
        // is routed in. Dropping them here made those keys unreachable at runtime.
        if (e.rect.w >= 0 && e.rect.h >= 0)
            impl->elements.push_back(e);
    }

    // a page with zero elements can never draw
    // anything; accepting it would arm the black-clear over an empty page
    // Fail loud so the legacy screen renders instead.
    if (!pageElementCountAcceptable(impl->elements.size())) {
        uiDefsLog("page load FAILED (zero elements): key=%s path=%s -- falling back to legacy screen",
                  impl->key.empty() ? "<none>" : impl->key.c_str(), path);
        clear();
        return false;
    }

    // Pages are initialized while the previous screen/loading screen is active.
    // Resolve authored art and queue decode here, not on the first visible draw.
    if (mc2::textures::ready()) {
        ZoneScopedN("UiDefs.PreloadTextures");
        for (auto& element : impl->elements) {
            ensureTexture(element);
            if (element.textureNode) (void)mc2::textures::uiId(element.textureNode);
        }
    }
    impl->loaded = true;
    uiDefsTrace("loaded page key=%s file=%s elements=%zu local=%dx%d",
        impl->key.c_str(), path, impl->elements.size(), impl->localWidth, impl->localHeight);
    return true;
}

void UiDefs::GameOSPage::clear()
{
    if (!impl)
        return;
    // Texture nodes are intentionally left under mc2::textures::ready() ownership.
    // Several legacy UI classes follow the same cache lifetime; removing here
    // can invalidate shared page art while another screen still references it.
    impl->elements.clear();
    impl->key.clear();
    impl->loaded = false;
}

bool UiDefs::GameOSPage::isLoaded() const
{
    return impl && impl->loaded;
}

bool UiDefs::GameOSPage::isLegacyPassthrough() const
{
    return impl && impl->legacyPassthrough;
}

bool UiDefs::GameOSPage::setElementText(const std::string& elementKey, const std::string& text)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key == elementKey) {
            e.text = text;
            // Runtime-populated text elements (cbills readout, help area) are authored
            // visible=false by the converter since they have no static string; reveal
            // them once real text is routed in (empty text still renders nothing).
            if (!text.empty()) e.visible = true;
            return true;
        }
    }
    uiDefsTrace("setElementText MISS key=%s", elementKey.c_str());
    return false;
}

bool UiDefs::GameOSPage::setElementVisible(const std::string& elementKey, bool visible)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key == elementKey) {
            e.visible = visible;
            return true;
        }
    }
    return false;
}

bool UiDefs::GameOSPage::setElementTexture(const std::string& key, const std::string& texturePath)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key == key) {
            if (e.texturePath != texturePath) {
                e.texturePath = texturePath;
                e.textureNodeAssigned = false;
                // the fit-authored pixel sub-rect (uvX/Y/W/H)
                // and cached texture size belong to the PREVIOUS texture. Left
                // stale, a runtime swap (e.g. per-pilot portrait) crops the new
                // texture with the old sheet's sub-rect -> striped garbage.
                // Reset like setElementTextureNode does; ensureTexture re-derives
                // size (and full-texture UV) for the new asset.
                e.uvX = e.uvY = e.uvW = e.uvH = 0;
                e.flipV = false;
                e.textureWidth = e.textureHeight = 0;
            }
            return true;
        }
    }
    return false;
}

bool UiDefs::GameOSPage::setElementTextureNode(const std::string& key, long textureNode)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key != key)
            continue;
        // Runtime-supplied texture (e.g. a mission tac-map extracted from a .pak
        // by getMissionTGA): inject the legacy texture manager node directly instead
        // of loading from a file path. drawUiImageElement resolves the GL handle
        // from textureNode every frame.
        e.texturePath.clear();
        if (textureNode <= 0) {
            e.textureNode = 0;
            e.textureNodeAssigned = false;   // nothing to draw
        } else {
            e.textureNode = static_cast<int>(textureNode);
            e.textureNodeAssigned = true;
            e.flipV = true;                           // pak TGA is stored flipped
            e.uvX = e.uvY = e.uvW = e.uvH = 0;        // full-texture UV (0..1)
            e.textureWidth = e.textureHeight = 0;     // query logical size lazily
        }
        return true;
    }
    return false;
}

// does this page carry an element mirroring the given
// legacy control section ("Static4" / "Rect0" / "Text2" / "Button3")? Used by
// LogisticsScreen's inverted bridging: legacy widgets WITHOUT page coverage
// render through the gui bridge; covered ones stand down (page owns them).
bool UiDefs::GameOSPage::coversLegacySection(const char* section) const
{
    if (!isLoaded() || !section || !section[0]) return false;
    for (const UiElement& e : impl->elements)
        if (e.legacySection == section)
            return true;
    return false;
}

// kind+index coverage. Legacy section names are NOT
// uniform across screens ("Text13" in some fits, "MechBayTextEntry13" in
// others), so match by kind substring + trailing integer instead of exact
// string. kind is "Rect" / "Static" / "Text" / "Button".
bool UiDefs::GameOSPage::coversLegacyControl(const char* kind, int index) const
{
    if (!isLoaded() || !kind || !kind[0]) return false;
    for (const UiElement& e : impl->elements) {
        const std::string& sec = e.legacySection;
        if (sec.empty() || sec.find(kind) == std::string::npos)
            continue;
        // trailing integer
        size_t end = sec.size();
        size_t beg = end;
        while (beg > 0 && isdigit((unsigned char)sec[beg-1])) --beg;
        if (beg == end) continue;   // no numeric suffix
        int idx = 0;
        for (size_t i = beg; i < end; ++i) idx = idx*10 + (sec[i]-'0');
        if (idx == index)
            return true;
    }
    return false;
}

bool UiDefs::GameOSPage::getElementScreenRect(const std::string& key, float& x, float& y, float& w, float& h)
{
    if (!isLoaded()) return false;
    for (const UiElement& e : impl->elements) {
        if (e.key != key)
            continue;
        const PageScale s = currentPageScale(impl->localWidth, impl->localHeight);
        // Use the same offsets render() draws with so this is a true SCREEN rect.
        const FRect r = scaledRect(e.rect, impl->lastRenderXOffset, impl->lastRenderYOffset, s);
        x = r.x; y = r.y; w = r.w; h = r.h;
        return true;
    }
    return false;
}

bool UiDefs::GameOSPage::setElementGosTexture(const std::string& key, unsigned int gosHandle)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key != key)
            continue;
        // Direct raw gos texture handle (e.g. a live decoded video frame). Unlike
        // setElementTextureNode this is NOT a texture-manager node: drawUiImageElement
        // draws the GL texture this gos handle backs and skips the manager lookup.
        // gosHandle == 0 clears the override so the element falls back to its
        // authored texture/node.
        e.gosTextureOverride = gosHandle;
        if (gosHandle != 0) {
            e.uvX = e.uvY = e.uvW = e.uvH = 0;     // full-frame UV (0..1)
            e.textureWidth = e.textureHeight = 0;
        }
        return true;
    }
    return false;
}

bool UiDefs::GameOSPage::setElementImageRegion(const std::string& key,
                                               const std::string& texturePath,
                                               int uvX, int uvY, int uvW, int uvH,
                                               int dstX, int dstY, int dstW, int dstH,
                                               bool legacyUvSpace)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.key != key)
            continue;
        // Runtime file-backed image with an explicit sub-rect (texel UVs) and a
        // destination rect -- e.g. the encyclopedia weapon icon, sized
        // cellW*48 x cellH*32 of its icon sheet, centered in the icon box.
        // legacyUvSpace: uv coords are in the legacy size/uvScale addressing
        // space (old setUVs math), not the logical PNG space.
        if (e.texturePath != texturePath) {
            e.texturePath = texturePath;
            e.textureNodeAssigned = false;        // force reload
            e.textureNode = 0;
            e.textureWidth = e.textureHeight = 0;  // re-query logical size on load
        }
        e.uvX = uvX; e.uvY = uvY; e.uvW = uvW; e.uvH = uvH;
        e.uvLegacySpace = legacyUvSpace;
        e.flipV = false;
        e.rect = { dstX, dstY, dstW, dstH };
        return true;
    }
    return false;
}

void UiDefs::GameOSPage::setSuppressAnimationElements(bool suppress)
{
    if (impl)
        impl->suppressAnimationElements = suppress;
}

const char* UiDefs::GameOSPage::key() const
{
    return impl ? impl->key.c_str() : "";
}

void UiDefs::GameOSPage::update(LogisticsScreen* target, int xOffset, int yOffset)
{
    if (!isLoaded() || !target || !userInput)
        return;

    // GameOS mouse coordinates live in Environment.screenWidth/Height space
    // (the legacy logical resolution), NOT window pixels -- the GL backend
    // scales legacy draws and input between Environment space and the real
    // window.  Elements live in the page's local space, so the hit-test
    // transform is Environment -> local.  When Environment matches the
    // authored local size (800x600) this is the identity; if the port is
    // ever switched to native-resolution Environment it stays correct.
    const float mouseSx = (Environment.screenWidth > 0 && impl->localWidth > 0)
        ? static_cast<float>(impl->localWidth) / static_cast<float>(Environment.screenWidth)
        : 1.0f;
    const float mouseSy = (Environment.screenHeight > 0 && impl->localHeight > 0)
        ? static_cast<float>(impl->localHeight) / static_cast<float>(Environment.screenHeight)
        : 1.0f;

    const bool clicked = userInput->isLeftClick();
    const bool released = userInput->leftMouseReleased();
    const int mx = static_cast<int>(userInput->getMouseX() * mouseSx) - xOffset;
    const int my = static_cast<int>(userInput->getMouseY() * mouseSy) - yOffset;

    if (clicked && uiDefsTraceEnabled())
        uiDefsTrace("click: raw=(%d,%d) env=%dx%d drawable=%dx%d local=(%d,%d) mscale=(%.3f,%.3f) offset=(%d,%d)",
                    (int)userInput->getMouseX(), (int)userInput->getMouseY(),
                    (int)Environment.screenWidth, (int)Environment.screenHeight,
                    (int)Environment.drawableWidth, (int)Environment.drawableHeight,
                    mx, my, mouseSx, mouseSy, xOffset, yOffset);

    // GuiCombo: consume click when it lands in an expanded popup or collapses it.
    bool clickConsumed = false;
    {
        for (UiElement& e : impl->elements) {
            if (e.kind != UiElement::Combo || !e.expanded)
                continue;
            const Rect popupRect = { e.rect.x, e.rect.y + e.rect.h, e.rect.w, e.popupHeight };
            if (clicked) {
                if (pointInRect(popupRect, mx, my)) {
                    const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
                    if (rowH > 0) {
                        const int row = (my - popupRect.y - e.itemOffsetY) / rowH;
                        const int itemIndex = e.scrollOffset + row;
                        if (row >= 0 && itemIndex >= 0 && itemIndex < static_cast<int>(e.items.size()))
                            e.selectedIndex = itemIndex;
                    }
                    e.expanded = false;
                    clickConsumed = true;
                } else if (!pointInRect(e.rect, mx, my)) {
                    // Click outside header and popup: collapse.
                    e.expanded = false;
                }
            }
        }
    }
    // Update hoverIndex for expanded combos so the render pass can draw hover highlight.
    for (UiElement& e : impl->elements) {
        if (e.kind != UiElement::Combo || !e.expanded) {
            e.hoverIndex = -1;
            continue;
        }
        const Rect popupRect = { e.rect.x, e.rect.y + e.rect.h, e.rect.w, e.popupHeight };
        if (!pointInRect(popupRect, mx, my)) {
            e.hoverIndex = -1;
            continue;
        }
        const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
        if (rowH <= 0) { e.hoverIndex = -1; continue; }
        const int row = (my - popupRect.y - e.itemOffsetY) / rowH;
        const int idx = e.scrollOffset + row;
        e.hoverIndex = (row >= 0 && idx >= 0 && idx < static_cast<int>(e.items.size())) ? idx : -1;
    }

    // GuiCombo header click: toggle expand, collapsing any other open combos.
    if (clicked && !clickConsumed) {
        for (UiElement& e : impl->elements) {
            if (e.kind != UiElement::Combo)
                continue;
            if (pointInRect(e.rect, mx, my)) {
                const bool newExpanded = !e.expanded;
                if (uiDefsTraceEnabled())
                    uiDefsTrace("combo click: key=%s items=%zu expanded=%d->%d click=(%d,%d) rect={%d,%d,%d,%d}",
                                e.key.c_str(), e.items.size(),
                                (int)e.expanded, (int)newExpanded,
                                mx, my,
                                e.rect.x, e.rect.y, e.rect.w, e.rect.h);
                for (UiElement& other : impl->elements)
                    if (other.kind == UiElement::Combo)
                        other.expanded = false;
                e.expanded = newExpanded;
                // Snap scrollOffset to show the selected item when the popup
                // first opens, so the current choice is immediately visible.
                if (newExpanded && !e.items.empty() && e.selectedIndex >= 0) {
                    const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
                    const int visRows = rowH > 0 ? e.popupHeight / rowH : 0;
                    const int icount = static_cast<int>(e.items.size());
                    const int maxSc = std::max(0, icount - visRows);
                    if (e.selectedIndex < e.scrollOffset)
                        e.scrollOffset = e.selectedIndex;
                    else if (visRows > 0 && e.selectedIndex >= e.scrollOffset + visRows)
                        e.scrollOffset = std::min(e.selectedIndex - visRows + 1, maxSc);
                }
                clickConsumed = true;
                break;
            }
        }
    }

    for (UiElement& e : impl->elements) {
        if (e.kind != UiElement::Button)
            continue;

        const int messageId = messageIdForElement(target, e);
        if (messageId <= 0)
            continue;

        // Sync persistent pressed state (checkbox/radio) from the legacy aButton.
        // This ensures initial state (set by reset()) is reflected visually, and
        // that toggle state persists after handleMessage() flips the legacy button.
        {
            aButton* syncBtn = target->getButton(static_cast<long>(messageId));
            if (syncBtn) e.pressed = syncBtn->isPressed();
        }

        const bool enabled = isButtonEnabled(target, e);
        const bool insideElement = pointInRect(e.rect, mx, my) || (e.hasTextRect && pointInRect(e.textRect, mx, my));
        if (!enabled) {
            e.pressed = false;
            e.isHovered = false;
        } else {
            if (insideElement && clicked && !clickConsumed) {
                e.pressed = true;
                target->handleMessage(aMSG_LEFTMOUSEDOWN, static_cast<unsigned long>(messageId));
            } else if (released) {
                e.pressed = false;
            }
            e.isHovered = insideElement && !e.pressed;
            // Feed the legacy help system: a hovered button publishes its help
            // caption id, which LogisticsScreen::update() (running right after
            // this call) loads into the screen's help-text box. Reset to 0 at the
            // end of that same update, so it only persists while hovered.
            if (insideElement && e.helpId > 0)
                ::helpTextID = e.helpId;
        }

        const UiElement::BtnState ns =
            !enabled                         ? UiElement::BtnState::Disabled
            : (e.pressed && insideElement)   ? UiElement::BtnState::HighlightPressed
            : e.pressed                      ? UiElement::BtnState::Pressed
            : insideElement                  ? UiElement::BtnState::Hover
                                             : UiElement::BtnState::Normal;
        if (ns != e.btnState) {
            // Reset one-shot state timers on entry; looping timers continue.
            if (ns == UiElement::BtnState::Pressed)          e.animTimePressed          = 0.0f;
            if (ns == UiElement::BtnState::HighlightPressed) e.animTimeHighlightPressed = 0.0f;
            if (ns == UiElement::BtnState::Hover)            e.animTimeHover            = 0.0f;
            e.btnState = ns;
        }
    }

    // GuiList scrollbar click: click inside the right kScrollbarLocalW-unit band
    // of a list scrolls to the proportional position. Must be checked before the
    // item-select pass so a scrollbar click doesn't also change selection.
    if (clicked && !clickConsumed) {
        for (UiElement& e : impl->elements) {
            if (e.kind != UiElement::List || e.items.empty())
                continue;
            if (!pointInRect(e.rect, mx, my))
                continue;

            const int sbBandX = e.rect.x + e.rect.w - kScrollbarLocalW;
            if (mx < sbBandX)
                continue;

            const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
            if (rowH <= 0)
                continue;
            const int visRows = e.rect.h / rowH;
            const int maxScroll = std::max(0, static_cast<int>(e.items.size()) - visRows);
            if (maxScroll <= 0)
                continue;

            const float trackRelY = static_cast<float>(my - e.rect.y) / static_cast<float>(e.rect.h);
            const float clamped = std::max(0.0f, std::min(1.0f, trackRelY));
            e.scrollOffset = static_cast<int>(std::round(clamped * static_cast<float>(maxScroll)));
            clickConsumed = true;
            break;
        }
    }

    // GuiList click-to-select: a click inside the box selects the row under
    // the cursor. Screens read the result back via getListSelection() and
    // decide what a selection change means (e.g. MPLoadMap::updateMapInfo).
    if (clicked && !clickConsumed) {
        for (UiElement& e : impl->elements) {
            if (e.kind != UiElement::List || e.items.empty())
                continue;
            if (!pointInRect(e.rect, mx, my))
                continue;

            const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
            if (rowH <= 0)
                continue;

            const int rowInBox = (my - e.rect.y - e.itemOffsetY) / rowH;
            const int itemIndex = e.scrollOffset + rowInBox;
            if (rowInBox >= 0 && itemIndex >= 0 && itemIndex < static_cast<int>(e.items.size()))
                e.selectedIndex = itemIndex;
        }
    }

    // GuiSlider: grab on press inside the track, then follow the mouse X while the
    // button stays down. Value 0 at the left edge, sliderMax at the right; the
    // thumb center tracks the cursor (matching drawUiSliderElement's geometry).
    {
        const bool heldDown = (userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN);
        for (UiElement& e : impl->elements) {
            if (e.kind != UiElement::Slider)
                continue;
            if (clicked && pointInRect(e.rect, mx, my))
                e.sliderActive = true;
            if (!heldDown)
                e.sliderActive = false;
            if (e.sliderActive && e.rect.w > 0) {
                const float thumbW = std::max(8.0f, static_cast<float>(e.rect.h) * 0.55f);
                const float travel = std::max(1.0f, static_cast<float>(e.rect.w) - thumbW);
                float frac = (static_cast<float>(mx - e.rect.x) - thumbW * 0.5f) / travel;
                frac = std::clamp(frac, 0.0f, 1.0f);
                e.sliderValue = static_cast<int>(frac * static_cast<float>(e.sliderMax) + 0.5f);
            }
        }
    }

    // Mouse wheel: scroll GuiList and expanded GuiCombo popups.
    // getMouseWheelDelta() > 0 = scroll down = increase scrollOffset.
    {
        const long wheelDelta = userInput->getMouseWheelDelta();
        if (wheelDelta != 0) {
            const int delta = (wheelDelta > 0) ? 3 : -3;
            for (UiElement& e : impl->elements) {
                bool isScrollable = false;
                Rect checkRect = e.rect;
                if (e.kind == UiElement::List) {
                    isScrollable = !e.items.empty();
                } else if (e.kind == UiElement::Combo && e.expanded) {
                    isScrollable = !e.items.empty();
                    // Accept scroll anywhere in the header+popup area so the
                    // user doesn't have to move into the popup to wheel-scroll.
                    checkRect = { e.rect.x, e.rect.y, e.rect.w, e.rect.h + e.popupHeight };
                }
                if (!isScrollable || !pointInRect(checkRect, mx, my))
                    continue;
                const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
                const int rectH = (e.kind == UiElement::Combo) ? e.popupHeight : e.rect.h;
                const int visibleRows = rowH > 0 ? rectH / rowH : 0;
                const int maxScroll = std::max(0, static_cast<int>(e.items.size()) - visibleRows);
                e.scrollOffset = std::clamp(e.scrollOffset + delta, 0, maxScroll);
                break;
            }
        }
    }
}

void UiDefs::GameOSPage::render(int xOffset, int yOffset)
{
    if (!isLoaded())
        return;

    const float frameDelta = getFrameDelta();
    impl->lastRenderXOffset = xOffset;
    impl->lastRenderYOffset = yOffset;
    const PageScale s = currentPageScale(impl->localWidth, impl->localHeight);

    // One-shot per page (MC2_UI_DEFS_TRACE=1): proves THIS scaled render
    // loop is the one in the running exe and shows the scale it applied.
    if (uiDefsTraceEnabled() && !impl->renderScaleTraced)
    {
        impl->renderScaleTraced = true;
        uiDefsTrace("render[scaled-v2]: page local=%dx%d sx=%.3f sy=%.3f elements=%d",
                    impl->localWidth, impl->localHeight, s.sx, s.sy,
                    (int)impl->elements.size());
    }

    // Legacy LogisticsScreen::render draws in z-passes, not file order:
    //   1. fully-opaque filled rects (panel backings, behind everything)
    //   2. statics/images
    //   3. outline rects
    //   4. semi-transparent filled rects ("transparencies after statics")
    //   5. buttons
    //   6. text
    // Reproduce that here; within a pass, file order holds.
    enum Pass { OpaqueFills, Images, Outlines, TransparentFills, Buttons, Texts, Popups, PassCount };
    // count draw-emitting element visits this frame.
    // Suppressed animation elements count as output: their content IS on
    // screen, drawn by the live legacy animObject twins through the aObject
    // GUI bridge (e.g. the splash intro page is ONLY animation elements --
    // without this it would false-positive the zero-draw fail-soft).
    int drawn = 0;
    if (impl->suppressAnimationElements) {
        for (const UiElement& e : impl->elements)
            if (e.visible && e.fromAnimation)
                ++drawn;
    }
    for (int pass = 0; pass < PassCount; ++pass) {
        for (UiElement& e : impl->elements) {
            if (!e.visible)
                continue;
            if (e.fromAnimation && impl->suppressAnimationElements)
                continue;

            switch (pass) {
            case OpaqueFills:
                if ((e.fillColor & 0xff000000) == 0xff000000 &&
                    ((e.kind == UiElement::RectElement && !e.outline) ||
                      e.kind == UiElement::Text)) {
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.fillColor, true);
                    ++drawn;
                }
                break;
            case Images:
                if (e.kind == UiElement::Image) {
                    drawUiImageElement(e, xOffset, yOffset, s);
                    ++drawn;
                } else if (e.kind == UiElement::List) {
                    drawUiListElement(e, xOffset, yOffset, s);
                    ++drawn;
                } else if (e.kind == UiElement::Slider) {
                    drawUiSliderElement(e, xOffset, yOffset, s);
                    ++drawn;
                } else if (e.kind == UiElement::Combo) {
                    ++drawn;
                    // Collapsed header: fill, border, arrow indicator, selected item text.
                    const FRect r = scaledRect(e.rect, xOffset, yOffset, s);
                    drawUiRectElement(r, e.fillColor, true);
                    drawUiRectElement(r, e.borderColor, false);
                    // Down-pointing triangle arrow on the right side of the header.
                    {
                        const int arrowW = e.rect.h;
                        const FRect ar = scaledRect(
                            { e.rect.x + e.rect.w - arrowW, e.rect.y, arrowW, e.rect.h },
                            xOffset, yOffset, s);
                        GuiRuntime::DrawUiTriangle(ar.x, ar.y, ar.w, ar.h, e.textColor);
                    }
                    if (e.selectedIndex >= 0 && e.selectedIndex < static_cast<int>(e.items.size())) {
                        UiElement row = e;
                        row.kind = UiElement::Text;
                        row.hasTextRect = false;
                        row.rect = { e.rect.x + e.itemOffsetX, e.rect.y + e.itemOffsetY,
                                     e.rect.w - 2 * e.itemOffsetX - e.rect.h, e.rect.h };
                        row.text = e.items[e.selectedIndex];
                        renderTextElement(row, xOffset, yOffset, e.textColor, s);
                    }
                } else if (e.kind == UiElement::Placeholder) {
                    const FRect r = scaledRect(e.rect, xOffset, yOffset, s);
                    drawUiRectElement(r, e.fillColor ? e.fillColor : 0x30000000, true);
                    drawUiRectElement(r, e.borderColor, false);
                    renderTextElement(e, xOffset, yOffset, e.textColor, s);
                    ++drawn;
                }
                break;
            case Outlines:
                if (e.kind == UiElement::RectElement &&
                    (e.outline || (e.borderColor & 0xff000000))) {
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.borderColor, false);
                    ++drawn;
                }
                break;
            case TransparentFills:
                if (e.kind == UiElement::RectElement && !e.outline &&
                    (e.fillColor & 0xff000000) != 0 &&
                    (e.fillColor & 0xff000000) != 0xff000000) {
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.fillColor, true);
                    ++drawn;
                }
                break;
            case Buttons: {
                if (e.kind == UiElement::Button &&
                    e.buttonStyle != UiElement::ButtonStyle::Normal) {
                    // Modern checkbox/radio glyph + stable label (no flash).
                    drawUiCheckRadioElement(e, xOffset, yOffset, s);
                    ++drawn;
                } else if (e.kind == UiElement::Button) {
                    ++drawn;
                    unsigned int artTint;
                    unsigned int textCol;
                    int stateUvX = e.uvX;
                    int stateUvY = e.uvY;
                    switch (e.btnState) {
                    case UiElement::BtnState::Disabled: {
                        e.animTimeDisabled += frameDelta;
                        const unsigned int ac = (e.legacyDisabledAnimKFCount > 0)
                            ? sampleLegacyAnim(e.legacyDisabledAnimKF,
                                               e.legacyDisabledAnimKFCount,
                                               e.legacyDisabledAnimLoops,
                                               e.animTimeDisabled)
                            : 0xffffffff;
                        // When the art is NOT animated, tint it with the button's
                        // own colour (colorArgb) rather than white -- so a frame
                        // that needs a base tint (e.g. the red DEFAULT button) stays
                        // that colour and doesn't vanish on hover. White-tinted
                        // sprites have colorArgb=white, so they are unaffected.
                        artTint = e.legacyAnimateBmp  ? ac : e.textColor;
                        textCol = e.legacyAnimateText ? ac : e.disabledTextColor;
                        if (e.uvDisabledX >= 0) { stateUvX = e.uvDisabledX; stateUvY = e.uvDisabledY; }
                        break;
                    }
                    case UiElement::BtnState::HighlightPressed: {
                        e.animTimeHighlightPressed += frameDelta;
                        unsigned int ac;
                        if (e.legacyHighlightPressedAnimKFCount > 0)
                            ac = sampleLegacyAnim(e.legacyHighlightPressedAnimKF,
                                                   e.legacyHighlightPressedAnimKFCount,
                                                   e.legacyHighlightPressedAnimLoops,
                                                   e.animTimeHighlightPressed);
                        else if (e.legacyHighlightAnimKFCount > 0)
                            ac = sampleLegacyAnim(e.legacyHighlightAnimKF,
                                                   e.legacyHighlightAnimKFCount,
                                                   e.legacyHighlightAnimLoops,
                                                   e.animTimeHighlightPressed);
                        else
                            ac = 0xffffffff;
                        // When the art is NOT animated, tint it with the button's
                        // own colour (colorArgb) rather than white -- so a frame
                        // that needs a base tint (e.g. the red DEFAULT button) stays
                        // that colour and doesn't vanish on hover. White-tinted
                        // sprites have colorArgb=white, so they are unaffected.
                        artTint = e.legacyAnimateBmp  ? ac : e.textColor;
                        textCol = e.legacyAnimateText ? ac : e.pressedTextColor;
                        if (e.uvPressedX >= 0) { stateUvX = e.uvPressedX; stateUvY = e.uvPressedY; }
                        break;
                    }
                    case UiElement::BtnState::Pressed: {
                        e.animTimePressed += frameDelta;
                        const unsigned int ac = (e.legacyPressedAnimKFCount > 0)
                            ? sampleLegacyAnim(e.legacyPressedAnimKF,
                                               e.legacyPressedAnimKFCount,
                                               e.legacyPressedAnimLoops,
                                               e.animTimePressed)
                            : 0xffffffff;
                        // When the art is NOT animated, tint it with the button's
                        // own colour (colorArgb) rather than white -- so a frame
                        // that needs a base tint (e.g. the red DEFAULT button) stays
                        // that colour and doesn't vanish on hover. White-tinted
                        // sprites have colorArgb=white, so they are unaffected.
                        artTint = e.legacyAnimateBmp  ? ac : e.textColor;
                        textCol = e.legacyAnimateText ? ac : e.pressedTextColor;
                        if (e.uvPressedX >= 0) { stateUvX = e.uvPressedX; stateUvY = e.uvPressedY; }
                        break;
                    }
                    case UiElement::BtnState::Hover: {
                        e.animTimeHover += frameDelta;
                        const unsigned int ac = (e.legacyHighlightAnimKFCount > 0)
                            ? sampleLegacyAnim(e.legacyHighlightAnimKF,
                                               e.legacyHighlightAnimKFCount,
                                               e.legacyHighlightAnimLoops,
                                               e.animTimeHover)
                            : 0xffffffff;
                        // When the art is NOT animated, tint it with the button's
                        // own colour (colorArgb) rather than white -- so a frame
                        // that needs a base tint (e.g. the red DEFAULT button) stays
                        // that colour and doesn't vanish on hover. White-tinted
                        // sprites have colorArgb=white, so they are unaffected.
                        artTint = e.legacyAnimateBmp  ? ac : e.textColor;
                        textCol = e.legacyAnimateText ? ac : e.highlightTextColor;
                        if (e.uvHoverX >= 0) { stateUvX = e.uvHoverX; stateUvY = e.uvHoverY; }
                        break;
                    }
                    default: { // Normal
                        e.animTimeNormal += frameDelta;
                        const unsigned int ac = (e.legacyNormalAnimKFCount > 0)
                            ? sampleLegacyAnim(e.legacyNormalAnimKF,
                                               e.legacyNormalAnimKFCount,
                                               e.legacyNormalAnimLoops,
                                               e.animTimeNormal)
                            : 0xffffffff;
                        // When the art is NOT animated, tint it with the button's
                        // own colour (colorArgb) rather than white -- so a frame
                        // that needs a base tint (e.g. the red DEFAULT button) stays
                        // that colour and doesn't vanish on hover. White-tinted
                        // sprites have colorArgb=white, so they are unaffected.
                        artTint = e.legacyAnimateBmp  ? ac : e.textColor;
                        textCol = e.legacyAnimateText ? ac : e.textColor;
                        break;
                    }
                    }

                    const int savedUvX = e.uvX;
                    const int savedUvY = e.uvY;
                    e.uvX = stateUvX;
                    e.uvY = stateUvY;
                    if (e.key.find("mcl_cm_op") != std::string::npos) {
                        // [UI_MARKER v1]: campaign marker draw trace, once per widget and state.
                        static std::set<std::string> s_markerLogged;
                        const std::string tag = e.key + "#" + std::to_string(static_cast<int>(e.btnState));
                        if (s_markerLogged.insert(tag).second) {
                            const FRect mr = scaledRect(e.rect, xOffset, yOffset, s);
                            printf("[UI_MARKER v1] path=defs widget=%s state=%d tex=%s texWH=%dx%d tint=0x%08x uv=%d,%d,%d,%d draw=%.1fx%.1f at %.1f,%.1f alphaTest=%d pixelPerfect=%d\n",
                                   e.key.c_str(), static_cast<int>(e.btnState), e.texturePath.c_str(),
                                   e.textureWidth, e.textureHeight, artTint, stateUvX, stateUvY, e.uvW, e.uvH,
                                   mr.w, mr.h, mr.x, mr.y, e.alphaTest ? 1 : 0, e.pixelPerfect ? 1 : 0);
                            fflush(stdout);
                        }
                    }
                    if (!drawUiImageElement(e, xOffset, yOffset, s, artTint)) {
                        const FRect r = scaledRect(e.rect, xOffset, yOffset, s);
                        drawUiRectElement(r, e.fillColor, true);
                    }
                    e.uvX = savedUvX;
                    e.uvY = savedUvY;
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.borderColor, false);
                    renderTextElement(e, xOffset, yOffset, textCol, s);
                    // Legacy data.outlineText: a hollow rect around the text rect in
                    // the current state's text colour (the blue/orange box the OG
                    // draws around the label).  Converter dropped this, so buttons
                    // came out with no outline; borderColorArgb is 0 on these.
                    if (e.textOutline) {
                        const Rect& tr = e.hasTextRect ? e.textRect : e.rect;
                        drawUiRectElement(scaledRect(tr, xOffset, yOffset, s), textCol, false);
                    }
                }
                break;
            }
            case Texts:
                if (e.kind == UiElement::Text) {
                    renderTextElement(e, xOffset, yOffset, e.textColor, s);
                    ++drawn;
                }
                break;
            case Popups:
                // Expanded GuiCombo popup renders last so it appears above all other elements.
                if (e.kind == UiElement::Combo && e.expanded && !e.items.empty()) {
                    ++drawn;
                    const Rect savedRect = e.rect;
                    e.rect = { savedRect.x, savedRect.y + savedRect.h, savedRect.w, e.popupHeight };
                    drawUiListElement(e, xOffset, yOffset, s);
                    e.rect = savedRect;
                }
                break;
            }
        }
    }

    // EditBox pass: ImGui::InputText widgets live in their own windows (middle
    // layer, above GameOS art but below the foreground draw list).  They are
    // rendered last so the draw-list elements above don't overlap the editable
    // area.  Each element's editBuffer is owned here and handed directly to
    // ImGui; hasFocus is stored back so callers can query keyboard capture.
    for (UiElement& e : impl->elements) {
        if (e.kind != UiElement::EditBox || !e.visible)
            continue;
        ++drawn;
        const FRect sr = scaledRect(e.rect, xOffset, yOffset, s);
        e.editBoxActive = GuiRuntime::DrawUiEditBox(
            e.key.c_str(),
            sr.x, sr.y, sr.w, sr.h,
            e.editBuffer, static_cast<std::size_t>(e.maxLength + 1),
            e.fillColor, e.borderColor, e.textColor,
            e.fontSize, e.fontPath.c_str(),
            e.focusRequested);
        e.focusRequested = false;

        // The InputText window renders below the page's foreground draw list, so
        // an opaque panel rect can cover it. When the box isn't being actively
        // edited, redraw its text in the foreground so the value stays visible
        // (e.g. the Load Game selected-save name over its black backing rect).
        if (!e.editBoxActive && e.editBuffer[0]) {
            UiElement disp = e;
            disp.kind = UiElement::Text;
            disp.hasTextRect = false;
            disp.text = e.editBuffer;
            renderTextElement(disp, xOffset, yOffset, e.textColor, s);
        }
    }

    // commit draw-output telemetry. A loaded
    // non-passthrough page that emitted zero draw commands means the owning
    // screen would show only the black-clear -- log once, loud; the owning
    // LogisticsScreen consults lastRenderProducedOutput() next frame and
    // fail-softs to the legacy screen.
    impl->hasRendered = true;
    impl->lastRenderDrawCount = drawn;
    if (drawn == 0 && !impl->legacyPassthrough && !impl->zeroDrawLogged) {
        impl->zeroDrawLogged = true;
        uiDefsLog("page '%s' rendered ZERO draw commands (elements=%zu) -- fail-soft to legacy screen next frame",
                  impl->key.empty() ? "<none>" : impl->key.c_str(), impl->elements.size());
    }
}

bool UiDefs::GameOSPage::lastRenderProducedOutput() const
{
    if (!impl)
        return false;
    return !UiDefs::pageShouldFailSoftToLegacy(
        impl->loaded, impl->legacyPassthrough, impl->hasRendered, impl->lastRenderDrawCount);
}

bool UiDefs::GameOSPage::inside(int x, int y, int xOffset, int yOffset) const
{
    if (!isLoaded())
        return false;
    // Same Environment -> local mapping as update(): callers pass GameOS
    // mouse coordinates, which are in legacy Environment space.
    const float mouseSx = (Environment.screenWidth > 0 && impl->localWidth > 0)
        ? static_cast<float>(impl->localWidth) / static_cast<float>(Environment.screenWidth)
        : 1.0f;
    const float mouseSy = (Environment.screenHeight > 0 && impl->localHeight > 0)
        ? static_cast<float>(impl->localHeight) / static_cast<float>(Environment.screenHeight)
        : 1.0f;
    const int lx = static_cast<int>(x * mouseSx) - xOffset;
    const int ly = static_cast<int>(y * mouseSy) - yOffset;
    for (const UiElement& e : impl->elements) {
        if (e.visible && pointInRect(e.rect, lx, ly))
            return true;
    }
    return false;
}

bool UiDefs::GameOSPage::setListItems(const std::string& elementKey, const std::vector<std::string>& items)
{
    if (!isLoaded())
        return false;
    for (UiElement& e : impl->elements) {
        if ((e.kind != UiElement::List && e.kind != UiElement::Combo) || e.key != elementKey)
            continue;
        // Unchanged content: preserve the user's scroll offset and selection.
        // Screens re-push their items every frame; resetting scrollOffset below
        // would make the list impossible to scroll. Skipping also avoids the
        // per-frame trace spam.
        if (e.items == items)
            return true;
        if (uiDefsTraceEnabled())
            uiDefsTrace("setListItems: key=%s count=%zu kind=%s",
                        elementKey.c_str(), items.size(),
                        e.kind == UiElement::Combo ? "Combo" : "List");
        e.items = items;
        if (e.selectedIndex >= static_cast<int>(e.items.size()))
            e.selectedIndex = e.items.empty() ? -1 : static_cast<int>(e.items.size()) - 1;
        e.scrollOffset = 0;
        return true;
    }
    if (uiDefsTraceEnabled())
        uiDefsTrace("setListItems: NO MATCH for key=%s (checked %zu elements)",
                    elementKey.c_str(), impl->elements.size());
    return false;
}

// pre-wrap helper for screens that push paragraph text into a
// fixed-row-height GuiList. drawUiListElement draws each row via DrawUiText with
// wrapWidth = the row rect width in DISPLAY pixels and fontSize scaled by sy; a
// char-count pre-wrap (e.g. the old 88-char briefing wrap) disagrees with that
// pixel wrap, so an overlong row re-wraps INSIDE its one-line-high row and the
// spill paints under the next row (the mission-briefing overlapping-paragraphs
// bug). This computes the wrap with the SAME width/font/size math the renderer
// uses, so rows and visual lines are 1:1.
bool UiDefs::GameOSPage::wrapListItemText(const std::string& elementKey,
                                          const std::string& text,
                                          std::vector<std::string>& outLines)
{
    outLines.clear();
    if (!isLoaded())
        return false;
    for (UiElement& e : impl->elements) {
        if ((e.kind != UiElement::List && e.kind != UiElement::Combo) || e.key != elementKey)
            continue;
        const PageScale s = currentPageScale(impl->localWidth, impl->localHeight);
        // Mirror drawUiListElement's itemRect width. The scrollbar is only
        // reserved when content overflows; assume it IS present (paragraph
        // producers overflow by definition) -- a conservatively narrower wrap
        // can only wrap earlier, never overlap.
        const int itemW = std::max(0, e.rect.w - 2 * e.itemOffsetX - kScrollbarLocalW);
        const float wrapWidth = static_cast<float>(itemW) * s.sx;
        return GuiRuntime::CalcUiTextWrapLines(text.c_str(), wrapWidth,
                                               scaledFontSize(e.fontSize, s),
                                               e.fontPath.c_str(), outLines);
    }
    return false;
}

// Per-item text colors for a GuiList, parallel to its items (ARGB). A shorter
// vector than the item list leaves the remaining rows on the element textColor.
// Pushed separately from setListItems so the unchanged-items fast-path there
// still skips work while colors can refresh independently.
bool UiDefs::GameOSPage::setListItemColors(const std::string& elementKey,
                                           const std::vector<unsigned int>& colors)
{
    if (!isLoaded())
        return false;
    for (UiElement& e : impl->elements) {
        if ((e.kind != UiElement::List && e.kind != UiElement::Combo) || e.key != elementKey)
            continue;
        e.itemColors = colors;
        return true;
    }
    return false;
}

bool UiDefs::GameOSPage::setSliderValue(const std::string& elementKey, int value)
{
    if (!isLoaded())
        return false;
    for (UiElement& e : impl->elements) {
        if (e.kind != UiElement::Slider || e.key != elementKey)
            continue;
        e.sliderValue = std::clamp(value, 0, e.sliderMax);
        return true;
    }
    return false;
}

int UiDefs::GameOSPage::getSliderValue(const std::string& elementKey) const
{
    if (!isLoaded())
        return 0;
    for (const UiElement& e : impl->elements) {
        if (e.kind == UiElement::Slider && e.key == elementKey)
            return e.sliderValue;
    }
    return 0;
}

int UiDefs::GameOSPage::getListItemCount(const std::string& elementKey) const
{
    if (!isLoaded())
        return 0;
    for (const UiElement& e : impl->elements) {
        if ((e.kind == UiElement::List || e.kind == UiElement::Combo) && e.key == elementKey)
            return static_cast<int>(e.items.size());
    }
    return 0;
}

int UiDefs::GameOSPage::getListSelection(const std::string& elementKey) const
{
    if (!isLoaded())
        return -1;
    for (const UiElement& e : impl->elements) {
        if ((e.kind == UiElement::List || e.kind == UiElement::Combo) && e.key == elementKey)
            return e.selectedIndex;
    }
    return -1;
}

void UiDefs::GameOSPage::setListSelection(const std::string& elementKey, int index)
{
    if (!isLoaded())
        return;
    for (UiElement& e : impl->elements) {
        if ((e.kind != UiElement::List && e.kind != UiElement::Combo) || e.key != elementKey)
            continue;
        if (index < -1)
            index = -1;
        if (index >= static_cast<int>(e.items.size()))
            index = static_cast<int>(e.items.size()) - 1;
        e.selectedIndex = index;
        return;
    }
}

bool UiDefs::GameOSPage::hasEditBox() const
{
    if (!isLoaded()) return false;
    for (const UiElement& e : impl->elements) {
        if (e.kind == UiElement::EditBox && e.visible)
            return true;
    }
    return false;
}

bool UiDefs::GameOSPage::getEditText(const std::string& elementKey, std::string& text) const
{
    if (!isLoaded()) return false;
    for (const UiElement& e : impl->elements) {
        if (e.kind == UiElement::EditBox && e.key == elementKey) {
            text = e.editBuffer;
            return true;
        }
    }
    return false;
}

bool UiDefs::GameOSPage::setEditText(const std::string& elementKey, const std::string& text)
{
    if (!isLoaded()) return false;
    for (UiElement& e : impl->elements) {
        if (e.kind != UiElement::EditBox || e.key != elementKey)
            continue;
        const std::size_t len = std::min(text.size(), static_cast<std::size_t>(e.maxLength));
        std::memcpy(e.editBuffer, text.c_str(), len);
        e.editBuffer[len] = '\0';
        return true;
    }
    return false;
}

bool UiDefs::GameOSPage::isEditBoxFocused(const std::string& elementKey) const
{
    if (!isLoaded()) return false;
    for (const UiElement& e : impl->elements) {
        if (e.kind == UiElement::EditBox && e.key == elementKey)
            return e.editBoxActive;
    }
    return false;
}

bool UiDefs::GameOSPage::isAnyEditBoxFocused() const
{
    if (!isLoaded()) return false;
    for (const UiElement& e : impl->elements) {
        if (e.kind == UiElement::EditBox && e.editBoxActive)
            return true;
    }
    return false;
}

void UiDefs::GameOSPage::requestEditFocus(const std::string& elementKey)
{
    if (!isLoaded()) return;
    for (UiElement& e : impl->elements) {
        if (e.kind == UiElement::EditBox && e.key == elementKey) {
            e.focusRequested = true;
            return;
        }
    }
}
