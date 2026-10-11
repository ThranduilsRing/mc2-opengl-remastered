// TEMPORARY determinism-hunt instrumentation (task 1). Never committed.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Gates/gate.h"
extern "C" unsigned dtRngState();
extern "C" unsigned dtRngCalls();
extern uint32_t g_mc2FrameCounter;
extern float scenarioTime;
inline bool dtOn() {
	static const bool on = mc2::gate::get<"MC2_SIM_IDENTITY_TRACE_DETAIL">();
	return on;
}
#define DT(...) do { if (dtOn()) { std::printf("[DT] f=%u t=%.4f ", (unsigned)g_mc2FrameCounter, (double)scenarioTime); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } } while (0)
