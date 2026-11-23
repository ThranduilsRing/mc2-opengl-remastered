// UI-CHROME-SPLIT-1: see ui_chrome_sheets.h.
#include "ui_chrome_sheets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ranges>
#include <set>
#include <span>

namespace mc2::ui_chrome {

namespace {
constexpr char kSlash = 0x2f;
constexpr char kBackslash = 0x5c;
}  // namespace

std::optional<std::reference_wrapper<const Sheet>> Manifest::find(std::string_view key) const {
    for (const auto& [name, sheet] : sheets)
        if (name == key) return std::cref(sheet);
    return std::nullopt;
}

bool parseManifest(std::string_view json, Manifest& out, std::string* err) {
    auto fail = [&](std::string_view why) {
        if (err) *err = std::string(why);
        out.sheets.clear();
        return false;
    };
    nlohmann::json j = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
    if (j.value("version", 0) != 1) return fail("unsupported manifest version");
    const auto sheetsIt = j.find("sheets");
    if (sheetsIt == j.end() || !sheetsIt->is_object()) return fail("no sheets object");
    out.sheets.clear();
    for (auto it = sheetsIt->begin(); it != sheetsIt->end(); ++it) {
        const auto& js = it.value();
        if (!js.is_object() || !js.contains("size") || !js["size"].is_array() || js["size"].size() != 2)
            return fail("sheet " + it.key() + ": bad size");
        Sheet s;
        s.key = it.key();
        s.w = js["size"][0].get<int>();
        s.h = js["size"][1].get<int>();
        s.rgba = js.value("mode", std::string("RGB")) == "RGBA";
        s.pad = js.value("pad", 0);
        if (s.pad < 0 || s.pad > 16) return fail("sheet " + it.key() + ": bad pad");
        s.scale = js.value("scale", 1);
        if (s.scale < 1 || s.scale > 8) return fail("sheet " + it.key() + ": bad scale");
        if (s.w <= 0 || s.h <= 0 || s.w > 8192 || s.h > 8192) return fail("sheet " + it.key() + ": bad dimensions");
        std::set<std::string> names;
        if (js.contains("widgets") && js["widgets"].is_object()) {
            for (auto wi = js["widgets"].begin(); wi != js["widgets"].end(); ++wi) {
                const auto& jw = wi.value();
                if (!jw.is_object() || !jw.contains("rect") || !jw["rect"].is_array() || jw["rect"].size() != 4)
                    return fail("widget " + wi.key() + ": bad rect");
                Widget w;
                w.name = wi.key();
                w.x = jw["rect"][0].get<int>();
                w.y = jw["rect"][1].get<int>();
                w.w = jw["rect"][2].get<int>();
                w.h = jw["rect"][3].get<int>();
                if (w.x < 0 || w.y < 0 || w.w <= 0 || w.h <= 0 || w.x + w.w > s.w || w.y + w.h > s.h)
                    return fail("widget " + w.name + ": rect outside sheet " + s.key);
                const auto [namePos, fresh] = names.insert(w.name);
                if (std::ranges::any_of(w.name, [](char c) { return c == kSlash || c == kBackslash; }) || !fresh)
                    return fail("widget " + w.name + ": bad or duplicate name");
                if (jw.contains("nine") && jw["nine"].is_array() && jw["nine"].size() == 4) {
                    for (size_t i = 0; i < 4; ++i) w.nine[i] = jw["nine"][i].get<int>();
                    w.hasNine = w.nine[0] >= 0 && w.nine[1] >= 0 && w.nine[2] >= 0 && w.nine[3] >= 0 &&
                                w.nine[0] + w.nine[2] < w.w && w.nine[1] + w.nine[3] < w.h;
                }
                s.widgets.push_back(std::move(w));
            }
        }
        out.sheets.emplace(s.key, std::move(s));
    }
    return true;
}

std::string manifestToJson(const Manifest& m) {
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json sheets = nlohmann::json::object();
    for (const auto& [key, s] : m.sheets) {
        nlohmann::json js;
        js["size"] = {s.w, s.h};
        js["mode"] = s.rgba ? "RGBA" : "RGB";
        if (s.pad) js["pad"] = s.pad;
        if (s.scale != 1) js["scale"] = s.scale;
        nlohmann::json ws = nlohmann::json::object();
        for (const auto& w : s.widgets) {
            nlohmann::json jw;
            jw["rect"] = {w.x, w.y, w.w, w.h};
            if (w.hasNine) jw["nine"] = {w.nine[0], w.nine[1], w.nine[2], w.nine[3]};
            ws[w.name] = std::move(jw);
        }
        js["widgets"] = std::move(ws);
        sheets[key] = std::move(js);
    }
    j["sheets"] = std::move(sheets);
    return j.dump();
}

// Stock menu mattes are baked black; text wells are separate filled widgets.
bool clearMenuMatte(std::string_view path, Pixels& px) {
    std::string key(path.substr(path.find_last_of("/\\") + 1));
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto dot = key.find_last_of('.');
    if (dot != std::string::npos) key.resize(dot);
    const bool menu = key == "mcl_mm_1" || key == "mcl_mm_2" || key == "mcl_mm_3" ||
                      key == "mcl_mm_5" || key == "mcl_mm_12";
    const bool dialog = key.starts_with("mcl_dialog_");
    const bool frame = key == "mcl_splashscreen_frame_1" || key == "mcl_splashscreen_frame_2";
    const bool saveList = key.size() == 8 && key.starts_with("mcl_sm_") && key[7] >= '1' && key[7] <= '4';
    if ((!menu && !dialog && !frame && !saveList) || px.w <= 0 || px.h <= 0 || (px.channels != 3 && px.channels != 4)) return false;
    std::vector<uint8_t> rgba(static_cast<size_t>(px.w) * px.h * 4);
    for (int y = 0; y < px.h; ++y) for (int x = 0; x < px.w; ++x) {
        const size_t i = static_cast<size_t>(y) * px.w + x, src = i * px.channels, dst = i * 4;
        std::copy_n(px.data.data() + src, 3, rgba.data() + dst);
        // The three stock button rows include opaque black wells and white
        // brackets. Their FIT animation supplies the tint; keep the source paint.
        if (key == "mcl_mm_12" && y < 96) {
            rgba[dst + 3] = px.channels == 4 ? px.data[src + 3] : 255;
            continue;
        }
        const int value = std::max({rgba[dst], rgba[dst + 1], rgba[dst + 2]});
        if (menu || frame) {
            auto paint = (static_cast<uint32_t>(rgba[dst]) << 16) |
                         (static_cast<uint32_t>(rgba[dst + 1]) << 8) | rgba[dst + 2];
            if (key == "mcl_mm_12") paint = menuTintCarrier(paint);
            const bool title = (key == "mcl_mm_1" && x >= 125 && y < 100) ||
                               (key == "mcl_mm_2" && (y < 94 || (y >= 116 && y < 210)));
            if (!title) paint = menuYellow(paint); // Only the logo retains its authored gold gradient.
            rgba[dst] = static_cast<uint8_t>(paint >> 16);
            rgba[dst + 1] = static_cast<uint8_t>(paint >> 8);
            rgba[dst + 2] = static_cast<uint8_t>(paint);
        }
        // Retain the dialog button wells; the surrounding cable voids remain open.
        const bool well = dialog && ((key.ends_with("_3") && x >= 50 && x < 178 && y >= 118 && y < 236) ||
                                     (key == "mcl_dialog_onebutton_4" && x < 99 && y < 118));
        // Open the title and outer-frame mattes; retain the hazard stripes' black paint.
        const bool opening = dialog || frame || key == "mcl_mm_5" || key == "mcl_mm_12" ||
            (key == "mcl_mm_1" && ((x >= 125 && y < 100) || (x >= 130 && y >= 112 && y < 181) ||
                                      (x >= 124 && x < 144 && y >= 214))) ||
            (key == "mcl_mm_2" && (y < 94 || (y >= 116 && y < 210))) ||
            (key == "mcl_mm_3" && ((x < 114 && y < 200) || (x < 57 && x + y < 255) ||
                                       (x >= 57 && x < 114 && y < 232 && x + 110 > y) ||
                                       (x >= 171 && y < 178)));
        const int coverage = well || !opening ? 255 : std::clamp((value - 4) * 255 / 12, 0, 255);
        rgba[dst + 3] = static_cast<uint8_t>((px.channels == 4 ? px.data[src + 3] : 255) * coverage / 255);
    }
    // Only exterior-connected matte is empty space. Enclosed black paint (hazard
    // stripes, vents and button wells) belongs to the chrome.
    std::vector<bool> exterior(static_cast<size_t>(px.w) * px.h, false);
    std::vector<size_t> pending;
    const auto visit = [&](size_t i) {
        const int authored = px.channels == 4 ? px.data[i * 4 + 3] : 255;
        if (!exterior[i] && (rgba[i * 4 + 3] < authored || authored == 0)) {
            exterior[i] = true;
            pending.push_back(i);
        }
    };
    for (int x = 0; x < px.w; ++x) { visit(x); visit(static_cast<size_t>(px.h - 1) * px.w + x); }
    for (int y = 0; y < px.h; ++y) { visit(static_cast<size_t>(y) * px.w); visit(static_cast<size_t>(y + 1) * px.w - 1); }
    for (size_t at = 0; at < pending.size(); ++at) {
        const size_t i = pending[at];
        if (i % px.w) visit(i - 1);
        if (i % px.w + 1 < static_cast<size_t>(px.w)) visit(i + 1);
        if (i >= static_cast<size_t>(px.w)) visit(i - px.w);
        if (i + px.w < exterior.size()) visit(i + px.w);
    }
    for (size_t i = 0; i < exterior.size(); ++i)
        if (dialog && !exterior[i]) rgba[i * 4 + 3] = px.channels == 4 ? px.data[i * 4 + 3] : 255;
    if (!dialog && !frame) {
        // Pure-black matte (value < 4) connected to the sheet edge is empty space, including the
        // hazard-stripe and frame pieces the openings above leave opaque; stripe paint is 4+ and stays.
        // Dark pixels (< 16) within two pixels of that matte ramp out so no black fringe is left.
        const size_t w = static_cast<size_t>(px.w), h = static_cast<size_t>(px.h);
        const auto srcValue = [&](size_t i) { return std::max({px.data[i * px.channels], px.data[i * px.channels + 1], px.data[i * px.channels + 2]}); };
        const auto skip = [&](size_t i) { return key == "mcl_mm_12" && i / w < 96; };
        std::vector<bool> clear(w * h, false);
        pending.clear();
        const auto seed = [&](size_t i) {
            if (!clear[i] && !skip(i) && (srcValue(i) < 4 || rgba[i * 4 + 3] == 0)) { clear[i] = true; pending.push_back(i); }
        };
        for (size_t x = 0; x < w; ++x) { seed(x); seed((h - 1) * w + x); }
        for (size_t y = 0; y < h; ++y) { seed(y * w); seed(y * w + w - 1); }
        for (size_t at = 0; at < pending.size(); ++at) {
            const size_t i = pending[at];
            if (i % w) seed(i - 1);
            if (i % w + 1 < w) seed(i + 1);
            if (i >= w) seed(i - w);
            if (i + w < clear.size()) seed(i + w);
        }
        std::vector<bool> grown = clear;
        for (size_t i = 0; i < clear.size(); ++i) if (clear[i]) rgba[i * 4 + 3] = 0;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<bool> next = grown;
            for (size_t y = 0; y < h; ++y) for (size_t x = 0; x < w; ++x) {
                const size_t i = y * w + x;
                if (grown[i] || skip(i) || srcValue(i) >= 16) continue;
                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const long nx = static_cast<long>(x) + dx, ny = static_cast<long>(y) + dy;
                    const long j = static_cast<long>(i) + dy * static_cast<long>(w) + dx;
                    if (nx >= 0 && ny >= 0 && nx < static_cast<long>(w) && ny < static_cast<long>(h) && grown[static_cast<size_t>(j)]) { touches = true; break; }
                }
                if (!touches) continue;
                next[i] = true;
                const int ramp = std::clamp((srcValue(i) - 4) * 255 / 12, 0, 255);
                rgba[i * 4 + 3] = static_cast<uint8_t>(std::min<int>(rgba[i * 4 + 3], ramp));
            }
            grown = std::move(next);
        }
    }
    px.channels = 4;
    px.data = std::move(rgba);
    return true;
}

