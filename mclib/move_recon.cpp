// move_recon.cpp — MC2_MOVE_RECON pathfinding cost instrumentation implementation.
// Default OFF. Zero behavior change when MC2_MOVE_RECON is unset.

#include "move_recon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Gates/gate.h"

// -----------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------

bool     g_moveReconEnabled        = false;

uint64_t g_moveRecon_ctrl_ns             = 0;
uint64_t g_moveRecon_pathlock_ns         = 0;
uint64_t g_moveRecon_astar_local_ns      = 0;
uint64_t g_moveRecon_astar_global_ns     = 0;

uint64_t g_moveRecon_movers_total        = 0;
uint64_t g_moveRecon_calcMovePath_calls  = 0;
uint64_t g_moveRecon_astar_local_calls   = 0;
uint64_t g_moveRecon_astar_global_calls  = 0;
uint64_t g_moveRecon_setPathlock_calls   = 0;
uint64_t g_moveRecon_getPathlock_calls   = 0;
uint64_t g_moveRecon_offmap_noops        = 0;
uint64_t g_moveRecon_frames              = 0;

uint64_t g_moveRecon_frame_ctrl_ns          = 0;
uint64_t g_moveRecon_frame_pathlock_ns      = 0;
uint64_t g_moveRecon_frame_astar_local_ns   = 0;
uint64_t g_moveRecon_frame_astar_global_ns  = 0;
uint64_t g_moveRecon_frame_movers           = 0;

uint64_t g_moveRecon_max_ctrl_ns            = 0;
uint64_t g_moveRecon_max_pathlock_ns        = 0;
uint64_t g_moveRecon_max_astar_local_ns     = 0;
uint64_t g_moveRecon_max_astar_global_ns    = 0;

uint64_t g_moveRecon_ag_prologue_ns         = 0;
uint64_t g_moveRecon_ag_search_ns           = 0;
uint64_t g_moveRecon_ag_nodesPopped         = 0;
uint64_t g_moveRecon_ag_gatesScanned        = 0;
uint64_t g_moveRecon_ag_maxNumDoors         = 0;
uint64_t g_moveRecon_ag_maxNumAreas         = 0;
uint64_t g_moveRecon_ag_max_prologue_ns     = 0;
uint64_t g_moveRecon_ag_max_search_ns       = 0;

uint64_t g_moveRecon_al_nodesPopped         = 0;
uint64_t g_moveRecon_al_maxNodes            = 0;
uint64_t g_moveRecon_al_goalFound           = 0;
uint64_t g_moveRecon_al_goalMissed          = 0;

uint64_t g_chunkShadow_calls          = 0;
uint64_t g_chunkShadow_inside0        = 0;
uint64_t g_chunkShadow_inside1        = 0;
uint64_t g_chunkShadow_inside2        = 0;
uint64_t g_chunkShadow_sum_poppedBox  = 0;
uint64_t g_chunkShadow_sum_pathBox    = 0;
uint64_t g_chunkShadow_sum_nodes      = 0;
uint64_t g_chunkShadow_sum_pathLen    = 0;
uint64_t g_chunkShadow_maxOverflow    = 0;
uint64_t g_chunkShadow_worstNodes     = 0;
uint64_t g_chunkShadow_worstOverflow  = 0;

uint64_t g_rect_full_nodes    = 0;
uint64_t g_rect0_nodes        = 0;
uint64_t g_rect1_nodes        = 0;
uint64_t g_rect2_nodes        = 0;
uint64_t g_rect_maxReductPct1 = 0;

uint64_t g_pcache_calls               = 0;
uint64_t g_pcache_would_hits          = 0;
uint64_t g_pcache_nodes_total         = 0;
uint64_t g_pcache_nodes_saved         = 0;
uint64_t g_pcache_max_burst_saved     = 0;
uint64_t g_pcache_same_path_hits      = 0;
uint64_t g_pcache_path_mismatch       = 0;
uint64_t g_pcache_mover_here_touched  = 0;
uint64_t g_pcache_mover_here_diverged = 0;
uint64_t g_pcache_max_frame_gap       = 0;

// Threshold: only sample the expensive tail (the 2ms Warrior.Path calls).
static const int CHUNK_SHADOW_MIN_NODES = 200;
static const int CHUNK_SHADOW_CHUNK_SIZE = 16;
static int s_chunkShadowSamplePrints = 0;  // cap verbose per-call lines

