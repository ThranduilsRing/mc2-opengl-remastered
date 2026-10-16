// link seams.
//
// Definitions for a handful of globals/functions that the current tgl.cpp /
// msl.cpp reference from render / frame-jobs / txmmgr-trace code paths the
// import-only tool never executes. They exist solely so the game-free tool
// links; none of these paths run in tg_import_dump. (Recent additions to the
// engine since tgl_loader_standalone_spike last built; kept out of the reused
// stubs.cpp so that file stays a verbatim copy.)
#include <atomic>

#include "render_contract.h"

// objmgr/frame_jobs worker flag (msl.cpp -> isFrameJobsWorkerThread()).
thread_local bool g_isFrameJobsWorker = false;

// txmmgr bounds-trace instrumentation (tgl.cpp references; default off / zero).
bool g_txmmgrBoundsTrace = false;
std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted{0};
std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented{0};

// Game-layer diagnostics tgl.cpp / msl.cpp reference; permanently off in the importer.
unsigned long g_fbDiagPtSpotConsumed = 0;
unsigned long g_fbDiagPtSpotZeroDir = 0;
int g_mechPreviewRenderDepth = 0;
bool mc2FullBakeDiagEnabled() { return false; }
namespace mc2_cone_glow {
bool cone_geom_hidden_by_alpha(unsigned) { return false; }
}

// Render-pass contract note — called only from TG_Shape::Render (never on the
// import path). No-op.
namespace render_contract {
void noteRenderPass(PassIdentity /*id*/, const char* /*callerHint*/) {}
}  // namespace render_contract