bool composeSheet(const Sheet& sheet, const WidgetLoader& load, Pixels& out, std::string* err) {
    auto fail = [&](std::string_view why) {
        if (err) *err = std::string(why);
        out = Pixels{};
        return false;
    };
    if (sheet.widgets.empty()) return fail("sheet " + sheet.key + " has no widgets");
    const int ch = sheet.rgba ? 4 : 3;
    const int k = sheet.scale < 1 ? 1 : sheet.scale;
    const int outW = sheet.w * k;
    out.w = outW;
    out.h = sheet.h * k;
    out.channels = ch;
    out.data.assign(static_cast<size_t>(out.w) * out.h * ch, 0);
    if (!sheet.rgba)   // the authored RGB sheets key transparency on FF00FF; uncovered texels stay "no art"
        for (size_t i = 0; i < out.data.size(); i += 3) { out.data[i] = 255; out.data[i + 2] = 255; }
    Pixels px;
    for (const auto& w : sheet.widgets) {
        px = Pixels{};
        const PaddedRect pr = paddedRect(sheet, w);
        const int pw = (pr.x1 - pr.x0) * k, ph = (pr.y1 - pr.y0) * k;
        if (!load(sheet, w, px) || px.w != pw || px.h != ph || px.data.size() != static_cast<size_t>(px.w) * px.h * px.channels ||
            (px.channels != 3 && px.channels != 4))
            return fail("widget " + sheet.key + ": " + w.name + " missing or wrong size");
        // The padded ring holds the authored neighbours, so it agrees with any widget whose core it overlaps.
        for (int y = 0; y < ph; ++y) {
            uint8_t* dst = out.data.data() + (static_cast<size_t>(pr.y0) * k + y) * outW * ch + static_cast<size_t>(pr.x0) * k * ch;
            const uint8_t* src = px.data.data() + static_cast<size_t>(y) * pw * px.channels;
            for (int x = 0; x < pw; ++x) {
                dst[x * ch + 0] = src[x * px.channels + 0];
                dst[x * ch + 1] = src[x * px.channels + 1];
                dst[x * ch + 2] = src[x * px.channels + 2];
                if (ch == 4) dst[x * ch + 3] = px.channels == 4 ? src[x * 4 + 3] : 255;
            }
        }
    }
    return true;
}