// Recent finished-search cache (key = start+goal cells). Exact key compare;
// hash only summarizes the path for divergence checks.
namespace {
    struct PCacheEntry {
        int sr, sc, gr, gc;
        uint64_t pathHash, moverHereHash, nodes, frame;
        bool valid;
    };
    static const int PCACHE_SIZE = 128;
    static PCacheEntry s_pcache[PCACHE_SIZE];
    // Burst tracking: consecutive same-key calls.
    static int s_pcLastSr = -1, s_pcLastSc = -1, s_pcLastGr = -1, s_pcLastGc = -1;
    static uint64_t s_pcBurstSaved = 0;
    static int s_pcBurstPrints = 0;
}

void moveReconPathCacheSample(int startR, int startC, int goalR, int goalC,
                             int pathLen, unsigned long long nodes,
                             unsigned long long pathHash, unsigned long long moverHereHash,
                             unsigned long long frame)
{
    if (!g_moveReconEnabled || nodes < (unsigned long long)CHUNK_SHADOW_MIN_NODES || pathLen <= 0)
        return;

    g_pcache_calls++;
    g_pcache_nodes_total += nodes;

    // Lookup exact (start,goal) key.
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < PCACHE_SIZE; i++) {
        if (!s_pcache[i].valid) { if (freeSlot < 0) freeSlot = i; continue; }
        if (s_pcache[i].sr == startR && s_pcache[i].sc == startC &&
            s_pcache[i].gr == goalR && s_pcache[i].gc == goalC) { slot = i; break; }
    }

    bool sameKeyAsLast = (startR == s_pcLastSr && startC == s_pcLastSc &&
                          goalR == s_pcLastGr && goalC == s_pcLastGc);

    if (slot >= 0) {
        // Would-be cache hit.
        g_pcache_would_hits++;
        g_pcache_nodes_saved += nodes;  // a hit skips THIS search's work
        g_pcache_mover_here_touched++;  // path always crosses cells we read occupancy on
        if (pathHash == s_pcache[slot].pathHash) g_pcache_same_path_hits++;
        else                                     g_pcache_path_mismatch++;
        if (moverHereHash != s_pcache[slot].moverHereHash) g_pcache_mover_here_diverged++;
        uint64_t gap = (frame >= s_pcache[slot].frame) ? (frame - s_pcache[slot].frame) : 0;
        if (gap > g_pcache_max_frame_gap) g_pcache_max_frame_gap = gap;

        if (sameKeyAsLast) {
            s_pcBurstSaved += nodes;
            if (s_pcBurstSaved > g_pcache_max_burst_saved) g_pcache_max_burst_saved = s_pcBurstSaved;
        } else {
            s_pcBurstSaved = nodes;
        }
        if (s_pcBurstPrints < 30) {
            s_pcBurstPrints++;
            std::printf("[MOVE_PATH_CACHE_BURST v1] frame=%llu key=(%d,%d)->(%d,%d) "
                "nodes_repeated=%llu would_saved=%llu path_match=%d mover_diverged=%d frame_gap=%llu\n",
                frame, startR, startC, goalR, goalC, nodes, (unsigned long long)s_pcBurstSaved,
                (pathHash == s_pcache[slot].pathHash) ? 1 : 0,
                (moverHereHash != s_pcache[slot].moverHereHash) ? 1 : 0, (unsigned long long)gap);
            std::fflush(stdout);
        }
    } else {
        s_pcBurstSaved = 0;  // miss resets the burst
        if (slot < 0 && freeSlot < 0) freeSlot = (int)(g_pcache_calls % PCACHE_SIZE);  // evict
        slot = freeSlot;
    }

    // Insert / refresh entry.
    s_pcache[slot].sr = startR; s_pcache[slot].sc = startC;
    s_pcache[slot].gr = goalR;  s_pcache[slot].gc = goalC;
    s_pcache[slot].pathHash = pathHash;
    s_pcache[slot].moverHereHash = moverHereHash;
    s_pcache[slot].nodes = nodes;
    s_pcache[slot].frame = frame;
    s_pcache[slot].valid = true;

    s_pcLastSr = startR; s_pcLastSc = startC; s_pcLastGr = goalR; s_pcLastGc = goalC;
}

