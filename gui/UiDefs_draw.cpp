// UiDefs: page scaling and per-element drawing.
// Declared in UiDefs_internal.h.
#include "UiDefs_internal.h"
#include "Gates/gate.h"

namespace uidefs_detail {


PageScale currentPageScale(int localWidth, int localHeight)
{
    PageScale s;
    float dw = 0.0f;
    float dh = 0.0f;
    const bool fromImGui = GuiRuntime::GetDisplaySize(dw, dh);
    if (!fromImGui)
    {
        // ImGui context/DisplaySize unavailable (or zero) at this call site:
        // fall back to the GameOS environment, which is what NotifyResize
        // feeds DisplaySize from in the first place.
        dw = static_cast<float>(Environment.screenWidth);
        dh = static_cast<float>(Environment.screenHeight);
    }
    // UI-ASPECT-ANCHOR-1: scale into the centered 16:9 UI canvas instead of
    // stretching to the full display (matches the legacy layer's canvas
    // transform in flushHUDBatch). Identity at exactly 16:9; inactive frames
    // (mission) and MC2_UI_ASPECT_ANCHOR=0 keep the full-stretch behavior.
    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        if (gos_ComputeUiCanvasBox(static_cast<int>(dw), static_cast<int>(dh),
                                   &bx, &by, &bw, &bh))
        {
            s.ox = static_cast<float>(bx);
            s.oy = static_cast<float>(by);
            dw = static_cast<float>(bw);
            dh = static_cast<float>(bh);
        }
    }
    if (localWidth > 0 && dw > 0.0f)
        s.sx = dw / static_cast<float>(localWidth);
    if (localHeight > 0 && dh > 0.0f)
        s.sy = dh / static_cast<float>(localHeight);

    // One-shot diagnostic (MC2_UI_DEFS_TRACE=1): every value the transform
    // depends on, so a single run shows why a page rendered at the wrong
    // size.  Re-arms when the display size changes (resolution switch).
    static float lastDw = -1.0f;
    static float lastDh = -1.0f;
    if (uiDefsTraceEnabled() && (dw != lastDw || dh != lastDh))
    {
        lastDw = dw;
        lastDh = dh;
        uiDefsTrace("page scale: display=%.0fx%.0f (source=%s) env=%dx%d local=%dx%d -> sx=%.3f sy=%.3f",
                    dw, dh, fromImGui ? "imgui" : "environment",
                    (int)Environment.screenWidth, (int)Environment.screenHeight,
                    localWidth, localHeight, s.sx, s.sy);
    }
    return s;
}


FRect scaledRect(const Rect& r, int xOffset, int yOffset, const PageScale& s)
{
    FRect out;
    out.x = static_cast<float>(r.x + xOffset) * s.sx + s.ox;
    out.y = static_cast<float>(r.y + yOffset) * s.sy + s.oy;
    out.w = static_cast<float>(r.w) * s.sx;
    out.h = static_cast<float>(r.h) * s.sy;
    return out;
}

int scaledFontSize(int fontSize, const PageScale& s)
{
    const float scaled = static_cast<float>(fontSize) * s.sy;
    return scaled > 0.0f ? static_cast<int>(scaled + 0.5f) : fontSize;
}

static aButton* legacyButtonForElement(LogisticsScreen* screen, const UiElement& e)
{
    if (!screen)
        return nullptr;
    if (e.legacyId > 0) {
        aButton* byId = screen->getButton(e.legacyId);
        if (byId)
            return byId;
    }
    if (e.legacyButtonIndex >= 0)
        return screen->getButtonByIndex(e.legacyButtonIndex);
    return nullptr;
}

int messageIdForElement(LogisticsScreen* screen, const UiElement& e)
{
    if (e.legacyId > 0)
        return e.legacyId;
    aButton* legacyButton = legacyButtonForElement(screen, e);
    return legacyButton ? legacyButton->getID() : 0;
}

bool isButtonEnabled(LogisticsScreen* screen, const UiElement& e)
{
    aButton* button = legacyButtonForElement(screen, e);
    return !button || button->isEnabled();
}

