// UiDefs: field helpers and UiElement construction.
// Declared in UiDefs_internal.h.
#include "UiDefs_internal.h"

namespace uidefs_detail {


// UI-PHASE1-INTEGRATION-GAP: no definition of mc2_stringCatalog_lookupKey
// exists anywhere in this tree or the modder's file drop (the "StringCatalog"
// this file's header comment references was never included). Stubbed as a
// permanent miss so resolvedText() falls through to its existing
// visibleText/runtimeTextBinding fallbacks rather than failing to link or
// inventing a catalog implementation. If real string-catalog-key lookup is
// wanted, this needs an actual key->string table wired in.
static const char* mc2_stringCatalog_lookupKey(const char* /*key*/)
{
    return nullptr;
}

static std::string resolvedText(const std::unordered_map<std::string, std::string>& f)
{
    const std::string key = !field(f, "textKey").empty() ? field(f, "textKey") : field(f, "proposedTextKey");
    if (!key.empty()) {
        const char* fromCatalog = mc2_stringCatalog_lookupKey(key.c_str());
        if (fromCatalog)
            return fromCatalog;
    }
    const std::string visible = field(f, "visibleText");
    if (!visible.empty())
        return visible;
    const std::string runtime = field(f, "runtimeTextBinding");
    if (!runtime.empty()) {
        const char* fromCatalog = mc2_stringCatalog_lookupKey(runtime.c_str());
        if (fromCatalog)
            return fromCatalog;
    }
    return std::string();
}

bool parseBool(const std::unordered_map<std::string, std::string>& f,
                      const char* name, bool fallback)
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    const std::string v = toLower(it->second);
    return v == "true" || v == "1" || v == "yes";
}

static float parseFloatField(const std::unordered_map<std::string, std::string>& f,
                             const char* name, float fallback = 0.0f)
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    try { return std::stof(it->second); } catch (...) { return fallback; }
}

static void parseLegacyAnimState(const std::unordered_map<std::string, std::string>& f,
                                  const char* tsKey, const char* loopsKey,
                                  const char* timePrefix, const char* colorPrefix,
                                  int& count, bool& loops, UiElement::AnimKeyframe* kf)
{
    count = 0;
    loops = false;
    int ts = 0;
    if (!parseInt(field(f, tsKey, "0"), ts) || ts <= 0)
        return;
    loops = parseBool(f, loopsKey, false);
    count = std::min(ts, UiElement::kMaxAnimKF);
    for (int i = 0; i < count; ++i) {
        char tKey[64], cKey[64];
        std::snprintf(tKey, sizeof(tKey), "%s%d", timePrefix, i);
        std::snprintf(cKey, sizeof(cKey), "%s%d", colorPrefix, i);
        kf[i].time  = parseFloatField(f, tKey, 0.0f);
        kf[i].color = parseColor(f, cKey, 0xffffffff);
    }
}