void moveReconChunkSample(int startR, int startC, int goalR, int goalC,
                          unsigned long long nodes, int pathLen,
                          int poppedMinR, int poppedMaxR, int poppedMinC, int poppedMaxC,
                          int pathMinR, int pathMaxR, int pathMinC, int pathMaxC,
                          unsigned long long inRect0, unsigned long long inRect1,
                          unsigned long long inRect2)
{
    if (!g_moveReconEnabled || nodes < (unsigned long long)CHUNK_SHADOW_MIN_NODES)
        return;

    const int cs = CHUNK_SHADOW_CHUNK_SIZE;
    uint64_t poppedBox = (uint64_t)(poppedMaxR - poppedMinR + 1) * (uint64_t)(poppedMaxC - poppedMinC + 1);

    int overflow = -1;          // -1 = goal missed (no path)
    uint64_t pathBox = 0;
    if (pathLen > 0) {
        pathBox = (uint64_t)(pathMaxR - pathMinR + 1) * (uint64_t)(pathMaxC - pathMinC + 1);
        // Start<->goal chunk rectangle.
        int sgMinR = (startR < goalR ? startR : goalR) / cs;
        int sgMaxR = (startR > goalR ? startR : goalR) / cs;
        int sgMinC = (startC < goalC ? startC : goalC) / cs;
        int sgMaxC = (startC > goalC ? startC : goalC) / cs;
        // Final-path chunk bbox.
        int pMinR = pathMinR / cs, pMaxR = pathMaxR / cs;
        int pMinC = pathMinC / cs, pMaxC = pathMaxC / cs;
        // Chunks the path strays beyond the start-goal rectangle (any side).
        int ofR0 = sgMinR - pMinR, ofR1 = pMaxR - sgMaxR;
        int ofC0 = sgMinC - pMinC, ofC1 = pMaxC - sgMaxC;
        overflow = 0;
        if (ofR0 > overflow) overflow = ofR0;
        if (ofR1 > overflow) overflow = ofR1;
        if (ofC0 > overflow) overflow = ofC0;
        if (ofC1 > overflow) overflow = ofC1;
    }

    g_chunkShadow_calls++;
    g_chunkShadow_sum_poppedBox += poppedBox;
    g_chunkShadow_sum_pathBox   += pathBox;
    g_chunkShadow_sum_nodes     += nodes;
    g_chunkShadow_sum_pathLen   += (pathLen > 0 ? (uint64_t)pathLen : 0);
    if (overflow >= 0) {
        if (overflow == 0) g_chunkShadow_inside0++;
        if (overflow <= 1) g_chunkShadow_inside1++;
        if (overflow <= 2) g_chunkShadow_inside2++;
        if ((uint64_t)overflow > g_chunkShadow_maxOverflow) g_chunkShadow_maxOverflow = (uint64_t)overflow;
    }
    // Track the worst (max-node) call and its overflow.
    if (nodes > g_chunkShadow_worstNodes) {
        g_chunkShadow_worstNodes = nodes;
        g_chunkShadow_worstOverflow = (overflow >= 0 ? (uint64_t)overflow : 0);
    }

    // RECT_CORRIDOR_SHADOW projection: constrained nodes <= popped-in-corridor.
    g_rect_full_nodes += nodes;
    g_rect0_nodes += inRect0;
    g_rect1_nodes += inRect1;
    g_rect2_nodes += inRect2;
    if (nodes > 0) {
        uint64_t reduct1 = (nodes > inRect1) ? ((nodes - inRect1) * 100 / nodes) : 0;
        if (reduct1 > g_rect_maxReductPct1) g_rect_maxReductPct1 = reduct1;
    }

    if (s_chunkShadowSamplePrints < 40) {
        s_chunkShadowSamplePrints++;
        // found/same at pad N is GUARANTEED iff overflow<=N (A* optimality on a
        // domain containing the optimal path). nodes_padN = popped-in-corridor.
        std::printf("[MOVE_CHUNK_SHADOW v1] call=%llu start=(%d,%d) goal=(%d,%d) "
            "cell_nodes=%llu cell_path_len=%d popped_box=%llu path_box=%llu "
            "chunk_size=%d overflow_chunks=%d "
            "rect0_same=%d rect0_nodes=%llu rect1_same=%d rect1_nodes=%llu "
            "rect2_same=%d rect2_nodes=%llu\n",
            (unsigned long long)g_chunkShadow_calls, startR, startC, goalR, goalC,
            nodes, pathLen, (unsigned long long)poppedBox, (unsigned long long)pathBox,
            cs, overflow,
            (overflow >= 0 && overflow <= 0) ? 1 : 0, (unsigned long long)inRect0,
            (overflow >= 0 && overflow <= 1) ? 1 : 0, (unsigned long long)inRect1,
            (overflow >= 0 && overflow <= 2) ? 1 : 0, (unsigned long long)inRect2);
        std::fflush(stdout);
    }
}

