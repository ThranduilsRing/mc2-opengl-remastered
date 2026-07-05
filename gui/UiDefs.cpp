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
extern unsigned int gos_GetGLTextureName(DWORD handle);
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

namespace {

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

static std::string trim(const std::string& in)
{
    const char* ws = " \t\r\n";
    const std::size_t first = in.find_first_not_of(ws);
    if (first == std::string::npos)
        return std::string();
    const std::size_t last = in.find_last_not_of(ws);
    return in.substr(first, last - first + 1);
}

static std::string toLower(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

static bool envValueIsFalse(const char* v)
{
    if (!v || !v[0])
        return false;
    const std::string lower = toLower(v);
    return lower == "0" || lower == "false" || lower == "off" || lower == "no";
}

static bool uiDefsTraceEnabled()
{
    const char* v = std::getenv("MC2_UI_DEFS_TRACE");
    return v && v[0] && !envValueIsFalse(v);
}

static void uiDefsTrace(const char* fmt, ...)
{
    if (!uiDefsTraceEnabled())
        return;
    std::fprintf(stderr, "[UI_DEFS] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

static std::string stripLineComment(const std::string& line)
{
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c == '/' && line[i + 1] == '/')
            return line.substr(0, i);
    }
    return line;
}

static std::string unquote(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string out;
        out.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                const char n = value[++i];
                switch (n) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    default: out.push_back(n); break;
                }
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }
    return value;
}

