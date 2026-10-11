// gameos_graphics: the gos_* C API: renderer lifecycle, fonts, textures, screen mode, buffers, materials and the camera preview.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"



// GAMEOS-GRAPHICS-SPLIT-1 slice 1: gosFont implementation moved to
// gameos_graphics_font.cpp. These shims give that TU renderer access without
// exporting the gosRenderer class definition.
DWORD gosRendererAddFontBmpTexture(const char* textureName)
{
    gosTexture* ptex = new gosTexture(gos_Texture_Alpha, textureName, 0, nullptr, 0, false);
    if(!ptex || !ptex->createHardwareTexture()) {
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
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawLines(Vertices, NumVertices);
}
void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawPoints(Vertices, NumVertices);
}

bool g_disable_quads = true;
void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(Vertices, NumVertices);
}
void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawTris(Vertices, NumVertices);
}

void __stdcall gos_GetViewport( float* pViewportMulX, float* pViewportMulY, float* pViewportAddX, float* pViewportAddY )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->getViewportTransform(pViewportMulX, pViewportMulY, pViewportAddX, pViewportAddY);
}

HGOSFONT3D __stdcall gos_LoadFont( const char* FontFile, DWORD StartLine/* = 0*/, int CharCount/* = 256*/, DWORD TextureHandle/*=0*/)
{

    gosFont* font = getGosRenderer()->findFont(FontFile);
    if(!font) {
        font = gosFont::load(FontFile);
        getGosRenderer()->addFont(font);
    } else {
        font->addRef();
    }

    return font;
}

void __stdcall gos_DeleteFont( HGOSFONT3D FontHandle )
{
    gosASSERT(FontHandle);
    gosFont* font = FontHandle;
    getGosRenderer()->deleteFont(font);
}

// FONT-IMGUI-SLICE-1: read-only atlas/glyph queries for the ImGui HUD bitmap
// text helper (code/hud_imgui_font.cpp). Contract documented in gameos.hpp.
bool __stdcall gos_FontGetAtlasDesc( HGOSFONT3D FontHandle, gos_FontAtlasDesc* Out )
{
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

    Out->glTexId    = tex->getTextureId();
    Out->texWidth   = ti.width_;
    Out->texHeight  = ti.height_;
    Out->lineSkip   = font->getMaxCharHeight();
    Out->ascent     = font->getFontAscent();
    Out->maxAdvance = font->getMaxCharWidth();

    if(Out->glTexId == 0 || Out->texWidth <= 0 || Out->texHeight <= 0)
        return false;

    // Pin the atlas sampling params to what every legacy text draw sets
    // (gos_FilterNone -> NEAREST, gos_TextureClamp -> CLAMP_TO_EDGE). The
    // legacy path re-applies these per draw via applyRenderStates; the
    // ImGui path binds the texture directly, so make them sticky here.
    GLint prev_bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
    glBindTexture(GL_TEXTURE_2D, Out->glTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);

    return true;
}

bool __stdcall gos_FontGetGlyphDesc( HGOSFONT3D FontHandle, int Char, gos_FontGlyphDesc* Out )
{
    if(!FontHandle || !Out)
        return false;

    const gosFont* font = FontHandle;
    const gosGlyphInfo& gi = font->getGlyphInfo();
    const int pos = Char - (int)gi.start_glyph_;
    if(pos < 0 || pos >= (int)gi.num_glyphs_)
        return false;

    const gosGlyphMetrics& gm = gi.glyphs_[pos];
    Out->offX    = gm.minx;
    Out->offY    = (int)gi.font_ascent_ - gm.maxy;
    Out->width   = gm.maxx - gm.minx;
    Out->height  = gm.maxy - gm.miny;
    Out->u       = gm.u;
    Out->v       = gm.v;
    Out->advance = gm.advance;
    return true;
}

