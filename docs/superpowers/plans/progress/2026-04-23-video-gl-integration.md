# Video GL Integration Decision — 2026-04-23

## Decision: Path GL-A

gos_NewEmptyTexture + gos_LockTexture per-frame is fully implemented in this port.
Tasks 9, 10, and 11 use only gos-owned handles. No raw GL in the video layer.

---

## Evidence

### Step 1 — gos_DrawQuads is available and widely used

`gos_DrawQuads` is present throughout the codebase. Canonical screen-space textured quad pattern:

```cpp
// code/gametacmap.cpp:135-170  (also code/gamecam.cpp:290-338)
gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero );
gos_SetRenderState( gos_State_Specular, FALSE );
gos_SetRenderState( gos_State_AlphaTest, FALSE );
gos_SetRenderState( gos_State_Filter, gos_FilterNone );
gos_SetRenderState( gos_State_ZWrite, 0 );
gos_SetRenderState( gos_State_ZCompare, 0 );
gos_SetRenderState( gos_State_Texture, textureHandle );  // DWORD handle

gos_VERTEX corners[4];
// ... fill x,y (screen pixels), z=0, rhw=1, argb=0xffffffff, frgb=0, u,v ...
gos_DrawQuads( corners, 4 );
```

`gos_VERTEX` layout (`GameOS/include/gameos.hpp:2131-2140`):
- `float x, y` — screen pixels (0 .. screenWidth/Height)
- `float z` — 0.0 to 0.99999
- `float rhw` — 1.0 for screen-space (no perspective)
- `DWORD argb` — 0xFFFFFFFF for full-bright, unmodulated
- `DWORD frgb` — specular/fog, set 0
- `float u, v` — 0..1 UV

### Step 2 — gos_NewEmptyTexture is implemented and supports rectangular textures

`GameOS/gameos/gameos_graphics.cpp:3669-3686`:
```cpp
DWORD __stdcall gos_NewEmptyTexture(
    gos_TextureFormat Format,   // gos_Texture_Alpha for RGBA
    const char* Name,           // debug name, can be NULL
    DWORD HeightWidth,          // width | (height<<16) via RECT_TEX macro for non-square
    DWORD Hints = 0,
    gos_RebuildFunction pFunc = 0,
    void* pInstance = 0
)
```

`RECT_TEX(width, height)` macro packs non-square dims: `((height)<<16)|(width)`.
Returns `DWORD` handle (INVALID_TEXTURE_ID on failure).

### Step 3 — gos_LockTexture / gos_UnLockTexture are fully implemented

`GameOS/gameos/gameos_graphics.cpp:3714-3743`:
```cpp
void __stdcall gos_LockTexture(
    DWORD Handle,
    DWORD MipMapSize,         // must be 0
    bool ReadOnly,            // false = will upload on Unlock
    TEXTUREPTR* TextureInfo   // out: pTexture, Width, Height, Pitch, Type
);

void __stdcall gos_UnLockTexture( DWORD Handle );
```

`TEXTUREPTR` (`GameOS/include/gameos.hpp:2895-2902`):
```cpp
typedef struct {
    DWORD* pTexture;        // pointer to BGRA pixel data (allocated by gos internally)
    DWORD  Width;           // width in pixels
    DWORD  Height;          // height in pixels
    DWORD  Pitch;           // header says "in DWORDs"; implementation sets tex_.w;
                            //   all existing callsites use .Width for row stride, not .Pitch
    gos_TextureFormat Type;
} TEXTUREPTR;
```

**Pixel format note:** Lock allocates an internal BGRA8 buffer, does a `getTextureData`
readback, and swizzles RGBA→BGRA before returning the pointer. Unlock swizzles
BGRA→RGBA and calls `updateTexture()` (which calls glTexSubImage2D internally).
When writing video pixels into the locked buffer, write **BGRA byte order**:
byte layout `[blue][green][red][alpha]` = DWORD `0xAARRGGBB`.

sws_scale target format must be `AV_PIX_FMT_BGRA` (not RGBA).

**Pitch note:** Use `textureData.Width` (pixels) for row stride arithmetic, not
`textureData.Pitch`. All existing callsites (mechicon.cpp, controlgui.cpp,
abutton.cpp) use `.Width`. The `.Pitch` field is set to the same value as width
with no bytes-per-pixel multiplier.

### Concrete per-frame texture update example

`code/mechicon.cpp:343-398` — updates mech damage schematic icons every render tick:

```cpp
// Allocation (once, at init):
s_textureHandle[textureIndex] = gos_NewTextureFromMemory(
    gos_Texture_Alpha, ".tga", pBitmap, size, 0);

// Update (per-frame or per-event):
TEXTUREPTR textureData;
gos_LockTexture( s_textureHandle[textureIndex], 0, 0, &textureData );

DWORD* pDestRow = textureData.pTexture + offsetY * textureData.Width + offsetX;
for (int j = 0; j < unitIconY; ++j) {
    // write DWORD pixels into pDestRow ...
    pDestRow += textureData.Width;  // stride in DWORDs
}

gos_UnLockTexture( s_textureHandle[textureIndex] );
```

For video, replace `gos_NewTextureFromMemory` with `gos_NewEmptyTexture` since we
do not have a TGA to hand — we just want a blank GPU texture to fill each frame.

---

## API Contract for Tasks 9, 10, 11

### Task 9 — Allocate per-session texture

