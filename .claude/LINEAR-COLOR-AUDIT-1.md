# LINEAR-COLOR-AUDIT-1

**Instrument-only. No look change, no defaults changed.** Gate `MC2_LIGHTING_LINEAR_AUDIT=1` adds a one-shot stderr probe at scene-FBO creation (gos_postprocess.cpp ~:880). Part of the lighting arc (see [[lighting-debug-views-arc]] / LIGHTING-SYSTEM-RECON-1).

Verified on nifty worktree, deploy v0.3, mission mc2_24.

## Runtime probe result (measured, not inferred)

```
[LINEAR_AUDIT] framebuffer_srgb=0  scene_color_internalformat=0x881A (GL_RGBA16F)
               hdr_post=off  tonemap_aces=off  bloom=off
               verdict=GAMMA_SPACE_MATH
```

- `GL_FRAMEBUFFER_SRGB` = **disabled** at runtime → GL performs **no** linear→sRGB encode on write. Confirms static finding (zero `glEnable(GL_FRAMEBUFFER_SRGB)` in tree; comments at gos_static_prop_batcher.cpp:2803/3426 say "engine has no GL_FRAMEBUFFER_SRGB").
- Scene render target = **GL_RGBA16F** (HDR-capable; gos_postprocess.cpp:874). The HDR container exists but is fed gamma-encoded values.
- HDR/tonemap/bloom default **off** (`MC2_HDR_POST` master gate; gos_postprocess.cpp:279/295).

## The core problem (OBSERVED)

Lighting math runs in **sRGB-encoded (gamma) space**, not linear:
- Albedo textures sampled as sRGB internalformat but **not decoded to linear** before lighting (tools/asset_viewer/LocalPbrMaterialBackend.cpp:43/67 note the decode assumption; main path does no `pow(2.2)`).
- Diffuse/shadow/ambient combine on those gamma values: terrain `lit = baseColor * normalLight * shadow` (terrain_lod_chunk.frag:549, gos_terrain.frag), static props `c = tex * v_argb` (static_prop.frag:333).
- No tonemap/encode on output by default → values written straight to RGBA16F, presented as-is.
- ORM (roughness/metal/AO) correctly LINEAR (gos_static_prop_batcher.cpp:997 uses non-sRGB BC7) — so that half is right; the albedo/light half is wrong.

Net: multiply/add of gamma-encoded colors is mathematically wrong (darkening/over-bright vs physically-correct linear), and **PBR specular/IBL will look wrong until this is fixed**. The recon's `overbright` debug channel (now live on terrain+props) is the visual companion to this audit.

## Per-stage color-space map

| Stage | Space today | Correct for PBR |
|---|---|---|
| Albedo texture decode | sRGB sampled, **used as-is (gamma)** | decode → linear on sample (sRGB sampler / `pow(2.2)`) |
| ORM (rough/metal/AO) | linear ✅ | linear ✅ |
| Light/shadow/ambient math | **gamma** ❌ | linear |
| Scene FBO storage | RGBA16F (holds gamma values) | RGBA16F holding **linear** values |
| Output encode | none (`GL_FRAMEBUFFER_SRGB` off) ❌ | encode linear→sRGB (FB_SRGB or explicit) |
| Tonemap/exposure | ACES present but default-off | on, after linear accumulate |

## What linear correctness requires (remediation order — NOT done here)

1. **Decode albedo → linear** on sample (terrain colormap, static-prop/mech albedo). Beware MC2 terrain colormap is **pre-baked-lit** — decoding it is a look change; gate hard.
2. **Encode linear → sRGB** on final output (enable `GL_FRAMEBUFFER_SRGB` for the present, or explicit encode in the post/blit). Global look shift → full Baseline-A pixel gate every mission.
3. Keep ORM linear (already correct).
4. Only then: PBR specular / IBL / tonemap-on become meaningful.

These are mutually dependent: enabling encode without decode (or vice-versa) double-darkens/double-brightens. Must land together behind one gate (e.g. future `MC2_LIGHTING_LINEAR (gate since removed)=1`), measured against Baseline-A.

## Boundaries held
Logging-only this slice. No `GL_FRAMEBUFFER_SRGB` enable, no decode/encode, no shader math change, no default change. tier1 unaffected (probe gated + one-shot).

## Recommendation
Linear correctness is a **prerequisite gate for all PBR-adjacent work** (confirmed). It is also the single highest-risk visual change in the arc (touches every textured surface + UI + sky + post). Do it as its own dedicated slice with the full Baseline-A pixel gate, AFTER SCENE-LIGHTING-STATE-1, not folded into a feature. Use the `overbright` and `albedo` debug channels to inspect before/after.
