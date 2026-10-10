<!-- doc-status: current | reviewed:2026-10-11 -->
# Buffer Lifetime Ledger

Maintained inventory of high-risk renderer GPU buffers by hazard class. Companion to
[buffer-lifetime-ownership-recon-1](buffer-lifetime-ownership-recon-1.md). Machine-read by
`scripts/check-buffer-lifetime-ownership.py` (registered `buffer_lifetime`). Source-verified
vs nifty `4bd55403`.

Classes: **A** fence-guarded ring · **B** GPU-produced + glMemoryBarrier · **C**
static/immutable per-mission · **D** implicit-sync (no fence/ring/explicit barrier —
GL-safe, Vulkan-debt) · **E** sticky-bit temporal accumulator.

The table below is parsed: `| symbol | class | producer | sync | TU |` (one buffer per row;
`sync` names the fence/barrier/none mechanism).

| symbol | class | producer | sync | TU |
|---|---|---|---|---|
| `s_boneSsbo` | A | CPU-map | ring3+fence | gos_mech_batcher.cpp |
| `s_instanceSsbo` (mech) | A | CPU-map | ring3+fence | gos_mech_batcher.cpp |
| `s_coalesceInstanceSsbo` | A | CPU-map | ring+coalesceFence | gos_static_prop_batcher.cpp |
| `s_staticInstanceSsbo` | C | CPU-map | dirty-gated+staticDrawFence | gos_static_prop_batcher.cpp |
| `s_staticIndirectCmdBuf` | C | CPU-glBufferData | dirty-gated | gos_static_prop_batcher.cpp |
| `s_indirectCmdBuf` (cull) | B | GPU-patch | COMMAND barrier | gpu_cull_compute.cpp |
| `s_bucketCountsBuf` | B | GPU-atomicAdd | SSBO barrier+clear | gpu_cull_compute.cpp |
| `s_actorVisBuf` | B | GPU-atomicOr | SSBO barrier+clear | gpu_cull_compute.cpp |
| `s_blockVisBuf` | E | GPU-atomicOr | mission-clear+SSBO barrier | gpu_cull_compute.cpp |
| `s_bucketCapsBuf` | C | CPU-glBufferData | static | gpu_cull_compute.cpp |
| `g_recipeSSBO` | C | CPU | static-mission | gos_terrain_indirect.cpp |
| `g_thinRecordSSBO` | A | CPU/GPU | ring3+thinRingFences | gos_terrain_indirect.cpp |
| `g_indirectCmdBuffer` | B | GPU-atomicAdd | COMMAND barrier+clear | gos_terrain_indirect.cpp |
| `s_heightSsbo` | C | CPU | static-mission | gos_terrain_lod_chunk.cpp |
| `g_recipeBuffer` (water) | C | CPU | static-mission | gos_terrain_water_stream.cpp |
| `g_thinBuffer` (water) | A | CPU/GPU | ring3+fence | gos_terrain_water_stream.cpp |
| `g_waterIndirectCmdBuffer` | B | GPU-atomicAdd | COMMAND barrier+clear | gos_terrain_water_stream.cpp |
| `lightData_` | D | CPU-upload | IMPLICIT-SYNC | txmmgr.cpp |
| `s_ssbo` (particle) | D | CPU-glBufferData | IMPLICIT-SYNC | gos_particle_bridge.cpp |
| `s_tubePosSsbo` | D | CPU-glBufferData | IMPLICIT-SYNC | gos_particle_bridge.cpp |
| `s_indirectCmdBuf` (terrain-mask) | D | CPU-glBufferData | orphan-then-upload (VK-OPT-4) | gameos_graphics.cpp |
| `s_waterIndirectCmdBuf` (terrain-mask) | D | CPU-glBufferData | orphan-then-upload (VK-OPT-4) | gameos_graphics.cpp |

## Known stale telemetry

- `gpu_drawn_instances` — reads CPU-side count that is 0 under live GPU-authority path;
  declared in [objbatcher-zero-gpu-drawn-recon-1](objbatcher-zero-gpu-drawn-recon-1.md).
  Retire or readback-gate (STALE-COUNTER-RETIRE-1).

## What the checker enforces (FAIL)

1. Every ledger row declares a class in {A,B,C,D,E}.
2. **Class-B COMMAND barrier presence** — `gpu_cull_compute.cpp` and
   `gos_terrain_indirect.cpp` each contain a `GL_COMMAND_BARRIER_BIT` glMemoryBarrier
   (removing the barrier that fences GPU-produced indirect buffers → FAIL).
3. **Class-D Vulkan-debt declared** — at least the known class-D buffers are listed so the
   implicit-sync migration debt stays visible.
4. **Stale counter declared** — `gpu_drawn_instances` is documented as known-stale.

## Open (next slices)

- STALE-COUNTER-RETIRE-1 (make gpu_drawn_instances honest).
- LIGHT-BUFFER-BARRIER-VERIFY-1 / PARTICLE-BUFFER-LIFETIME-1 (class-D: confirm/insert the
  explicit barrier or ring; freshest highest-touch risk).

## VK-OPT-4 — class-D terrain-mask orphan mitigation (2026-07-04)

`s_indirectCmdBuf` / `s_waterIndirectCmdBuf` (terrain-mask indirect command buffers,
`gameos_graphics.cpp`) now use **orphan-then-upload** (`maskOrphanUploadCmd`): a
`glBufferData(size, NULL)` rename precedes the per-frame `glBufferSubData`, so the GL
driver hands back fresh storage instead of implicit-sync-stalling on the previous frame's
`glDrawArraysIndirect` read. Byte-identical output (same 16-byte command); only the driver
serialization is removed. Mirrors the `tubeOrphanUpload` template (VK-OPT-1,
`gos_particle_bridge.cpp`). They remain **class-D** in the taxonomy (orphaning is a driver
rename, not a fence/ring/explicit barrier), so the debt stays visible for the Vulkan
migration — VK has no implicit-sync, so these still need an explicit barrier/ring at port
time. Measurable via `MC2_TERRAIN_MASK_UPLOAD_STATS` (orphan+upload byte counter).