UiElement makeElement(const Block& b)
{
    UiElement e;
    e.key = field(b.fields, "key");
    e.visible = field(b.fields, "visible", "true") != "false";

    if (b.type == "GuiImage")
        e.kind = UiElement::Image;
    else if (b.type == "GuiRect")
        e.kind = UiElement::RectElement;
    else if (b.type == "GuiText" || b.type == "GuiTextBinding")
        e.kind = UiElement::Text;
    else if (b.type == "GuiButton")
        e.kind = UiElement::Button;
    else if (b.type == "GuiCheckbox") {
        e.kind = UiElement::Button;
        e.buttonStyle = UiElement::ButtonStyle::Checkbox;
    }
    else if (b.type == "GuiRadio") {
        e.kind = UiElement::Button;
        e.buttonStyle = UiElement::ButtonStyle::Radio;
    }
    else if (b.type == "GuiList")
        e.kind = UiElement::List;
    else if (b.type == "GuiSlider")
        e.kind = UiElement::Slider;
    else if (b.type == "GuiAnimation") {
        // Legacy AnimObject statics.  The converter exports a keyframe COUNT
        // but no keyframe data, so playback is not possible yet; render the
        // textured ones as static images (this is the entire splash
        // background and intro logo art).  Textureless GuiAnimations are
        // fade vehicles (fullscreen black fade-from/fade-to rects) -- drawn
        // statically at full alpha they would black out the page, so they
        // are skipped until the converter exports keyframes.
        // When the owning screen has live legacy animObjects, these static
        // snapshots are suppressed entirely and the aObject GUI bridge
        // renders the real animated objects instead (see fromAnimation).
        e.fromAnimation = true;
        if (!field(b.fields, "texture").empty())
            e.kind = UiElement::Image;
        else
            e.visible = false;
    }
    else if (b.type == "GuiMfcControl") {
        e.widgetType = field(b.fields, "widgetType");
        const std::string wt = toLower(e.widgetType);
        if (wt.find("button") != std::string::npos || wt == "control")
            e.kind = UiElement::Button;
        else if (wt == "label")
            e.kind = UiElement::Text;
        else
            e.kind = UiElement::Placeholder;
    }
    // --- V2 block types ---
    else if (b.type == "UiV2Static" || b.type == "UiV2ButtonChild")
        e.kind = UiElement::Image;
    else if (b.type == "UiV2Rect")
        e.kind = UiElement::RectElement;
    else if (b.type == "UiV2Text")
        e.kind = UiElement::Text;
    else if (b.type == "UiV2Button")
        e.kind = UiElement::Button;
    else if (b.type == "UiV2AnimObject") {
        e.fromAnimation = true;
        if (!field(b.fields, "texture").empty())
            e.kind = UiElement::Image;
        else
            e.visible = false;
    }
    else if (b.type == "GuiEditBox")
        e.kind = UiElement::EditBox;
    else if (b.type == "GuiCombo")
        e.kind = UiElement::Combo;
    else if (b.type == "UiV2Edit" || b.type == "UiV2ComboBox" ||
             b.type == "UiV2DropList" || b.type == "UiV2HudElement")
        e.kind = UiElement::Placeholder;

    e.rect = parseRect(field(b.fields, "rect", field(b.fields, "controlRect").c_str()));
    if (e.rect.w == 0 && e.rect.h == 0)
        e.rect = parseRect(field(b.fields, "controlRect"));

    if (b.type == "GuiAnimation") {
        // The animation's resting position; the exported rect is often the
        // raw art rect at 0,0 while animationInitialPos carries placement.
        // "-1,-1" means "no explicit position" -- keep the rect as-is.
        const std::string initPos = field(b.fields, "animationInitialPos");
        int px = 0;
        int py = 0;
        if (!initPos.empty() && initPos != "-1,-1" &&
            std::sscanf(initPos.c_str(), "%d,%d", &px, &py) == 2) {
            e.rect.x = px;
            e.rect.y = py;
        }
    }

    const std::string textRect = field(b.fields, "textRect");
    if (!textRect.empty()) {
        e.textRect = parseRect(textRect);
        e.hasTextRect = true;
    }

    parseInt(field(b.fields, "legacyId", "0"), e.legacyId);
    if (e.legacyId == 0)
        parseInt(field(b.fields, "controlValue", "0"), e.legacyId);
    e.legacyButtonIndex = parseLegacyButtonIndex(b.fields);
    // UI-LAYER-CONTRACT-2: remember which legacy control this element mirrors
    // so LogisticsScreen can suppress the legacy twin (coverage query).
    e.legacySection = field(b.fields, "legacySection");
    if (e.legacySection.empty())
        e.legacySection = field(b.fields, "sourceControlType");
    e.texturePath = field(b.fields, "texture");
    e.textKey = !field(b.fields, "textKey").empty() ? field(b.fields, "textKey") : field(b.fields, "proposedTextKey");
    e.text = resolvedText(b.fields);

    // Help-text areas (role="help") are updated dynamically by
    // LogisticsScreen::update at runtime; the converter bakes whatever help
    // string was resolved at conversion time (e.g. "MECH LAB"), which is
    // never the right thing to show.  Keep the element and its rect for a
    // future dynamic help binding, but render no stale text.
    if (toLower(field(b.fields, "role")) == "help")
        e.text.clear();

    e.fontPath = field(b.fields, "fontPath");
    if (e.fontPath.empty())
        e.fontPath = field(b.fields, "ttf");
    if (e.fontPath.empty())
        e.fontPath = field(b.fields, "font");

    parseInt(field(b.fields, "legacyFontId", "0"), e.fontId);
    if (e.fontId == 0)
        parseInt(field(b.fields, "fontLegacyId", "0"), e.fontId);
    parseInt(field(b.fields, "fontSize", "12"), e.fontSize);
    e.textAlign = alignmentFromString(field(b.fields, "textAlign"));

    if (e.kind == UiElement::List) {
        e.itemHeight = e.fontSize + 6;
        int ih = 0;
        if (parseInt(field(b.fields, "itemHeight", ""), ih) && ih > 0)
            e.itemHeight = ih;
        int iox = 0;
        if (parseInt(field(b.fields, "itemOffsetX", ""), iox))
            e.itemOffsetX = iox;
        int ioy = 0;
        if (parseInt(field(b.fields, "itemOffsetY", ""), ioy))
            e.itemOffsetY = ioy;
        int scx = 0;
        if (parseInt(field(b.fields, "secondColumnX", ""), scx) && scx > 0)
            e.secondColumnX = scx;
    }

    if (e.kind == UiElement::Combo) {
        e.itemHeight = e.fontSize + 6;
        int ih = 0;
        if (parseInt(field(b.fields, "itemHeight", ""), ih) && ih > 0)
            e.itemHeight = ih;
        int iox = 0;
        if (parseInt(field(b.fields, "itemOffsetX", ""), iox))
            e.itemOffsetX = iox;
        int ph = 0;
        if (parseInt(field(b.fields, "popupHeight", ""), ph) && ph > 0)
            e.popupHeight = ph;
    }

    if (e.kind == UiElement::EditBox) {
        int ml = 0;
        if (parseInt(field(b.fields, "maxLength", ""), ml) && ml > 0)
            e.maxLength = std::min(ml, UiElement::kEditBufSize - 1);
    }

    parseInt(field(b.fields, "uvX", "0"), e.uvX);
    parseInt(field(b.fields, "uvY", "0"), e.uvY);
    parseInt(field(b.fields, "uvWidth", "0"), e.uvW);
    parseInt(field(b.fields, "uvHeight", "0"), e.uvH);

    // V2 UV fields: statics use "uv" = "u,v,uw,vh"; buttons use
    // "uvSheet" = "uw,vh" + "uvNormal" = "u,v".
    {
        const std::string uvQuad = field(b.fields, "uv");
        if (!uvQuad.empty()) {
            int qu = 0, qv = 0, qw = 0, qh = 0;
            if (std::sscanf(uvQuad.c_str(), "%d,%d,%d,%d", &qu, &qv, &qw, &qh) >= 4) {
                e.uvX = qu;
                e.uvY = qv;
                e.uvW = qw;
                e.uvH = qh;
            }
        }
        const std::string uvSheet = field(b.fields, "uvSheet");
        if (!uvSheet.empty()) {
            int sw = 0, sh = 0;
            if (std::sscanf(uvSheet.c_str(), "%d,%d", &sw, &sh) >= 2) {
                e.uvW = sw;
                e.uvH = sh;
            }
        }
        const std::string uvNormal = field(b.fields, "uvNormal");
        if (!uvNormal.empty()) {
            int nu = 0, nv = 0;
            if (std::sscanf(uvNormal.c_str(), "%d,%d", &nu, &nv) >= 2) {
                e.uvX = nu;
                e.uvY = nv;
            }
        }
    }

    e.fillColor = parseColor(b.fields, "fillColorArgb",
                              e.kind == UiElement::Button  ? 0xff243040 :
                              e.kind == UiElement::List    ? 0xcc1a1a1a :
                              e.kind == UiElement::Combo   ? 0xff000000 :
                              e.kind == UiElement::EditBox ? 0xcc0a0a0a : 0x00000000);
    e.borderColor = parseColor(b.fields, "borderColorArgb", 0xff5f6f80);
    e.highlightColor = parseColor(b.fields, "highlightColorArgb", e.highlightColor);
    e.textColor = parseColor(b.fields, "normalTextColorArgb", parseColor(b.fields, "colorArgb", e.textColor));
    e.pressedTextColor = parseColor(b.fields, "pressedTextColorArgb",
                         parseColor(b.fields, "textPressedArgb", e.pressedTextColor));
    e.highlightTextColor = parseColor(b.fields, "highlightTextColorArgb", e.highlightTextColor);
    e.disabledTextColor = parseColor(b.fields, "disabledTextColorArgb",
                          parseColor(b.fields, "textDisabledArgb", e.disabledTextColor));
    e.selectedTextColor = parseColor(b.fields, "selectedColorArgb", e.selectedTextColor);
    // Hovered elements feed the legacy ::helpTextID so the screen's help box
    // (LogisticsScreen::update) shows the same caption text the legacy widgets did.
    parseInt(field(b.fields, "helpDescLegacyId", "0"), e.helpId);
    // Images: "contain" letterboxes the texture (preserve aspect, center) inside
    // the element rect instead of stretching it to fill.
    e.fitContain = toLower(field(b.fields, "imageFit", "")) == "contain";
    // Nearest-filter (crisp) sprites — pixel art / markers that should match the OG's
    // gos_FilterNone + alpha-test rendering instead of the default bilinear blur.
    e.pixelPerfect = parseBool(b.fields, "pixelPerfect", false);
    e.alphaTest = parseBool(b.fields, "alphaTest", false);
    // Slider: full-scale value (e.g. 255 for a 0..255 volume).
    parseInt(field(b.fields, "maxValue", "255"), e.sliderMax);
    if (e.sliderMax <= 0)
        e.sliderMax = 255;
    // Use parseBool (accepts "true"/"1"/"yes"): the converter writes outline = 1,
    // not "true".  A hand-rolled == "true" check here silently made every converted
    // outlined rect (outline = 1) non-outlined, so it fell through to the opaque-fill
    // pass and drew as a solid legacyColor box instead of a hollow outline.
    e.outline = parseBool(b.fields, "outline", false) ||
                parseBool(b.fields, "bOutline", false);
    // Button text-rect outline (legacy aButton data.outlineText / FIT "TextOutline").
    // The converter writes it as legacyTextOutline = "TRUE".
    e.textOutline = parseBool(b.fields, "legacyTextOutline", false) ||
                    parseBool(b.fields, "TextOutline", false) ||
                    parseBool(b.fields, "textOutline", false);

    if (e.kind == UiElement::Button) {
        parseInt(field(b.fields, "uvPressedX",   "-1"), e.uvPressedX);
        parseInt(field(b.fields, "uvPressedY",   "-1"), e.uvPressedY);
        parseInt(field(b.fields, "uvHighlightX", "-1"), e.uvHoverX);
        parseInt(field(b.fields, "uvHighlightY", "-1"), e.uvHoverY);
        parseInt(field(b.fields, "uvDisabledX",  "-1"), e.uvDisabledX);
        parseInt(field(b.fields, "uvDisabledY",  "-1"), e.uvDisabledY);
        e.legacyAnimateBmp  = parseBool(b.fields, "legacyAnimateBmp",  true);
        e.legacyAnimateText = parseBool(b.fields, "legacyAnimateText", false);
        parseLegacyAnimState(b.fields,
            "legacyNormalAnimationTimeStamps", "legacyNormalAnimationLoops",
            "legacyNormalAnimTime", "legacyNormalAnimColor",
            e.legacyNormalAnimKFCount, e.legacyNormalAnimLoops, e.legacyNormalAnimKF);
        parseLegacyAnimState(b.fields,
            "legacyPressedAnimationTimeStamps", "legacyPressedAnimationLoops",
            "legacyPressedAnimTime", "legacyPressedAnimColor",
            e.legacyPressedAnimKFCount, e.legacyPressedAnimLoops, e.legacyPressedAnimKF);
        parseLegacyAnimState(b.fields,
            "legacyHighlightAnimationTimeStamps", "legacyHighlightAnimationLoops",
            "legacyHighlightAnimTime", "legacyHighlightAnimColor",
            e.legacyHighlightAnimKFCount, e.legacyHighlightAnimLoops, e.legacyHighlightAnimKF);
        parseLegacyAnimState(b.fields,
            "legacyHighlightPressedAnimationTimeStamps", "legacyHighlightPressedAnimationLoops",
            "legacyHighlightPressedAnimTime", "legacyHighlightPressedAnimColor",
            e.legacyHighlightPressedAnimKFCount, e.legacyHighlightPressedAnimLoops,
            e.legacyHighlightPressedAnimKF);
        parseLegacyAnimState(b.fields,
            "legacyDisabledAnimationTimeStamps", "legacyDisabledAnimationLoops",
            "legacyDisabledAnimTime", "legacyDisabledAnimColor",
            e.legacyDisabledAnimKFCount, e.legacyDisabledAnimLoops, e.legacyDisabledAnimKF);
    }

    // V2 rects: colorArgb is the single color; outline flag determines
    // whether it is fill or border (matching legacy aRect semantics).
    if (e.kind == UiElement::RectElement &&
        (b.type == "UiV2Rect")) {
        const unsigned int c = parseColor(b.fields, "colorArgb", 0x00000000);
        if ((c & 0xff000000) != 0) {
            if (e.outline)
                e.borderColor = c;
            else
                e.fillColor = c;
        }
    }

    // V1 converter fallback: legacy GUI_RECTs carry their color in legacyColor
    // (e.g. 0xEC000000 panel backings) while the converter writes
    // fillColorArgb/borderColorArgb as 0x00000000.  When the authored keys
    // are fully transparent and legacyColor is not, apply legacy semantics:
    // outline=TRUE draws the border in legacyColor, outline=FALSE fills with
    // it.  Authoring an explicit non-zero color in the UI Editor overrides
    // this; to get an intentionally invisible rect, strip legacyColor too.
    if (e.kind == UiElement::RectElement) {
        const unsigned int legacy = parseColor(b.fields, "legacyColor", 0x00000000);
        if ((legacy & 0xff000000) != 0) {
            if (e.outline) {
                if ((e.borderColor & 0xff000000) == 0)
                    e.borderColor = legacy;
            } else if ((e.fillColor & 0xff000000) == 0) {
                e.fillColor = legacy;
            }
        }
    }

    return e;
}


} // namespace uidefs_detail
