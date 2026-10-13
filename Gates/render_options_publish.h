// Gates/render_options_publish.h -- publish the render options from options.cfg and the MC2_OPTIONS_OVERRIDE test channel, and log a rejected value.
//
// WHAT: the one start-up routine every process that renders runs before its renderer exists: read options.cfg from the working directory, apply the
// override channel on top, publish the result for the readers in render_options.h. Also the one formatter of the `[OPTIONS v1] event=rejected` line.
// WHY: the shader-define and pipeline readers run during renderer start-up, long before the game's CPrefs loads the file, and the mission editor has
// no CPrefs load at all; sharing this keeps the game and the editor reading the same file with the same grammar, and keeps a bad value reported in
// one format wherever it is found. Header-only, no link dependency beyond the gate table and the fmt shim every engine target already has.
#pragma once

#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options_cfg.h"
#include "mc2fmt.h"

#include <string_view>
#include <vector>

namespace mc2::options {

// One log line per rejected value; the default (or the value the source had before) stays.
inline void logRejectedOption(std::string_view key, std::string_view text, Verdict why, const Option* opt) {
    const char* reason = why == Verdict::OutOfRange ? "out_of_range" : why == Verdict::UnknownKey ? "unknown_key" : "garbage";
    if (opt && opt->kind != Kind::Bool)
        mc2::fmt::print_err("[OPTIONS v1] event=rejected key={} value={} why={} want={:g}..{:g} kept=default\n", key, text, reason, opt->lo, opt->hi);
    else
        mc2::fmt::print_err("[OPTIONS v1] event=rejected key={} value={} why={} kept=default\n", key, text, reason);
}

// Publish options.cfg (working directory) then MC2_OPTIONS_OVERRIDE; returns every rejected entry for the caller that owns the log (the game logs
// the file's entries from CPrefs::load, which reads the same file again; the editor logs both).
inline std::vector<Reject> publishFromInstall() {
    RenderOptions options;
    std::vector<Reject> rejects;
    cfg::loadFromText(options, cfg::readTextFile("options.cfg"), &rejects);
    applyOverrides(options, mc2::gate::str<"MC2_OPTIONS_OVERRIDE">(), rejects);
    publish(options);
    return rejects;
}

}  // namespace mc2::options