static void ensureTexture(UiElement& e)
{
    if (e.texturePath.empty() || e.textureNodeAssigned)
        return;
    std::filesystem::path p = resolveAssetPath(e.texturePath);

    // Prefer the original .tga sibling when one ships alongside the authored .png.
    // Hand-masked PNG alpha can't match the source TGA's clean transparency
    // (mission markers, etc.), so when a .tga is present next to the .png it wins.
    // loadTexture and readUiTextureSize both handle TGA (with its alpha); this
    // falls through to the .png when no .tga is shipped.
    {
        std::filesystem::path tgaSibling = p;
        tgaSibling.replace_extension(".tga");
        if (fileExistsPath(tgaSibling))
            p = tgaSibling;
    }

    bool onDisk = fileExistsPath(p);
    // Runtime images often carry a legacy ".tga" name (e.g. encyclopedia
    // portraits / weapon icons resolved via cLoadString) while the actual asset
    // ships as ".png". Fall back to the .png sibling so the on-disk size read
    // below succeeds -- needed for sub-rect UVs and contain-fit centering.
    if (!onDisk) {
        std::filesystem::path png = p;
        png.replace_extension(".png");
        if (fileExistsPath(png)) {
            p = png;
            onDisk = true;
        }
    }
    if (onDisk)
        readUiTextureSize(p, e.textureWidth, e.textureHeight);

    // Try loading even when not on disk — texture may be pak-packed (FST archive)
    // and findable by the texture manager's own search, which fileExistsPath() misses.
    const char* loadPath = onDisk ? p.string().c_str() : e.texturePath.c_str();
    e.textureNode = mcTextureManager->loadTexture(loadPath, gos_Texture_Alpha, 0, 0, 0x2);
    e.textureNodeAssigned = true;
    if (!e.textureNode) {
        traceMissingTextureOnce(e.texturePath, p);
        return;
    }
    {
        e.gosTexture = mcTextureManager->get_gosTextureHandle(e.textureNode);
        if (e.textureWidth == 0 || e.textureHeight == 0) {
            DWORD logicalWidth = 0;
            DWORD logicalHeight = 0;
            if (mcTextureManager->tryGetTextureLogicalSize(e.textureNode, logicalWidth, logicalHeight)) {
                e.textureWidth = static_cast<int>(logicalWidth);
                e.textureHeight = static_cast<int>(logicalHeight);
            }
        }
        if (e.uvW == 0)
            e.uvW = e.textureWidth;
        if (e.uvH == 0)
            e.uvH = e.textureHeight;
    }
}

static void textureUVs(UiElement& e, float& u1, float& v1, float& u2, float& v2)
{
    u1 = 0.0f;
    v1 = 0.0f;
    u2 = 1.0f;
    v2 = 1.0f;
    if (e.uvW <= 0 || e.uvH <= 0)
        return;

    int logicalWidth = e.textureWidth;
    int logicalHeight = e.textureHeight;
    if ((logicalWidth <= 0 || logicalHeight <= 0) && e.textureNode) {
        DWORD w = 0;
        DWORD h = 0;
        if (mcTextureManager->tryGetTextureLogicalSize(e.textureNode, w, h)) {
            logicalWidth = static_cast<int>(w);
            logicalHeight = static_cast<int>(h);
        }
    }

    // PILOT-PHOTO-UV-1: two UV-authoring spaces exist.
    //   - Defs-fit and runtime PNG-space sub-rects address the LOGICAL texture
    //     size (the modder authored coords straight off the PNG) — divide by
    //     logical size (default, unchanged).
    //   - LEGACY-space sub-rects (runtime callers mirroring old setUVs math,
    //     e.g. the 92x128 per-pilot portrait crop) address size/uvScale — the
    //     divisor aObject::init derives (fileWidth = physical/uvScale). Opt-in
    //     via e.uvLegacySpace (setElementImageRegion legacyUvSpace=true).
    DWORD uvScale = 1;
    if (e.uvLegacySpace && e.textureNode && mcTextureManager)
        uvScale = mcTextureManager->getUVScale(e.textureNode);
    if (uvScale < 1) uvScale = 1;

    if (logicalWidth > 0 && logicalHeight > 0) {
        const float effW = static_cast<float>(logicalWidth)  / static_cast<float>(uvScale);
        const float effH = static_cast<float>(logicalHeight) / static_cast<float>(uvScale);
        u1 = static_cast<float>(e.uvX) / effW;
        v1 = static_cast<float>(e.uvY) / effH;
        u2 = static_cast<float>(e.uvX + e.uvW) / effW;
        v2 = static_cast<float>(e.uvY + e.uvH) / effH;
        if (u2 > 1.0f) u2 = 1.0f;
        if (v2 > 1.0f) v2 = 1.0f;
        if (u1 > 1.0f) u1 = 1.0f;
        if (v1 > 1.0f) v1 = 1.0f;
    }
}

void drawUiRectElement(const FRect& r, unsigned int color, bool filled)
{
    if ((color & 0xff000000) == 0)
        return;
    GuiRuntime::DrawUiRect(r.x, r.y, r.w, r.h, color, filled);
}

