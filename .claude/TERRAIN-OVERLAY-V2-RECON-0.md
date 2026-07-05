# TERRAIN-OVERLAY-V2-RECON-0 — authored-overlay-mask replacement for cement/road/runway/pad bake

**Worktree:** `A:/Games/mc2-controlmap-sample-1` @ `fcb57b9d` (branch base `5ded0f19`). **RECON ONLY** — no code, no build, no launch.
**Arc goal (advisor-directed):** REPLACE the cement/road/runway/pad colormap+cement-word burn-in with **authored overlay masks + edge blending**; keep the old bake as a **fallback + parity source**. This is *not* "fix the old bake."
**Cross-refs (read):** `.claude/TERRAIN-CONTROLMAP-GENERATE-1-RECON.md` (this WT), `.claude/TERRAIN-SCATTER-MASK-1-RECON.md`, `.claude/TERRAIN-VISUAL-HEIGHT-CONSUMER-1-RECON.md`, `.claude/TERRAIN-MATERIAL-LIB-1-RECON.md` (this WT); root `.claude/CEMENT-BAKE-INTO-TERRAIN-RECON-1.md`, `.claude/CONCRETE-OVERLAY-UNIFY-RECON-1.md`.

> **KEY DISCOVERY that reframes the whole arc:** the two sockets this arc needs are **ALREADY LANDED on this branch, default-OFF.**
> 1. **Control map** — `MC2_TERRAIN_CONTROLMAP`, RGBA8 texture **unit 12** `u_controlMap`, sidecar `data/missions/<stem>.beauty/control_map.png`, **A = concrete channel**. Load `mclib/terrain.cpp:824-856`; GL owner `gos_terrain_lod_chunk.cpp:95-99,192 s_controlMapTex/s_locControlMap`.
> 2. **Bounds-aware high-res cement sidecar** design (root `CEMENT-BAKE-INTO-TERRAIN-RECON-1.md §1-6`) — the recommended non-destructive replacement for the per-cell cement-word+atlas machinery, feeding the SAME composite pass.
> So OVERLAY-V2 is largely **wiring + authoring-tool + parity extraction on top of shipped seams**, not a greenfield renderer.

---

## 1. Cement/road/runway/pad pipeline TODAY (LIVE chunk path, `MC2_TERRAIN_LOD_CHUNK` default ON)

