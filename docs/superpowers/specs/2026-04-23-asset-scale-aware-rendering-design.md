# Asset-Scale-Aware Rendering — Design

**Status:** approved, ready for implementation plan
**Date:** 2026-04-23
**Owner:** terrain-pbr-mod / nifty-mendeleev worktree
**Scope:** unify the fix for two related bug classes caused by loose-file upscaled assets — atlas UV/blit garbage (mech/vehicle/pilot icons) and menu chrome seams (gaps between adjacent UI pieces). Result: GUI renders correctly regardless of source asset size.

## Problem

MC2's UI code assumes assets are at their authored dimensions. Two failure modes follow when loose-file upscales (`*_4x_gpu/`) override originals:

1. **Atlas UV / CPU-blit math** in `code/mechicon.cpp` computes `u = xIndex * unitIconX / texture->width` and `offsetX = whichMech * unitIconX`. The constants `unitIconX/Y = 32/38` (and `pilotIconX/Y = 25/36`) are nominal cell-pixels; `texture->width` grows with the upscale; the ratio breaks. Mech damage schematics render as scrambled noise. Vehicles in the post-mission screen are confirmed reproducing this. CLAUDE.md currently documents a workaround: do not upscale `mcui_high7/med4/low4.tga`.
2. **Menu chrome anchor math** places adjacent `.tga` pieces (top header bar, side columns, corner triangles) so that `neighbor.left = previous.x + previous.authored_width`. When `previous.actual_width != previous.authored_width`, the seam opens — visible black gaps in the main menu, load menu, save menu, and (worse) mech bay.

Both are the same root cause: a nominal pixel constant divided by an actual dimension that changed.

## Goals

- Vehicle/mech/pilot icons render correctly at any source asset size.
- Menu chrome assemblies have no visible gaps regardless of per-asset scale factor.
- The CLAUDE.md "do not upscale" blocklist for `mcui_high7/med4/low4.tga` can be removed.
- Future asset-size mistakes leave a fingerprint (loud first occurrence, suppressed after) instead of silent visual garbage.

## Non-goals

- Rewriting `.fit` files or moving widget layout to normalized coords. (Considered as Approach 2 in brainstorming, rejected as too broad.)
- Changing screen-resolution scaling (`Environment.screenWidth/600.f`) — that path is orthogonal and works.
- A general "asset manifest" or mod-pack format. The user plans this separately; today's manifest is one CSV with one purpose.
- Save-game versioning, loader-validation hardening — separate Tier-2 specs to follow.

## Architecture

A new subsystem **`AssetScale`** — one header, one .cpp, ~150 LOC. Pure data; no GL state, no widget knowledge, no per-frame tick.

**Three responsibilities:**

1. **Manifest loader** (once at startup). Reads `data/art/asset_sizes.csv`. Schema: `asset_path,nominal_width,nominal_height`. Canonicalizes keys at load time.
2. **Scale-factor lookup.** `factorFor(AssetKey, actualW, actualH, callerTag) -> vec2` returns `{actualW/nominalW, actualH/nominalH}`. Unknown asset returns `{1.0, 1.0}` and emits one `unknown_asset` warning per asset.
3. **Rect transform + clamp.** `nominalToActualRect(...)` centralizes floor-origin / ceil-extent rounding and OOB clamping so callsites do not reimplement rounding.

Subsystem location: `GameOS/gameos/asset_scale.{h,cpp}` (final placement decided at impl time based on include graph; mclib is a fallback if a circular include surfaces).

### Public API

```cpp
namespace AssetScale {

enum class Axis { X, Y };

struct IRect { int x, y, w, h; };

// Canonicalized asset path. Construct via AssetScale::key(rawPath).
// Raw const char* cannot be passed where AssetKey is expected — prevents
// un-canonicalized lookups at compile time.
class AssetKey {
public:
    AssetKey() = default;          // empty (== unknown)
    bool empty() const;
    const std::string& str() const;
private:
    explicit AssetKey(std::string canon);
    friend AssetKey key(const char*);
    std::string canon_;
};

AssetKey key(const char* rawPath);  // applies canonicalization rule

void init(const char* manifestPath);     // call once at startup
void shutdown();

vec2  factorFor(AssetKey, uint32_t actualW, uint32_t actualH, const char* callerTag);
int   nominalToActualAxis(AssetKey, int n, Axis, const char* callerTag);
ivec2 nominalToActual(AssetKey, int nx, int ny, const char* callerTag);
IRect nominalToActualRect(AssetKey, float nx, float ny, float nw, float nh,
                          const char* callerTag);  // primary surface; centralizes rounding + clamp

void  dumpCountersTo(FILE*);   // for debug hotkey
}
```

