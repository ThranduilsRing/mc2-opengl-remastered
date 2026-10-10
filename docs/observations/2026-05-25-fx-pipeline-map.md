<!-- doc-status: stale | reviewed:2026-10-11 -->
# FX Pipeline Map — GPU particle path (post-B2 beta)

> Branch: `claude/nifty-mendeleev` · Gate: `MC2_GPU_PARTICLES=1` (default OFF)
>
> Renders in any Mermaid-aware viewer (GitHub, VS Code with the Markdown Preview Mermaid extension, Obsidian, Typora). ASCII fallback below for terminal viewing.

---

## Mermaid — full pipeline

```mermaid
flowchart TD
    classDef producer fill:#2d3748,stroke:#a0aec0,color:#f7fafc
    classDef batcher fill:#553c9a,stroke:#d6bcfa,color:#faf5ff
    classDef bridge fill:#22543d,stroke:#9ae6b4,color:#f0fff4
    classDef shader fill:#7b341e,stroke:#fbd38d,color:#fffaf0
    classDef side fill:#1a365d,stroke:#90cdf4,color:#ebf8ff
    classDef known fill:#742a2a,stroke:#feb2b2,color:#fff5f5

    subgraph PRODUCERS["PRODUCERS (per frame)"]
        direction TB
        B1["gosFX specs (B1)<br/>Card / CardCloud / PointCloud<br/>ShardCloud / Tube<br/><i>spawn_*.cpp, age=0.5 baked</i>"]:::producer
        B2["WeaponBolt trails (B2)<br/><i>update() per frame:</i><br/>• snapshot laserPosition<br/>• axis-swap (-x, z, y)<br/>• push trail_history[24]<br/>• walk ring → Spawn pairs"]:::producer
        TRAIL["GpuTrailEmitter::Spawn<br/><i>gpu_trail.cpp</i><br/>• tuning_for(kind)<br/>• len = |cur-prev|<br/>• N = clamp(ceil(len*density), MAX_PER_SEGMENT)<br/>• for i in [0..N): Emit lerp(prev,cur,i/N)"]:::producer
    end

    subgraph BATCHER["BATCHER (singleton, per-frame state)"]
        direction TB
        BG["BeginGroup(handle, u0,v0,us,vs, blend)<br/><i>pushes new GroupInfo every call<br/>NO coalescing by handle</i>"]:::batcher
        EM["Emit(GpuParticle)<br/><i>append to staging vec<br/>overflow at staging.size ≥ budget (4096)</i>"]:::batcher
        RT["ResolveTextures()<br/><i>once per frame, before Flush<br/>MLR pool idx → gos handle</i>"]:::batcher
        FL["Flush()<br/><i>called from gamecam.cpp ~L268-272</i><br/>• uploads staging<br/>• per-group glDrawElements<br/>• CLEARS staging + groups ⚠"]:::batcher

        BG --> EM
        EM --> RT
        RT --> FL
    end

    subgraph BRIDGE["BRIDGE (GameOS/gameos/gos_particle_bridge)"]
        direction TB
        SSBO["SSBO upload<br/><i>staging vec → GL buffer</i>"]:::bridge
        UNIF["Uniform binding<br/>u_cameraRight / u_cameraUp<br/><i>cached via s_loc_* statics</i>"]:::bridge
        DRAW["per-group glDrawElements<br/><i>one draw per GroupInfo</i>"]:::bridge

        SSBO --> UNIF
        UNIF --> DRAW
    end

    subgraph SHADER["SHADER (particle_billboard.vert + .frag)"]
        VS["vert: view-aligned offset<br/><code>worldPos = center<br/>+ u_cameraRight * x * size<br/>+ u_cameraUp    * y * size</code><br/><i>(P1 fix — was fixed XZ axes)</i>"]:::shader
        FS["frag: sample texture page<br/>multiply by per-particle color"]:::shader

        VS --> FS
    end

    subgraph TEX["Texture resolution"]
        T1["MLR pool index<br/><i>e.g. 41 = smoke</i>"]:::side
        T2["MLRTexturePool"]:::side
        T3["GOSImage::GetHandle<br/><i>gos handle, 980+</i>"]:::side
        T1 --> T2 --> T3
    end

    subgraph CAM["Camera basis bridge (P1, temp)"]
        C1["GameCamera::render()"]:::side
        C2["cameraOrigin.GetLocalRight/UpInWorld<br/><i>Stuff/MC2 space</i>"]:::side
        C3["axis-swap to GL space<br/><code>GL.x = -Stuff.x, .y = Stuff.z, .z = Stuff.y</code>"]:::side
        C4["gos_SetActiveCamera (before Flush)<br/>gos_ClearActiveCamera (after Flush)"]:::side
        C1 --> C2 --> C3 --> C4
    end

    subgraph KNOWN["KNOWN LIMITS (beta debt)"]
        K1["Batcher::Flush clears staging<br/>→ zero particle persistence<br/>→ producers must ring-buffer for trails"]:::known
        K2["BeginGroup never coalesces<br/>→ N producers = ≥N groups/frame<br/>→ group count scales with active emitters"]:::known
        K3["UV sub-rect only flows from gosFX specs<br/>→ B2 trail hardcodes (0,0,1,1)<br/>→ atlased textures show as 'white squares'"]:::known
        K4["PpcBolt enum exists, no tuning<br/>→ all bolts use MissileSmoke today"]:::known
        K5["No CPU trailEffect suppression<br/>→ CPU + GPU trails render together"]:::known
    end

    B1 --> BG
    B2 --> TRAIL
    TRAIL --> BG
    TRAIL --> EM

    FL --> SSBO
    C4 -.publishes.-> UNIF
    RT -.reads.-> T1
    DRAW --> VS

    K1 -.affects.-> FL
    K2 -.affects.-> BG
    K3 -.affects.-> TRAIL
```

