// MC_TextureManager: SPFLUSH cost accumulators and the TXMMGR_BOUNDS exhaustion counters.
// Declared in the class headers; shared names in txmmgr_internal.h.
#include "txmmgr_internal.h"
#include "mc2fmt.h"

// [SPFLUSH_COST_SPLIT v1] — txmmgr-side RDTSC accumulator.
// Measures the batcher_prepareBaseInstanceTable() + bucket-upload span each
// frame. Gate mirrors the registry-side gate (same env var). The consume
// function is called by the registry summary emit (extern declaration there).
const bool s_spflushTxmEnabled = mc2::gate::get<"MC2_STATIC_PROP_FLUSH_COST_SPLIT">();
unsigned long long s_spflush_baseinstance_upload_cyc = 0;
// recipe_rebuilds: monotonic + window delta. light_index_writes==recipe_rebuilds (same site).
unsigned long long s_spflush_recipe_rebuilds_txm      = 0;
static unsigned long long s_spflush_recipe_rebuilds_total     = 0;

// Exposed via extern in gos_static_prop_registry.cpp summary emit.
unsigned long long spflush_ConsumeBaseInstanceUploadCycles() {
    const unsigned long long v = s_spflush_baseinstance_upload_cyc;
    s_spflush_baseinstance_upload_cyc = 0;
    return v;
}
// Returns the window delta and the lifetime total for recipe_rebuilds.
// Delta is reset on call; total is monotonic.
unsigned long long spflush_ConsumeRecipeRebuildsDelta() {
    const unsigned long long v = s_spflush_recipe_rebuilds_txm;
    s_spflush_recipe_rebuilds_total += v;
    s_spflush_recipe_rebuilds_txm    = 0;
    return v;
}
unsigned long long spflush_GetRecipeRebuildTotal() {
    return s_spflush_recipe_rebuilds_total;
}

// [TXMMGR_BOUNDS v1] — TXMMGR-BOUNDS-HARDEN-1 exhaustion counters.
// Declared extern in txmmgr.h so the inline pool allocators + addVertices paths
// can bump them. atomic+relaxed because addVertices/addRenderShape run on
// frame-jobs workers; the increment only happens on the rare overflow path.
// A teardown dump surfaces any nonzero counter even with the trace gate off, so
// silent pool exhaustion can no longer pass unnoticed.
bool g_txmmgrBoundsTrace = mc2::gate::get<"MC2_TXMMGR_BOUNDS_TRACE">();
std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented{0};
std::atomic<unsigned long long> g_txmmgr_add_shape_overflow_prevented{0};
namespace {
struct TxmmgrBoundsDump {
    ~TxmmgrBoundsDump() {
        const unsigned long long a = g_txmmgr_vertex_block_exhausted.load();
        const unsigned long long b = g_txmmgr_block_exhausted.load();
        const unsigned long long c = g_txmmgr_add_vertices_overflow_prevented.load();
        const unsigned long long d = g_txmmgr_add_shape_overflow_prevented.load();
        if (a | b | c | d)
            mc2::fmt::print_err("[TXMMGR_BOUNDS v1] SUMMARY vertex_block_exhausted={:d} block_exhausted={:d} add_vertices_overflow_prevented={:d} add_shape_overflow_prevented={:d}\n", mc2::fmt::as_u64(a), mc2::fmt::as_u64(b), mc2::fmt::as_u64(c), mc2::fmt::as_u64(d));
    }
};
TxmmgrBoundsDump s_txmmgrBoundsDump;
}
