// gameos_graphics: the gos_* C API: renderer lifecycle, fonts, textures, screen mode, buffers, materials and the camera preview.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "KtxLoader.h"          // kKtxMaxEdge: the edge bound every pre-compressed texture shares with the KTX2 parser
#include "mc2fmt.h"
#include "present_thread.h"
#include "../../mclib/sim_overlap.h"
#include "texture_decode.h"
#include "ui_texture.h"
#include "vulkan_legacy_shape_island.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#endif
#include "utils/mc2log.h"



// GAMEOS-GRAPHICS-SPLIT-1 slice 1: gosFont implementation moved to
// gameos_graphics_font.cpp. These shims give that TU renderer access without
// exporting the gosRenderer class definition.
DWORD gosRendererAddFontBmpTexture(const char* textureName)
{
    gosTexture* ptex = new gosTexture(gos_Texture_Alpha, textureName, 0, nullptr, 0, false);
    if(!ptex || !ptex->prepareSource()) {
        STOP(("Failed to create font texture: %s\n", textureName));
    }
    return getGosRenderer()->addTexture(ptex);
}

// MERGE-WAVE-1 shim: texture fetch for the font TU (rgba-mirror publication).
gosTexture* gosRendererGetTexture(DWORD texId)
{
    return g_gos_renderer ? g_gos_renderer->getTexture(texId) : nullptr;
}

void gosRendererDeleteTexture(DWORD texId)
{
    getGosRenderer()->deleteTexture(texId);
}

//
void _stdcall gos_DrawLines(gos_VERTEX* Vertices, int NumVertices)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DrawLines");
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawLines(Vertices, NumVertices);
}
void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DrawPoints");
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawPoints(Vertices, NumVertices);
}

bool g_disable_quads = true;
void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DrawQuads");
    gosASSERT(g_gos_renderer);
    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(Vertices, NumVertices);
}
void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DrawTriangles");
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawTris(Vertices, NumVertices);
}

void __stdcall gos_GetViewport( float* pViewportMulX, float* pViewportMulY, float* pViewportAddX, float* pViewportAddY )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_GetViewport");
    gosASSERT(g_gos_renderer);
#ifdef MC2_VULKAN_ISLAND
    if (mc2_vk_preview_active()) { *pViewportMulX=800; *pViewportMulY=600; *pViewportAddX=*pViewportAddY=0; return; }
#endif
    g_gos_renderer->getViewportTransform(pViewportMulX, pViewportMulY, pViewportAddX, pViewportAddY);
}

// Stock default font the game already uses for the debug window; the fallback
// when a requested font (mod or otherwise) cannot be loaded.
static const char kFontFallbackId[] = "assets/graphics/arial8.tga";

static gosFont* gosLoadFontCached(std::string_view fontView)
{
    const std::string fontStr(fontView);
    gosRenderer* r = getGosRenderer();
    gosFont* font = r->findFont(fontStr.c_str());
    if(font) {
        font->addRef();
        return font;
    }
    font = gosFont::load(fontStr.c_str());
    if(font)
        r->addFont(font);   // never register a null font: findFont() dereferences every entry
    return font;
}

HGOSFONT3D __stdcall gos_LoadFont( const char* FontFile, DWORD StartLine/* = 0*/, int CharCount/* = 256*/, DWORD TextureHandle/*=0*/)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_LoadFont");
    if(!FontFile || !g_gos_renderer)
        return nullptr;

    gosFont* font = gosLoadFontCached(FontFile);
    if(!font && _stricmp(FontFile, kFontFallbackId) != 0) {
        MC2_LOG(Assets, Warn, "[FONT_FALLBACK v1] font '{}' failed to load; using '{}'",
                mc2::fmt::safe(FontFile), mc2::fmt::safe(kFontFallbackId));
        font = gosLoadFontCached(kFontFallbackId);
    }
    if(!font)
        MC2_LOG(Assets, Warn, "[FONT_FALLBACK v1] font '{}' failed to load and no fallback is available",
                mc2::fmt::safe(FontFile));
    return font;
}

void __stdcall gos_DeleteFont( HGOSFONT3D FontHandle )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DeleteFont");
    gosASSERT(FontHandle);
    gosFont* font = FontHandle;
    getGosRenderer()->deleteFont(font);
}