```
AUTHORING (editor, EditRel GPU-only)
  Concrete/cement = TERRAIN-TYPE (not overlay):
    Paint-Material "Concrete" button -> TerrainBrush(MC_CONCRETE_TYPE=10)
      editor/EditorInterface.cpp:5497-5530 -> editor/terrainBrush.cpp:81-140 setTerrain per-vertex
    Auto edge/fill/diagonal is EMERGENT from 4 corner vertex types via createTransition()
      mclib/terrtxm.cpp:1159 (called :1489), keyed by typeInfo (4 corner type bytes :1440-1443)
    Live refresh: terrainBrush.cpp endPaint -> refreshTerrainAfterEdit + land->refreshTerrainTypeSSBO()
  Roads/runway/bridge = OVERLAY system (separate), AUTO-CONNECT auto-tiler:
    OverlayBrush (enum Overlays mapdata.h:39-60: DIRT/PAVED_ROAD, RUNWAY=7, bridges, X_* transitions)
      editor/OverlayBrush.cpp: connectivityMaskBasedOnAdjacentTiles :435, ConnectivityMaskToTexture :191,
      paintRoadStep :479 (set cell + recompute touched neighbor). packed (overlayType<<16)|offset.
    Per-cell storage: PostcompVertex.textureData (vertex.h:46): LOW word=base TXM idx, HIGH word=OVERLAY TXM idx
  SAVE: EditorInterface.cpp:753 -> EditorData::save :2054 -> land->save (terrain.cpp:3941)
        -> MapData::save (mapdata.cpp:215) writes PostcompVertex[] packet 0. Colormap NAME only (saveColorMapName).
        HOOK POINT for any bake step = EditorData.cpp:2179-2181 (just before land->save).

CLASSIFY at cache build (mclib/quad.cpp:596 / mapdata.cpp:280)
  isCement = terrainTextures->isCement(textureData & 0xffff)  (flag MC2_TERRAIN_CEMENT_FLAG, terrtxm.h:348)
  isAlpha  = terrainTextures->isAlpha(...)
  3 classes: (1)!isCement -> COLORMAP  (2)isCement&isAlpha -> colormap + overlay decal tile (TRANSITION edge)
             (3)isCement&!isAlpha -> flat cement fill (overlayHandle=0xffffffff)

RUNTIME DRAW (chunk path)
  Material weights come from COLORMAP COLOUR classifier, NOT terrainType/slope:
    shaders/terrain_lod_chunk.frag:167 chunkColorWeights(base) / :256 chunkWeights  <-- de-facto control map today
  v_terrainType (SSBO binding 24) drives ONLY concrete: frag pureConcrete = smoothstep(2,3,v_terrainType)
    (float material index 0..3, terrain.cpp:802-814 terrainTypeToMaterial; concrete-only, NOT raw 21-enum, NOT slope)
  CEMENT composite (chunk frag, in-shader, ~:435-540):
    cement WORDS SSBO binding 25 (bit31=VALID,bit30=IS_TRANSITION,bits29:24=maskId,bits15:0=atlasLayer)
      built CPU: gos_terrain_indirect.cpp:852 PopulateRecipeCementWords, :1112 BuildCementCatalogAtlas
    per-pixel: tile ctX/ctY from (worldXY+halfMap)/128 (128wu grid), read word:
      solid -> base = u_cementAtlas sample (unit 3, kChunkTexUnitCement)
      TRANSITION -> CEMENT-HARD-EDGE-1 neighbor-derived coverage: base = mix(base, cementColor, alpha)
        (chunk frag ~:535; alpha from u_transitionMaskArray 14-layer R8 unit 11 + neighbor bits)
  SEPARATE OVERLAY PASS (roads/runway/transition ART + PBR):
    shaders/terrain_overlay.frag: alpha-blended, binary-alpha discard (if c.a<0.5 discard, :122),
      ~18% edge darken (c.rgb * vec3(0.82,0.80,0.76), :82). ROAD-PBR asphalt/gravel shipped (units 0 tex1, 4).
    Draw: gosRenderer::drawTerrainOverlays (gameos_graphics.cpp:2183; pushTerrainOverlayTri :2181).
    Depth: terrain_overlay.vert:41 clip4.z += OVERLAY_DEPTH_BIAS * clip4.w  (PRE-DIVIDE, vertex-stage, vulkan-aligned)
      OVERLAY_DEPTH_BIAS = 0.00005 reverse-Z (include/terrain_depth_bias.hglsl:61; > 0 overlays WIN GEQUAL tie).
    Mines share this overlay layer/pass (quad.cpp:4103 drawMine; z-fight landmine — same bias band).
  NEW (landed, OFF): control map (unit 12) REPLACES chunkColorWeights classifier when MC2_TERRAIN_CONTROLMAP=1;
    A channel = concrete weight. Does NOT touch cement-word/overlay machinery today.

  markTerrainDrawn (gos_terrain_indirect.cpp:3830 / postprocess :2550) gates cloud-shadow/shoreline/godrays.

INDIRECT/LEGACY path (MC2_TERRAIN_INDIRECT=1, DEFAULT OFF, DEAD): gos_terrain.frag, quad.cpp, gos_terrain_indirect.cpp.
  gos_terrain.frag:449 "Transition: legacy overlay draw handles cement blend. Shader pass-through." = the
  transition NO-OP LANDMINE — it is on the DEAD frag; the LIVE chunk frag now does neighbor-derived blend
  (chunk :535). Don't confuse the two frags.
```

**Free chunk texture units:** 1, 2, 4, 6, 7, 8 (0 colormap, 3 cement atlas, 5 matNormalArray, 9/10 shadows, 11 transition-mask, 12 controlmap, 13 dyn map). **Terrain SSBO bindings 23 height / 24 terrainType / 25 cement / 26 visual-height** all taken.

**Gameplay safety (unchanged, load-bearing):** road movement bonus uses dedicated `road` BIT (`move.h:409` bit25, consumed `move.cpp:5026/5249`), **NOT** the visual overlay word. Visual-only changes never touch pathing. Bridges mutate overlay at runtime (`bldng.cpp:861-876`) — MUST be excluded from any static bake.

---