---

## Mermaid — focused B2 control flow (per-bolt, per-frame)

```mermaid
sequenceDiagram
    participant Bolt as WeaponBolt::update
    participant Ring as trail_history[24]
    participant Emit as GpuTrailEmitter::Spawn
    participant Bat as Batcher
    participant Br as gos_particle_bridge
    participant Sh as particle_billboard shader

    Bolt->>Bolt: laserPosition += laserVelocity·dt
    Bolt->>Bolt: swap = (-x, z, y) of laserPosition
    Bolt->>Ring: push swap at trail_head
    Bolt->>Ring: head = (head+1) % 24; count = min(count+1, 24)

    loop oldest → newest, count-1 pairs
        Bolt->>Emit: Spawn(MissileSmoke, prev_i, cur_i, dt)
        Emit->>Emit: len = |cur-prev|; gate len < ε
        Emit->>Emit: N = clamp(ceil(len * 2.0), MAX 32)
        Emit->>Bat: BeginGroup(handle=41, full UV, alpha)
        loop i in [0..N)
            Emit->>Bat: Emit(GpuParticle at lerp)
            Bat->>Bat: ++s_trail_spawn_total
        end
    end

    Note over Bolt: …other bolts repeat the dance…

    Note over Bat: GameCamera::render() runs ONCE
    Bat->>Bat: ResolveTextures (MLR→gos handles)
    Bat->>Br: Flush(staging, groups)
    Br->>Sh: SSBO upload + uniform bind
    Sh->>Sh: view-aligned worldPos + texture sample
    Bat->>Bat: staging.clear(); groups.clear()  ⚠ no persistence
```

---

## ASCII fallback (for terminal viewers)