// FONT-IMGUI-SLICE-1: read-only atlas/glyph queries for the ImGui HUD bitmap
// text helper (code/hud_imgui_font.cpp). Contract documented in gameos.hpp.
bool __stdcall gos_FontGetAtlasDesc( HGOSFONT3D FontHandle, gos_FontAtlasDesc* Out )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_FontGetAtlasDesc");
    if(!FontHandle || !Out || !g_gos_renderer)
        return false;

    const gosFont* font = FontHandle;
    const DWORD tex_id = font->getTextureId();
    if(tex_id == INVALID_TEXTURE_ID)
        return false;

    gosTexture* tex = g_gos_renderer->getTexture(tex_id);
    if(!tex)
        return false;

    gosTextureInfo ti;
    tex->getTextureInfo(&ti);

    Out->textureId  = gos_GetUiTextureId(tex_id);
    Out->texWidth   = ti.width_;
    Out->texHeight  = ti.height_;
    Out->lineSkip   = font->getMaxCharHeight();
    Out->ascent     = font->getFontAscent();
    Out->maxAdvance = font->getMaxCharWidth();

    if(Out->textureId == 0 || Out->texWidth <= 0 || Out->texHeight <= 0)
        return false;

    return true;
}

bool __stdcall gos_FontGetGlyphDesc( HGOSFONT3D FontHandle, int Char, gos_FontGlyphDesc* Out )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_FontGetGlyphDesc");
    if(!FontHandle || !Out)
        return false;

    const gosFont* font = FontHandle;
    const gosGlyphInfo& gi = font->getGlyphInfo();
    const int pos = Char - static_cast<int>(gi.start_glyph_);
    if(pos < 0 || pos >= static_cast<int>(gi.num_glyphs_))
        return false;

    const gosGlyphMetrics& gm = gi.glyphs_[pos];
    Out->offX    = gm.minx;
    Out->offY    = static_cast<int>(gi.font_ascent_) - gm.maxy;
    Out->width   = gm.maxx - gm.minx;
    Out->height  = gm.maxy - gm.miny;
    Out->u       = gm.u;
    Out->v       = gm.v;
    Out->advance = gm.advance;
    return true;
}

DWORD __stdcall gos_NewEmptyTexture( gos_TextureFormat Format, const char* Name, DWORD HeightWidth, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "gos_NewEmptyTexture");   // GPU resource entry: never from the sim worker
    int w = HeightWidth;
    int h = HeightWidth;
    if(HeightWidth&0xffff0000)
    {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    gosTexture* ptex = new gosTexture(Format, Hints, w, h, Name);

    if(!ptex->prepareSource()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}
DWORD __stdcall gos_NewTextureFromMemory( gos_TextureFormat Format, const char* FileName, BYTE* pBitmap, DWORD Size, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "gos_NewTextureFromMemory");   // GPU resource entry: never from the sim worker
    gosASSERT(pFunc == 0);

    gosTexture* ptex = new gosTexture(Format, FileName, Hints, pBitmap, Size, true);
    if(!ptex->prepareSource()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}

// TEXMGR-COMPRESSED-UPLOAD-1: upload a single-level BC7 (or other block-
// compressed) 2D texture from a pre-loaded block stream and register it as a
// gosTexture handle. Mirrors gos_NewTextureFromMemory's lifecycle (gosTexture
// + addTexture), but uploads via glCompressedTexImage2D instead of decoding a
// TGA. Single level only (legacy texture manager data/textures load with
// gosHint_DisableMipmap), so GL_TEXTURE_MAX_LEVEL=0 and GL_LINEAR filtering.
// Returns INVALID_TEXTURE_ID on GL failure (caller falls through to the RGBA8
// path). The internal format is passed in by the caller (sRGB BPTC for albedo).
// Only BC7 is accepted, with exactly one level's worth of blocks; the blocks stay with the handle as its CPU texels, so other
// backends import them as they are (gos_GetTextureTexels).
DWORD __stdcall gos_NewCompressedTexture2D( uint32_t vkFormat, int w, int h,
                                            const uint8_t* blockData, size_t byteLen,
                                            const char* name )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_NewCompressedTexture2D");
    const bool bc7 = vkFormat == mc2tex::kVkFormatBc7Unorm || vkFormat == mc2tex::kVkFormatBc7Srgb;
    if(!blockData || !bc7 || w <= 0 || h <= 0 || w > static_cast<int>(RenderCore::kKtxMaxEdge) || h > static_cast<int>(RenderCore::kKtxMaxEdge) ||
       byteLen != mc2tex::bc7LevelBytes(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected compressed texture {}: format 0x{:x}, {:d}x{:d}, {:d} bytes",
                mc2::fmt::safe(name ? name : "<null>"), mc2::fmt::as_u32(vkFormat), mc2::fmt::as_i32(w),
                mc2::fmt::as_i32(h), mc2::fmt::as_size(byteLen));
        return INVALID_TEXTURE_ID;
    }
    const uint32_t blockVkFormat =
        vkFormat;

    CpuTextureDesc tex;
    tex.w = w;
    tex.h = h;
    tex.fmt_ = TF_NONE;          // block-compressed; not one of the RGBA8 TexFormats
    tex.has_mipmaps = false;

    auto blocks = std::make_shared<const std::vector<uint8_t>>(blockData, blockData + byteLen);
    gosTexture* ptex = new gosTexture(tex, gos_Texture_Solid, name, blockVkFormat, std::move(blocks));
    return g_gos_renderer->addTexture(ptex);
}