namespace {
uint32_t crc32Of(std::span<const uint8_t> p) {
    // Compile-time table: no lazy init, so concurrent callers share only read-only data.
    static constexpr std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xffffffffu;
    for (const uint8_t b : p) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (const int shift : {24, 16, 8, 0}) v.push_back(static_cast<uint8_t>(x >> shift));
}
void chunk(std::vector<uint8_t>& png, std::string_view type, const std::vector<uint8_t>& body) {
    be32(png, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tb(type.begin(), type.end());
    tb.insert(tb.end(), body.begin(), body.end());
    png.insert(png.end(), tb.begin(), tb.end());
    be32(png, crc32Of(tb));
}
}  // namespace

std::vector<uint8_t> encodePngStored(const Pixels& px) {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (px.w <= 0 || px.h <= 0 || (px.channels != 3 && px.channels != 4) ||
        px.data.size() != static_cast<size_t>(px.w) * px.h * px.channels)
        return {};
    std::vector<uint8_t> ihdr;
    be32(ihdr, static_cast<uint32_t>(px.w));
    be32(ihdr, static_cast<uint32_t>(px.h));
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(px.channels == 4 ? 6 : 2), 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(px.h) * (static_cast<size_t>(px.w) * px.channels + 1));
    for (int y = 0; y < px.h; ++y) {
        raw.push_back(0);   // filter: none
        const uint8_t* row = px.data.data() + static_cast<size_t>(y) * px.w * px.channels;
        raw.insert(raw.end(), row, row + static_cast<size_t>(px.w) * px.channels);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        const auto len = static_cast<uint16_t>(n);
        const auto nlen = static_cast<uint16_t>(~len);
        z.push_back(static_cast<uint8_t>(len)); z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(nlen)); z.push_back(static_cast<uint8_t>(nlen >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

std::string sheetKeyFromPath(std::string_view path) {
    std::string p(path);
    for (char& c : p) c = c == kBackslash ? kSlash : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char kDir[] = "data/art/gui/";
    const auto at = p.find(kDir);
    if (at == std::string::npos) return {};
    std::string name = p.substr(at + sizeof(kDir) - 1);
    if (name.empty() || name.find('/') != std::string::npos) return {};
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

bool nineSliceQuads(int srcW, int srcH, const std::array<int, 4>& in, float dx, float dy, float dw, float dh,
                    std::array<Quad, 9>& out, float cornerScaleX, float cornerScaleY) {
    const int l = in[0], t = in[1], r = in[2], b = in[3];
    if (l < 0 || t < 0 || r < 0 || b < 0 || l + r >= srcW || t + b >= srcH) return false;
    if (cornerScaleX <= 0.f || cornerScaleY <= 0.f) return false;
    if (dw < static_cast<float>(l + r) * cornerScaleX || dh < static_cast<float>(t + b) * cornerScaleY) return false;
    const auto f = [](int v) { return static_cast<float>(v); };
    const float sx[4] = {0.f, f(l), f(srcW - r), f(srcW)};
    const float sy[4] = {0.f, f(t), f(srcH - b), f(srcH)};
    const float ox[4] = {dx, dx + f(l) * cornerScaleX, dx + dw - f(r) * cornerScaleX, dx + dw};
    const float oy[4] = {dy, dy + f(t) * cornerScaleY, dy + dh - f(b) * cornerScaleY, dy + dh};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row * 3 + col] = {ox[col], oy[row], ox[col + 1], oy[row + 1], sx[col], sy[row], sx[col + 1], sy[row + 1]};
    return true;
}

}  // namespace mc2::ui_chrome