## 2. OVERLAY-V2-PARITY-1 — extract current bake into a generated mask, re-render ≈ old output

**Objective:** a mask/asset the new overlay system consumes that reproduces today's cement/road/runway/pad look, so the old bake becomes a **parity oracle** and a **fallback** (gate off → legacy).

### Option matrix

| Option | Carrier | Extraction source | Fidelity | Off-grid fix | Runtime cost | Verdict |
|---|---|---|---|---|---|---|
| **A. Extend control-map A-channel (concrete only)** | landed unit-12 RGBA A | offline python: `isCement` cells from pak packet0 -> rasterize A weight | Concrete FILL good; **no edge ART / no road markings / capped at controlmap res** | No (rides colormap UV grid) | zero-new (already bound) | **Partial** — good for pure pads; can't carry road/runway edge art |
| **B. Bounds-aware high-res cement/overlay SIDECAR (RGBA + world bounds vec4)** | NEW `u_overlaySidecar` (free unit 1/2/4) + `vec4 u_overlayBounds` | offline python OR editor rasterizer: composite pure cement diffuse + transition tile RGB (0.82/0.80/0.76 tint pre-baked) + road/runway art, alpha=edge mask | **HIGH** (N× res, 8-bit edge alpha, carries ART) | **YES** (explicit world bounds, tile-grid-independent) | 1 tex + 1 vec4 + ~10 frag lines; can RETIRE cement-word SSBO + atlas + transition-mask + separate overlay pass | **RECOMMENDED** (matches CEMENT-BAKE-RECON-1 §1-6) |
| C. Decal instance list (per-region rects+sub-UV, SSBO/UBO loop) | small region table + atlas | editor emits per-cement-cluster rects | HIGH, no wasted texels for disjoint pads | YES | frag loops ≤4-8 regions | **Adopt as B's disjoint-region encoding** (CEMENT-RECON §2b), not a separate option |

