#pragma once
// move_recon.h — MC2_MOVE_RECON per-frame + atexit pathfinding cost instrumentation.
// Default OFF (zero behavior change when MC2_MOVE_RECON is unset).
// State lives in mclib so both mclib (move.h inlines) and code/ (mover/mech/warrior/objmgr)
// can reference it without creating a dependency in the wrong direction.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include "move_recon_stats.h"

// Master enable flag — set once from the MC2_MOVE_RECON / *_SHADOW gates on first moveReconFrameTick().

// Monotonic (lifetime) accumulators — never reset.
extern uint64_t g_moveRecon_ctrl_ns;
extern uint64_t g_moveRecon_pathlock_ns;

// Monotonic counters.
extern uint64_t g_moveRecon_movers_total;
extern uint64_t g_moveRecon_calcMovePath_calls;
extern uint64_t g_moveRecon_astar_local_calls;
extern uint64_t g_moveRecon_astar_global_calls;
extern uint64_t g_moveRecon_setPathlock_calls;
extern uint64_t g_moveRecon_getPathlock_calls;
extern uint64_t g_moveRecon_offmap_noops;
extern uint64_t g_moveRecon_frames;

// Global-A* (GlobalMap::calcPath) internal split — sizes the per-call FIXED
// reinit cost (door-clear loop O(numDoors) + gate-scan loop O(numAreas)) vs the
// actual A* search (the while loop over openHeap). Hypothesis: dense-urban prologue dominates.
extern uint64_t g_moveRecon_ag_prologue_ns;   // door-clear + gate-scan reinit
extern uint64_t g_moveRecon_ag_search_ns;     // the while(!openHeap.isEmpty())
extern uint64_t g_moveRecon_ag_nodesPopped;   // total A* nodes expanded
extern uint64_t g_moveRecon_ag_gatesScanned;  // total gate-areas open/closed
extern uint64_t g_moveRecon_ag_maxNumDoors;   // map size (last/max seen)
extern uint64_t g_moveRecon_ag_maxNumAreas;
extern uint64_t g_moveRecon_ag_max_prologue_ns; // worst single-call prologue
extern uint64_t g_moveRecon_ag_max_search_ns;   // worst single-call search

// Local-A* (MoveMap::calcPath) node-expansion size. This is the layer the
// Warrior.Path 2ms self-time actually lives in: MoveMap::calcPath has NO Tracy
// zone of its own, so its per-node loop folds into GameLogic.Warrior.Path self
// time; only calcHPrime is carved out as the CalcPath3 child. CalcPath3 xN ==
// N nodes expanded by ONE local search. Sizes deep-search vs many-calls.
extern uint64_t g_moveRecon_al_nodesPopped;   // total local nodes expanded
extern uint64_t g_moveRecon_al_maxNodes;      // worst single-call node count
extern uint64_t g_moveRecon_al_goalFound;     // local searches that found goal
extern uint64_t g_moveRecon_al_goalMissed;    // local searches that flooded to cutoff

// MOVE-CHUNK-PATH SHADOW (recon only, no behavior change). For each expensive
// local A* call, tests the HPA* premise: does the final cell path stay inside a
// coarse chunk corridor? Cheap rectangular proxy first (no chunk graph yet):
// compares the popped-cell bbox vs the final-path bbox (over-exploration ratio)
// and measures how many chunks the path strays beyond the start<->goal chunk
// rectangle (overflow). overflow==0 => a trivial rectangle corridor already
// contains the path; large overflow => a real chunk A* corridor is needed.
// Gated on g_moveReconEnabled; threshold-filtered to the expensive tail.
extern uint64_t g_chunkShadow_calls;       // expensive local calls sampled
extern uint64_t g_chunkShadow_inside0;     // path within start-goal chunk rect
extern uint64_t g_chunkShadow_inside1;     // ... within rect +/-1 chunk
extern uint64_t g_chunkShadow_inside2;     // ... within rect +/-2 chunks
extern uint64_t g_chunkShadow_sum_poppedBox; // sum of popped-bbox cell areas
extern uint64_t g_chunkShadow_sum_pathBox;   // sum of path-bbox cell areas
extern uint64_t g_chunkShadow_sum_nodes;     // sum nodes popped (expensive only)
extern uint64_t g_chunkShadow_sum_pathLen;   // sum path lengths
extern uint64_t g_chunkShadow_maxOverflow;   // worst single-call chunk overflow
extern uint64_t g_chunkShadow_worstNodes;    // node count of the worst (max-node) call
extern uint64_t g_chunkShadow_worstOverflow; // overflow of that worst-node call

// RECT_CORRIDOR_SHADOW: provable constrained-A* projection without re-running A*.
// By A* optimality, a corridor that contains the full optimal path yields the
// SAME path; nodes the constrained search would expand are a subset of the full
// search's popped nodes that lie inside the corridor. So "same" == (overflow<=N)
// and constrained-nodes <= popped-in-corridor (an upper bound). No re-run, no
// map-state corruption risk.
extern uint64_t g_rect_full_nodes;   // sum popped, expensive calls (== sum_nodes)
extern uint64_t g_rect0_nodes;       // sum popped INSIDE start-goal chunk rect +/-0
extern uint64_t g_rect1_nodes;       // ... +/-1 chunk
extern uint64_t g_rect2_nodes;       // ... +/-2 chunks
extern uint64_t g_rect_maxReductPct1;// best single-call node reduction % at pad1