// -----------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------

static bool s_initialized   = false;
static bool s_atexitRegistered = false;

// Forward-declared here; GameMap/GlobalMoveMap types are in move.h which
// is not included from this file to keep includes minimal.  We obtain the
// dimensions via weak external symbols set by moveReconFrameTick callers.
int g_moveRecon_gameMapW = 0;
int g_moveRecon_gameMapH = 0;

// -----------------------------------------------------------------------
// moveReconEmit — atexit handler
// -----------------------------------------------------------------------

void moveReconEmit()
{
    if (!g_moveReconEnabled)
        return;

    std::printf(
        "[MOVE_RECON v1] event=shutdown"
        " frames=%llu"
        " movers_total=%llu"
        " ctrl_ns=%llu"
        " pathlock_ns=%llu"
        " astar_local_ns=%llu"
        " astar_global_ns=%llu"
        " calcMovePath_calls=%llu"
        " astar_local_calls=%llu"
        " astar_global_calls=%llu"
        " setPathlock_calls=%llu"
        " getPathlock_calls=%llu"
        " offmap_noops=%llu"
        " max_ctrl_ns=%llu"
        " max_pathlock_ns=%llu"
        " max_astar_local_ns=%llu"
        " max_astar_global_ns=%llu"
        " ag_prologue_ns=%llu"
        " ag_search_ns=%llu"
        " ag_nodesPopped=%llu"
        " ag_gatesScanned=%llu"
        " ag_numDoors=%llu"
        " ag_numAreas=%llu"
        " ag_max_prologue_ns=%llu"
        " ag_max_search_ns=%llu"
        " al_nodesPopped=%llu"
        " al_maxNodes=%llu"
        " al_goalFound=%llu"
        " al_goalMissed=%llu\n",
        (unsigned long long)g_moveRecon_frames,
        (unsigned long long)g_moveRecon_movers_total,
        (unsigned long long)g_moveRecon_ctrl_ns,
        (unsigned long long)g_moveRecon_pathlock_ns,
        (unsigned long long)g_moveRecon_astar_local_ns,
        (unsigned long long)g_moveRecon_astar_global_ns,
        (unsigned long long)g_moveRecon_calcMovePath_calls,
        (unsigned long long)g_moveRecon_astar_local_calls,
        (unsigned long long)g_moveRecon_astar_global_calls,
        (unsigned long long)g_moveRecon_setPathlock_calls,
        (unsigned long long)g_moveRecon_getPathlock_calls,
        (unsigned long long)g_moveRecon_offmap_noops,
        (unsigned long long)g_moveRecon_max_ctrl_ns,
        (unsigned long long)g_moveRecon_max_pathlock_ns,
        (unsigned long long)g_moveRecon_max_astar_local_ns,
        (unsigned long long)g_moveRecon_max_astar_global_ns,
        (unsigned long long)g_moveRecon_ag_prologue_ns,
        (unsigned long long)g_moveRecon_ag_search_ns,
        (unsigned long long)g_moveRecon_ag_nodesPopped,
        (unsigned long long)g_moveRecon_ag_gatesScanned,
        (unsigned long long)g_moveRecon_ag_maxNumDoors,
        (unsigned long long)g_moveRecon_ag_maxNumAreas,
        (unsigned long long)g_moveRecon_ag_max_prologue_ns,
        (unsigned long long)g_moveRecon_ag_max_search_ns,
        (unsigned long long)g_moveRecon_al_nodesPopped,
        (unsigned long long)g_moveRecon_al_maxNodes,
        (unsigned long long)g_moveRecon_al_goalFound,
        (unsigned long long)g_moveRecon_al_goalMissed
    );
    std::fflush(stdout);

    std::printf(
        "[MOVE_CHUNK_SHADOW v1] event=shutdown"
        " sampled_calls=%llu"
        " inside0=%llu inside1=%llu inside2=%llu"
        " max_overflow_chunks=%llu"
        " sum_popped_box=%llu sum_path_box=%llu"
        " sum_nodes=%llu sum_path_len=%llu"
        " worst_call_nodes=%llu worst_call_overflow=%llu"
        " (chunk_size=%d node_threshold=%d)\n",
        (unsigned long long)g_chunkShadow_calls,
        (unsigned long long)g_chunkShadow_inside0,
        (unsigned long long)g_chunkShadow_inside1,
        (unsigned long long)g_chunkShadow_inside2,
        (unsigned long long)g_chunkShadow_maxOverflow,
        (unsigned long long)g_chunkShadow_sum_poppedBox,
        (unsigned long long)g_chunkShadow_sum_pathBox,
        (unsigned long long)g_chunkShadow_sum_nodes,
        (unsigned long long)g_chunkShadow_sum_pathLen,
        (unsigned long long)g_chunkShadow_worstNodes,
        (unsigned long long)g_chunkShadow_worstOverflow,
        CHUNK_SHADOW_CHUNK_SIZE, CHUNK_SHADOW_MIN_NODES);
    std::fflush(stdout);

    // RECT_CORRIDOR_SHADOW: provable constrained-A* projection (no re-run).
    // rectN_same == inside(N) (A* optimality); rectN_nodes is an UPPER BOUND on
    // the constrained search's expansions; fallbackN = calls with overflow>N.
    uint64_t fb0 = (g_chunkShadow_calls > g_chunkShadow_inside0) ? (g_chunkShadow_calls - g_chunkShadow_inside0) : 0;
    uint64_t fb1 = (g_chunkShadow_calls > g_chunkShadow_inside1) ? (g_chunkShadow_calls - g_chunkShadow_inside1) : 0;
    uint64_t fb2 = (g_chunkShadow_calls > g_chunkShadow_inside2) ? (g_chunkShadow_calls - g_chunkShadow_inside2) : 0;
    uint64_t red0 = (g_rect_full_nodes > g_rect0_nodes) ? ((g_rect_full_nodes - g_rect0_nodes) * 100 / g_rect_full_nodes) : 0;
    uint64_t red1 = (g_rect_full_nodes > g_rect1_nodes) ? ((g_rect_full_nodes - g_rect1_nodes) * 100 / g_rect_full_nodes) : 0;
    uint64_t red2 = (g_rect_full_nodes > g_rect2_nodes) ? ((g_rect_full_nodes - g_rect2_nodes) * 100 / g_rect_full_nodes) : 0;
    std::printf(
        "[RECT_CORRIDOR_SHADOW v1] event=shutdown"
        " calls=%llu full_nodes=%llu"
        " rect0_same=%llu rect0_nodes=%llu rect0_reduct_pct=%llu fallback0=%llu"
        " rect1_same=%llu rect1_nodes=%llu rect1_reduct_pct=%llu fallback1=%llu"
        " rect2_same=%llu rect2_nodes=%llu rect2_reduct_pct=%llu fallback2=%llu"
        " max_node_reduction_pct_pad1=%llu (nodes are UPPER-BOUND; same is PROVEN)\n",
        (unsigned long long)g_chunkShadow_calls, (unsigned long long)g_rect_full_nodes,
        (unsigned long long)g_chunkShadow_inside0, (unsigned long long)g_rect0_nodes, (unsigned long long)red0, (unsigned long long)fb0,
        (unsigned long long)g_chunkShadow_inside1, (unsigned long long)g_rect1_nodes, (unsigned long long)red1, (unsigned long long)fb1,
        (unsigned long long)g_chunkShadow_inside2, (unsigned long long)g_rect2_nodes, (unsigned long long)red2, (unsigned long long)fb2,
        (unsigned long long)g_rect_maxReductPct1);
    std::fflush(stdout);

    uint64_t hitRate = (g_pcache_calls ? g_pcache_would_hits * 100 / g_pcache_calls : 0);
    std::printf(
        "[MOVE_PATH_CACHE_SHADOW v1] event=shutdown"
        " calls=%llu would_hits=%llu would_hit_rate_pct=%llu"
        " nodes_total=%llu nodes_saved=%llu max_burst_saved=%llu"
        " same_path_hits=%llu path_mismatch=%llu"
        " mover_here_touched=%llu mover_here_diverged=%llu"
        " max_frame_gap=%llu (node_threshold=%d cache_size=%d)\n",
        (unsigned long long)g_pcache_calls,
        (unsigned long long)g_pcache_would_hits,
        (unsigned long long)hitRate,
        (unsigned long long)g_pcache_nodes_total,
        (unsigned long long)g_pcache_nodes_saved,
        (unsigned long long)g_pcache_max_burst_saved,
        (unsigned long long)g_pcache_same_path_hits,
        (unsigned long long)g_pcache_path_mismatch,
        (unsigned long long)g_pcache_mover_here_touched,
        (unsigned long long)g_pcache_mover_here_diverged,
        (unsigned long long)g_pcache_max_frame_gap,
        CHUNK_SHADOW_MIN_NODES, PCACHE_SIZE);
    std::fflush(stdout);
}