DWORD __stdcall gos_NewEmptyTexture( gos_TextureFormat Format, const char* Name, DWORD HeightWidth, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    int w = HeightWidth;
    int h = HeightWidth;
    if(HeightWidth&0xffff0000)
    {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    gosTexture* ptex = new gosTexture(Format, Hints, w, h, Name);

    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}
DWORD __stdcall gos_NewTextureFromMemory( gos_TextureFormat Format, const char* FileName, BYTE* pBitmap, DWORD Size, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosASSERT(pFunc == 0);

    gosTexture* ptex = new gosTexture(Format, FileName, Hints, pBitmap, Size, true);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}

// TEXMGR-COMPRESSED-UPLOAD-1: upload a single-level BC7 (or other block-
// compressed) 2D texture from a pre-loaded block stream and register it as a
// gosTexture handle. Mirrors gos_NewTextureFromMemory's lifecycle (gosTexture
// + addTexture), but uploads via glCompressedTexImage2D instead of decoding a
// TGA. Single level only (MC_TextureManager data/textures load with
// gosHint_DisableMipmap), so GL_TEXTURE_MAX_LEVEL=0 and GL_LINEAR filtering.
// Returns INVALID_TEXTURE_ID on GL failure (caller falls through to the RGBA8
// path). The internal format is passed in by the caller (sRGB BPTC for albedo).
DWORD __stdcall gos_NewCompressedTexture2D( uint32_t glInternalFormat, int w, int h,
                                            const uint8_t* blockData, size_t byteLen,
                                            const char* name )
{
    if(!blockData || byteLen == 0 || w <= 0 || h <= 0)
        return INVALID_TEXTURE_ID;

    while(glGetError() != GL_NO_ERROR) { /* drain */ }

    GLuint texID = 0;
    // TEX-CLASS: asset-pool -- gos_NewCompressedTexture2D content factory (BC7)
    glGenTextures(1, &texID);
    if(texID == 0)
        return INVALID_TEXTURE_ID;
    glBindTexture(GL_TEXTURE_2D, texID);

    // data/textures BC7 sidecars are single-level (DisableMipmap); match the
    // RGBA8 path's filtering (GL_LINEAR/no-mip) and cap the mip ladder.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glCompressedTexImage2D(GL_TEXTURE_2D, 0, (GLenum)glInternalFormat,
                           w, h, 0, (GLsizei)byteLen, blockData);

    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if(err != GL_NO_ERROR) {
        mc2::fmt::print_out("[TEXMGR_BC7] glCompressedTexImage2D failed name={} {:d}x{:d} fmt=0x{:x} err={}\n", mc2::fmt::safe(name ? name : "<null>"), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::as_u32(glInternalFormat), mc2::fmt::safe(ogl_get_error_code_str(err)));
        glDeleteTextures(1, &texID);
        return INVALID_TEXTURE_ID;
    }

    Texture tex;
    tex.id = texID;
    tex.w = w;
    tex.h = h;
    tex.fmt_ = TF_NONE;          // block-compressed; not one of the RGBA8 TexFormats
    tex.type_ = TT_2D;
    tex.format = (GLenum)glInternalFormat;
    tex.has_mipmaps = false;

    gosTexture* ptex = new gosTexture(tex, gos_Texture_Solid, name);
    return g_gos_renderer->addTexture(ptex);
}

DWORD __stdcall gos_NewTextureFromFile( gos_TextureFormat Format, const char* FileName, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosTexture* ptex = new gosTexture(Format, FileName, Hints, nullptr, 0, false);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }
    return g_gos_renderer->addTexture(ptex);
}
void __stdcall gos_DestroyTexture( DWORD Handle )
{
    g_gos_renderer->deleteTexture(Handle);
}

void __stdcall gos_LockTexture( DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo )
{
    // TODO: does not really locks texture
    
    gosASSERT(MipMapSize == 0);
    int mip_level = 0; //func(MipMapSize);

    gosTextureInfo info;
    int pitch = 0;
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->getTextureInfo(&info);
    BYTE* pdata = ptex->Lock(mip_level, ReadOnly, &pitch);

    TextureInfo->pTexture = (DWORD*)pdata;
    TextureInfo->Width = info.width_;
    TextureInfo->Height = info.height_;
    TextureInfo->Pitch = pitch;
    TextureInfo->Type = info.format_;

}

void __stdcall gos_UnLockTexture( DWORD Handle )
{
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->Unlock();

}

uint32_t __stdcall gos_GetTextureGLId( DWORD Handle )
{
    // gosRenderer::getTexture asserts on INVALID_TEXTURE_ID and
    // out-of-range handles; in release builds gosASSERT is a no-op so
    // the out-of-range case is UB. Pre-validate the obvious invalid
    // cases so the documented "returns 0 for invalid handle" contract
    // holds for callers that may probe with a sentinel.
    if(!g_gos_renderer || Handle == INVALID_TEXTURE_ID)
        return 0;
    gosTexture* tex = g_gos_renderer->getTexture( Handle );
    return tex ? tex->getTextureId() : 0;
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

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl, mvp);
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl);
}