unsigned int sampleLegacyAnim(const UiElement::AnimKeyframe* kf, int count,
                                      bool loops, float t)
{
    if (count <= 0)
        return 0xffffffff;
    if (count == 1)
        return kf[0].color;
    const float period = kf[count - 1].time;
    if (loops && period > 0.0f)
        t = std::fmod(t, period);
    if (t <= kf[0].time)
        return kf[0].color;
    for (int i = 1; i < count; ++i) {
        if (t <= kf[i].time)
            return kf[i - 1].color;
    }
    return kf[count - 1].color;
}

bool drawUiImageElement(UiElement& e, int xOffset, int yOffset, const PageScale& s,
                                unsigned int tint)
{
    DWORD gosTex = 0;
    if (e.gosTextureOverride != 0) {
        // Direct raw gos handle (e.g. a live decoded video frame): draw whatever
        // GL texture this handle currently backs and skip the texture manager —
        // the override is NOT a manager node, so get_gosTextureHandle() must not
        // run on it.  drawn full-frame; UVs are reset to 0..1 when the override
        // is set (see setElementGosTexture).
        gosTex = static_cast<DWORD>(e.gosTextureOverride);
    } else {
        ensureTexture(e);
        if (!e.textureNodeAssigned)
            return false;

        // Re-resolve every frame rather than trusting a handle cached at first
        // draw: MC_TextureManager::update()/flushCache() can gos_DestroyTexture()
        // a node that hasn't been "used" (lastUsed refreshed) in ~60 turns, which
        // is exactly what happens to a texture whose handle is only fetched once.
        // get_gosTextureHandle() both refreshes lastUsed (so an on-screen UI
        // texture is never considered idle) and re-caches the texture in
        // (recreating the GL texture) if it had already been evicted -- matching
        // how the legacy aButton/asystem draw paths call it every frame.
        e.gosTexture = mcTextureManager->get_gosTextureHandle(e.textureNode);
        gosTex = static_cast<DWORD>(e.gosTexture);
    }
    if (!gosTex)
        return false;

    const unsigned int glTexture = gos_GetGLTextureName(gosTex);
    if (!glTexture) {
        if (uiDefsTraceEnabled())
            uiDefsTrace("UI texture has no GL name: key=%s texture=%s gos=%u",
                        e.key.c_str(), e.texturePath.c_str(), e.gosTexture);
        return false;
    }

    float u1 = 0.0f;
    float v1 = 0.0f;
    float u2 = 1.0f;
    float v2 = 1.0f;
    textureUVs(e, u1, v1, u2, v2);
    if (e.flipV) { const float tmp = v1; v1 = v2; v2 = tmp; }

    FRect r = scaledRect(e.rect, xOffset, yOffset, s);

    // PILOT-PHOTO-UV-1 diagnostic: full number dump for the pilot portrait
    // element (red-striped-panel bug). Env-gated, one line per texture change.
    if (mc2::gate::get<"MC2_LOG_PREVIEW">() && e.key.find("this_rect_defines") != std::string::npos) {
        static std::string s_lastDump;
        if (s_lastDump != e.texturePath) {
            s_lastDump = e.texturePath;
            printf("[PHOTO] key=%s path='%s' node=%d gos=%u gl=%u texWH=%dx%d uvXYWH=%d,%d,%d,%d "
                   "uv=(%.3f,%.3f)-(%.3f,%.3f) rect=(%.0f,%.0f %.0fx%.0f)\n",
                e.key.c_str(), e.texturePath.c_str(), e.textureNode, e.gosTexture, glTexture,
                e.textureWidth, e.textureHeight, e.uvX, e.uvY, e.uvW, e.uvH,
                u1, v1, u2, v2, r.x, r.y, r.w, r.h);
            fflush(stdout);
        }
    }
    // "contain" fit: scale the texture to fit inside the rect preserving aspect,
    // centered (letterboxed). Used by the personality portrait so off-aspect
    // images sit centered in their box instead of being stretched.
    if (e.fitContain && e.textureWidth > 0 && e.textureHeight > 0 && r.w > 0.0f && r.h > 0.0f) {
        const float boxAspect = r.w / r.h;
        const float texAspect = static_cast<float>(e.textureWidth) / static_cast<float>(e.textureHeight);
        if (texAspect > boxAspect) {
            const float fitH = r.w / texAspect;
            r.y += (r.h - fitH) * 0.5f;
            r.h = fitH;
        } else {
            const float fitW = r.h * texAspect;
            r.x += (r.w - fitW) * 0.5f;
            r.w = fitW;
        }
    }
    return GuiRuntime::DrawUiImage(glTexture,
                                   r.x,
                                   r.y,
                                   r.w,
                                   r.h,
                                   u1, v1, u2, v2,
                                   tint,
                                   e.pixelPerfect,
                                   e.alphaTest);
}

