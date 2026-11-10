// UI-CHROME-SPLIT-1: see ui_chrome_sheets.h.
#include "ui_chrome_sheets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

bool composeSheet(const Sheet& sheet, const WidgetLoader& load, Pixels& out, std::string* err) {
    auto fail = [&](std::string_view why) {
        if (err) *err = std::string(why);
        out = Pixels{};
        return false;
    };
    if (sheet.widgets.empty()) return fail("sheet " + sheet.key + " has no widgets");
    const int ch = sheet.rgba ? 4 : 3;
    out.w = sheet.w;
    out.h = sheet.h;
    out.channels = ch;
    out.data.assign(static_cast<size_t>(sheet.w) * sheet.h * ch, 0);
    if (!sheet.rgba)   // the authored RGB sheets key transparency on FF00FF; uncovered texels stay "no art"
        for (size_t i = 0; i < out.data.size(); i += 3) { out.data[i] = 255; out.data[i + 2] = 255; }
    Pixels px;
    for (const auto& w : sheet.widgets) {
        px = Pixels{};
        if (!load(sheet, w, px) || px.w != w.w || px.h != w.h || px.data.size() != static_cast<size_t>(px.w) * px.h * px.channels ||
            (px.channels != 3 && px.channels != 4))
            return fail("widget " + sheet.key + ": " + w.name + " missing or wrong size");
        for (int y = 0; y < w.h; ++y) {
            uint8_t* dst = out.data.data() + (static_cast<size_t>(w.y + y) * sheet.w + w.x) * ch;
            const uint8_t* src = px.data.data() + static_cast<size_t>(y) * w.w * px.channels;
            for (int x = 0; x < w.w; ++x) {
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
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
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