DWORD gos_NewTerrainNormalTexture(const unsigned char* rgbaData, int width)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_NewTerrainNormalTexture");
    if(!rgbaData || width <= 0 || width > static_cast<int>(RenderCore::kKtxMaxEdge)) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected terrain normal texture: width {:d}", mc2::fmt::as_i32(width));
        return INVALID_TEXTURE_ID;
    }
    CpuTextureDesc tex;
    tex.w = width;
    tex.h = width;
    tex.fmt_ = TF_RGBA8;
    tex.has_mipmaps = false;
    gosTexture* ptex = new gosTexture(tex, gos_Texture_Alpha, "terrain_detail_normal", 0u, nullptr);
    // The texels are the Vulkan terrain island's upload source and nothing else reads them.
    ptex->adoptRgbaMirror(width, width, std::vector<uint8_t>(rgbaData, rgbaData + static_cast<size_t>(width) * static_cast<size_t>(width) * 4u));
    return g_gos_renderer->addTexture(ptex);
}

DWORD __stdcall gos_NewTextureFromFile( gos_TextureFormat Format, const char* FileName, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "gos_NewTextureFromFile");   // GPU resource entry: never from the sim worker
    gosTexture* ptex = new gosTexture(Format, FileName, Hints, nullptr, 0, false);
    if(!ptex->prepareSource()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }
    return g_gos_renderer->addTexture(ptex);
}
void __stdcall gos_DestroyTexture( DWORD Handle )
{
    mc2::overlap::requireOwner(mc2::overlap::Owner::Render, "gos_DestroyTexture");   // GPU resource entry: never from the sim worker
#ifdef MC2_VULKAN_ISLAND
    if (mc2vk::present_thread::enabled()) mc2vk::present_thread::forgetTexture(Handle);
    else if (mc2vk::engineVulkanContextValid()) mc2vk::getOrCreateEngineVulkanContext().textures().forgetLegacyUi(Handle);
#endif
    g_gos_renderer->deleteTexture(Handle);
}

void __stdcall gos_LockTexture( DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_LockTexture");
    // TODO: does not really locks texture
    
    gosASSERT(MipMapSize == 0);
    int mip_level = 0; //func(MipMapSize);

    gosTextureInfo info;
    int pitch = 0;
    gosTexture* ptex = (Handle < g_gos_renderer->getTextureListSize()) ? g_gos_renderer->getTexture(Handle) : nullptr;
    if (!ptex)
    {
        // A texture that failed to build must not take the process down; the
        // caller sees an empty lock (null pixels, zero size).
        mc2::fmt::print_out("[LOCK_TEXTURE_NULL] no texture for handle {}\n", mc2::fmt::as_u32(Handle));
        *TextureInfo = TEXTUREPTR{};
        return;
    }
    ptex->getTextureInfo(&info);
    BYTE* pdata = ptex->Lock(mip_level, ReadOnly, &pitch);

    TextureInfo->pTexture = reinterpret_cast<DWORD*>(pdata);
    TextureInfo->Width = info.width_;
    TextureInfo->Height = info.height_;
    TextureInfo->Pitch = pitch;
    TextureInfo->Type = info.format_;

}

void __stdcall gos_UnLockTexture( DWORD Handle )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_UnLockTexture");
    // Pairs with gos_LockTexture: a handle that gave an empty lock has nothing to release.
    gosTexture* ptex = (Handle < g_gos_renderer->getTextureListSize()) ? g_gos_renderer->getTexture(Handle) : nullptr;
    if (!ptex)
        return;
    ptex->Unlock();

}