```cpp
// At session open (once per video clip):
DWORD videoTexHandle = gos_NewEmptyTexture(
    gos_Texture_Alpha,                   // RGBA, alpha channel present
    "video_frame",                       // debug name
    RECT_TEX(videoWidth, videoHeight),   // pack w/h; RECT_TEX macro from gameos.hpp
    gosHint_DisableMipmap                // no mipmaps on a video frame texture
);
// Store videoTexHandle (DWORD) for lifetime of the video session.
```

### Task 10 — Upload decoded frame

```cpp
// Each decoded frame (sws_scale into a temp BGRA buffer first):
TEXTUREPTR tp;
gos_LockTexture( videoTexHandle, 0, false, &tp );

// tp.pTexture is BGRA. Row stride = tp.Width DWORDs.
// sws_scale output must be AV_PIX_FMT_BGRA, linesize[0] = videoWidth*4.
// Either scale directly into tp.pTexture, or memcpy row-by-row:
const uint8_t* src = avFrame->data[0];
DWORD* dst = tp.pTexture;
for (int y = 0; y < videoHeight; ++y) {
    memcpy(dst, src, videoWidth * sizeof(DWORD));
    dst += tp.Width;      // use .Width, not .Pitch
    src += avFrame->linesize[0];
}

gos_UnLockTexture( videoTexHandle );
```

### Task 11 — Draw fullscreen quad

```cpp
// At draw time (inside the MC2 render loop, after scene, before HUD flip):
gos_PushRenderStates();

gos_SetRenderState( gos_State_AlphaMode,       gos_Alpha_OneZero );
gos_SetRenderState( gos_State_Specular,        FALSE );
gos_SetRenderState( gos_State_AlphaTest,       FALSE );
gos_SetRenderState( gos_State_ZWrite,          0 );
gos_SetRenderState( gos_State_ZCompare,        0 );
gos_SetRenderState( gos_State_Filter,          gos_FilterBiLinear );
gos_SetRenderState( gos_State_TextureAddress,  gos_TextureClamp );
gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate );
gos_SetRenderState( gos_State_Texture,         videoTexHandle );

gos_VERTEX v[4];
// corners: (0,0) top-left → (screenW,0) top-right → (screenW,screenH) → (0,screenH)
// triangle-strip quad — gos_DrawQuads expects 4 vertices in fan order:
//   v[0]=TL, v[1]=TR, v[2]=BL, v[3]=BR  (check gametacmap.cpp:155-167 for reference)
const float sw = (float)Environment.screenWidth;
const float sh = (float)Environment.screenHeight;
v[0] = { 0,  0,  0.f, 1.f, 0xffffffff, 0, 0.f, 0.f };
v[1] = { sw, 0,  0.f, 1.f, 0xffffffff, 0, 1.f, 0.f };
v[2] = { 0,  sh, 0.f, 1.f, 0xffffffff, 0, 0.f, 1.f };
v[3] = { sw, sh, 0.f, 1.f, 0xffffffff, 0, 1.f, 1.f };
gos_DrawQuads( v, 4 );

gos_PopRenderStates();
```

### Task 9 — Cleanup

```cpp
// At session close:
gos_DestroyTexture( videoTexHandle );
videoTexHandle = INVALID_TEXTURE_ID;
```

---

## Why not GL-B or GL-C

**GL-B rejected:** `gosTexture::setGLName` / `gos_GetTextureName` do not exist in
this port. The internal `tex_.id` (GLuint) is entirely private — there is no
back-channel to inject a raw GL texture name into the gos handle system. The only
public back-channel grep found was `gos_static_prop_killswitch.h` which resolves
*from* gos *to* GL for reading (one-way, internal to the shadow batcher).

**GL-C rejected:** `gos_DrawQuads` is available, works, and is the established
screen-space draw primitive. Bypassing it with a raw VBO+shader would require
saving/restoring gos render state (unknown set), writing a new vertex shader
(`#version 430` prefix required per CLAUDE.md), and adding a dedicated VBO
lifecycle — all unnecessary overhead when the lock/unlock upload path works.

---

## References

| Symbol | File | Lines |
|--------|------|-------|
| `gos_NewEmptyTexture` impl | `GameOS/gameos/gameos_graphics.cpp` | 3669–3686 |
| `gos_LockTexture` impl | `GameOS/gameos/gameos_graphics.cpp` | 3714–3735 |
| `gos_UnLockTexture` impl | `GameOS/gameos/gameos_graphics.cpp` | 3737–3743 |
| `gosTexture::Lock` (BGRA swap) | `GameOS/gameos/gameos_graphics.cpp` | 794–827 |
| `gosTexture::Unlock` (upload) | `GameOS/gameos/gameos_graphics.cpp` | 829–851 |
| `gos_VERTEX` struct | `GameOS/include/gameos.hpp` | 2131–2140 |
| `TEXTUREPTR` struct | `GameOS/include/gameos.hpp` | 2895–2902 |
| `gos_NewEmptyTexture` sig | `GameOS/include/gameos.hpp` | 2959 |
| `gos_LockTexture` sig | `GameOS/include/gameos.hpp` | 2975 |
| `gos_UnLockTexture` sig | `GameOS/include/gameos.hpp` | 2980 |
| `RECT_TEX` macro | `GameOS/include/gameos.hpp` | 2937 |
| mechicon per-frame lock example | `code/mechicon.cpp` | 343–398 |
| gametacmap quad draw example | `code/gametacmap.cpp` | 133–171 |
