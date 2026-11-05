// move_recon.cpp — MC2_MOVE_RECON pathfinding cost instrumentation implementation.
// Default OFF. Zero behavior change when MC2_MOVE_RECON is unset.

#include "move_recon.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Gates/gate.h"
#include "mc2fmt.h"


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
    if (!g_moveReconEnabled || nodes < static_cast<unsigned long long>(CHUNK_SHADOW_MIN_NODES) || pathLen <= 0)
        return;

    g_pcache_calls++;
    g_pcache_nodes_total += nodes;

    int slot = -1, freeSlot = -1;
    for (int i = 0; i < PCACHE_SIZE; i++) {
        if (!s_pcache[i].valid) { if (freeSlot < 0) freeSlot = i; continue; }
        if (s_pcache[i].sr == startR && s_pcache[i].sc == startC &&
            s_pcache[i].gr == goalR && s_pcache[i].gc == goalC) { slot = i; break; }
    }

    bool sameKeyAsLast = (startR == s_pcLastSr && startC == s_pcLastSc &&
                          goalR == s_pcLastGr && goalC == s_pcLastGc);

    if (slot >= 0) {
        g_pcache_would_hits++;
        g_pcache_nodes_saved += nodes;  // a hit skips THIS search's work
        g_pcache_mover_here_touched++;  // path always crosses cells we read occupancy on
        if (pathHash == s_pcache[slot].pathHash) g_pcache_same_path_hits++;
        else                                     g_pcache_path_mismatch++;
        if (moverHereHash != s_pcache[slot].moverHereHash) g_pcache_mover_here_diverged++;
        uint64_t gap = (frame >= s_pcache[slot].frame) ? (frame - s_pcache[slot].frame) : 0;
        g_pcache_max_frame_gap = std::max(g_pcache_max_frame_gap, gap);

        if (sameKeyAsLast) {
            s_pcBurstSaved += nodes;
            g_pcache_max_burst_saved = std::max(g_pcache_max_burst_saved, s_pcBurstSaved);
        } else {
            s_pcBurstSaved = nodes;
        }
        if (s_pcBurstPrints < 30) {
            s_pcBurstPrints++;
            mc2::fmt::print_out("[MOVE_PATH_CACHE_BURST v1] frame={:d} key=({:d},{:d})->({:d},{:d}) nodes_repeated={:d} would_saved={:d} path_match={:d} mover_diverged={:d} frame_gap={:d}\n", mc2::fmt::as_u64(frame), mc2::fmt::as_i32(startR), mc2::fmt::as_i32(startC), mc2::fmt::as_i32(goalR), mc2::fmt::as_i32(goalC), mc2::fmt::as_u64(nodes), mc2::fmt::as_u64(static_cast<unsigned long long>(s_pcBurstSaved)), mc2::fmt::as_i32((pathHash == s_pcache[slot].pathHash) ? 1 : 0), mc2::fmt::as_i32((moverHereHash != s_pcache[slot].moverHereHash) ? 1 : 0), mc2::fmt::as_u64(static_cast<unsigned long long>(gap)));
            std::fflush(stdout);
        }
    } else {
        s_pcBurstSaved = 0;  // miss resets the burst
        if (slot < 0 && freeSlot < 0) freeSlot = static_cast<int>((g_pcache_calls % PCACHE_SIZE));  // evict
        slot = freeSlot;
    }

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
    if (!g_moveReconEnabled || nodes < static_cast<unsigned long long>(CHUNK_SHADOW_MIN_NODES))
        return;

    const int cs = CHUNK_SHADOW_CHUNK_SIZE;
    uint64_t poppedBox = static_cast<uint64_t>((poppedMaxR - poppedMinR + 1)) * static_cast<uint64_t>((poppedMaxC - poppedMinC + 1));

    int overflow = -1;          // -1 = goal missed (no path)
    uint64_t pathBox = 0;
    if (pathLen > 0) {
        pathBox = static_cast<uint64_t>((pathMaxR - pathMinR + 1)) * static_cast<uint64_t>((pathMaxC - pathMinC + 1));
        int sgMinR = (startR < goalR ? startR : goalR) / cs;
        int sgMaxR = (startR > goalR ? startR : goalR) / cs;
        int sgMinC = (startC < goalC ? startC : goalC) / cs;
        int sgMaxC = (startC > goalC ? startC : goalC) / cs;
        int pMinR = pathMinR / cs, pMaxR = pathMaxR / cs;
        int pMinC = pathMinC / cs, pMaxC = pathMaxC / cs;
        // Chunks the path strays beyond the start-goal rectangle (any side).
        int ofR0 = sgMinR - pMinR, ofR1 = pMaxR - sgMaxR;
        int ofC0 = sgMinC - pMinC, ofC1 = pMaxC - sgMaxC;
        overflow = 0;
        overflow = std::max(overflow, ofR0);
        overflow = std::max(overflow, ofR1);
        overflow = std::max(overflow, ofC0);
        overflow = std::max(overflow, ofC1);
    }

    g_chunkShadow_calls++;
    g_chunkShadow_sum_poppedBox += poppedBox;
    g_chunkShadow_sum_pathBox   += pathBox;
    g_chunkShadow_sum_nodes     += nodes;
    g_chunkShadow_sum_pathLen   += (pathLen > 0 ? static_cast<uint64_t>(pathLen) : 0);
    if (overflow >= 0) {
        if (overflow == 0) g_chunkShadow_inside0++;
        if (overflow <= 1) g_chunkShadow_inside1++;
        if (overflow <= 2) g_chunkShadow_inside2++;
        g_chunkShadow_maxOverflow = std::max(g_chunkShadow_maxOverflow, static_cast<uint64_t>(overflow));
    }
    if (nodes > g_chunkShadow_worstNodes) {
        g_chunkShadow_worstOverflow = (overflow >= 0 ? static_cast<uint64_t>(overflow) : 0);
        g_chunkShadow_worstNodes = nodes;
    }

    // RECT_CORRIDOR_SHADOW projection: constrained nodes <= popped-in-corridor.
    g_rect_full_nodes += nodes;
    g_rect0_nodes += inRect0;
    g_rect1_nodes += inRect1;
    g_rect2_nodes += inRect2;
    if (nodes > 0) {
        uint64_t reduct1 = (nodes > inRect1) ? ((nodes - inRect1) * 100 / nodes) : 0;
        g_rect_maxReductPct1 = std::max(g_rect_maxReductPct1, reduct1);
    }

    if (s_chunkShadowSamplePrints < 40) {
        s_chunkShadowSamplePrints++;
        // found/same at pad N is GUARANTEED iff overflow<=N (A* optimality on a
        // domain containing the optimal path). nodes_padN = popped-in-corridor.
        mc2::fmt::print_out("[MOVE_CHUNK_SHADOW v1] call={:d} start=({:d},{:d}) goal=({:d},{:d}) cell_nodes={:d} cell_path_len={:d} popped_box={:d} path_box={:d} chunk_size={:d} overflow_chunks={:d} rect0_same={:d} rect0_nodes={:d} rect1_same={:d} rect1_nodes={:d} rect2_same={:d} rect2_nodes={:d}\n",
            mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_calls)), mc2::fmt::as_i32(startR), mc2::fmt::as_i32(startC), mc2::fmt::as_i32(goalR), mc2::fmt::as_i32(goalC),
            mc2::fmt::as_u64(nodes), mc2::fmt::as_i32(pathLen), mc2::fmt::as_u64(static_cast<unsigned long long>(poppedBox)), mc2::fmt::as_u64(static_cast<unsigned long long>(pathBox)),
            mc2::fmt::as_i32(cs), mc2::fmt::as_i32(overflow),
            mc2::fmt::as_i32((overflow >= 0 && overflow <= 0) ? 1 : 0), mc2::fmt::as_u64(static_cast<unsigned long long>(inRect0)),
            mc2::fmt::as_i32((overflow >= 0 && overflow <= 1) ? 1 : 0), mc2::fmt::as_u64(static_cast<unsigned long long>(inRect1)),
            mc2::fmt::as_i32((overflow >= 0 && overflow <= 2) ? 1 : 0), mc2::fmt::as_u64(static_cast<unsigned long long>(inRect2)));
        std::fflush(stdout);
    }
}