void __stdcall gos_SetRenderState( gos_RenderState RenderState, int Value )
{
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
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setupViewport(FillZ, ZBuffer, FillBG, BGColor, top, left, bottom, right, ClearStencil, StencilValue);
}


void __stdcall gos_SetRenderViewport(float x, float y, float w, float h)
{
    gosASSERT(g_gos_renderer);
	// IN-MISSION-DETACH-1: store-only, EXACTLY like the nifty base (the
	// glViewport call has been commented out there since the GL port). The
	// UI-phase1 merge armed it with a HiDPI-scaled glViewport, which stomps
	// the GL viewport for every in-mission caller — terrain (own MVP
	// dispatch) and TGL objects/water (viewport-transformed) desync, so
	// props/mechs/trees/water visibly slide off the terrain while panning.
	// The defs/ImGui UI never needed this: it scales via
	// GuiRuntime::GetDisplaySize, and the preview FBO sets its own viewport.
	//glViewport(x, y, w, h);
	g_gos_renderer->setRenderViewport(vec4(x, y, w, h));
}

void __stdcall gos_GetRenderViewport(float* x, float* y, float* w, float* h)
{
    gosASSERT(x && y && w && h);
    gosASSERT(g_gos_renderer);
	vec4 vp = g_gos_renderer->getRenderViewport();
	*x = vp.x;
	*y = vp.y;
	*w = vp.z;
	*h = vp.w;
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
    if(mi == gos_Info_GetDeviceName)
        return (size_t)glGetString(GL_RENDERER);
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
    
    return graphics::get_window_display_index(g_gos_renderer->getRenderContextHandle());
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}



GLenum getGLBufferType(gosBUFFER_TYPE type)
{
	GLenum t = -1;
	switch (type)
	{
		case gosBUFFER_TYPE::VERTEX: t = GL_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::INDEX: t = GL_ELEMENT_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::UNIFORM: t = GL_UNIFORM_BUFFER; break;

		default:
			gosASSERT(0 && "unknows buffer type");
	}
	return t;
}

GLenum getGLBufferUsage(gosBUFFER_USAGE usage)
{
	GLenum u = -1;
	switch (usage)
	{
		case gosBUFFER_USAGE::STREAM_DRAW: u = GL_STREAM_DRAW; break;
		case gosBUFFER_USAGE::STATIC_DRAW: u = GL_STATIC_DRAW; break;
		case gosBUFFER_USAGE::DYNAMIC_DRAW: u = GL_DYNAMIC_DRAW; break;

		default:
			gosASSERT(0 && "unknows buffer usage");
	}
	return u;
}



gosBuffer* __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage, int element_size, uint32_t count, void* buffer_data)
{
	GLenum gl_target = getGLBufferType(type);
	GLenum gl_usage = getGLBufferUsage(usage);

	size_t buffer_size = element_size * count;
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(gl_target, buffer);
	glBufferData(gl_target, buffer_size, buffer_data, gl_usage);
	glBindBuffer(gl_target, 0);

	gosBuffer* pbuffer = new gosBuffer();
	pbuffer->buffer_ = buffer;
	pbuffer->element_size_ = element_size;
	pbuffer->count_ = count;
	pbuffer->type_ = type;
	pbuffer->usage_ = usage;

    gosASSERT(g_gos_renderer);
	g_gos_renderer->addBuffer(pbuffer);

	return pbuffer;
}

uint32_t gos_GetBufferSizeBytes(HGOSBUFFER buffer)
{
	gosASSERT(buffer);
    return buffer->element_size_ * buffer->count_;
}

void __stdcall gos_DestroyBuffer(gosBuffer* buffer)
{
	gosASSERT(buffer);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteBuffer(buffer);
    (void)rv;
	delete buffer;
}

void __stdcall gos_BindBufferBase(gosBuffer* buffer, uint32_t slot)
{
	gosASSERT(buffer);

	GLenum gl_target = getGLBufferType(buffer->type_);
	glBindBufferBase(gl_target, slot, buffer->buffer_);
}