```
                              FX PIPELINE (GPU path, MC2_GPU_PARTICLES=1)
                              ================================================

  PRODUCERS                              BATCHER                           BRIDGE / SHADER
  ---------                              -------                           ---------------

  gosFX specs (B1)
  ----------------
    Card          ─┐
    CardCloud     ─┤      spawn_card.cpp ─┐
    PointCloud    ─┤      spawn_*.cpp ────┤
    ShardCloud    ─┤      (1/frame per     │
    Tube          ─┘       effect, age=0.5)│
                                          │
                                          ▼
  WeaponBolt trails (B2)                ┌─────────────────────┐
  ---------------------                 │  Batcher (singleton)│
    WeaponBolt::update()                │  mclib/particles/   │
      ├─ snapshot laserPosition         │    batcher.{h,cpp}  │
      ├─ axis-swap (-x, z, y) ────────► │                     │
      ├─ push into trail_history[24]    │  • BeginGroup(      │
      └─ walk ring oldest→newest        │     handle,uvs,blend)│      ┌──────────────────────┐
         for each pair:                 │  • Emit(GpuParticle)│      │ gos_particle_bridge  │
           GpuTrailEmitter::Spawn ─────►│  • ResolveTextures()│─────►│ .{h,cpp}             │
              mclib/particles/          │     MLR pool → gos  │      │                      │
              gpu_trail.{h,cpp}         │     handle (one-shot│      │ • SSBO upload        │
                                        │     per handle)     │      │   (staging vec → GL) │
                                        │                     │      │ • per-group          │
                                        │  state per FRAME:   │      │   glDrawElements     │
                                        │   staging<GpuPart>  │      │ • g_active_camera    │
                                        │   groups<GroupInfo> │      │   (P1 bridge)        │
                                        │                     │      │ • s_loc_cameraRight/Up
                                        │  Flush() CLEARS     │      │   (cached uniforms)  │
                                        │  staging+groups     │      └──────────┬───────────┘
                                        │  every frame ◀── B2 │                 │
                                        │  bug source:        │                 ▼
                                        │  no persistence     │      ┌──────────────────────┐
                                        │                     │      │ shaders/             │
                                        │  budget = 4096      │      │  particle_billboard  │
                                        │  records / flush    │      │  .vert + .frag       │
                                        │                     │      │                      │
                                        └──────┬──────────────┘      │ vert:                │
                                               │                     │  worldPos = center   │
                                               │ called from         │   + u_cameraRight*x  │
                                               │ code/gamecam.cpp    │   + u_cameraUp*y     │
                                               │ ~L268-272 (between  │   (P1 — view-aligned)│
                                               │ renderLists() and   │                      │
                                               │ theClipper->Render) │ frag:                │
                                               │                     │  sample texture page │
                                               ▼                     │  multiply color/alpha│
                                        ┌─────────────────────┐      └──────────────────────┘
                                        │ Texture resolution  │
                                        │ ------------------- │      ┌──────────────────────┐
                                        │ MLR pool index      │      │ Camera basis bridge  │
                                        │  (small int, e.g.   │      │ (P1, temp)           │
                                        │   41 = smoke)       │      │                      │
                                        │      │              │      │ GameCamera::render() │
                                        │      ▼              │      │  ├─ get right/up via │
                                        │ MLRTexturePool ────►│      │  │  cameraOrigin.    │
                                        │ MLRTexture::GetImage│      │  │  GetLocalRight/Up │
                                        │      │              │      │  ├─ axis-swap to GL  │
                                        │      ▼              │      │  └─ gos_SetActiveCam │
                                        │ GOSImage::GetHandle │      │     before Flush     │
                                        │  (gos handle, 980+) │      │     gos_ClearActiveCam
                                        │  → stored in        │      │     after Flush      │
                                        │    GroupInfo.handle │      │                      │
                                        └─────────────────────┘      └──────────────────────┘
```

---

## Key call sites (worktree-relative)

