<!-- doc-status: stale | reviewed:2026-10-11 -->
# Terrain Height Audit — empirical run

- Script: `scripts/terrain_height_audit.py` (TERRAIN-HEIGHT-AUDIT-SCRIPT-1)
- Run at: 2026-05-28T08:21:55
- Missions dir: `A:\Games\Carver5-feasibility\data\missions`
- Missions: mc2_01, mc2_03, mc2_10, mc2_17, mc2_24

Read-only audit; reads only mission `.pak` files, makes no engine, gameplay, or render mutation. Format spec lives at [docs/terrain-height-audit.md](../../docs/terrain-height-audit.md).

### mc2_01

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_01.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 100×100 = 10000 samples
- World scale: 128.0 wu/vertex; map extent ≈ 12672 wu/side
- Raw heightfield bytes: 312.5 KB

**Elevation (world units):**

| min | max | range | mean | stdev |
|---|---|---|---|---|
| 200.00 | 710.00 | 510.00 | 262.11 | 96.19 |

**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**

| n | min | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 19800 | 0.00 | 0.00 | 25.53 | 111.37 | 359.18 |

**Slope (degrees, atan(Δh / 128)):**

| p50 | p90 | p99 | max |
|---|---|---|---|
| 0.00° | 11.28° | 41.03° | 70.39° |

**Blockiness (variance of 2nd-difference):** 649.089 (lower = smoother)

**Render-height texture memory estimate (R32F):**

| scale | size       | bytes |
|---|---|---|
| 1x    | 100×100  | 39.1 KB |
| 2x    | 200×200  | 156.2 KB |
| 4x    | 400×400  | 625.0 KB |
| 8x    | 800×800  | 2.44 MB |

### mc2_03

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_03.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 100×100 = 10000 samples
- World scale: 128.0 wu/vertex; map extent ≈ 12672 wu/side
- Raw heightfield bytes: 312.5 KB

**Elevation (world units):**

| min | max | range | mean | stdev |
|---|---|---|---|---|
| -300.00 | 800.00 | 1100.00 | 206.17 | 245.74 |

**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**

| n | min | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 19800 | 0.00 | 12.94 | 47.45 | 129.41 | 250.20 |

**Slope (degrees, atan(Δh / 128)):**

| p50 | p90 | p99 | max |
|---|---|---|---|
| 5.77° | 20.34° | 45.31° | 62.91° |

**Blockiness (variance of 2nd-difference):** 617.586 (lower = smoother)

**Render-height texture memory estimate (R32F):**

| scale | size       | bytes |
|---|---|---|
| 1x    | 100×100  | 39.1 KB |
| 2x    | 200×200  | 156.2 KB |
| 4x    | 400×400  | 625.0 KB |
| 8x    | 800×800  | 2.44 MB |

### mc2_10

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_10.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120 = 14400 samples
- World scale: 128.0 wu/vertex; map extent ≈ 15232 wu/side
- Raw heightfield bytes: 450.0 KB

**Elevation (world units):**

| min | max | range | mean | stdev |
|---|---|---|---|---|
| 0.00 | 750.00 | 750.00 | 307.24 | 203.75 |

**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**

| n | min | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 28560 | 0.00 | 12.15 | 48.58 | 97.17 | 279.35 |

**Slope (degrees, atan(Δh / 128)):**

| p50 | p90 | p99 | max |
|---|---|---|---|
| 5.42° | 20.78° | 37.20° | 65.38° |

**Blockiness (variance of 2nd-difference):** 781.739 (lower = smoother)

**Render-height texture memory estimate (R32F):**

| scale | size       | bytes |
|---|---|---|
| 1x    | 120×120  | 56.2 KB |
| 2x    | 240×240  | 225.0 KB |
| 4x    | 480×480  | 900.0 KB |
| 8x    | 960×960  | 3.52 MB |

### mc2_17

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_17.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120 = 14400 samples
- World scale: 128.0 wu/vertex; map extent ≈ 15232 wu/side
- Raw heightfield bytes: 450.0 KB

**Elevation (world units):**

| min | max | range | mean | stdev |
|---|---|---|---|---|
| 0.00 | 486.70 | 486.70 | 149.73 | 96.04 |

**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**

| n | min | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 28560 | 0.00 | 2.66 | 26.60 | 61.17 | 178.53 |

**Slope (degrees, atan(Δh / 128)):**

| p50 | p90 | p99 | max |
|---|---|---|---|
| 1.19° | 11.74° | 25.54° | 54.36° |

**Blockiness (variance of 2nd-difference):** 221.490 (lower = smoother)

**Render-height texture memory estimate (R32F):**

| scale | size       | bytes |
|---|---|---|
| 1x    | 120×120  | 56.2 KB |
| 2x    | 240×240  | 225.0 KB |
| 4x    | 480×480  | 900.0 KB |
| 8x    | 960×960  | 3.52 MB |

### mc2_24

- Path: `A:\Games\Carver5-feasibility\data\missions\mc2_24.pak`
- MapData packet: index 0 (storage RAW)
- Grid: 120×120 = 14400 samples
- World scale: 128.0 wu/vertex; map extent ≈ 15232 wu/side
- Raw heightfield bytes: 450.0 KB

**Elevation (world units):**

| min | max | range | mean | stdev |
|---|---|---|---|---|
| -100.00 | 1400.00 | 1500.00 | 248.32 | 309.24 |

**Adjacent-vertex delta (|Δh| per 128 wu step, axial only):**

| n | min | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| 28560 | 0.00 | 5.88 | 47.06 | 158.82 | 358.13 |

**Slope (degrees, atan(Δh / 128)):**

| p50 | p90 | p99 | max |
|---|---|---|---|
| 2.63° | 20.19° | 51.13° | 70.33° |

**Blockiness (variance of 2nd-difference):** 1288.218 (lower = smoother)

**Render-height texture memory estimate (R32F):**

| scale | size       | bytes |
|---|---|---|
| 1x    | 120×120  | 56.2 KB |
| 2x    | 240×240  | 225.0 KB |
| 4x    | 480×480  | 900.0 KB |
| 8x    | 960×960  | 3.52 MB |

