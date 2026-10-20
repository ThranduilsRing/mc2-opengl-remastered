<!-- doc-status: current | reviewed:2026-10-19 -->
# Buffer Lifetime Ledger

Maintained inventory of high-risk renderer GPU buffers by hazard class. Companion to
[buffer-lifetime-ownership-recon-1](buffer-lifetime-ownership-recon-1.md). Machine-read by
`scripts/check-buffer-lifetime-ownership.py` (registered `buffer_lifetime`). Source-verified
for the native actor-cull migration in lane2 s68.

Classes: **A** fence-guarded ring · **B** GPU-produced + explicit visibility barriers · **C**
static/immutable per-mission · **D** implicit-sync (no fence/ring/explicit barrier —
GL-safe, Vulkan-debt) · **E** sticky-bit temporal accumulator.

The table below is parsed: `| symbol | class | producer | sync | TU |` (one buffer per row;
`sync` names the fence/barrier/none mechanism).

| symbol | class | producer | sync | TU |
|---|---|---|---|---|
| `s_boneSsbo` | A | CPU-map | ring3+fence | gos_mech_batcher.cpp |
| `s_instanceSsbo` (mech) | A | CPU-map | ring3+fence | gos_mech_batcher.cpp |
| `NativeCull::Slot::records/params` | A | CPU-upload | ring3+completed-ticket before upload/reuse | vk_actor_cull_native.cpp |
| `NativeCull::Slot::output` | B | VK compute | buffer compute-write -> transfer-read | vk_actor_cull_native.cpp |
| `NativeCull::Slot::staging` | B | VK transfer | buffer transfer-write -> host-read + completed-ticket before map | vk_actor_cull_native.cpp |
| `g_recipeBuffer` (water) | C | CPU | static-mission | gos_terrain_water_stream.cpp |
| `g_thinBuffer` (water) | A | CPU/GPU | ring3+fence | gos_terrain_water_stream.cpp |
| `g_waterIndirectCmdBuffer` | B | GPU-atomicAdd | COMMAND barrier+clear | gos_terrain_water_stream.cpp |
| `s_ssbo` (particle) | D | CPU-glBufferData | IMPLICIT-SYNC | gos_particle_bridge.cpp |
| `s_tubePosSsbo` | D | CPU-glBufferData | IMPLICIT-SYNC | gos_particle_bridge.cpp |
| `s_indirectCmdBuf` (terrain-mask) | D | CPU-glBufferData | orphan-then-upload (VK-OPT-4) | gameos_graphics.cpp |
| `s_waterIndirectCmdBuf` (terrain-mask) | D | CPU-glBufferData | orphan-then-upload (VK-OPT-4) | gameos_graphics.cpp |

Terrain GL recipe/thin/command buffers were retired in s65. The terrain path now
captures CPU MVP inputs for Vulkan; it produces no GL indirect commands to fence.

The GL light-data buffer and its dirty-prefix/orphan uploads are retired. The
native light feed still reads the CPU table and its live row count directly.
CardCloud CPU simulation remains authoritative; the unused GL compare oracle is gone.

Static-prop geometry, albedo layers, material rows and cull identity mappings now
remain CPU-owned until native consumers copy them; their GL buffers and fences are
retired. Terrain height/type/cement/sidecar sources are CPU-owned as well.

Actor-cull GL indirect buckets, visible-ID scatter and sticky block output were
retired after their last draw consumer. Only actor visibility had a live CPU
consumer; it now uses native compute, copied actor IDs and lagged CPU history.

## Known stale telemetry

- `gpu_drawn_instances` — reads CPU-side count that is 0 under live GPU-authority path;
  declared in [objbatcher-zero-gpu-drawn-recon-1](objbatcher-zero-gpu-drawn-recon-1.md).
  Retire or readback-gate (STALE-COUNTER-RETIRE-1).

## What the checker enforces (FAIL)

1. Every ledger row declares a class in {A,B,C,D,E}.
2. **Native actor-cull ownership**: a slot ticket must complete before CPU input
   uploads/pool reset or host readback mapping. Buffer barriers order compute -> copy
   -> host; device-wide barriers for these private buffers fail the check.
3. **Class-D Vulkan-debt declared** — at least the known class-D buffers are listed so the
   implicit-sync migration debt stays visible.
4. **Stale counter declared** — `gpu_drawn_instances` is documented as known-stale.

## Open (next slices)

- STALE-COUNTER-RETIRE-1 (make gpu_drawn_instances honest).
- PARTICLE-BUFFER-LIFETIME-1 (class-D: confirm/insert the
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
