// Gates/render_options.h -- the player-facing render options that live in options.cfg.
//
// WHAT: one struct of values, one table that names each options.cfg key with its type, default and bounds, and the validation shared by the
// two ways a value arrives (the options.cfg loader in CPrefs, and the MC2_OPTIONS_OVERRIDE test channel).
// WHY: these were environment gates that a launcher checkbox set for a player. A player setting belongs in the file the game already owns,
// read once at startup, with a bad value rejected loudly instead of silently becoming something else. Header-only so the engine, the
// mclib readers, the editor and the unit tests see the same table with no link dependency; it never logs (the caller owns the log line).
#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mc2::options {

// Defaults here ARE the shipped defaults: a missing key, a missing file, and a tool that never loads options.cfg all behave alike.
struct RenderOptions {
    bool remasteredBuildings = true; // bundled building art; Classic leaves campaign/mod assets intact
    // Post-process look.
    bool  postCas          = false;   // contrast-adaptive sharpening on the final image
    float postCasSharpness = 0.5f;
    int   tonemapMode      = 0;       // 0 off, 1 AgX, 2 Hable, 3 Reinhard
    int   spatialAA        = 1;       // 0 off, 1 SMAA 1x, 2 FXAA, 3 CMAA2; no history or jitter

    // Shadows and terrain lighting.
    int shadowQuality = 2; // 0 Potato, 1 Medium, 2 High
    int baseQuality = 2;   // 0 Potato, 1 Low, 2 Medium/High; bandwidth/ALU budget
    bool shorelineLegacySuppress = true;    // suppress the legacy shoreline post pass
    bool cpuPhongRetire          = true;    // gameplay elevation follows the drawn terrain surface

    // Static prop lighting.
    bool  staticPropAmbient           = true;    // hemisphere ambient fill
    bool  staticPropIblSh             = true;    // SH-L2 image-based ambient
    bool  staticPropIblSunRotate      = false;   // aim the IBL probe at the sun
    float staticPropIblSunRotateFloor = 0.45f;   // probe floor, 0 = bare
    bool  staticPropPbr               = false;   // Schlick-Fresnel specular (OFF: most props carry no PBR data, the fallback sheen reads as a glint)

    // Sun direction.
    bool sunAxisFix      = false;   // mech/vehicle diffuse axis
    bool sunElevationFix = false;   // whole-world authored elevation

    // Particles.
    bool vfxSoftParticles = false;  // depth-fade at scene intersections
    bool vfxLitParticles  = false;  // scene-lit smoke and dust

    // Weapon impact decals (docs/impact-damage-design.md): 0 off (the base-game look), 1 low (96 decals, no unit scars),
    // 2 full (256 decals).
    int impactDecals = 2;

    bool operator==(const RenderOptions&) const = default;
};

enum class Kind { Bool, Long, Float };

struct Option {
    const char* key;                       // options.cfg key without its type prefix (the file writes "b <key> = ...")
    Kind        kind;
    double      lo, hi;                    // accepted range for Long and Float
    bool  RenderOptions::*b;
    int   RenderOptions::*l;
    float RenderOptions::*f;
};

namespace detail {
constexpr Option boolOpt(const char* k, bool RenderOptions::*m) { return {k, Kind::Bool, 0, 1, m, nullptr, nullptr}; }
constexpr Option longOpt(const char* k, int RenderOptions::*m, long lo, long hi) { return {k, Kind::Long, double(lo), double(hi), nullptr, m, nullptr}; }
constexpr Option floatOpt(const char* k, float RenderOptions::*m, double lo, double hi) { return {k, Kind::Float, lo, hi, nullptr, nullptr, m}; }
}  // namespace detail

