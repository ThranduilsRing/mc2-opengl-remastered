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
#include <string>
#include <string_view>
#include <vector>

namespace mc2::options {

// Defaults here ARE the shipped defaults: a missing key, a missing file, and a tool that never loads options.cfg all behave alike.
struct RenderOptions {
    // Post-process look.
    bool  postCas          = false;   // contrast-adaptive sharpening on the final image
    float postCasSharpness = 0.5f;
    int   tonemapMode      = 0;       // 0 off, 1 AgX, 2 Hable, 3 Reinhard

    // Shadows and terrain lighting.
    bool shadowCsm               = true;    // cascaded dynamic shadow maps (off = the single legacy dynamic map)
    bool shadowBoundedNearFit    = true;    // crisp near shadows: bound the near cascade fit radius
    int  propShadowReceive       = 0;       // 1 = static props receive the dynamic cascade shadow
    bool shorelineLegacySuppress = true;    // suppress the legacy shoreline post pass
    bool cpuPhongRetire          = true;    // gameplay elevation follows the drawn terrain surface

    // Static prop lighting.
    bool  staticPropAmbient           = false;   // hemisphere ambient fill
    bool  staticPropIblSh             = true;    // SH-L2 image-based ambient
    bool  staticPropIblSunRotate      = false;   // aim the IBL probe at the sun
    float staticPropIblSunRotateFloor = 0.45f;   // probe floor, 0 = bare
    bool  staticPropPbr               = false;   // Schlick-Fresnel specular

    // Sun direction.
    bool sunAxisFix      = false;   // mech/vehicle diffuse axis
    bool sunElevationFix = false;   // whole-world authored elevation

    // Particles.
    bool vfxSoftParticles = false;  // depth-fade at scene intersections
    bool vfxLitParticles  = false;  // scene-lit smoke and dust
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
    detail::boolOpt ("PostCAS",                       &RenderOptions::postCas),
    detail::floatOpt("PostCASSharpness",              &RenderOptions::postCasSharpness, 0.0, 1.0),
    detail::longOpt ("TonemapMode",                   &RenderOptions::tonemapMode, 0, 3),
    detail::boolOpt ("ShadowCSM",                     &RenderOptions::shadowCsm),
    detail::boolOpt ("ShadowBoundedNearFit",          &RenderOptions::shadowBoundedNearFit),
    detail::longOpt ("PropShadowReceive",             &RenderOptions::propShadowReceive, 0, 1),
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
};

inline constexpr std::size_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

enum class Verdict { Ok, Garbage, OutOfRange };

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

inline const Option* find(std::string_view key) {
    for (const Option& opt : kOptions)
        if (key == opt.key) return &opt;
    return nullptr;
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

// Text form of a value (the override channel): booleans are TRUE/FALSE/1/0 in any case, numbers must parse in full.
inline Verdict setFromText(RenderOptions& o, const Option& opt, std::string_view text) {
    text = detail::trim(text);
    switch (opt.kind) {
    case Kind::Bool:
        if (detail::equalsNoCase(text, "true") || text == "1")  { o.*(opt.b) = true;  return Verdict::Ok; }
        if (detail::equalsNoCase(text, "false") || text == "0") { o.*(opt.b) = false; return Verdict::Ok; }
        return Verdict::Garbage;
    case Kind::Long: {
        long v = 0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
        if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size()) return Verdict::Garbage;
        return setLong(o, opt, v);
    }
    case Kind::Float: {
        float v = 0.0f;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
        if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size()) return Verdict::Garbage;
        return setFloat(o, opt, v);
    }
    }
    return Verdict::Garbage;
}

struct Reject {
    std::string key;
    std::string text;
    Verdict     why;
};

// MC2_OPTIONS_OVERRIDE grammar: "Key=Value,Key=Value" with the options.cfg key names. Used by the smoke matrices and capture drivers to run
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
        const Option* opt = (eq == std::string_view::npos) ? nullptr : find(key);
        if (!opt) { rejects.push_back({std::string(key), std::string(val), Verdict::Garbage}); continue; }
        const Verdict v = setFromText(o, *opt, val);
        if (v == Verdict::Ok) ++accepted;
        else rejects.push_back({std::string(key), std::string(detail::trim(val)), v});
    }
    return accepted;
}

// The process-wide values the renderer and the mclib readers consult. CPrefs::applyPrefs() publishes after options.cfg is loaded; a tool
// that never loads options.cfg keeps the defaults. earlyReads() counts reads that happened before the publish (a reader that cached its
// value too soon); CPrefs logs it so a reader ordering bug shows up in the engine log instead of as a setting that does nothing.
inline RenderOptions g_current{};
inline bool          g_published = false;
inline int           g_earlyReads = 0;

inline const RenderOptions& current() {
    if (!g_published) ++g_earlyReads;
    return g_current;
}
inline void publish(const RenderOptions& o) { g_current = o; g_published = true; }
inline int  earlyReads() { return g_earlyReads; }

}  // namespace mc2::options
