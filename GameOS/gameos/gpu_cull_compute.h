#pragma once
namespace gpu_cull {
bool compute_init();
void compute_shutdown();
bool compute_isEnabled();
void compute_dispatch();
void compute_emitParitySummary();
}