static bool s_initialized   = false;
static bool s_atexitRegistered = false;

// Forward-declared here; GameMap/GlobalMoveMap types are in move.h which
// is not included from this file to keep includes minimal.  We obtain the
// dimensions via weak external symbols set by moveReconFrameTick callers.
int g_moveRecon_gameMapW = 0;
int g_moveRecon_gameMapH = 0;


void moveReconEmit()
{
    if (!g_moveReconEnabled)
        return;

    mc2::fmt::print_out("[MOVE_RECON v1] event=shutdown frames={:d} movers_total={:d} ctrl_ns={:d} pathlock_ns={:d} astar_local_ns={:d} astar_global_ns={:d} calcMovePath_calls={:d} astar_local_calls={:d} astar_global_calls={:d} setPathlock_calls={:d} getPathlock_calls={:d} offmap_noops={:d} max_ctrl_ns={:d} max_pathlock_ns={:d} max_astar_local_ns={:d} max_astar_global_ns={:d} ag_prologue_ns={:d} ag_search_ns={:d} ag_nodesPopped={:d} ag_gatesScanned={:d} ag_numDoors={:d} ag_numAreas={:d} ag_max_prologue_ns={:d} ag_max_search_ns={:d} al_nodesPopped={:d} al_maxNodes={:d} al_goalFound={:d} al_goalMissed={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frames)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_movers_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ctrl_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_pathlock_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_astar_local_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_astar_global_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_calcMovePath_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_astar_local_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_astar_global_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_setPathlock_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_getPathlock_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_offmap_noops)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_max_ctrl_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_max_pathlock_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_max_astar_local_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_max_astar_global_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_prologue_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_search_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_nodesPopped)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_gatesScanned)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_maxNumDoors)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_maxNumAreas)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_max_prologue_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_ag_max_search_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_al_nodesPopped)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_al_maxNodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_al_goalFound)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_al_goalMissed)));
    std::fflush(stdout);

    mc2::fmt::print_out("[MOVE_CHUNK_SHADOW v1] event=shutdown sampled_calls={:d} inside0={:d} inside1={:d} inside2={:d} max_overflow_chunks={:d} sum_popped_box={:d} sum_path_box={:d} sum_nodes={:d} sum_path_len={:d} worst_call_nodes={:d} worst_call_overflow={:d} (chunk_size={:d} node_threshold={:d})\n", mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside0)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside1)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside2)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_maxOverflow)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_sum_poppedBox)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_sum_pathBox)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_sum_nodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_sum_pathLen)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_worstNodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_worstOverflow)), mc2::fmt::as_i32(CHUNK_SHADOW_CHUNK_SIZE), mc2::fmt::as_i32(CHUNK_SHADOW_MIN_NODES));
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
    mc2::fmt::print_out("[RECT_CORRIDOR_SHADOW v1] event=shutdown calls={:d} full_nodes={:d} rect0_same={:d} rect0_nodes={:d} rect0_reduct_pct={:d} fallback0={:d} rect1_same={:d} rect1_nodes={:d} rect1_reduct_pct={:d} fallback1={:d} rect2_same={:d} rect2_nodes={:d} rect2_reduct_pct={:d} fallback2={:d} max_node_reduction_pct_pad1={:d} (nodes are UPPER-BOUND; same is PROVEN)\n", mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_rect_full_nodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside0)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_rect0_nodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(red0)), mc2::fmt::as_u64(static_cast<unsigned long long>(fb0)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside1)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_rect1_nodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(red1)), mc2::fmt::as_u64(static_cast<unsigned long long>(fb1)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_chunkShadow_inside2)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_rect2_nodes)), mc2::fmt::as_u64(static_cast<unsigned long long>(red2)), mc2::fmt::as_u64(static_cast<unsigned long long>(fb2)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_rect_maxReductPct1)));
    std::fflush(stdout);

    uint64_t hitRate = (g_pcache_calls ? g_pcache_would_hits * 100 / g_pcache_calls : 0);
    mc2::fmt::print_out("[MOVE_PATH_CACHE_SHADOW v1] event=shutdown calls={:d} would_hits={:d} would_hit_rate_pct={:d} nodes_total={:d} nodes_saved={:d} max_burst_saved={:d} same_path_hits={:d} path_mismatch={:d} mover_here_touched={:d} mover_here_diverged={:d} max_frame_gap={:d} (node_threshold={:d} cache_size={:d})\n", mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_would_hits)), mc2::fmt::as_u64(static_cast<unsigned long long>(hitRate)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_nodes_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_nodes_saved)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_max_burst_saved)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_same_path_hits)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_path_mismatch)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_mover_here_touched)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_mover_here_diverged)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_pcache_max_frame_gap)), mc2::fmt::as_i32(CHUNK_SHADOW_MIN_NODES), mc2::fmt::as_i32(PCACHE_SIZE));
    std::fflush(stdout);
}


