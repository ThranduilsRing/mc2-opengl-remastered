#pragma once
//===========================================================================//
// mclib/tgl_load_guard.h — META-FIX design B PART 1: checked helper for
// REQUIRED appearance-shape load slots (fail-loud asset-load policy).
//
// Contract (.claude/META-FIX-DESIGNS-2026-07-16.json CLASS B): every
// REQUIRED base-shape load either succeeds, or is classified + logged
// ("[ASSET_LOAD v1] event=required_missing ..." — always-on) and populates a
// VISIBLE magenta placeholder box that renders on BOTH the legacy CPU path and the GPU
// static-prop/mech batchers. The placeholder registers a REAL 4x4 magenta
// texture through mcTextureManager and sets valid handles — NOT
// numTextures=0 — because the batchers cull packets with layerForPacket=-1
// (bdactor.cpp layerForPacket evidence); a textureless placeholder would
// itself be invisible on the modern path, defeating the contract.
//
// OPTIONAL slots (arms, shadows, damage shapes, anims) take NO code change:
// a missing optional file is normal stock degradation, is not logged, and
// their delete/NULL fallbacks stay
// byte-identical. Stock installs never fail a REQUIRED base shape, so every
// branch below is cold on stock — byte-identical behavior.
//
// Static sibling: scripts/check-tgl-load-callers.sh (PART 5) — raw loader
// calls outside msl.cpp + this guard must carry a TGL-LOAD-EXEMPT/VERIFIED
// tag or the repo goes red.
//===========================================================================//

class TG_TypeMultiShape;

// Post-check form (for the opt-in import branches whose load call cannot be
// wrapped): classify `rc` + GetNumShapes()==0 as a REQUIRED-slot failure.
// On failure: (1) "[ASSET_LOAD v1] event=required_missing ctx=... path=..."
// (always-on, rate-limited); (2) MC2_VERIFY fire (mode-aware: log/fatal/off);
// (3) *failFlag = true when failFlag != NULL (AppearanceType::
// baseShapeLoadFailed); (4) under the killswitch, populate the magenta
// placeholder box into `shape`. Returns true when the shape is usable
// (loaded OR placeholder-populated), false when left empty (killswitch OFF).
bool tglVerifyShapeLoaded(TG_TypeMultiShape* shape, long rc, const char* ctx,
                          const char* assetPath, bool* failFlag);

// Wrapping form for the stock ASE call sites:
//   shape->LoadTGMultiShapeFromASE(path)  ->  tglLoadShapeChecked(shape, path, ctx, &flag)
// Calls LoadTGMultiShapeFromASE then tglVerifyShapeLoaded. Returns the
// loader rc unchanged (call sites that ignored it keep ignoring it).
long tglLoadShapeChecked(TG_TypeMultiShape* shape, const char* asePath,
                         const char* ctx, bool* failFlag);