**Recommendation (3 bullets):**
1. **Ship B, with C as its disjoint-region encoding.** One bounds-aware RGBA sidecar per mission (or a small region-list atlas) sampled by **world XY** — decouples cement/road from the 128wu `cementWordsF` tile grid (the user's off-grid pain) and folds pure-fill + transition-edge ART + road markings + the ~18% darken into ONE baked asset. Consume it in `terrain_lod_chunk.frag` at the cement branch (~:435-540) under `u_useOverlaySidecar != 0`.
2. **PARITY-1 = extract, don't re-author.** First milestone is an **offline python** (`tools/terrain_beautify/`, reuse `mission_terrain_analyzer.py` PostcompVertex + overlay-hi16 readers) that rasterizes the sidecar from the *existing* baked pak: pure cement -> cement diffuse; `isAlpha` transition cells -> the same transition atlas tile RGB+alpha the overlay pass draws; road/runway overlay cells -> the overlay tile art. Output ≈ current composite by construction. Parity metric = SSIM/mean-abs pixel delta of static-cam ON-sidecar vs OFF-legacy on a cement-heavy mission.
3. **Gate + fallback:** `MC2_TERRAIN_OVERLAY_V2` default OFF (mirror `MC2_TERRAIN_CONTROLMAP` read at `terrain.cpp:832`). Per-mission "sidecar present" flag: absent OR gate-off -> **legacy cement-word + overlay pass verbatim (byte-identical)**. When active+present -> sample sidecar; skip cement-word upload, transition-mask bind, and the separate `drawTerrainOverlays` **for static cement only** (bridges/gates stay on the live overlay path). Uniform-branch identity, NOT "bound-but-unsampled."

**Why not colormap-writeback (root recon's original verdict):** destructive, capped at colormap texel density, inherits atlas-UV grid (no off-grid fix), and needs a NEW colormap-TGA writeback path that doesn't exist. Sidecar is non-destructive, reversible, higher-fidelity, off-grid-correct, and REUSES the composite. Keep colormap-writeback documented only as the rejected fallback.

**Control-map interplay:** control map (unit 12) owns the non-cement material *classifier* (rock/grass/dirt weights). The overlay sidecar owns the cement/road *decal* on top. Order in frag: control-map selection (chunk ~:590) BEFORE cement/sidecar composite (~:435-540 already runs after). A-channel concrete stays authoritative for the material blend; sidecar wins the pixel where alpha>=0.5. Don't double-own.

---

## 3. OVERLAY-V2-EDGE-BLEND-1 — soft edges, wear/detail, height-aware, scatter-exclusion feed

1. **Soft edge blend:** replace the binary `if(a<0.5)discard` (overlay.frag:122) / hard neighbor coverage with the sidecar's **8-bit alpha ramp** (`base = mix(base, s.rgb, s.a)`), feathered at bake time. Antialiased edges "for free" vs today's binary discard.
2. **Detail masks (wear/crack/tire/scorch):** additional sidecar channels or a second detail atlas modulating cement RGB (multiply/overlay). Author under `<stem>.beauty/`; v0 = derived (edge-distance -> wear ramp), v1 = hand-authored. These are appearance-only, ride the same sidecar sampler.
3. **Height-aware placement (displacement interaction):** the visual-height displace arc (`MC2_TERRAIN_VISUAL_DISPLACE`, binding 26, `terrain_lod_chunk.vert:33-102`) makes terrain bumpier while overlays stay flat — the **"roads vanish/z-fight on slopes"** landmine. The sidecar is sampled IN the terrain frag (rides the displaced surface Z automatically) — a structural WIN over the separate flat-triangle overlay pass. For the decals that stay geometry-based, route their Z through the existing `decalElevation` chokepoint (`terrain_runtime.h:56`, gate `MC2_TERRAIN_RUNTIME_DECALS`; VISUAL-HEIGHT-RECON S3). Ship sidecar-in-frag first (no z-fight by construction), geometry-decal reroute second.
4. **Scatter exclusion feed:** the scatter cook (`TERRAIN-SCATTER-MASK-1`, `tools/terrain_beautify/cook_scatter.py`) already reads `protected_hard = roads/concrete overlay | footprints | water` from the analyzer. Have it read the SAME overlay sidecar/mask as the exclusion source so scatter never places trees on authored roads/pads — one mask, two consumers (render + cook).

---

## 4. Landmines

1. **Two frags — don't edit the dead one.** `gos_terrain.frag:449` transition NO-OP + `quad.cpp` + `gos_terrain_indirect.cpp` are the DEAD indirect/legacy path (`MC2_TERRAIN_INDIRECT` default OFF). LIVE = `terrain_lod_chunk.frag`. All V2 shader work goes in the chunk frag.
2. **Mine overlay coexistence.** Mines draw in the shared overlay pass/layer (`quad.cpp:4103 drawMine`) at the SAME `OVERLAY_DEPTH_BIAS` band. If V2 retires the static-cement overlay draw, mines MUST stay on the live overlay path; never blanket-disable `drawTerrainOverlays`. Verify mines still render + don't z-fight the sidecar.
3. **Depth-bias lockstep (vulkan-aligned).** `OVERLAY_DEPTH_BIAS=0.00005`, reverse-Z, applied **pre-divide in the vertex stage** (`terrain_overlay.vert:41 clip4.z += BIAS*clip4.w`), single-sourced in `include/terrain_depth_bias.hglsl:61`. Sidecar-in-frag rides terrain depth (no separate bias needed). Any residual geometry decal MUST keep the exact pre-divide bias and the ordering `WATER < TERRAIN < OVERLAY` — don't reintroduce a post-divide or per-pass bias.
4. **markTerrainDrawn.** Chunk draw calls `pp->markTerrainDrawn()` — any early-return in the cement branch that skips it silently kills cloud-shadow/shoreline/godrays. Keep it unconditional.
5. **Editor authoring round-trip.** EditRel is GPU-only, same chunk draw (no CPU fallback). New sampler/uniform must bind in the editor draw too — the uniform-branch default-OFF keeps editor identical unless gate set. Bake hook = `EditorData.cpp:2179-2181` before `land->save`. Cooked/baked sidecar is a NEW sidecar file, non-destructive — editor stays authoritative on `PostcompVertex`.
6. **BC7 colormap interplay.** Colormap (unit 0) is the LIVE material classifier input; the sidecar composites OVER `base` AFTER `chunkColorWeights`/control-map selection. Don't classify from the sidecar; don't writeback into the BC7 colormap (in-editor pixel writeback is hard on the KTX2 cook path).
7. **Bridge/gate exclusion.** Static sidecar can't reflect runtime overlay mutation (destroyed bridge decal, `bldng.cpp:861-876`). Bake MUST exclude bridge/gate/destructible cells; they keep rendering via the live overlay pass. Scope the machinery-off to STATIC cement only.
8. **Gameplay `road` bit untouched.** Never derive/clear the gameplay `road` bit (`move.h:409`) from the visual overlay. Visual-only.
9. **128wu tile-grid dependence is the thing being retired** — do NOT key the sidecar on `cementWordsF` tile index; key on explicit world bounds (`sUV = (worldXY - bounds.xy)/bounds.zw`). That IS the off-grid fix.
10. **Gate-OFF byte-identity via uniform else-branch**, not "unbound texture is free." Off -> no sidecar created, `u_useOverlaySidecar=0`, else-branch = current cement/overlay code verbatim.

---

## 5. Acceptance per slice

**Common:** RelWithDebInfo, deploy exe+shaders lockstep (never exe-only when shaders change), check console (hot-reload silent-fail). Canonical tier1 smoke verbatim from CLAUDE.md.

- **PARITY-1 (offline extractor + gated frag consume):**
  - *Offline pytest:* extractor deterministic (seed -> byte-identical sidecar); untouched pak packets byte-identical (round-trip like `test_pak_append.py`); 0 sidecar coverage on bridge/gate cells.
  - *Gate-OFF:* `MC2_TERRAIN_OVERLAY_V2` unset -> tier1 exit 0, no new `crash_*`/GL errors, byte-identical (uniform else-branch).
  - *Gate-ON parity metric:* static-cam at a fixed position on a cement-heavy mission (airfield/runway) + mc2_24, sidecar-ON vs legacy-OFF -> **mean-abs pixel delta / SSIM within tolerance** (parity oracle = the legacy composite). Workbench contact sheet (`terrain_workbench.py`) sidecar vs in-engine.
  - *slice_gate:* one `--mission` with gate ON -> exit 0; `gos_push_overlay_calls`≈0 for static cement; mines + bridges still render.
- **EDGE-BLEND-1:**
  - Soft-edge screenshot (8-bit alpha ramp, no binary aliasing) vs legacy discard.
  - Displacement interaction: `MC2_TERRAIN_VISUAL_DISPLACE=1` + sidecar -> road/pad does NOT z-fight/vanish on slopes (the north-star fix); geometry-decal reroute via `decalElevation` if any remain.
  - Detail-mask visible change (author a wear/scorch channel -> only that region changes).
  - Scatter cook reads the sidecar as exclusion -> 0 scatter instances on authored roads/pads.

---

## 6. Open rulings (need user)

1. **PARITY-1 carrier:** bounds-aware sidecar (Option B, recommended) vs extend control-map A-channel (A, concrete-only, no road art)? B carries road/runway/markings; A is cheaper but can't reproduce the overlay ART.
2. **Extraction locus:** offline python from the baked pak/colormap (recommended, zero editor risk, reversible) vs editor-side rasterizer at save (`EditorData.cpp:2179`)? Offline first, editor UX later (mirrors CEMENT-RECON Phase-1 sidecar).
3. **Machinery retirement scope:** when active, retire cement-word SSBO + atlas + transition-mask + separate overlay pass for static cement (bigger cleanup, more risk) — or ADDITIVE-only v1 (sidecar composites, legacy machinery stays, dedupe later)? Recommend additive v1, retire in a later slice once parity proven.
4. **Roads/runway in v1 sidecar, or cement/pads only first?** Cement/pads is the cleanest parity target; roads add the auto-tiler art + PBR asphalt/gravel interplay (units 0/4). Recommend pads/runway first, roads follow.
5. **Detail masks (wear/crack/tire/scorch):** in-scope for EDGE-BLEND-1, or defer to a V2.1? They're appearance-only, low-risk, but add authoring surface.
6. **Displacement reroute for residual geometry decals:** activate `MC2_TERRAIN_RUNTIME_DECALS` (`decalElevation` chokepoint) this arc, or only after sidecar-in-frag proves the slope case?
7. **Sidecar file vs in-pak (Phase 2):** loose `<map>.overlaysidecar.tga`+bounds first (inspectable/deletable), repackage into `.pak`/`.fit` later (CEMENT-RECON §Phase-2)? Recommend loose first.
