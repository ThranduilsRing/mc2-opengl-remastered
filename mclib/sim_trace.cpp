// The [DT] determinism trace; see sim_trace.h. An empty translation unit when MC2_SIM_TRACE is OFF.
#include "mc2_czstring.h"
#include <atomic>
#include "sim_trace.h"

#if MC2_SIM_TRACE

#include <cstdint>
#include <cstdio>
#if __has_include(<stacktrace>)
#include <stacktrace>
#endif

extern std::atomic<uint32_t> g_mc2FrameCounter;

namespace mc2_sim_trace {

namespace {
double s_time = 0.0;
bool s_basePrinted = false;
}

void prefix()
{
	std::printf("[DT] f=%u t=%.4f ", static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed)), s_time);
}

void setTime(double scenarioTime)
{
	s_time = scenarioTime;
}

void stack(mc2::czstring tag)
{
	if (!s_basePrinted) {
		s_basePrinted = true;
		prefix();
		std::printf("TRACEBASE line=%p\n", reinterpret_cast<void*>(&prefix));
	}
	prefix();
	std::printf("%s stack=", tag);
#if defined(__cpp_lib_stacktrace)
	const auto st = std::stacktrace::current(1, 12);
	for (const auto& e : st)
		std::printf("%llx ", reinterpret_cast<unsigned long long>(e.native_handle()));
#else
	std::printf("(no <stacktrace> in this toolchain)");
#endif
	std::printf("\n");
	std::fflush(stdout);
}

} // namespace mc2_sim_trace

#endif
