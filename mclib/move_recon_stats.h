#pragma once
#include <cstdint>

// Path timing totals can be reported by simulation fixtures without importing
// the wall-clock timer implementation into simulation translation units.
extern bool g_moveReconEnabled;
extern uint64_t g_moveRecon_astar_local_ns;
extern uint64_t g_moveRecon_astar_global_ns;
extern uint64_t g_moveRecon_max_astar_global_ns;