void __stdcall gos_PushRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->pushRenderStates();
} 

void __stdcall gos_PopRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->popRenderStates();
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(pVertexArray, NumberVertices, lpwIndices, NumberIndices);
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX_2UV* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
   gosASSERT(0 && "not implemented");
}





void __stdcall gos_SetRenderState( gos_RenderState RenderState, int Value )
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_SetRenderState");
    gosASSERT(g_gos_renderer);
    // gos_BlendDecal mode is not suported (currently texture color always modulated with vertex color)
    //gosASSERT(RenderState!=gos_State_TextureMapBlend || (Value == gos_BlendDecal));
    g_gos_renderer->setRenderState(RenderState, Value);
}

// [HUD-RES-CLAMP v1] runtime gate. The Mission Editor disables it via
// gos_SetHudResClampEnabled(false) so it renders at native window resolution;
// clamping the editor's render base to 800x600 desyncs the GL viewport
// (gos_GetViewport) from the MFC window/mouse space and corrupts editor
// object pick + drag-move projection. Runtime gate (not #ifdef MC2_IS_EDITOR)
// because gameos_graphics.cpp is compiled into the gameos_editor library
// WITHOUT that define (GameOS/gameos/CMakeLists.txt:104).
//
// ui-phase1 investigation update: tried flipping this default OFF to make
// Environment.screenWidth/Height reflect the real resolution for the new
// defs/ImGui pages. REVERTED -- adversarial+meta review (multi-agent, see
// session notes) found several OTHER legacy files (mechicon.cpp,
// keyboardref.cpp, loadscreen.cpp) branch on Environment.screenWidth==800/
// 640/etc as a TUNED-RESOLUTION discriminator, and other code
// (mechcmd2.cpp:2988-2989, logmain.cpp:780-781) independently re-hardcodes
// 800x600/640x480 elsewhere in the same state machine regardless of this
// gate -- so flipping this default doesn't reliably help and risks breaking
// those other tuned-resolution branches. Real fix for the mech-preview-panel
// scale bug: render the legacy MLR preview into its own fixed 800x600
// offscreen FBO (untouched legacy math, Environment.screenWidth stays 800
// exactly as every other legacy system already expects) and composite that
// texture into the ImGui panel via GuiRuntime::DrawUiImage with UV cropping
// -- see SimpleCamera::render()/mechlopedia.cpp. Default back to ON.
static bool g_hudResClampEnabled = true;
void __stdcall gos_SetHudResClampEnabled( bool enabled )
{
    g_hudResClampEnabled = enabled;
}

// META-FIX design D (MC2_RES_ASSERT): read side of the clamp gate — the
// per-frame audit asserts "clamp enabled => Environment.screenWidth/Height
// == 800x600" (the HUD-RES-CLAMP merge-drop class).
bool __stdcall gos_GetHudResClampEnabled()
{
    return g_hudResClampEnabled;
}

// META-FIX design D: fresh drawable probe for the audit's staleness check.
bool __stdcall gos_ProbeDrawableSize( int* w, int* h )
{
    if (!g_gos_renderer)
        return false;
    return g_gos_renderer->probeDrawableSize(w, h);
}

