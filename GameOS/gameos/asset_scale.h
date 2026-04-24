// GameOS/gameos/asset_scale.h
//
// Asset-scale-aware rendering helper. Resolves nominal (authored) vs actual
// (on-disk, possibly upscaled) texture dimensions so UV math and CPU sub-rect
// blits remain correct when loose-file overrides change asset sizes.
//
// Spec: docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md
//
// Telemetry:
//   - Always-on: startup banner, manifest-missing/bad-line warnings,
//     first oob_blit per (path, callerTag).
//   - Gated by MC2_ASSET_SCALE_TRACE=1: per-key unknown_asset lines, all
//     subsequent oob_blit lines, 600-frame summary.
//   - Self-test: MC2_ASSET_SCALE_SELFTEST=1 runs synthetic golden tests at
//     startup and prints PASS/FAIL per case.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace AssetScale {

enum class Axis { X, Y };

struct IRect { int x; int y; int w; int h; };
struct Vec2  { float x; float y; };
struct IVec2 { int   x; int   y; };

// Canonicalized asset path. Construct only via AssetScale::key().
class AssetKey {
public:
    AssetKey() = default;
    bool empty() const { return canon_.empty(); }
    const std::string& str() const { return canon_; }
    bool operator==(const AssetKey& o) const { return canon_ == o.canon_; }
private:
    explicit AssetKey(std::string canon) : canon_(std::move(canon)) {}
    friend AssetKey key(const char*);
    std::string canon_;
};

// Canonicalize: lowercase, backslash->forward, strip leading "./",
// strip leading "data/", collapse "//" -> "/".
AssetKey key(const char* rawPath);

// Lifecycle (called from gameosmain.cpp).
void init(const char* manifestPath);
void shutdown();

// All transform/lookup APIs take actualW/actualH; the manifest stores only
// nominal, so the caller (which already has the actual texture dimensions
// from the GL upload / TGA header) must supply them.
Vec2  factorFor(const AssetKey& k, uint32_t actualW, uint32_t actualH,
                const char* callerTag);

int   nominalToActualAxis(const AssetKey& k, int n, Axis axis,
                          uint32_t actualW, uint32_t actualH,
                          const char* callerTag);

IVec2 nominalToActual(const AssetKey& k, int nx, int ny,
                      uint32_t actualW, uint32_t actualH,
                      const char* callerTag);

// Primary surface. Centralizes rounding (floor-origin / ceil-extent) and
// OOB clamp. Always returns a rect inside [0,actualW] x [0,actualH].
IRect nominalToActualRect(const AssetKey& k,
                          uint32_t actualW, uint32_t actualH,
                          float nx, float ny, float nw, float nh,
                          const char* callerTag);

// True if this asset is manifest-tagged as chrome (CSV 4th field == "chrome").
// Chrome assets opt into the 1-pixel destination overlap in aObject::init to
// cover upscaler-softened seams between adjacent widgets. Icon atlases
// (mcui_*, pilot) MUST NOT be tagged chrome — the overlap cascades into
// child-widget positioning and shifts ForceGroupIcon rendering by half-width.
bool isChromeAsset(const AssetKey& k);

// Debug hotkey output (counters always accumulate, even when trace is off).
void  dumpCountersTo(FILE*);

} // namespace AssetScale
