// BRAIN-DECISION-INTENT-QUEUE-1: stub for getBrainTickIndex().
// In the full engine this is defined in warrior.cpp (static var accessor).
// The harness never enables MC2_BRAIN_INTENT_QUEUE so emitBrainIntent is never
// called, but the linker still needs the symbol resolved.
#include <cstdint>
uint32_t getBrainTickIndex() { return 0; }

// BRAINSPECIAL-FLOW-WAIT-1: settable brain-time stub for WAIT deadline tests.
// Engine definition (warrior.cpp) reads scenarioTime*1000; the harness advances
// this value between flow-sequential passes via harnessSetBrainTimeMs().
static uint32_t g_stubBrainTimeMs = 0;
uint32_t getBrainTimeMs() { return g_stubBrainTimeMs; }
void harnessSetBrainTimeMs(uint32_t ms) { g_stubBrainTimeMs = ms; }