// Sample one local-A* call. No-op when off or nodes below threshold. Coords are
// LOCAL move-map cell indices (consistent within a call). pathLen<=0 = goal
// missed (no path; path bbox ignored). inRect0/1/2 = popped nodes that fell
// inside the start-goal chunk rectangle dilated by 0/1/2 chunks.
void moveReconChunkSample(int startR, int startC, int goalR, int goalC,
                          unsigned long long nodes, int pathLen,
                          int poppedMinR, int poppedMaxR, int poppedMinC, int poppedMaxC,
                          int pathMinR, int pathMaxR, int pathMinC, int pathMaxC,
                          unsigned long long inRect0, unsigned long long inRect1,
                          unsigned long long inRect2);

// Per-frame accumulators (reset each frame by moveReconFrameTick).
extern uint64_t g_moveRecon_frame_ctrl_ns;
extern uint64_t g_moveRecon_frame_pathlock_ns;
extern uint64_t g_moveRecon_frame_astar_local_ns;
extern uint64_t g_moveRecon_frame_astar_global_ns;
extern uint64_t g_moveRecon_frame_movers;

// Per-frame maxima (rolling maximum across all frames, never reset).
extern uint64_t g_moveRecon_max_ctrl_ns;
extern uint64_t g_moveRecon_max_pathlock_ns;
extern uint64_t g_moveRecon_max_astar_local_ns;

// Called once per frame at end of GameObjectManager::update.
// Lazily inits, registers atexit on first call, emits every 30 frames, resets per-frame accums.
void moveReconFrameTick();

// atexit handler — prints shutdown summary to stdout.
void moveReconEmit();

// SAME-QUERY-CACHE SHADOW (recon only, no behavior change). The expensive wave
// is the SAME local path recomputed 5-6x (lance replan burst). This measures the
// would-be cache hit-rate + nodes saved, and probes correctness: whether the
// MOVER_HERE (dynamic occupancy) signature along the path DIVERGED between two
// same-key calls -- if it never diverges, a (start,goal,passGen)-keyed cache is
// safe; if it does, dynamic occupancy must be in the key (or paths that touched
// it excluded). Never returns a cached result. Threshold-filtered to the tail.
extern uint64_t g_pcache_calls;             // expensive local calls seen
extern uint64_t g_pcache_would_hits;        // key matched a prior entry
extern uint64_t g_pcache_nodes_total;       // sum nodes (expensive)
extern uint64_t g_pcache_nodes_saved;       // sum nodes a hit would have skipped
extern uint64_t g_pcache_max_burst_saved;   // most nodes saved in one key-burst
extern uint64_t g_pcache_same_path_hits;    // hit AND identical path bytes
extern uint64_t g_pcache_path_mismatch;     // hit BUT path differed (same key!)
extern uint64_t g_pcache_mover_here_touched;// hits where path crossed MOVER_HERE
extern uint64_t g_pcache_mover_here_diverged;// hits where MOVER_HERE sig changed
extern uint64_t g_pcache_max_frame_gap;     // worst staleness (frames) of a hit

// Sample one finished local search for the cache shadow. pathHash = FNV of path
// cells; moverHereHash = FNV of (cell, MOVER_HERE bit) along the path; frame =
// g_moveRecon_frames. No-op when off / below threshold / no path (pathLen<=0).
void moveReconPathCacheSample(int startR, int startC, int goalR, int goalC,
                             int pathLen, unsigned long long nodes,
                             unsigned long long pathHash, unsigned long long moverHereHash,
                             unsigned long long frame);

// Return-safe per-call local-A* node counter. Increment .n per node popped;
// on destruction folds the count into the totals + rolling max. No-op when off.
struct MoveReconNodeCounter {
    uint64_t n = 0;
    bool found = false;
    ~MoveReconNodeCounter()
    {
        if (g_moveReconEnabled) {
            g_moveRecon_al_nodesPopped += n;
            g_moveRecon_al_maxNodes = std::max(g_moveRecon_al_maxNodes, n);
            if (found) g_moveRecon_al_goalFound++;
            else       g_moveRecon_al_goalMissed++;
        }
    }
};

// RAII scope timer — no-op when !g_moveReconEnabled.
struct MoveReconScope {
    uint64_t* acc_mono;
    uint64_t* acc_frame;
    std::chrono::steady_clock::time_point t0;

    MoveReconScope(uint64_t* mono, uint64_t* frame)
        : acc_mono(mono), acc_frame(frame)
    {
        if (g_moveReconEnabled)
            t0 = std::chrono::steady_clock::now();
    }

    ~MoveReconScope()
    {
        if (g_moveReconEnabled) {
            auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            *acc_mono  += ns;
            *acc_frame += ns;
        }
    }
};