| Where | What |
|---|---|
| `code/gamecam.cpp:268-272` | `Batcher::ResolveTextures()` + `Flush()` sandwich; camera basis set/clear wraps this |
| `code/weaponbolt.cpp:380` (update entry) | snapshot + ring push + Spawn loop (B2 hook) |
| `code/weaponbolt.cpp:1155` | `laserPosition += laserVelocity` — the real in-flight position (NOT `position`, which is launcher hotspot) |
| `code/weaponbolt_update.cpp:12-14` | gosFX axis swap pattern (source of truth; mirrored in trail push) |
| `code/weaponbolt.cpp:~2519` | `init()` — hardcoded `gpu_trail_kind = MissileSmoke` for every bolt (P3 will replace with INI table) |
| `mclib/particles/batcher.h:40` | `GroupInfo` struct (no `kind` field — P3 work adds it) |
| `mclib/particles/batcher.cpp:111` | `BeginGroup` — pushes new `GroupInfo` every call, no coalescing by handle |
| `mclib/particles/batcher.cpp:135` | `Emit` — overflow check at `staging.size >= budget` |
| `mclib/particles/batcher.cpp:155-200` | `ResolveTextures` — MLR handle → gos handle, one-shot per handle |
| `mclib/particles/batcher.cpp:204` | `Flush` — **clears staging + groups every frame** (root cause of "no persistence") |
| `mclib/particles/gpu_trail.cpp` | `GpuTrailEmitter::Spawn` + tuning table + `MC2_GPU_TRAIL_DISABLE` gate + `[B2 TRAIL_PROBE]` |
| `mclib/particles/spec.h:41` | `GpuParticle` 64-byte schema (static_asserts pin every offset) |
| `GameOS/gameos/gos_particle_bridge.cpp` | SSBO upload, draw loop, `g_active_camera` storage, cached uniform locations |
| `GameOS/gameos/utils/camera.cpp:110-115` | Where `cameraOrigin.GetLocalRight/UpInWorld` come from (extracted from `invView`) |
| `shaders/particle_billboard.vert` | View-aligned offset via `u_cameraRight/Up` (P1 fix) |
| `shaders/particle_billboard.frag` | Texture sample + per-particle color/alpha multiply |

---

## Env gates / probes (current truth)

| Var | Default | Effect |
|---|---|---|
| `MC2_GPU_PARTICLES` | OFF | Master gate for the entire GPU particle path |
| `MC2_GPU_PARTICLES_LOG` | OFF | `SPAWN_PROBE` / `RESOLVE_PROBE` + `[B2 TRAIL_PROBE]` (one-shot) |
| `MC2_GOSFX_GROUP_LOG` | OFF | Bridge UV dump + missing-texture errors + missing-camera-basis warning |
| `MC2_GPU_TRAIL_DISABLE` | OFF | Force-off B2 trail emitter while keeping rest of GPU path on |

| Counter | Source | Meaning |
|---|---|---|
| `emit_total` | Batcher (existing) | total Emit calls this run |
| `flush_total` | Batcher (existing) | total Flush calls this run |
| `nonempty_flush_total` | Batcher (existing) | flushes that actually drew |
| `records_per_flush_max` | Batcher (existing) | peak staging size; **= budget means saturation** |
| `trail_spawn` | gpu_trail.cpp (B2) | total trail particles emitted |
| `trail_head` | gpu_trail.cpp (B2) | total head sprites (0 for MissileSmoke, will fire for PpcBolt) |
| `event=overflow budget=4096 record dropped` | Batcher (existing) | budget hit; ANY producer's later Emits get dropped this frame |

---

## Known limits (beta debt, in priority order)

1. **No particle persistence in Batcher.** `Flush()` wipes staging every frame. Producers needing trails must ring-buffer positions themselves and re-stamp the full trail every frame. A real persistence substrate (age + despawn at lifetime) would retire every producer's ring buffer.
2. **No group coalescing by handle.** `BeginGroup` always pushes a new `GroupInfo`. With N active emitters using the same texture, you still get ≥N draw calls per frame.
3. **UV sub-rect is gosFX-spec-only.** B2 trail hardcodes `(0,0,1,1)` full page. If MLR handle 41 (smoke) is atlased, we sample the whole atlas — observed as "white-ish squares" instead of soft puffs.
4. **`PpcBolt` enum exists but no tuning, no head-sprite path, no INI mapping.** Every bolt today uses `MissileSmoke`.
5. **No CPU `trailEffect` suppression.** Both CPU gosFX trail AND GPU trail render in parallel (intentional through P3; P4 deferred).
6. **`g_active_camera` global is a stop-gap.** Future API form: `RenderFrameContext` passed into `Batcher::Flush`.

---

## Last-known smoke baseline

```
mc2_10, 40s, MC2_GPU_PARTICLES=1, MC2_GPU_PARTICLES_LOG=1, default tunings
PASS, 5471 frames @ 137 fps, 0 destroy delta
emit_total            =   926,304
trail_spawn           =   684,043
trail_head            =         0   (MissileSmoke has no head; PpcBolt will populate)
records_per_flush_max =     1,032   (well under 4096 budget)
event=overflow        =         0
groups=8 / draws=8 first flush (B1 baseline preserved)
```
