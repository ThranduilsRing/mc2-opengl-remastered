#pragma once
// The [DT] determinism trace, a compile-time facility (CMake option MC2_SIM_TRACE, default OFF; no runtime gate).
//
// WHAT: MC2_SIM_TRACE_LINE(...) prints one `[DT] f=<render frame> t=<scenario time> <TAG> ...` line per event at the places a
// determinism hunt reads first: the per-frame random draw counts (RNGF: the simulation, appearance and weather streams), the call stack
// of every simulation draw (RNGCALL), weapon hits and body damage (HIT, INJURE, DESTROYLOC, CTBRANCH), bolt flight (BOLT), path requests
// and calculations (REQ, CALC, CALCDONE), target changes (SETTGT), sensor contacts (SADD, SREM), weather inputs (WX), ABL calls (ABL),
// and camera moves with their call stack (SETPOS, SCROLL). MC2_SIM_TRACE_STACK(tag) prints the caller's return addresses.
// WHY: two runs of one mission that diverge are explained by diffing their [DT] streams: the first frame whose RNGF counts differ names the
// consumer that drew out of order, its RNGCALL stack names the call site, and the item lines show which state moved first. The hunts
// that found the mc2_10 and mc2_24 causes worked this way (docs/sim-trace.md). Compiled out by default so a shipping build pays nothing.
#if MC2_SIM_TRACE

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <format>

namespace mc2_sim_trace {

// Self-contained on purpose: this header sits in sim TUs, so it may not pull GameOS's mc2fmt.h (sim include firewall).
// safe() is mc2::fmt::safe for the trace: a null C string prints "(null)" as printf("%s") did.
inline const char* safe(const char* s) noexcept { return s ? s : "(null)"; }

// The scenario clock the lines carry; the simulation sets it once per sim frame.
void setTime(double scenarioTime);

// One [DT] line; std::format formatting; prefix() adds frame and scenario time.
void prefix();
template <class... Args>
void line(std::format_string<Args...> fmt, Args&&... args)
{
	prefix();
	try {
		const std::string text = std::vformat(fmt.get(), std::make_format_args(args...));
		std::fwrite(text.data(), 1, text.size(), stdout);
	} catch (...) {
	}
	std::printf("\n");
	std::fflush(stdout);
}

// A `[DT] f=.. t=.. <tag> stack=<addr> <addr> ...` line of the caller's return addresses (absolute; the first call also prints the
// address of prefix() as TRACEBASE so a symbolizer can rebase them against the PDB; line() is a template and has no one address).
void stack(const char* tag);

} // namespace mc2_sim_trace

#define MC2_SIM_TRACE_LINE(...) mc2_sim_trace::line(__VA_ARGS__)
#define MC2_SIM_TRACE_STACK(tag) mc2_sim_trace::stack(tag)

#else

#define MC2_SIM_TRACE_LINE(...) ((void)0)
#define MC2_SIM_TRACE_STACK(tag) ((void)0)

#endif