### Canonicalization rule (applied at both load and lookup)

1. Lowercase.
2. Backslash → forward slash.
3. Strip leading `./`.
4. Strip leading `data/`.
5. Collapse `//` → `/`.

Examples:
| Input | Canonical |
|---|---|
| `data/art/mcui_high7.tga` | `art/mcui_high7.tga` |
| `art/mcui_high7.tga` | `art/mcui_high7.tga` |
| `.\Data\Art\MCUI_HIGH7.TGA` | `art/mcui_high7.tga` |

### Rounding policy (single source of truth, documented in header)

- Nominal constants: `float`.
- Scaled origins: `floor`.
- Scaled extents (width, height, right/bottom edges): `ceil`.
- Scaled centers: `round`.

Rationale: floor-origin + ceil-extent guarantees adjacent pieces with shared edges always overlap by ≤1 px, never separate. Eliminates the gap class.

### Telemetry gating

- `MC2_ASSET_SCALE_TRACE=0` (default): silent. Counters live in module-static atomics; `AssetScale::dumpCountersTo(stdout)` callable from a debug hotkey. No log spam in release.
- `MC2_ASSET_SCALE_TRACE=1`: detailed once-per-key lines, plus a 600-frame summary listing accumulated counts.
- **One exception:** `event=oob_blit` always logs the first occurrence per `(path, callerTag)` regardless of env, because OOB means the upscaled asset is being misread *now* and would otherwise present as silent visual garbage. Subsequent OOBs are suppressed unless trace is on.

Banner at startup: `[ASSET_SCALE v1] event=manifest_loaded entries=<N> dupes=<M> bad_lines=<K>`.

OOB log line schema:
```
[ASSET_SCALE v1] event=oob_blit path=<key> actual=WxH nominal=WxH src=(x,y,w,h) clamped=(x,y,w,h) caller=<tag>
```

Schema-version grep pattern matches the project's existing `\[SUBSYS v[0-9]+\]` convention.

## Components

### 2.1 Atlas UV / blit callsites (icons)

| Callsite | File | Lines | Type | Fix |
|---|---|---|---|---|
| `MechIcon` UV draw | `code/mechicon.cpp` | 1057–1106 | UV math | wrap `unitIcon{X,Y} / texture->width` via `factorFor` |
| `MechIcon` CPU blit (×3) | `code/mechicon.cpp` | 318–393, 501–546, 795–847 | sub-rect blit | `nominalToActualRect(key, col*32, row*38, 32, 38, "mechicon.blit")` |
| `VehicleIcon` mirror | `code/mechicon.cpp` | uses `s_VehicleTextures` | mirror of MechIcon | same template; **single short follow-up commit** per user direction |
| `PilotIcon` | `code/mechicon.cpp` | uses `s_pilotTextureHandle/Width`, `pilotIcon{X,Y}=25/36` | atlas UV | same template |

**Source-path retention (load time, prevents tier mix-up):**

Today the path string is built locally at load (lines 443/445/447 for the resolution tier choice between `mcui_high7/med4/low4`), used for `file.open()`, then thrown away. Render code has no way to know which tier is loaded. We add:

```cpp
TGAFileHeader*    MechIcon::s_MechTextures = nullptr;
AssetScale::AssetKey MechIcon::s_MechTexturesKey;   // populated at load
```

Same trio for `VehicleIcon::s_VehicleTextures` and `PilotIcon::s_pilotTextureHandle`. Render reads `factorFor(s_MechTexturesKey, ...)`. No hardcoded asset paths in render code.

**UV math, written non-collapsibly so a future "simplification" pass cannot drop the scale factor:**

```cpp
const vec2  f          = AssetScale::factorFor(s_MechTexturesKey, actualW, actualH, "mechicon.uv");
const float actualCellW = unitIconX * f.x;   // nominal cell scaled to actual atlas
const float actualCellH = unitIconY * f.y;
const float u0 = xIndex * actualCellW / actualW;
const float v0 = yIndex * actualCellH / actualH;
```