void renderTextElement(UiElement& e, int xOffset, int yOffset, unsigned int color, const PageScale& s)
{
    if (e.text.empty())
        return;

    const Rect& local = e.hasTextRect ? e.textRect : e.rect;
    const FRect r = scaledRect(local, xOffset, yOffset, s);

    if (!GuiRuntime::DrawUiText(r.x,
                                r.y,
                                r.w,
                                r.h,
                                e.text.c_str(),
                                color,
                                scaledFontSize(e.fontSize, s),
                                e.textAlign,
                                e.fontPath.c_str())) {
        static bool s_loggedMissingImGuiText = false;
        if (!s_loggedMissingImGuiText) {
            uiDefsTrace("ImGui text path is not active; data/defs UI text will not render until GuiRuntime::NewFrame/Render are wired");
            s_loggedMissingImGuiText = true;
        }
    }
}

// GuiList elements: the FIT describes the box (e.rect) and one item-row
// template (font/fontSize/textAlign/itemOffset*); the row contents come from
// e.items, populated at runtime via GameOSPage::setListItems. Draws a
// background + border (the box is otherwise indistinguishable from the
// transparent overlay behind it), then as many item rows as fit, with a
// highlight rect behind the selected row. scrollOffset is clamped within
// [0, maxScroll] but NOT snapped to selectedIndex here — render must not
// mutate scroll state, or wheel input is silently undone each frame.
// Snap-to-selection happens at the point the selection or items change.
void drawUiListElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
{
    const FRect r = scaledRect(e.rect, xOffset, yOffset, s);

    drawUiRectElement(r, e.fillColor, true);
    drawUiRectElement(r, e.borderColor, false);

    if (e.items.empty())
        return;

    const int rowH = e.itemHeight > 0 ? e.itemHeight : (e.fontSize + 6);
    const float scaledRowH = static_cast<float>(rowH) * s.sy;
    if (scaledRowH <= 0.0f)
        return;

    const int visibleRows = static_cast<int>(r.h / scaledRowH);
    if (visibleRows <= 0)
        return;

    const int itemCount = static_cast<int>(e.items.size());
    const int maxScroll = std::max(0, itemCount - visibleRows);
    e.scrollOffset = std::min(std::max(e.scrollOffset, 0), maxScroll);

    // Scrollbar: kScrollbarLocalW-unit track on right edge when content overflows.
    // Using local units keeps the bar proportionally sized across display resolutions.
    const bool needScrollbar = (maxScroll > 0);
    const int sbLocalW = needScrollbar ? kScrollbarLocalW : 0;
    const float sbScreenW = needScrollbar ? static_cast<float>(sbLocalW) * s.sx : 0.0f;

    UiElement row = e;
    row.kind = UiElement::Text;
    row.hasTextRect = false;

    for (int i = 0; i < visibleRows; ++i) {
        const int itemIndex = e.scrollOffset + i;
        if (itemIndex >= itemCount)
            break;

        Rect itemRect;
        itemRect.x = e.rect.x + e.itemOffsetX;
        itemRect.y = e.rect.y + e.itemOffsetY + i * rowH;
        itemRect.w = std::max(0, e.rect.w - 2 * e.itemOffsetX - sbLocalW);
        itemRect.h = rowH;

        if (itemIndex == e.selectedIndex)
            drawUiRectElement(scaledRect(itemRect, xOffset, yOffset, s), e.highlightColor, true);
        else if (itemIndex == e.hoverIndex)
            drawUiRectElement(scaledRect(itemRect, xOffset, yOffset, s), 0x60c66600, true);

        row.rect = itemRect;
        const std::string& rawText = e.items[itemIndex];
        unsigned int rowColor =
            (itemIndex < static_cast<int>(e.itemColors.size())) ? e.itemColors[itemIndex]
                                                                : e.textColor;
        // Selected row text switches to selectedTextColor (default white) so an
        // orange list reads white on the highlighted entry.
        if (itemIndex == e.selectedIndex)
            rowColor = e.selectedTextColor;
        const auto tabPos = rawText.find('\t');
        if (e.secondColumnX > 0 && tabPos != std::string::npos) {
            Rect leftRect  = itemRect;
            leftRect.w = e.secondColumnX - e.itemOffsetX;
            row.rect = leftRect;
            row.text = rawText.substr(0, tabPos);
            renderTextElement(row, xOffset, yOffset, rowColor, s);
            Rect rightRect = itemRect;
            rightRect.x = itemRect.x + e.secondColumnX;
            rightRect.w = std::max(0, itemRect.w - e.secondColumnX);
            row.rect = rightRect;
            row.text = rawText.substr(tabPos + 1);
            renderTextElement(row, xOffset, yOffset, rowColor, s);
        } else {
            row.text = rawText;
            renderTextElement(row, xOffset, yOffset, rowColor, s);
        }
    }

    if (needScrollbar) {
        const float sbX = r.x + r.w - sbScreenW;
        const float thumbPct = static_cast<float>(visibleRows) / static_cast<float>(itemCount);
        const float thumbH = std::max(sbScreenW, r.h * thumbPct);
        const float scrollPct = static_cast<float>(e.scrollOffset) / static_cast<float>(maxScroll);
        const float thumbY = r.y + (r.h - thumbH) * scrollPct;
        GuiRuntime::DrawUiRect(sbX, r.y, sbScreenW, r.h, 0x60404040, true);
        GuiRuntime::DrawUiRect(sbX, thumbY, sbScreenW, thumbH, 0xffc66600, true);
    }
}