void __stdcall gos_SetScreenMode( DWORD Width, DWORD Height, DWORD bitDepth/*=16*/, DWORD Device/*=0*/, bool disableZBuffer/*=0*/, bool AntiAlias/*=0*/, bool RenderToVram/*=0*/, bool GotoFullScreen/*=0*/, int DirtyRectangle/*=0*/, bool GotoWindowMode/*=0*/, bool EnableStencil/*=0*/, DWORD Renderer/*=0*/)
{
    ZoneScopedN("gos_SetScreenMode");
    gosASSERT(g_gos_renderer);
    gosASSERT((GotoFullScreen && !GotoWindowMode) || (!GotoFullScreen&&GotoWindowMode) || (!GotoFullScreen&&!GotoWindowMode));

    // [HUD-RES-CLAMP v1] The legacy 2D HUD/UI is authored only for the discrete
    // tuned widths {640,800,1024,1280,1600,1920} (ControlGui::swapResolutions).
    // A non-tuned width (e.g. 2560/4096 — what the in-game options menu writes
    // on a hi-dpi desktop, see options_cfg_resolution_drift) lands on the
    // untuned else path and breaks the HUD (pause top-right, loading top-left,
    // comms-video misposition, double-shrunk bottom bar) while the scene stays
    // fine. We render the whole frame at the canonical 800x600 base and let
    // FULLSCREEN_DESKTOP upscale it — identical to the known-good shipped 4K
    // config (options.cfg ResolutionX=800). This makes the HUD immune to
    // whatever the options menu writes. (A native-scene + 800-HUD lane split is
    // deferred to the incoming imgui UI; it would touch camera+input.)
    // Memory: hud_scene_resolution_separation.
    // EDITOR: gated off (gos_SetHudResClampEnabled(false)) — the editor has no
    // legacy 2D HUD (ImGui) and must render at native res so pick/drag align.
    // WINDOWED-8006-1 (issue #49): MC2_WINDOWED wins over every later
    // fullscreen request too — prefs.applyPrefs() re-issues SetScreenMode
    // with GotoFullScreen from options.cfg, which was overriding the boot
    // override and bouncing the launcher's Windowed toggle back to
    // fullscreen.
    {
        static int s_forceWindowed = -1;
        if (s_forceWindowed < 0) {
            s_forceWindowed = mc2::gate::get<"MC2_WINDOWED">();
        }
        if (s_forceWindowed) {
            GotoFullScreen = false;
            GotoWindowMode = true;
        }
    }

    // WINDOWED-8006-1 (issue #49): clamped path keeps the PHYSICAL window at
    // its current (boot/options.cfg) size — phys 0,0 = keep — while the game's
    // logical canvas goes to 800x600. Unclamped (editor) resizes for real.
    DWORD physW = 0, physH = 0;
    if (g_hudResClampEnabled)
    {
        Width  = 800;
        Height = 600;
    }
    else
    {
        physW = Width;
        physH = Height;
    }

    g_gos_renderer->setScreenMode(Width, Height, bitDepth, GotoFullScreen, AntiAlias, physW, physH);
}

// [FORCE-43 v1] Centered 4:3 pillarbox rect. Single source of truth shared by
// the scene composite letterbox, the mouse box-relative remap, and HUD viewport.
bool __stdcall gos_Compute43Box( int w, int h, int* ox, int* oy, int* obw, int* obh )
{
    // Default-OFF gate (matches the launcher toggle model: checked -> MC2_FORCE_43=1,
    // unchecked -> unset). ON only when the var is present and not "0". Unset ->
    // byte-identical legacy (full surface), so smoke without the launcher is a no-op.
    static const bool s_on = mc2::gate::get<"MC2_FORCE_43">();
    if (!s_on) {                            // gate off -> full surface, no-op
        if (ox) *ox = 0;
        if (oy) *oy = 0;
        if (obw) *obw = w;
        if (obh) *obh = h;
        return false;
    }
    // CAMERA-FRUSTUM-HARNESS-1: pure rect math delegated to camera_frustum_math.h
    // (same arithmetic), exercised game-free by tools/camera_frustum_harness/.
    return camera_frustum_math::compute43BoxRect(w, h, ox, oy, obw, obh);
}

void __stdcall gos_SetupViewport( bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil/*=0*/, DWORD StencilValue/*=0*/)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_SetupViewport");
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setupViewport(FillZ, ZBuffer, FillBG, BGColor, top, left, bottom, right, ClearStencil, StencilValue);
}


void __stdcall gos_SetRenderViewport(float x, float y, float w, float h)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_SetRenderViewport");
    gosASSERT(g_gos_renderer);
	// IN-MISSION-DETACH-1: store-only, EXACTLY like the nifty base (the
	// glViewport call has been commented out there since the GL port). The
	// UI-phase1 merge armed it with a HiDPI-scaled glViewport, which stomps
	// the GL viewport for every in-mission caller — terrain (own MVP
	// dispatch) and TGL objects/water (viewport-transformed) desync, so
	// props/mechs/trees/water visibly slide off the terrain while panning.
	// The defs/ImGui UI never needed this: it scales via
	// GuiRuntime::GetDisplaySize, and the preview FBO sets its own viewport.
	g_gos_renderer->setRenderViewport(vec4(x, y, w, h));
}