The existing `+ .1f / 256.f` epsilon stays as-is for now (separate cleanup; not load-bearing).

### 2.2 Menu chrome callsites (gap class)

Single integration point: **`gui/aSystem.cpp`**, in `aObject::render` / `aBitMap::render`.

Add two opt-in fields to `aObject`:

```cpp
enum class SrcRectSpace { ActualPixels, NominalPixels };

class aObject {
    // ...existing fields...
    AssetScale::AssetKey assetKey;                         // empty = unknown, no scaling
    SrcRectSpace          srcRectSpace = SrcRectSpace::ActualPixels;
};
```

`aBitMap::render` consults `AssetScale` only when **both** `assetKey` is non-empty **and** `srcRectSpace == NominalPixels`.

| `assetKey` | `srcRectSpace` | Behavior |
|---|---|---|
| empty | ActualPixels | Untouched (legacy default — zero-risk) |
| set | ActualPixels | Counter-only (validates the asset is known; no rect transform) |
| set | NominalPixels | Full `nominalToActualRect` transform + OOB clamp + telemetry |

The `(set, ActualPixels)` middle state is the **audit mode** — lets a screen tag a chrome widget as "known asset" before deciding its rects are nominal. Useful during incremental migration.

**Migration order (one screen at a time):** main menu → load/save menu → mech bay → expand from there based on visible gaps.

Destination rect (screen-space) is untouched by this subsystem. The existing screen-resolution scale path (`Environment.screenWidth/600.f`) continues to own destination scaling.

### 2.3 Manifest

Initial `data/art/asset_sizes.csv` ships with these entries (CLAUDE.md blocklist + chrome assets visible in user screenshots). Values marked `<TBD>` are filled at implementation time by reading the original FST-archive (un-upscaled) copies — `identify` from ImageMagick on a temp extraction works. Spec locks the schema and the seed file's existence; spec does not lock the values.

```csv
# canonical key = lowercase, forward-slash, no leading ./, no leading data/
# input may include data/ prefix; loader strips it.
asset_path,nominal_width,nominal_height
art/mcui_high7.tga, <TBD>, <TBD>
art/mcui_med4.tga,  <TBD>, <TBD>
art/mcui_low4.tga,  <TBD>, <TBD>
art/mcui_high8.tga, <TBD>, <TBD>      # vehicle icon high
art/mcui_med5.tga,  <TBD>, <TBD>
art/mcui_low5.tga,  <TBD>, <TBD>
art/mcui_high2.tga, <TBD>, <TBD>      # pilot icon high (verify at impl time)
art/mcui_med2.tga,  <TBD>, <TBD>
art/mcui_low2.tga,  <TBD>, <TBD>
# main menu chrome — discover asset paths via mainmenu.fit at impl time
# load/save menu chrome — discover via load/save .fit at impl time
# mech bay chrome — discover via mechbayscreen .fit at impl time
```

## Data flow

**Startup (once):**
1. `gos_initialize` → `AssetScale::init("data/art/asset_sizes.csv")`.
2. Missing manifest = warn + run with empty map. Game still works; everything reports `unknown_asset`.

**Texture load (existing path, `MC2TextureManager` etc.):**
- Unchanged. Asset dimensions read by GL upload as today.
- Icon classes additionally store `s_*TexturesKey = AssetScale::key(path)` after the existing `file.open(path)`.

**Per-frame icon draw (UV path):**
1. `MechIcon::render` already knows `texture->width/height` and `unitIconX/Y`.
2. New: `vec2 f = AssetScale::factorFor(s_MechTexturesKey, actualW, actualH, "mechicon.uv")`.
3. UV computed as shown in 2.1.

**Per-icon CPU blit:**
1. `IRect r = AssetScale::nominalToActualRect(key, whichMech * 32, 38, 32, 38, "mechicon.blit")`.
2. Source pointer arithmetic uses `r.x, r.y, r.w, r.h`. Floor-origin/ceil-extent built in. OOB clamp emits the loud log on first hit per `(key, "mechicon.blit")`.

**Per-screen chrome render (opt-in widgets):**
1. Screen `init` populates `aObject.assetKey = AssetScale::key(path); aObject.srcRectSpace = NominalPixels;` for chrome pieces it owns.
2. `aBitMap::render` checks state, calls `nominalToActualRect` when applicable.

**Per-frame: nothing global.** No tick, no update — pure lookup.