// -----------------------------------------------------------------------
// moveReconFrameTick — call once per frame at end of GOM::update
// -----------------------------------------------------------------------

void moveReconFrameTick()
{
    // Lazy init on first call (safe: GOM::update is the guaranteed-run path).
    if (!s_initialized) {
        s_initialized = true;
        g_moveReconEnabled =
            mc2::gate::get<"MC2_MOVE_RECON">() ||
            mc2::gate::get<"MC2_MOVE_CHUNK_SHADOW">() ||       // alt enable
            mc2::gate::get<"MC2_MOVE_PATH_CACHE_SHADOW">();    // alt enable
        if (g_moveReconEnabled && !s_atexitRegistered) {
            std::atexit(moveReconEmit);
            s_atexitRegistered = true;
        }
        if (g_moveReconEnabled) {
            std::printf("[MOVE_RECON v1] init enabled\n");
            std::fflush(stdout);
        }
    }

    if (!g_moveReconEnabled)
        return;

    g_moveRecon_frames++;

    // Update rolling maxima.
    if (g_moveRecon_frame_ctrl_ns         > g_moveRecon_max_ctrl_ns)
        g_moveRecon_max_ctrl_ns         = g_moveRecon_frame_ctrl_ns;
    if (g_moveRecon_frame_pathlock_ns     > g_moveRecon_max_pathlock_ns)
        g_moveRecon_max_pathlock_ns     = g_moveRecon_frame_pathlock_ns;
    if (g_moveRecon_frame_astar_local_ns  > g_moveRecon_max_astar_local_ns)
        g_moveRecon_max_astar_local_ns  = g_moveRecon_frame_astar_local_ns;
    if (g_moveRecon_frame_astar_global_ns > g_moveRecon_max_astar_global_ns)
        g_moveRecon_max_astar_global_ns = g_moveRecon_frame_astar_global_ns;

    // Emit every 30 frames.
    if (g_moveRecon_frames % 30 == 0) {
        std::printf(
            "[MOVE_RECON v1] frame=%llu"
            " movers=%llu"
            " ctrl_ns=%llu"
            " pathlock_ns=%llu"
            " astar_local_ns=%llu"
            " astar_global_ns=%llu"
            " calcMovePath_calls=%llu"
            " offmap_noops=%llu\n",
            (unsigned long long)g_moveRecon_frames,
            (unsigned long long)g_moveRecon_frame_movers,
            (unsigned long long)g_moveRecon_frame_ctrl_ns,
            (unsigned long long)g_moveRecon_frame_pathlock_ns,
            (unsigned long long)g_moveRecon_frame_astar_local_ns,
            (unsigned long long)g_moveRecon_frame_astar_global_ns,
            (unsigned long long)g_moveRecon_calcMovePath_calls,
            (unsigned long long)g_moveRecon_offmap_noops
        );
        std::fflush(stdout);
    }

    // Reset per-frame accumulators.
    g_moveRecon_frame_ctrl_ns         = 0;
    g_moveRecon_frame_pathlock_ns     = 0;
    g_moveRecon_frame_astar_local_ns  = 0;
    g_moveRecon_frame_astar_global_ns = 0;
    g_moveRecon_frame_movers          = 0;
}