void __stdcall gos_UpdateBuffer(HGOSBUFFER buffer, void* data, size_t offset, size_t num_bytes)
{
	gosASSERT(buffer);
    gosASSERT(buffer->element_size_ * buffer->count_ >= num_bytes);
	GLenum gl_target = getGLBufferType(buffer->type_);
    glBindBuffer(gl_target, buffer->buffer_);
	// NOTE (unchanged behavior): this full glBufferData re-spec IGNORES the
	// `offset` arg — it always orphans the whole buffer from byte 0. Left as-is
	// by GPU-UPDATE-BUFFER-COUNTER-1 (measurement-only); the macro just tallies
	// this orphan-on-write under owner GosUpdateBuffer when the gate is on.
	MC2_GL_BufferData_Owner(gl_target, num_bytes, data, GL_DYNAMIC_DRAW, GosUpdateBuffer);
    glBindBuffer(gl_target, 0);
}

HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD* records, int count)
{
	gosASSERT(records && count > 0);
    gosASSERT(g_gos_renderer);
	gosVertexDeclaration* vdecl = gosVertexDeclaration::create(records, count);
	g_gos_renderer->addVertexDeclaration(vdecl);
	return vdecl;
}

void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION vdecl)
{
	gosASSERT(vdecl);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteVertexDeclaration(vdecl);
    (void)rv;
	gosVertexDeclaration::destroy(vdecl);

}

HGOSRENDERMATERIAL __stdcall gos_getRenderMaterial(const char* material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);
	return g_gos_renderer->getRenderMaterial(material);
}

void __stdcall gos_ApplyRenderMaterial(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);

	gos_SetCommonMaterialParameters(material);

	material->apply();
	material->setSamplerUnit(gosMesh::s_tex1, 0);
	material->setUniformBlock("lights_data", 0);
}

void __stdcall gos_SetRenderMaterialParameterFloat4(HGOSRENDERMATERIAL material, const char* name, const float* v)
{
	gosASSERT(material);
	gosASSERT(v);
	material->getShader()->setFloat4(name, v);
}

void __stdcall gos_SetRenderMaterialParameterMat4(HGOSRENDERMATERIAL material, const char* name, const float* m)
{
	gosASSERT(material);
	gosASSERT(m);
	material->getShader()->setMat4(name, m);
}

void __stdcall gos_SetRenderMaterialParameterInt(HGOSRENDERMATERIAL material, const char* name, int v)
{
	gosASSERT(material);
	material->getShader()->setInt(name, v);
}

void __stdcall gos_SetRenderMaterialSamplerUnit(HGOSRENDERMATERIAL material, const char* name, uint32_t unit)
{
	gosASSERT(material);
	gosASSERT(name);
	material->setSamplerUnit(name, unit);
}

// GAMEOS-GRAPHICS-SPLIT-1 slice 4: the [LIGHTSSBO v1] LightsData SSBO
// cluster (owner record, grow-once, prefix GPU-copy, upload/destroy) moved
// to gameos_graphics_light_ssbo.cpp. gos_BindLightDataStorageBlock stays
// here (needs gosRenderMaterial internals).


// RF1: bind the LightsData SSBO block for a lit material's program.
// Unconditional per-draw (idempotent, ~free, and immune to the CLAUDE.md
// shader-hot-reload relink which resets program block bindings to 0 — a
// cached guard would silently revert). Called from BOTH the legacy lit
// paths (ShapeRenderer::render in txmmgr.cpp, gosRenderer::drawIndexedTris
// here). Replaces the now-silent UBO-reflection
// gos_SetRenderMaterialUniformBlockBindingPoint(mat,"LightsData",...).
void __stdcall gos_BindLightDataStorageBlock(HGOSRENDERMATERIAL material)
{
	if (!material || !material->getShader() || !material->getShader()->shp_)
		return;
	GLuint shp = material->getShader()->shp_;
	GLuint idx = glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK, "LightsData");
	if (idx != GL_INVALID_INDEX)
		glShaderStorageBlockBinding(shp, idx, LIGHT_DATA_SSBO_BINDING);
}

// VEHICLE-LIGHTING-PATH-1: expose the night light-pool bind/upload to callers
// that only hold an HGOSRENDERMATERIAL (mclib's ShapeRenderer), mirroring
// gos_BindLightDataStorageBlock above. The two GPU batchers call
// UploadObjectLightPoolUniforms directly with a raw GL program id
// (gos_mech_batcher.cpp:2519, gos_static_prop_batcher.cpp:6860/:8084); the
// legacy lit path had no way to reach it, which is why vehicles were the only
// object class receiving NO night pool light at all.
// Safe when gated off: UploadObjectLightPoolUniforms force-uploads
// u_numPoolLights=0, so calcLightPools returns exact vec3(0.0).
void __stdcall gos_UploadObjectLightPools(HGOSRENDERMATERIAL material)
{
	if (!material || !material->getShader() || !material->getShader()->shp_)
		return;
	gos_terrain_lighting::UploadObjectLightPoolUniforms(material->getShader()->shp_);
}