size_t __stdcall gos_GetMachineInformation( MachineInfo mi, int Param1/*=0*/, int Param2/*=0*/, int Param3/*=0*/, int Param4/*=0*/)
{
    // TODO:
    if(mi == gos_Info_GetDeviceLocalMemory)
        return 1024*1024*1024;
    if(mi == gos_Info_GetDeviceAGPMemory)
        return 512*1024*1024; 
    if (mi == gos_Info_CanMultitextureDetail)
        return true;
    if(mi == gos_Info_NumberDevices)
        return 1;
    if(mi == gos_Info_GetDeviceName) {
#ifdef MC2_VULKAN_ISLAND
        return mc2vk::engineVulkanContextValid()
            ? reinterpret_cast<size_t>(mc2vk::getOrCreateEngineVulkanContext().health().deviceName) : 0;
#else
        return 0;
#endif
    }
    if(mi == gos_Info_ValidMode) {
        int xres = Param2;
        int yres = Param3;
        int bpp = Param4;
        return graphics::is_mode_supported(xres, yres, bpp) ? 1 : 0;
    }
    if(mi == gos_Info_GetIMECaretStatus)
        return 1;

    return 0;
}

int gos_GetWindowDisplayIndex()
{   
    gosASSERT(g_gos_renderer);
    
    return graphics::get_window_display_index(g_gos_renderer->getRenderWindowHandle());
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}









gosBuffer* __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage, int element_size, uint32_t count, void* buffer_data)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_CreateBuffer");
    if (element_size <= 0 || count > SIZE_MAX / static_cast<size_t>(element_size)) return nullptr;
    auto* buffer = new gosBuffer;
    buffer->element_size_ = element_size;
    buffer->count_ = count;
    buffer->type_ = type;
    buffer->usage_ = usage;
    buffer->bytes_.resize(static_cast<size_t>(element_size) * count);
    if (buffer_data) std::memcpy(buffer->bytes_.data(), buffer_data, buffer->bytes_.size());
    return buffer;
}


void __stdcall gos_DestroyBuffer(gosBuffer* buffer)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DestroyBuffer");
    delete buffer;
}




HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD* records, int count)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_CreateVertexDeclaration");
	gosASSERT(records && count > 0);
    gosASSERT(g_gos_renderer);
	gosVertexDeclaration* vdecl = gosVertexDeclaration::create(records, count);
	g_gos_renderer->addVertexDeclaration(vdecl);
	return vdecl;
}

void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION vdecl)
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_DestroyVertexDeclaration");
	gosASSERT(vdecl);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteVertexDeclaration(vdecl);
    (void)rv;
	gosVertexDeclaration::destroy(vdecl);

}


// RENDER_STATES v1: invalidation hook for fast paths that disturb GL state outside
// of applyRenderStates() (terrain bridges, water fast path, post-process composite,
// shadow direct draws, indirect terrain). Callers MUST call this AFTER finishing
// their direct-GL work so the next applyRenderStates() does a full re-apply.
void __stdcall gos_InvalidateRenderStateCache() {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_InvalidateRenderStateCache");
    if (!g_gos_renderer) return;
    g_gos_renderer->invalidateRenderStateCache();
}

// Called from GuiRuntime::Render() just before post-ImGui 3D camera callbacks.
// Restores GL state that shadow sub-passes leave dirty and that applyRenderStates()
// has no tracking for (glColorMask, scissor test, FB binding).


// ---------------------------------------------------------------------------
// Camera preview FBO — used by SimpleCamera to render 3D panel views
// (encyclopedia, options gameplay) into a clean offscreen buffer so they
// are unaffected by the scene's reverse-Z depth state or gosPostProcess
// lifecycle.
//
// Native previews retain the legacy virtual canvas and explicit raster scale.
static int camPreviewScale()
{
    static int s_scale = -1;
    if (s_scale < 0) {
        s_scale = 4;
        if (mc2::gate::isNonEmpty<"MC2_PREVIEW_FBO_SCALE">()) {
            const int n = mc2::gate::get<"MC2_PREVIEW_FBO_SCALE">();
            if (n >= 1 && n <= 8) s_scale = n;
        }
    }
    return s_scale;
}

void __stdcall gos_BeginCameraPreviewRender()
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_BeginCameraPreviewRender");
    if (!g_gos_renderer) return;
    const int sc = camPreviewScale();
#ifdef MC2_VULKAN_ISLAND
    if (mc2vk::uiTextureServiceReady()) { mc2_vk_preview_begin(800*sc,600*sc); return; }
#endif
    // Preview callers wait for the native renderer to be ready.
}

void __stdcall gos_EndCameraPreviewRender()
{
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "gos_EndCameraPreviewRender");
#ifdef MC2_VULKAN_ISLAND
    if (mc2_vk_preview_active()) { mc2_vk_preview_end(); return; }
#endif
}
