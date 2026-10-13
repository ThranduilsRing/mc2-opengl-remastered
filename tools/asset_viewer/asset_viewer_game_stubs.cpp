// asset_viewer_game_stubs.cpp -- inert definitions for game-layer globals that
// mclib/tgl.cpp and mclib/msl.cpp (compiled into mc2_tglloader) reference but
// whose defining TUs (mclib/bdactor.cpp, cone_glow.cpp, txmmgr.cpp,
// GameOS/gameos/gos_static_prop_batcher.cpp, gos_mech_batcher.cpp) pull in the
// whole renderer/game. The viewer only loads and previews meshes, so the
// diagnostics are permanently "off". No real logic is duplicated here.
#include <atomic>
#include <cstdint>

unsigned long g_fbDiagPtSpotConsumed = 0;                   // bdactor.cpp:166
unsigned long g_fbDiagPtSpotZeroDir = 0;                    // bdactor.cpp:167
bool g_txmmgrBoundsTrace = false;                           // txmmgr.cpp:416 (env-gated there)
std::atomic<unsigned long long> g_txmmgr_vertex_block_exhausted{0};          // txmmgr.cpp:418
std::atomic<unsigned long long> g_txmmgr_add_vertices_overflow_prevented{0}; // txmmgr.cpp:420
int g_mechPreviewRenderDepth = 0;                           // gos_mech_batcher.cpp:193

bool mc2FullBakeDiagEnabled() { return false; }             // bdactor.cpp:152

namespace mc2_cone_glow {
bool cone_geom_hidden_by_alpha(unsigned) { return false; }  // cone_glow.cpp:355
}