inline constexpr Option kOptions[] = {
    detail::boolOpt ("RemasteredBuildings",           &RenderOptions::remasteredBuildings),
    detail::boolOpt ("PostCAS",                       &RenderOptions::postCas),
    detail::floatOpt("PostCASSharpness",              &RenderOptions::postCasSharpness, 0.0, 1.0),
    detail::longOpt ("TonemapMode",                   &RenderOptions::tonemapMode, 0, 3),
    detail::longOpt ("SpatialAA",                     &RenderOptions::spatialAA, 0, 3),
    detail::longOpt ("ShadowQuality", &RenderOptions::shadowQuality, 0, 2),
    detail::longOpt ("BaseQuality", &RenderOptions::baseQuality, 0, 2),
    detail::boolOpt ("ShorelineLegacySuppress",       &RenderOptions::shorelineLegacySuppress),
    detail::boolOpt ("CpuPhongRetire",                &RenderOptions::cpuPhongRetire),
    detail::boolOpt ("StaticPropAmbient",             &RenderOptions::staticPropAmbient),
    detail::boolOpt ("StaticPropIblSh",               &RenderOptions::staticPropIblSh),
    detail::boolOpt ("StaticPropIblSunRotate",        &RenderOptions::staticPropIblSunRotate),
    detail::floatOpt("StaticPropIblSunRotateFloor",   &RenderOptions::staticPropIblSunRotateFloor, 0.0, 2.0),
    detail::boolOpt ("StaticPropPbr",                 &RenderOptions::staticPropPbr),
    detail::boolOpt ("SunAxisFix",                    &RenderOptions::sunAxisFix),
    detail::boolOpt ("SunElevationFix",               &RenderOptions::sunElevationFix),
    detail::boolOpt ("VfxSoftParticles",              &RenderOptions::vfxSoftParticles),
    detail::boolOpt ("VfxLitParticles",               &RenderOptions::vfxLitParticles),
    detail::longOpt ("ImpactDecals",                  &RenderOptions::impactDecals, 0, 2),
};

inline constexpr std::size_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

enum class Verdict { Ok, Garbage, OutOfRange, UnknownKey };

// The ways a typed value reaches the struct; a rejected value leaves the field untouched (the default or the earlier value stays).
inline Verdict setLong(RenderOptions& o, const Option& opt, long v) {
    if (opt.kind != Kind::Long) return Verdict::Garbage;
    if (double(v) < opt.lo || double(v) > opt.hi) return Verdict::OutOfRange;
    o.*(opt.l) = static_cast<int>(v);
    return Verdict::Ok;
}

inline Verdict setFloat(RenderOptions& o, const Option& opt, float v) {
    if (opt.kind != Kind::Float || !std::isfinite(v)) return Verdict::Garbage;
    if (double(v) < opt.lo || double(v) > opt.hi) return Verdict::OutOfRange;
    o.*(opt.f) = v;
    return Verdict::Ok;
}

namespace detail {
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
}  // namespace detail

// Keys are case-insensitive, like the options.cfg reader and the launcher editor.
inline const Option* find(std::string_view key) {
    for (const Option& opt : kOptions)
        if (detail::equalsNoCase(key, opt.key)) return &opt;
    return nullptr;
}

// The one place that branches on an option's kind: calls `f` with the field of `o` that the option names (a bool, int or float, const when `o`
// is), so each caller handles the three kinds with `if constexpr` on the field type instead of keeping its own switch.
template <class Options, class F>
decltype(auto) withValue(Options& o, const Option& opt, F&& f) {
    switch (opt.kind) {
    case Kind::Bool: return f(o.*(opt.b));
    case Kind::Long: return f(o.*(opt.l));
    default:         return f(o.*(opt.f));
    }
}

// Text form of a value (the override channel and options.cfg): booleans are TRUE/FALSE/1/0 in any case, numbers must parse in full.
inline Verdict setFromText(RenderOptions& o, const Option& opt, std::string_view text) {
    text = detail::trim(text);
    return withValue(o, opt, [&](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, bool>) {
            if (detail::equalsNoCase(text, "true") || text == "1")  { field = true;  return Verdict::Ok; }
            if (detail::equalsNoCase(text, "false") || text == "0") { field = false; return Verdict::Ok; }
            return Verdict::Garbage;
        } else {
            T v{};
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size()) return Verdict::Garbage;
            if constexpr (std::is_same_v<T, float>) return setFloat(o, opt, v);
            else return setLong(o, opt, v);
        }
    });
}