static bool parseInt(const std::string& s, int& out)
{
    try {
        std::size_t pos = 0;
        long v = std::stol(trim(s), &pos, 0);
        if (pos == 0)
            return false;
        out = static_cast<int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parseUInt(const std::string& s, unsigned int& out)
{
    try {
        std::size_t pos = 0;
        unsigned long v = std::stoul(trim(s), &pos, 0);
        if (pos == 0)
            return false;
        out = static_cast<unsigned int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

static unsigned int parseColor(const std::unordered_map<std::string, std::string>& f,
                              const char* name,
                              unsigned int fallback)
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    unsigned int out = fallback;
    parseUInt(it->second, out);
    return out;
}

static std::string field(const std::unordered_map<std::string, std::string>& f,
                         const char* name,
                         const char* fallback = "")
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    return it->second;
}

static Rect parseRect(const std::string& value)
{
    Rect r;
    std::string part;
    std::vector<int> values;
    for (char c : value) {
        if (c == ',') {
            int v = 0;
            parseInt(part, v);
            values.push_back(v);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty() || value.size()) {
        int v = 0;
        parseInt(part, v);
        values.push_back(v);
    }
    if (values.size() >= 4) {
        r.x = values[0];
        r.y = values[1];
        r.w = values[2];
        r.h = values[3];
    }
    return r;
}

static bool pointInRect(const Rect& r, int x, int y)
{
    return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
}

static std::vector<Block> parseTypedFit(const char* path)
{
    std::vector<Block> blocks;
    std::ifstream in(path);
    if (!in)
        return blocks;

    bool inBlock = false;
    Block current;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(stripLineComment(line));
        if (line.empty())
            continue;

        if (!inBlock) {
            const std::size_t brace = line.find('{');
            if (brace == std::string::npos)
                continue;
            current = Block();
            current.type = trim(line.substr(0, brace));
            inBlock = !current.type.empty();
            continue;
        }

        if (line.find('}') != std::string::npos) {
            blocks.push_back(current);
            current = Block();
            inBlock = false;
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string name = trim(line.substr(0, eq));
        if (!name.empty())
            current.fields[name] = unquote(line.substr(eq + 1));
    }
    return blocks;
}


static std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    char buffer[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

static std::filesystem::path defsRoot()
{
    const char* explicitRoot = std::getenv("MC2_DEFS_ROOT");
    if (explicitRoot && explicitRoot[0])
        return std::filesystem::path(explicitRoot);

    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path exe = executableDirectory();
    const std::filesystem::path candidates[] = {
        cwd / "data" / "defs",
        exe / "data" / "defs",
        cwd.parent_path() / "data" / "defs",
        exe.parent_path() / "data" / "defs",
    };

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    return cwd / "data" / "defs";
}

static bool fileExistsPath(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

static std::filesystem::path resolveCaseInsensitive(const std::filesystem::path& raw)
{
    if (fileExistsPath(raw))
        return raw;

    std::filesystem::path cur;
    for (const auto& part : raw) {
        if (cur.empty()) {
            cur = part;
            if (!fileExistsPath(cur) && part == raw.root_path())
                continue;
            if (fileExistsPath(cur))
                continue;
        }

        std::filesystem::path candidate = cur / part;
        if (fileExistsPath(candidate)) {
            cur = candidate;
            continue;
        }

        if (!fileExistsPath(cur))
            return raw;

        const std::string wanted = toLower(part.string());
        bool found = false;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (toLower(entry.path().filename().string()) == wanted) {
                cur = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
            return raw;
    }
    return cur;
}

static std::filesystem::path resolveAssetPath(const std::string& value)
{
    if (value.empty())
        return std::filesystem::path();

    const std::filesystem::path raw(value);
    if (raw.is_absolute())
        return resolveCaseInsensitive(raw);

    std::vector<std::filesystem::path> roots;
    auto addRoot = [&roots](std::filesystem::path p) {
        std::error_code ec;
        p = std::filesystem::weakly_canonical(p, ec);
        if (ec)
            p = p.lexically_normal();
        if (std::find(roots.begin(), roots.end(), p) == roots.end())
            roots.push_back(p);
    };

    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path exe = executableDirectory();

    for (int i = 0; i < 8; ++i) {
        addRoot(cwd);
        addRoot(exe);
        if (!cwd.has_parent_path() && !exe.has_parent_path())
            break;
        cwd = cwd.parent_path();
        exe = exe.parent_path();
    }

    for (const std::filesystem::path& root : roots) {
        const std::filesystem::path candidate = resolveCaseInsensitive(root / raw);
        if (fileExistsPath(candidate))
            return candidate;
    }

    return resolveCaseInsensitive(raw);
}

static void traceMissingTextureOnce(const std::string& texturePath, const std::filesystem::path& resolved)
{
    if (!uiDefsTraceEnabled())
        return;
    static std::unordered_map<std::string, bool> s_seen;
    if (s_seen[texturePath])
        return;
    s_seen[texturePath] = true;
    uiDefsTrace("missing UI texture: requested=%s resolved=%s",
                texturePath.c_str(), resolved.string().c_str());
}

static bool readUiTextureSize(const std::filesystem::path& path, int& width, int& height)
{
    width = 0;
    height = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    unsigned char header[32] = {0};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    const std::streamsize got = in.gcount();
    if (got >= 24 &&
        header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
        width = (static_cast<int>(header[16]) << 24) |
                (static_cast<int>(header[17]) << 16) |
                (static_cast<int>(header[18]) << 8) |
                static_cast<int>(header[19]);
        height = (static_cast<int>(header[20]) << 24) |
                 (static_cast<int>(header[21]) << 16) |
                 (static_cast<int>(header[22]) << 8) |
                 static_cast<int>(header[23]);
        return width > 0 && height > 0;
    }

    if (got >= 18) {
        const std::string ext = toLower(path.extension().string());
        if (ext == ".tga") {
            width = static_cast<int>(header[12]) | (static_cast<int>(header[13]) << 8);
            height = static_cast<int>(header[14]) | (static_cast<int>(header[15]) << 8);
            return width > 0 && height > 0;
        }
    }

    return false;
}

static std::string baseNameNoExt(const char* path)
{
    if (!path)
        return std::string();
    std::filesystem::path p(path);
    return toLower(p.stem().string());
}

static int alignmentFromString(const std::string& value)
{
    const std::string v = toLower(value);
    if (v == "right")
        return 1;
    if (v == "center" || v == "centre")
        return 2;
    return 0;
}

static int parseLegacyButtonIndex(const std::unordered_map<std::string, std::string>& f)
{
    std::string section = field(f, "legacySection");
    if (section.empty())
        section = field(f, "sourceControlType");
    const std::string prefix = "Button";
    if (section.rfind(prefix, 0) != 0)
        return -1;

    int index = -1;
    if (parseInt(section.substr(prefix.size()), index))
        return index;
    return -1;
}

static void drawQuad(const Rect& r, unsigned int color, unsigned int textureHandle,
                     float u1 = 0.0f, float v1 = 0.0f, float u2 = 1.0f, float v2 = 1.0f)
{
    gos_VERTEX verts[4];
    verts[0].x = static_cast<float>(r.x);       verts[0].y = static_cast<float>(r.y);
    verts[1].x = static_cast<float>(r.x);       verts[1].y = static_cast<float>(r.y + r.h);
    verts[2].x = static_cast<float>(r.x + r.w); verts[2].y = static_cast<float>(r.y + r.h);
    verts[3].x = static_cast<float>(r.x + r.w); verts[3].y = static_cast<float>(r.y);

    verts[0].u = u1; verts[0].v = v1;
    verts[1].u = u1; verts[1].v = v2;
    verts[2].u = u2; verts[2].v = v2;
    verts[3].u = u2; verts[3].v = v1;

    for (int i = 0; i < 4; ++i) {
        verts[i].argb = color;
        verts[i].frgb = 0;
        verts[i].z = 0.0f;
        verts[i].rhw = 0.5f;
    }

    gos_SetRenderState(gos_State_Texture, textureHandle);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_Filter, gos_FilterNone);
    gos_SetRenderState(gos_State_AlphaTest, true);
    gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
    gos_SetRenderState(gos_State_TextureMapBlend, gos_BlendModulateAlpha);
    gos_DrawQuads(verts, 4);
}

static void drawLineRect(const Rect& r, unsigned int color)
{
    gos_VERTEX verts[5];
    const float x1 = static_cast<float>(r.x);
    const float y1 = static_cast<float>(r.y);
    const float x2 = static_cast<float>(r.x + r.w);
    const float y2 = static_cast<float>(r.y + r.h);
    verts[0].x = x1; verts[0].y = y1;
    verts[1].x = x2; verts[1].y = y1;
    verts[2].x = x2; verts[2].y = y2;
    verts[3].x = x1; verts[3].y = y2;
    verts[4].x = x1; verts[4].y = y1;
    for (int i = 0; i < 5; ++i) {
        verts[i].u = verts[i].v = 0.0f;
        verts[i].argb = color;
        verts[i].frgb = 0;
        verts[i].z = 0.0f;
        verts[i].rhw = 0.5f;
    }
    gos_SetRenderState(gos_State_Texture, 0);
    gos_DrawLines(verts, 5);
}

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
    std::string legacySection;   // UI-LAYER-CONTRACT-2: e.g. "Static4" / "Rect0" / "Text2" / "Button3"
    std::string texturePath;
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
    bool uvLegacySpace = false; // uvXYWH addressed in legacy size/uvScale space (PILOT-PHOTO-UV-1)
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

    // Legacy button animation keyframes (parsed from legacyNormalAnimationTimeStamps etc.)
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

    // Per-state animation accumulators (advance only while that state is active).
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

    // List (GuiList) runtime state.
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

    // Combo (GuiCombo) runtime state.
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

static bool parseBool(const std::unordered_map<std::string, std::string>& f,
                      const char* name, bool fallback = false)
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

static UiElement makeElement(const Block& b)
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

// FIT pages are authored in a local coordinate space (GuiPage localWidth/
// localHeight, typically 800x600).  ImGui draws in window-logical pixels
// (io.DisplaySize), so every rect, font size, and mouse coordinate must be
// transformed between the two spaces.  Without this the page renders at
// authored size in the upper-left of the window.
struct PageScale {
    float sx = 1.0f;
    float sy = 1.0f;
    // UI-ASPECT-ANCHOR-1: display-pixel origin of the 16:9 UI canvas
    // (pillarbox/letterbox pads); 0,0 when the canvas fills the display.
    float ox = 0.0f;
    float oy = 0.0f;
};

static PageScale currentPageScale(int localWidth, int localHeight)
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

struct FRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

static FRect scaledRect(const Rect& r, int xOffset, int yOffset, const PageScale& s)
{
    FRect out;
    out.x = static_cast<float>(r.x + xOffset) * s.sx + s.ox;
    out.y = static_cast<float>(r.y + yOffset) * s.sy + s.oy;
    out.w = static_cast<float>(r.w) * s.sx;
    out.h = static_cast<float>(r.h) * s.sy;
    return out;
}

static int scaledFontSize(int fontSize, const PageScale& s)
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

static int messageIdForElement(LogisticsScreen* screen, const UiElement& e)
{
    if (e.legacyId > 0)
        return e.legacyId;
    aButton* legacyButton = legacyButtonForElement(screen, e);
    return legacyButton ? legacyButton->getID() : 0;
}

static bool isButtonEnabled(LogisticsScreen* screen, const UiElement& e)
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

static void drawUiRectElement(const FRect& r, unsigned int color, bool filled)
{
    if ((color & 0xff000000) == 0)
        return;
    GuiRuntime::DrawUiRect(r.x, r.y, r.w, r.h, color, filled);
}

static unsigned int sampleLegacyAnim(const UiElement::AnimKeyframe* kf, int count,
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

static bool drawUiImageElement(UiElement& e, int xOffset, int yOffset, const PageScale& s,
                                unsigned int tint = 0xffffffff)
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
    if (getenv("MC2_LOG_PREVIEW") && e.key.find("this_rect_defines") != std::string::npos) {
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

static void renderTextElement(UiElement& e, int xOffset, int yOffset, unsigned int color, const PageScale& s)
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
static void drawUiListElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
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
static void drawUiSliderElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
{
    const FRect r = scaledRect(e.rect, xOffset, yOffset, s);
    if (r.w <= 0.0f || r.h <= 0.0f)
        return;

    const float frac = (e.sliderMax > 0)
        ? std::min(1.0f, std::max(0.0f, static_cast<float>(e.sliderValue) /
                                        static_cast<float>(e.sliderMax)))
        : 0.0f;

    // Groove: a slim bar centered vertically in the element rect.
    const float grooveH = std::max(3.0f, r.h * 0.34f);
    const float grooveY = r.y + (r.h - grooveH) * 0.5f;
    const unsigned int grooveColor = (e.fillColor & 0xff000000) ? e.fillColor : 0xff202830;
    GuiRuntime::DrawUiRect(r.x, grooveY, r.w, grooveH, grooveColor, true);

    // Thumb travels inside the track so it never overhangs the ends.
    const float thumbW = std::max(8.0f, r.h * 0.55f);
    const float thumbX = r.x + frac * (r.w - thumbW);

    // Filled portion up to the thumb center.
    const float fillW = (thumbX + thumbW * 0.5f) - r.x;
    if (fillW > 0.0f)
        GuiRuntime::DrawUiRect(r.x, grooveY, fillW, grooveH, e.textColor, true);

    // Thumb (full element height for an easy hit target).
    GuiRuntime::DrawUiRect(thumbX, r.y, thumbW, r.h, e.highlightColor, true);
}

// GuiCheckbox / GuiRadio: a modern glyph driven by e.pressed (the toggle/radio
// state, synced from the legacy button), plus the button's text label in a STABLE
// MC2-orange (no hover/press flash). The element rect is the glyph box; the label
// lives in textRect. Glyph colors are fixed to the MC2 theme.
static void drawUiCheckRadioElement(UiElement& e, int xOffset, int yOffset, const PageScale& s)
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

static float getFrameDelta()
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last = Clock::now();
    const auto now = Clock::now();
    const float dt = std::chrono::duration<float>(now - s_last).count();
    s_last = now;
    return (dt > 0.0f && dt < 0.2f) ? dt : (1.0f / 60.0f);
}

} // namespace

struct UiDefs::GameOSPage::Impl {
    bool loaded = false;
    std::string key;
    int localWidth = 800;
    int localHeight = 600;
    bool renderScaleTraced = false;
    // ENCYCLO-3D-2: page-local -> screen transform needs the same offsets
    // render() was last called with; getElementScreenRect used (0,0) and
    // returned page-local coords, so the mech-preview composite landed at the
    // wrong screen position (e.g. mechlopedia's (285,58) page offset lost).
    int lastRenderXOffset = 0;
    int lastRenderYOffset = 0;
    bool suppressAnimationElements = false;
    bool legacyPassthrough = false;
    std::vector<UiElement> elements;
};

bool UiDefs::gameOsUiDefsEnabled()
{
    // The data/defs GameOS UI path is the replacement path, not an opt-in demo.
    // Set MC2_GAMEOS_UI_DEFS=0/false/off/no to force the legacy GameOS FIT path
    // when comparing behavior or bisecting UI regressions.
    const char* v = std::getenv("MC2_GAMEOS_UI_DEFS");
    return !envValueIsFalse(v);
}

bool UiDefs::editorUiDefsEnabled()
{
    // Mission Editor shell overlay is data/defs-first by default.  Force it off
    // with MC2_EDITOR_UI_DEFS=0 when comparing against the old pure-MFC shell.
    const char* v = std::getenv("MC2_EDITOR_UI_DEFS");
    return !envValueIsFalse(v);
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

    // RESULTS-LEGACY-FALLBACK-1: the mission-results defs page
    // (mcui_mr_layout: salvage / pilot review / promotion) is UNFINISHED
    // modder content — the game-side screens were explicitly not done in the
    // ImGui-port handoff, and the generated page breaks the AAR while pure
    // legacy renders it correctly. Keep the AAR on the legacy path until the
    // page is actually authored. MC2_UI_DEFS_RESULTS=1 opts the page back in
    // for iterating on it.
    // LOADSCREEN-LEGACY-FALLBACK-1: same ruling as the AAR — the generated
    // loading-screen pages (mcl_loadingscreen*) regressed the load sequence
    // (door art shifted to the top-left instead of centered, residual
    // logo/hourglass banner art over pilot-ready and mission start) while
    // pure legacy renders it correctly. Pin them to legacy until authored.
    // MC2_UI_DEFS_LOADSCREEN=1 opts back in for iteration.
    if (stem.rfind("mcl_loadingscreen", 0) == 0) {
        static const bool s_loadDefs = (std::getenv("MC2_UI_DEFS_LOADSCREEN") != nullptr);
        if (!s_loadDefs) {
            uiDefsTrace("legacy FIT %s -> LEGACY (loadscreen page opt-out)", legacyFitPath ? legacyFitPath : "<null>");
            return std::string();
        }
    }

    if (stem == "mcui_mr_layout") {
        static const bool s_resultsDefs = (std::getenv("MC2_UI_DEFS_RESULTS") != nullptr);
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

    const std::vector<Block> blocks = parseTypedFit(path);
    if (blocks.empty()) {
        uiDefsTrace("load failed, no typed blocks: %s", path);
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

    impl->loaded = true;
    uiDefsTrace("loaded page key=%s file=%s elements=%zu local=%dx%d",
        impl->key.c_str(), path, impl->elements.size(), impl->localWidth, impl->localHeight);
    return true;
}

void UiDefs::GameOSPage::clear()
{
    if (!impl)
        return;
    // Texture nodes are intentionally left under mcTextureManager ownership.
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
                // PILOT-PHOTO-UV-1: the fit-authored pixel sub-rect (uvX/Y/W/H)
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
        // by getMissionTGA): inject the MC_TextureManager node directly instead
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

// UI-LAYER-CONTRACT-2: does this page carry an element mirroring the given
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

// UI-LAYER-CONTRACT-2: kind+index coverage. Legacy section names are NOT
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
        // space (old setUVs math), not the logical PNG space (PILOT-PHOTO-UV-1).
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
                frac = std::min(1.0f, std::max(0.0f, frac));
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
                e.scrollOffset = std::max(0, std::min(e.scrollOffset + delta, maxScroll));
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
                      e.kind == UiElement::Text))
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.fillColor, true);
                break;
            case Images:
                if (e.kind == UiElement::Image) {
                    drawUiImageElement(e, xOffset, yOffset, s);
                } else if (e.kind == UiElement::List) {
                    drawUiListElement(e, xOffset, yOffset, s);
                } else if (e.kind == UiElement::Slider) {
                    drawUiSliderElement(e, xOffset, yOffset, s);
                } else if (e.kind == UiElement::Combo) {
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
                }
                break;
            case Outlines:
                if (e.kind == UiElement::RectElement &&
                    (e.outline || (e.borderColor & 0xff000000)))
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.borderColor, false);
                break;
            case TransparentFills:
                if (e.kind == UiElement::RectElement && !e.outline &&
                    (e.fillColor & 0xff000000) != 0 &&
                    (e.fillColor & 0xff000000) != 0xff000000)
                    drawUiRectElement(scaledRect(e.rect, xOffset, yOffset, s), e.fillColor, true);
                break;
            case Buttons: {
                if (e.kind == UiElement::Button &&
                    e.buttonStyle != UiElement::ButtonStyle::Normal) {
                    // Modern checkbox/radio glyph + stable label (no flash).
                    drawUiCheckRadioElement(e, xOffset, yOffset, s);
                } else if (e.kind == UiElement::Button) {
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
                if (e.kind == UiElement::Text)
                    renderTextElement(e, xOffset, yOffset, e.textColor, s);
                break;
            case Popups:
                // Expanded GuiCombo popup renders last so it appears above all other elements.
                if (e.kind == UiElement::Combo && e.expanded && !e.items.empty()) {
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
        e.sliderValue = std::min(std::max(value, 0), e.sliderMax);
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