## Error handling

- **Manifest missing**: warn at startup, run with empty map.
- **Manifest malformed line**: skip line, increment `bad_lines`, surface in `manifest_loaded` summary. Don't fail startup.
- **Asset not in manifest at lookup**: `factorFor` returns `{1.0, 1.0}`, fires `unknown_asset` once per key.
- **Computed source rect exceeds actual texture bounds**: clamp to `[0,actualW] × [0,actualH]`, fire `oob_blit` (always-on first occurrence per `(path, callerTag)`, suppressed after unless trace on).
- **Un-canonicalized lookup**: prevented at compile time by the `AssetKey` constructor being non-public; only `AssetScale::key()` produces them.
- **Counter overflow**: `uint32_t`, saturating at max. Telemetry, not control flow.

No exceptions, no asserts that fire in release. Subsystem degrades visibly to legacy behavior + leaves fingerprints, matching the project's instrumentation rule (CLAUDE.md "Debug Instrumentation Rule for reworks").

## Testing

### Golden test (automated)

Build a synthetic upscaled fake atlas in-memory, exercise each scale factor:

- **Setup:** nominal atlas 256×256, actual 512×512 (2×). Manifest entry registers nominal=256×256.
- **For every** `(xIndex, yIndex)` pair across the cell grid, assert `nominalToActualRect(key, xIndex*nominalCellW, yIndex*nominalCellH, nominalCellW, nominalCellH, "test")` returns a rect that:
  1. Covers exactly one cell in the actual texture, modulo floor/ceil rounding.
  2. Overlaps the next cell by **≤1 px** (intentional seam protection from the rounding policy; not a bug).
  3. Does not exceed actual bounds.
- **Repeat at 4×** (nominal 256×256, actual 1024×1024) and **8×** (actual 2048×2048) to catch rounding edge cases at integer scales.
- **Repeat at 1.5×** (nominal 256×256, actual 384×384) — modders will use odd sizes; this catches non-integer rounding regressions.

### Manual / visual tests (in-game)

Renderer behavior cannot be fully automated. Required visual checks:

1. Boot main menu, screenshot, eyeball-diff against pre-change screenshot. Gaps closed.
2. Load menu — same.
3. Mech bay — same.
4. Run a mission to vehicle salvage screen, verify vehicles in post-mission do not render garbage.
5. Re-deploy `mcui_high7_4x_gpu.tga` over `mcui_high7.tga`, verify in-mission HUD schematic now renders correctly. Proves the CLAUDE.md blocklist can come down.
6. With `MC2_ASSET_SCALE_TRACE=1`, confirm `[ASSET_SCALE v1]` lines appear at expected lifecycle points only (load, first OOB, 600-frame summary). No per-frame spam.

### Regression guard

`scripts/check-asset-scale-callers.sh` greps for the bug pattern `unitIcon[XY]\s*/\s*\S+->width|height` (and the pilot/vehicle equivalents) outside of `AssetScale` impl. Exit non-zero on hit. Same shape as the existing `scripts/check-destroy-invariant.sh` invariant guard.

## Implementation phasing

Surfaces in the implementation plan as a sequence of small, deployable commits:

1. `AssetScale` subsystem + manifest loader + golden tests (no callsite changes).
2. `MechIcon` adoption (UV path + 3 blit paths + `s_MechTexturesKey`). Unblocks in-mission HUD upscale.
3. `VehicleIcon` adoption (mirror of MechIcon — short commit). Unblocks post-mission vehicle render.
4. `PilotIcon` adoption.
5. `aObject` opt-in fields + `aBitMap::render` integration.
6. Main menu chrome adoption. Verify with screenshot.
7. Load/save menu chrome adoption. Verify.
8. Mech bay chrome adoption. Verify.
9. Remove CLAUDE.md "Do Not Upscale These Art Assets" section. Deploy `*_4x_gpu` overrides for the previously-blocked files.

Each commit is independently revertable. Steps 6–9 expand based on visible gaps, not a fixed list.

## Open items (not blockers)

- Final placement of `AssetScale` (`GameOS/gameos/` vs `mclib/`) decided at impl time per include graph.
- Discovery of menu chrome `.tga` paths happens during steps 6–8 by reading the corresponding `.fit` files.
- The `+ .1f / 256.f` UV epsilon in `mechicon.cpp` is left as-is; separate cleanup not in scope.