void __stdcall gos_ForceApplyRenderStates() {
    // Force-apply by directly setting GL state to match the requested render states.
    // applyRenderStates() short-circuits if curStates_ == renderStates_ (RENDER_STATES v1),
    // so invalidate the cache first to force the full body to run.
    if (!g_gos_renderer) return;
    g_gos_renderer->invalidateRenderStateCache();
    // Just set GL directly for the critical states that renderLists() dirties
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (force-apply scene)
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    g_gos_renderer->applyRenderStates();
}

// RENDER_STATES v1: invalidation hook for fast paths that disturb GL state outside
// of applyRenderStates() (terrain bridges, water fast path, post-process composite,
// shadow direct draws, indirect terrain). Callers MUST call this AFTER finishing
// their direct-GL work so the next applyRenderStates() does a full re-apply.
void __stdcall gos_InvalidateRenderStateCache() {
    if (!g_gos_renderer) return;
    g_gos_renderer->invalidateRenderStateCache();
}

// Called from GuiRuntime::Render() just before post-ImGui 3D camera callbacks.
// Restores GL state that shadow sub-passes leave dirty and that applyRenderStates()
// has no tracking for (glColorMask, scissor test, FB binding).
void __stdcall gos_PrepareForPostImGuiRender() {
    // Shadow passes (gosPostProcess::beginShadowPass*) call
    // glColorMask(GL_FALSE,...) and restore in endShadowPass(). If endScene()
    // finishes with colorMask still FALSE (e.g. shadows disabled mid-frame or
    // early-out path), all subsequent color writes are silently suppressed.
    // applyRenderStates() has no gos_State_ColorMask and never fixes this.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);

    static const bool s_diag = mc2::gate::get<"MC2_3DVIEW_DIAG">();
    if (s_diag) {
        static bool s_diagDone = false;
        if (!s_diagDone) {
            s_diagDone = true;
            GLint fbo = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
            GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean scissor   = glIsEnabled(GL_SCISSOR_TEST);
            GLint depthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            GLboolean depthMask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            GLboolean cm[4] = {}; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
            GLint vp[4] = {}; glGetIntegerv(GL_VIEWPORT, vp);
            mc2::fmt::print_err("[3DVIEW_DIAG v1] post-imgui: fbo={:d} depthTest={:d} depthFunc=0x{:x} depthMask={:d} scissor={:d} colorMask={:d}{:d}{:d}{:d} vp=[{:d},{:d},{:d},{:d}]\n", mc2::fmt::as_i32(fbo), mc2::fmt::as_i32((int)depthTest), mc2::fmt::as_u32(depthFunc), mc2::fmt::as_i32((int)depthMask), mc2::fmt::as_i32((int)scissor), mc2::fmt::as_i32((int)cm[0]), mc2::fmt::as_i32((int)cm[1]), mc2::fmt::as_i32((int)cm[2]), mc2::fmt::as_i32((int)cm[3]), mc2::fmt::as_i32(vp[0]), mc2::fmt::as_i32(vp[1]), mc2::fmt::as_i32(vp[2]), mc2::fmt::as_i32(vp[3]));
            fflush(stderr);
        }
    }
}

// ---------------------------------------------------------------------------
// Camera preview FBO — used by SimpleCamera to render 3D panel views
// (encyclopedia, options gameplay) into a clean offscreen buffer so they
// are unaffected by the scene's reverse-Z depth state or gosPostProcess
// lifecycle.
//
// PREVIEW-FBO-FIXED-800x600-1: the FBO is a FIXED 800x600 (the legacy 2D
// UI's native virtual canvas), NOT the real drawable size. The legacy MLR
// mech-preview draw (SimpleCamera::render(), gos_GetViewport(), TG_Shape's
// screen-space math) is written entirely in terms of that 800x600 canvas
// and Environment.screenWidth/Height (which stays 800x600 by design -- see
// g_hudResClampEnabled above; do not "fix" that to real resolution, it desyncs
// other legacy tuned-resolution code). Rendering into an 800x600 FBO means
// none of that legacy math needs to know or care about the real window size
// at all -- it draws exactly as it always has. The caller then samples just
// the small preview rect (in 0..800/0..600 UV space) out of this texture and
// draws it, scaled to fit, as a normal ImGui image in the real-resolution
// defs UI panel (see GuiRuntime::DrawUiImage + gos_GetCameraPreviewTexture
// below, wired from SimpleCamera::render()/mechlopedia.cpp/mechbayscreen.cpp).
// This avoids EVERY resolution-mismatch class of bug the direct-draw
// approach hit: no real-vs-800 scale math, no glViewport scoping, no
// depth-state-after-ImGui issues (the FBO gets its own clean depth buffer
// every frame).
// ---------------------------------------------------------------------------
namespace {
    GLuint s_camPreviewFbo = 0;
    GLuint s_camPreviewColorTex = 0;
    GLuint s_camPreviewDepthRbo = 0;
    int    s_camPreviewW = 0;
    int    s_camPreviewH = 0;