// GuiSlider: a horizontal groove with a value-filled portion and a draggable
// thumb. Colors come from the element -- fillColor = groove, textColor = filled
// portion, highlightColor = thumb. Value + drag state live on the element; the
// owning screen reads it back each frame via getSliderValue().
void drawUiSliderElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
{
    const FRect r = scaledRect(e.rect, xOffset, yOffset, s);
    if (r.w <= 0.0f || r.h <= 0.0f)
        return;

    const float frac = (e.sliderMax > 0)
        ? std::min(1.0f, std::max(0.0f, static_cast<float>(e.sliderValue) /
                                        static_cast<float>(e.sliderMax)))
        : 0.0f;

    const float grooveH = std::max(3.0f, r.h * 0.34f);
    const float grooveY = r.y + (r.h - grooveH) * 0.5f;
    const unsigned int grooveColor = (e.fillColor & 0xff000000) ? e.fillColor : 0xff202830;
    GuiRuntime::DrawUiRect(r.x, grooveY, r.w, grooveH, grooveColor, true);

    // Thumb travels inside the track so it never overhangs the ends.
    const float thumbW = std::max(8.0f, r.h * 0.55f);
    const float thumbX = r.x + frac * (r.w - thumbW);

    const float fillW = (thumbX + thumbW * 0.5f) - r.x;
    if (fillW > 0.0f)
        GuiRuntime::DrawUiRect(r.x, grooveY, fillW, grooveH, e.textColor, true);

    GuiRuntime::DrawUiRect(thumbX, r.y, thumbW, r.h, e.highlightColor, true);
}

// GuiCheckbox / GuiRadio: a modern glyph driven by e.pressed (the toggle/radio
// state, synced from the legacy button), plus the button's text label in a STABLE
// MC2-orange (no hover/press flash). The element rect is the glyph box; the label
// lives in textRect. Glyph colors are fixed to the MC2 theme.
void drawUiCheckRadioElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
{
    const FRect box = scaledRect(e.rect, xOffset, yOffset, s);
    const unsigned int interior = 0xff10242e;  // dark groove
    const unsigned int outline  = 0xffc66600;  // MC2 orange frame
    const unsigned int mark     = 0xffff8a00;  // bright orange check / dot

    if (e.buttonStyle == UiElement::ButtonStyle::Radio) {
        const float cx  = box.x + box.w * 0.5f;
        const float cy  = box.y + box.h * 0.5f;
        const float rad = std::min(box.w, box.h) * 0.5f;
        GuiRuntime::DrawUiCircle(cx, cy, rad, interior, true);
        GuiRuntime::DrawUiCircle(cx, cy, rad, outline, false);
        if (e.pressed)
            GuiRuntime::DrawUiCircle(cx, cy, rad * 0.5f, mark, true);
    } else {  // Checkbox
        drawUiRectElement(box, interior, true);
        drawUiRectElement(box, outline, false);
        if (e.pressed) {
            const float pad = std::max(2.0f, box.w * 0.24f);
            const FRect inner = { box.x + pad, box.y + pad,
                                  box.w - 2.0f * pad, box.h - 2.0f * pad };
            drawUiRectElement(inner, mark, true);
        }
    }

    renderTextElement(e, xOffset, yOffset, e.textColor, s);
}

float getFrameDelta()
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last = Clock::now();
    const auto now = Clock::now();
    const float dt = std::chrono::duration<float>(now - s_last).count();
    s_last = now;
    return (dt > 0.0f && dt < 0.2f) ? dt : (1.0f / 60.0f);
}


} // namespace uidefs_detail
