// Geometry submission exhaustion diagnostics.
#include "render_scene_support.h"
#include "mc2fmt.h"

// [TXMMGR_BOUNDS v1] — TXMMGR-BOUNDS-HARDEN-1 exhaustion counters.
// Declared extern in render_submissions.h so the inline pool allocators + addVertices paths
// can bump them. atomic+relaxed because addVertices/addRenderShape run on
// frame-jobs workers; the increment only happens on the rare overflow path.
// A teardown dump surfaces any nonzero counter even with the trace gate off, so
// silent pool exhaustion can no longer pass unnoticed.
bool g_txmmgrBoundsTrace = mc2::gate::get<"MC2_TXMMGR_BOUNDS_TRACE">();
std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented{0};
namespace {
struct TxmmgrBoundsDump {
    ~TxmmgrBoundsDump() {
        const unsigned long long a = g_txmmgr_vertex_block_exhausted.load();
        const unsigned long long c = g_txmmgr_add_vertices_overflow_prevented.load();
        if (a | c)
            mc2::fmt::print_err("[TXMMGR_BOUNDS v1] SUMMARY vertex_block_exhausted={:d} add_vertices_overflow_prevented={:d}\n", mc2::fmt::as_u64(a), mc2::fmt::as_u64(c));
    }
};
TxmmgrBoundsDump s_txmmgrBoundsDump;
}