    void ensureCamPreviewFbo(int w, int h)
    {
        if (s_camPreviewFbo && s_camPreviewW == w && s_camPreviewH == h)
            return;

        if (s_camPreviewFbo)
        {
            glDeleteFramebuffers(1, &s_camPreviewFbo);
            glDeleteTextures(1, &s_camPreviewColorTex);
            glDeleteRenderbuffers(1, &s_camPreviewDepthRbo);
            s_camPreviewFbo = s_camPreviewColorTex = s_camPreviewDepthRbo = 0;
        }

        glGenFramebuffers(1, &s_camPreviewFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_camPreviewFbo);

        glGenTextures(1, &s_camPreviewColorTex);
        glBindTexture(GL_TEXTURE_2D, s_camPreviewColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // PREVIEW-SUPERSAMPLE-1: LINEAR, not NEAREST — the panel crop is a small
        // sub-rect scaled up to the real-resolution ImGui panel; NEAREST is what
        // made the preview look chunky even before supersampling.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_camPreviewColorTex, 0);

        // TEX-CLASS: render-target -- camera-preview FBO depth renderbuffer
        glGenRenderbuffers(1, &s_camPreviewDepthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, s_camPreviewDepthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_camPreviewDepthRbo);

        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        s_camPreviewW = w;
        s_camPreviewH = h;
    }
}

// Bind the offscreen preview FBO (fixed 800x600, the legacy 2D UI's native
// canvas) and prepare it for a fresh mech render. Must be paired with
// gos_EndCameraPreviewRender(). See PREVIEW-FBO-FIXED-800x600-1 above for why
// this is fixed-size rather than drawable-size.
// PREVIEW-SUPERSAMPLE-1: raster scale for the preview FBO. The legacy math
// stays in its 800x600 virtual canvas — the ortho projection maps 0..800/0..600
// to NDC regardless of viewport size, so an NxN-scaled FBO + viewport simply
// rasterizes the same geometry at N-times the resolution (free supersampling;
// the ImGui composite crops in normalized UV space, unaffected). Default 4x
// (3200x2400 RGBA8 ~= 30 MB, one instance); MC2_PREVIEW_FBO_SCALE overrides 1-8.
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
    if (!g_gos_renderer) return;
    const int sc = camPreviewScale();
    ensureCamPreviewFbo(800 * sc, 600 * sc);

    static bool s_once = false;
    if (!s_once) {
        s_once = true;
        GLenum status = GL_FRAMEBUFFER_UNDEFINED;
        if (s_camPreviewFbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_camPreviewFbo);
            status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        mc2::fmt::print_err("[3DVIEW_DIAG v2] BeginCameraPreviewRender: fbo={:d} 800x600 status=0x{:x}\n", mc2::fmt::as_u32(s_camPreviewFbo), mc2::fmt::as_u32(status));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_camPreviewFbo);
    glViewport(0, 0, 800 * sc, 600 * sc);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0);  // reverse-Z: far=0, near=1
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
}

// Restore the default framebuffer (and its real-resolution glViewport) after
// camera preview rendering.
void __stdcall gos_EndCameraPreviewRender()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (g_gos_renderer) {
        glViewport(0, 0, g_gos_renderer->getWidth(), g_gos_renderer->getHeight());
        g_gos_renderer->invalidateRenderStateCache();
    }
}

// GL texture name of the 800x600 preview color attachment, for a caller to
// wrap as an ImTextureID and draw via GuiRuntime::DrawUiImage with UV
// cropping -- the composite-to-real-screen step happens entirely in ImGui
// space, not via glBlitFramebuffer, so no resolution/scale math is needed
// here at all.
GLuint __stdcall gos_GetCameraPreviewTexture()
{
    return s_camPreviewColorTex;
}