// The values as one JSON object keyed by the options.cfg key names ({"PostCAS": false, "PostCASSharpness": 0.5, ...}), floats in their shortest
// round-trip form. The state dump and the capture sidecar carry this so a proof records which options it ran with.
inline std::string toJson(const RenderOptions& o) {
    std::string s = "{";
    for (const Option& opt : kOptions) {
        if (s.size() > 1) s += ", ";
        s += '"';
        s += opt.key;
        s += "\": ";
        withValue(o, opt, [&](const auto& v) {
            using T = std::remove_cvref_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                s += v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int>) {
                s += std::to_string(v);
            } else {
                char buf[32];
                const auto r = std::to_chars(buf, buf + sizeof(buf), v);
                s.append(buf, r.ptr);
            }
        });
    }
    s += "}";
    return s;
}

struct Reject {
    std::string key;
    std::string text;
    Verdict     why;
};

// MC2_OPTIONS_OVERRIDE grammar: "Key=Value,Key=Value" with the options.cfg key names (any case). Used by the smoke matrices and capture drivers to run
// one setting differently without editing the install's options.cfg. Returns the number of accepted entries; each rejected entry (unknown
// key, garbage, out of range) is appended to `rejects` and changes nothing.
inline int applyOverrides(RenderOptions& o, std::string_view spec, std::vector<Reject>& rejects) {
    int accepted = 0;
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view item = detail::trim(spec.substr(0, comma));
        spec = (comma == std::string_view::npos) ? std::string_view{} : spec.substr(comma + 1);
        if (item.empty()) continue;
        const std::size_t eq = item.find('=');
        const std::string_view key = detail::trim(item.substr(0, eq));
        const std::string_view val = (eq == std::string_view::npos) ? std::string_view{} : item.substr(eq + 1);
        const Option* opt = find(key);
        if (!opt) { rejects.push_back({std::string(key), std::string(val), Verdict::UnknownKey}); continue; }
        if (eq == std::string_view::npos) { rejects.push_back({opt->key, std::string(), Verdict::Garbage}); continue; }
        const Verdict v = setFromText(o, *opt, val);
        if (v == Verdict::Ok) ++accepted;
        else rejects.push_back({opt->key, std::string(detail::trim(val)), v});
    }
    return accepted;
}

// The process-wide values the renderer and the mclib readers consult. The game publishes twice with the same inputs: once before the window
// exists (gameosmain_init.cpp, so readers that run during renderer start-up see options.cfg), and again from CPrefs::applyPrefs() once the
// game has loaded and validated the file (CPrefs owns the log lines and the save). A tool that never loads options.cfg keeps the defaults.
// earlyReads() counts reads before the first publish (a reader that cached its value too soon) and publishChangedValues() whether the second
// publish differed from the first; CPrefs logs both, so a reader ordering bug shows up in the engine log instead of as a setting that does
// nothing.
inline RenderOptions g_current{};
inline bool          g_published = false;
inline int           g_earlyReads = 0;
inline const char*   g_firstEarlyFile = "";   // where the first early read came from, for the log line
inline unsigned      g_firstEarlyLine = 0;

inline const RenderOptions& current(std::source_location where = std::source_location::current()) {
    if (!g_published) {
        if (g_earlyReads == 0) { g_firstEarlyFile = where.file_name(); g_firstEarlyLine = where.line(); }
        ++g_earlyReads;
    }
    return g_current;
}
// Legacy launcher Low used ShadowQuality=0. Keep that preset on the lean path too.
inline bool lowCost() { return current().baseQuality < 2 || current().shadowQuality == 0; }
inline bool          g_publishChanged = false;
inline int g_buildingArtCommandLine = -1;

inline bool buildingArtArgument(std::string_view argument) {
    if (argument == "--classic-assets") { g_buildingArtCommandLine = 0; return true; }
    if (argument == "--remastered-assets") { g_buildingArtCommandLine = 1; return true; }
    return false;
}

inline void publish(const RenderOptions& o) {
    auto effective = o;
    if (g_buildingArtCommandLine >= 0) effective.remasteredBuildings = g_buildingArtCommandLine != 0;
    if (g_published && !(g_current == effective)) g_publishChanged = true;
    g_current = effective;
    g_published = true;
}
inline int  earlyReads() { return g_earlyReads; }
inline bool publishChangedValues() { return g_publishChanged; }
// The game owns the preference draft and atomic save; shared graphics UI cannot depend on CPrefs.
inline bool (*saveSpatialAA)(int) = nullptr;
inline const char* firstEarlyFile() { return g_firstEarlyFile; }
inline unsigned    firstEarlyLine() { return g_firstEarlyLine; }

}  // namespace mc2::options