void moveReconFrameTick()
{
    // Lazy init on first call (safe: GOM::update is the guaranteed-run path).
    if (!s_initialized) {
        s_initialized = true;
        g_moveReconEnabled =
            (false && mc2::gate::get<"MC2_DIAG_VERBOSE">()) ||
            false ||       // alt enable
            false;    // alt enable
        if (g_moveReconEnabled && !s_atexitRegistered) {
            std::atexit(moveReconEmit);
            s_atexitRegistered = true;
        }
        if (g_moveReconEnabled) {
            mc2::fmt::print_out("[MOVE_RECON v1] init enabled\n");
            std::fflush(stdout);
        }
    }

    if (!g_moveReconEnabled)
        return;

    g_moveRecon_frames++;

    g_moveRecon_max_ctrl_ns = std::max(g_moveRecon_max_ctrl_ns, g_moveRecon_frame_ctrl_ns);
    g_moveRecon_max_pathlock_ns = std::max(g_moveRecon_max_pathlock_ns, g_moveRecon_frame_pathlock_ns);
    g_moveRecon_max_astar_local_ns = std::max(g_moveRecon_max_astar_local_ns, g_moveRecon_frame_astar_local_ns);
    g_moveRecon_max_astar_global_ns = std::max(g_moveRecon_max_astar_global_ns, g_moveRecon_frame_astar_global_ns);

    if (g_moveRecon_frames % 30 == 0) {
        mc2::fmt::print_out("[MOVE_RECON v1] frame={:d} movers={:d} ctrl_ns={:d} pathlock_ns={:d} astar_local_ns={:d} astar_global_ns={:d} calcMovePath_calls={:d} offmap_noops={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frames)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frame_movers)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frame_ctrl_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frame_pathlock_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frame_astar_local_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_frame_astar_global_ns)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_calcMovePath_calls)), mc2::fmt::as_u64(static_cast<unsigned long long>(g_moveRecon_offmap_noops)));
        std::fflush(stdout);
    }

    g_moveRecon_frame_ctrl_ns         = 0;
    g_moveRecon_frame_pathlock_ns     = 0;
    g_moveRecon_frame_astar_local_ns  = 0;
    g_moveRecon_frame_astar_global_ns = 0;
    g_moveRecon_frame_movers          = 0;
}
