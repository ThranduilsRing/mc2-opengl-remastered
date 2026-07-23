#ifndef GAMEOS_GRAPHICS_INTERNAL_H
#define GAMEOS_GRAPHICS_INTERNAL_H

// GAMEOS-GRAPHICS-SPLIT-1: shared internals for the gameos_graphics_*.cpp
// extraction TUs. Rules (see docs/splits/gameos-graphics-split-1-plan.md on
// nifty): class definitions moved here must be VERBATIM (every field — the
// classes were TU-internal, so any drift is an ODR bug); gosRenderer and gVAO
// stay in the main TU, extraction TUs reach the renderer only through the
// shim functions declared at the bottom (defined in gameos_graphics.cpp).

#include "gameos.hpp"
#include "gos_font.h"
#include "utils/gl_utils.h"
#include "utils/Image.h"

#include <GL/glew.h>
#include <cstring>

#include <cstdint>

class gosRenderer;

struct gosTextureInfo {
    int width_;
    int height_;
    gos_TextureFormat format_;
};

////////////////////////////////////////////////////////////////////////////////
// GlPixelStoreGuard - moved verbatim from gameos_graphics.cpp (SPLIT-1
// slice 2); used by gosTexture::Lock/Unlock below and by main-TU callers.
struct GlPixelStoreGuard {
    GLint packBuffer = 0, unpackBuffer = 0;
    GLint packAlign = 0, unpackAlign = 0;
    GLint packRowLen = 0, unpackRowLen = 0;
    GLint packSkipRows = 0, packSkipPixels = 0;
    GLint unpackSkipRows = 0, unpackSkipPixels = 0;
    GLint activeTex = 0;
    GLint binding2D = 0, binding2DArray = 0;

    GlPixelStoreGuard() {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,   &packBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT,    &packAlign);
        glGetIntegerv(GL_UNPACK_ALIGNMENT,  &unpackAlign);
        glGetIntegerv(GL_PACK_ROW_LENGTH,   &packRowLen);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLen);
        glGetIntegerv(GL_PACK_SKIP_ROWS,    &packSkipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS,  &packSkipPixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,  &unpackSkipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpackSkipPixels);
        glGetIntegerv(GL_ACTIVE_TEXTURE,    &activeTex);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,       &binding2D);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &binding2DArray);

        glBindBuffer(GL_PIXEL_PACK_BUFFER,   0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT,    1);
        glPixelStorei(GL_PACK_ROW_LENGTH,   0);
        glPixelStorei(GL_PACK_SKIP_ROWS,    0);
        glPixelStorei(GL_PACK_SKIP_PIXELS,  0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,  1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,  0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    }

    ~GlPixelStoreGuard() {
        glBindBuffer(GL_PIXEL_PACK_BUFFER,   (GLuint)packBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
        glPixelStorei(GL_PACK_ALIGNMENT,    packAlign);
        glPixelStorei(GL_PACK_ROW_LENGTH,   packRowLen);
        glPixelStorei(GL_PACK_SKIP_ROWS,    packSkipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS,  packSkipPixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT,  unpackAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLen);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,  unpackSkipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,unpackSkipPixels);
        glBindTexture(GL_TEXTURE_2D,       (GLuint)binding2D);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)binding2DArray);
        glActiveTexture((GLenum)activeTex);
    }
};

////////////////////////////////////////////////////////////////////////////////
// gosTexture - definition moved verbatim from gameos_graphics.cpp (SPLIT-1
// slice 2). createHardwareTexture lives in gameos_graphics_texture.cpp.
class gosTexture {
    public:
        gosTexture(gos_TextureFormat fmt, const char* fname, DWORD hints, BYTE* pdata, DWORD size, bool from_memory)
        {

	        //if(fmt == gos_Texture_Detect || /*fmt == gos_Texture_Keyed ||*/ fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(fname) {
                filename_ = new char[strlen(fname)+1];
                strcpy(filename_, fname);
            } else {
                filename_ = 0;
            }
            texname_ = NULL;

            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            if(size) {
                size_ = size;
                pcompdata_ = new BYTE[size];
                memcpy(pcompdata_, pdata, size);
            }

            is_locked_ = false;
            is_from_memory_ = from_memory;
            content_serial_ = 0;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        gosTexture(gos_TextureFormat fmt, DWORD hints, DWORD w, DWORD h, const char* texname)
        {
	        //if(fmt == gos_Texture_Detect /*|| fmt == gos_Texture_Keyed*/ || fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(texname) {
                texname_ = new char[strlen(texname)+1];
                strcpy(texname_, texname);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            tex_.w = w;
            tex_.h = h;

            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 0;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        // TEXMGR-COMPRESSED-UPLOAD-1: wrap a pre-built (already GL-uploaded)
        // Texture so the handle integrates with textureList_/bind/destroy
        // exactly like the other gosTexture flavors. No createHardwareTexture()
        // call — the GL object is supplied ready. Used by
        // gos_NewCompressedTexture2D for BC7 .ktx2 sidecar uploads.
        gosTexture(const Texture& prebuilt, gos_TextureFormat fmt, const char* name)
        {
            format_ = fmt;
            if(name) {
                texname_ = new char[strlen(name)+1];
                strcpy(texname_, name);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = 0;
            plocked_area_ = NULL;
            size_ = 0;
            pcompdata_ = NULL;
            tex_ = prebuilt;
            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 1;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        bool createHardwareTexture();

        ~gosTexture() {

            //SPEW(("Destroying texture: %s\n", filename_));

            gosASSERT(is_locked_ == false);

            if(pcompdata_)
                delete[] pcompdata_;
            if(filename_)
                delete[] filename_;
            if(texname_)
                delete[] texname_;

            destroyTexture(&tex_);
        }

        uint32_t getTextureId() const { return tex_.id; }
        TexType getTextureType() const { return tex_.type_; }
        const Texture& getTexture() const { return tex_; }
        uint32_t getContentSerial() const { return content_serial_; }
        void markContentMutated() { ++content_serial_; }
        // VK-HUD-MIRRORCOPY-1: the mirror is held as a shared immutable vector so
        // VkHudDraw can reference it by refcount bump instead of deep-copying the
        // full atlas per textured HUD draw. INVARIANT: the pointee is NEVER
        // mutated after publication — setRgbaMirror always builds a FRESH vector
        // and swaps the pointer, so queued draws holding an older serial's bytes
        // stay valid across mid-frame re-bakes and texture destruction.
        std::shared_ptr<const std::vector<uint8_t>> getRgbaMirror() const { return rgba_mirror_; }
        bool hasRgbaMirror() const { return rgba_mirror_ && !rgba_mirror_->empty() && mirror_w_ == tex_.w && mirror_h_ == tex_.h; }

        void setRgbaMirror(int w, int h, TexFormat format, const void* pixels) {
            if(w <= 0 || h <= 0 || pixels == nullptr) {
                rgba_mirror_.reset();
                mirror_w_ = 0;
                mirror_h_ = 0;
                return;
            }
            const size_t pixelCount = (size_t)w * (size_t)h;
            auto fresh = std::make_shared<std::vector<uint8_t>>();
            fresh->resize(pixelCount * 4u);
            std::vector<uint8_t>& dst = *fresh;
            mirror_w_ = w;
            mirror_h_ = h;
            if(format == TF_RGBA8) {
                std::memcpy(dst.data(), pixels, dst.size());
            } else if(format == TF_RGB8) {
                const uint8_t* src = (const uint8_t*)pixels;
                for(size_t i = 0; i < pixelCount; ++i) {
                    dst[i * 4u + 0u] = src[i * 3u + 0u];
                    dst[i * 4u + 1u] = src[i * 3u + 1u];
                    dst[i * 4u + 2u] = src[i * 3u + 2u];
                    dst[i * 4u + 3u] = 0xffu;
                }
            } else {
                rgba_mirror_.reset();
                mirror_w_ = 0;
                mirror_h_ = 0;
                return;
            }
            rgba_mirror_ = std::move(fresh);
        }

        // VK-FIELD-VFX-4: lazily build the HUD rgba mirror by reading the texel
        // content back from the GPU. Covers textures whose upload path never set
        // a CPU mirror — above all the BC7 .ktx2 sidecar path
        // (gos_NewCompressedTexture2D in txmmgr realize), which effect sprites
        // (weapon-beam gradients etc.) take; the VK present tail then declined
        // them as textured draws. glGetTexImage with GL_RGBA/GL_UNSIGNED_BYTE is
        // legal on a BPTC-compressed texture (the driver decompresses); guard the
        // result (GL error / all-zero alpha+rgb) so a driver that can't decode
        // just leaves the mirror empty and behavior is unchanged (per-draw
        // decline, as today). Attempted at most once per content serial.
        bool ensureRgbaMirrorFromGpu() {
            if (hasRgbaMirror())
                return true;
            if (mirror_gpu_attempt_serial_ == content_serial_)
                return false; // already tried this content; don't retry per frame
            mirror_gpu_attempt_serial_ = content_serial_;
            if (tex_.w <= 0 || tex_.h <= 0 || tex_.id == 0 ||
                tex_.type_ != TT_2D)
                return false;
            const size_t bytes = (size_t)tex_.w * (size_t)tex_.h * 4u;
            std::vector<uint8_t> px(bytes, 0u);
            {
                GlPixelStoreGuard pixelStoreGuard;
                while (glGetError() != GL_NO_ERROR) {} // clear stale errors
                glBindTexture(GL_TEXTURE_2D, tex_.id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                const GLenum err = glGetError();
                glBindTexture(GL_TEXTURE_2D, 0);
                if (err != GL_NO_ERROR) {
                    static int s_mirrorReadFails = 0;
                    if (++s_mirrorReadFails <= 3)
                        std::fprintf(stderr,
                            "[VK_HUD_MIRROR v1] gpu readback FAILED id=%u err=0x%x\n",
                            tex_.id, (unsigned)err);
                    return false;
                }
            }
            bool anyNonZero = false;
            for (size_t i = 0; i < bytes; ++i) {
                if (px[i] != 0u) { anyNonZero = true; break; }
            }
            if (!anyNonZero)
                return false; // silently-unwritten readback: keep declining
            setRgbaMirror(tex_.w, tex_.h, TF_RGBA8, px.data());
            static int s_mirrorReadOk = 0;
            if (++s_mirrorReadOk <= 8)
                std::fprintf(stderr,
                    "[VK_HUD_MIRROR v1] gpu readback mirror built id=%u %dx%d\n",
                    tex_.id, tex_.w, tex_.h);
            return true;
        }

        BYTE* Lock(int mipl_level, bool is_read_only, int* pitch) {
            gosASSERT(is_locked_ == false);
            is_locked_ = true;
            // TODO:
            gosASSERT(pitch);
            *pitch = tex_.w;

            gosASSERT(!plocked_area_);
#if 0 
            glBindTexture(GL_TEXTURE_2D, tex_.id);
            GLint pack_row_length;
            GLint pack_alignment;
            glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
            // always return rgba8 formatted data
            lock_type_read_only_ = is_read_only;
            const uint32_t ts = tex_.w*tex_.h * getTexFormatPixelSize(TF_RGBA8);
            plocked_area_ = new BYTE[ts];
            // Zero before readback: getTextureData early-returns WITHOUT writing
            // for block-compressed (BC7/TF_NONE) textures, which would otherwise
            // leave this buffer full of heap garbage that the paint-scheme
            // classifier then re-uploads. (A paint texture can land on BC7 when
            // its paintInstance hashes low.)
            memset(plocked_area_, 0, ts);
            // glGetTexImage readback is sensitive to inherited GL_PACK_* state and
            // a left-bound GL_PIXEL_PACK_BUFFER; guard save/resets/restores it so
            // the mech-paint recolour reads the real texels on NVIDIA.
            GlPixelStoreGuard pixelStoreGuard;
            getTextureData(tex_, 0, plocked_area_, TF_RGBA8);
            for(int y=0;y<tex_.h;++y) {
                for(int x=0;x<tex_.w;++x) {
                    DWORD rgba = ((DWORD*)plocked_area_)[tex_.w*y + x];
                    DWORD r = rgba&0xff;
                    DWORD g = (rgba&0xff00)>>8;
                    DWORD b = (rgba&0xff0000)>>16;
                    DWORD a = (rgba&0xff000000)>>24;
                    DWORD bgra = (a<<24) | (r<<16) | (g<<8) | b;
                    ((DWORD*)plocked_area_)[tex_.w*y + x] = bgra;
                }
            }
            return plocked_area_;
        }

        void Unlock() {
            gosASSERT(is_locked_ == true);
        
            if(!lock_type_read_only_) {
                for(int y=0;y<tex_.h;++y) {
                    for(int x=0;x<tex_.w;++x) {
                        DWORD bgra = ((DWORD*)plocked_area_)[tex_.w*y + x];
                        DWORD b = bgra&0xff;
                        DWORD g = (bgra&0xff00)>>8;
                        DWORD r = (bgra&0xff0000)>>16;
                        DWORD a = (bgra&0xff000000)>>24;
                        DWORD argb = (a<<24) | (b<<16) | (g<<8) | r;
                        ((DWORD*)plocked_area_)[tex_.w*y + x] = argb;
                    }
                }
                // Same hazard as Lock's readback, upload side: glTexSubImage2D
                // reads from a left-bound GL_PIXEL_UNPACK_BUFFER (see applyPBO)
                // instead of client memory, and honours inherited GL_UNPACK_*.
                // Guard neutralises both so the recoloured texels actually land.
                GlPixelStoreGuard pixelStoreGuard;
                updateTexture(tex_, plocked_area_, TF_RGBA8);
                setRgbaMirror(tex_.w, tex_.h, TF_RGBA8, plocked_area_);
                markContentMutated();
            }

            delete[] plocked_area_;
            plocked_area_ = NULL;

            is_locked_ = false;
        }

        void getTextureInfo(gosTextureInfo* texinfo) const {
            gosASSERT(texinfo);
            texinfo->width_ = tex_.w;
            texinfo->height_ = tex_.h;
            texinfo->format_ = format_;
        }

    private:
        BYTE* pcompdata_;
        BYTE* plocked_area_;
        DWORD size_;
        Texture tex_;

        gos_TextureFormat format_;
        char* filename_;
        char* texname_;
        DWORD hints_;

        bool is_locked_;
        bool lock_type_read_only_;
        bool is_from_memory_; // not loaded from file
        uint32_t content_serial_;
        std::shared_ptr<const std::vector<uint8_t>> rgba_mirror_;   // VK-HUD-MIRRORCOPY-1: immutable-after-publication
        int mirror_w_;
        int mirror_h_;
        // VK-FIELD-VFX-4: content serial of the last GPU-readback mirror attempt
        // (ensureRgbaMirrorFromGpu), so a failed readback is not retried per frame.
        uint32_t mirror_gpu_attempt_serial_ = 0xffffffffu;
};

////////////////////////////////////////////////////////////////////////////////
// gosFont — definition moved verbatim from gameos_graphics.cpp (SPLIT-1
// slice 1). Implementation lives in gameos_graphics_font.cpp.
class gosFont {
        friend class gosRenderer;
    public:
        static gosFont* load(const char* fontFile);

        int getMaxCharWidth() const { return gi_.max_advance_; }
        int getMaxCharHeight() const { return gi_.font_line_skip_; }
        int getFontAscent() const { return gi_.font_ascent_; }

        int getCharWidth(int c) const;
        void getCharUV(int c, uint32_t* u, uint32_t* v) const;
        int getCharAdvance(int c) const;
        const gosGlyphMetrics& getGlyphMetrics(int c) const;
        const gosGlyphInfo& getGlyphInfo() const { return gi_; }


        DWORD getTextureId() const { return tex_id_; }
        const char* getName() const { return font_name_; }
        const char* getId() const { return font_id_; }

        uint32_t getRefCount() { return ref_count_; }
        uint32_t addRef() { return ++ref_count_; }
        uint32_t decRef() { gosASSERT(ref_count_>0); return --ref_count_; }

    private:
        static uint32_t destroy(gosFont* font);
        gosFont():font_name_(0), font_id_(0), tex_id_(0), ref_count_(1) {};
        ~gosFont();

        char* font_name_;
        char* font_id_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
        uint32_t ref_count_;
};
// ── SPLIT-1 slice 5: HUD-scale / UI-canvas shared state ─────────────────────
// Defined in gameos_graphics_params.cpp; referenced by the main TU
// (flushHUDBatch remaps, HUD record sites) and by the params accessors.
extern float s_hud_scale;
extern bool  s_hud_scale_active;
extern bool  s_hud_scale_exempt;
extern int   s_hud_canvas_exempt_mode;
extern bool  s_uiCanvasAssert;
extern bool  s_uiCanvasLatch;
extern bool  s_hudCanvasActive;

// Logical canvas size shims (defined in gameos_graphics.cpp; 0 = no renderer).
int gosRendererLogicalWidth();
int gosRendererLogicalHeight();


// ── Renderer shims (defined in gameos_graphics.cpp) ─────────────────────────
// The gosRenderer class definition stays private to the main TU; extraction
// TUs use these instead of member calls.

// .bmp+.glyph fallback: create the font page gosTexture, createHardwareTexture
// (STOPs on failure, matching the old inline behavior) and register it with
// the renderer. Returns the gos texture handle.
DWORD gosRendererAddFontBmpTexture(const char* textureName);

// gosFont::~gosFont: release the font page texture.
void gosRendererDeleteTexture(DWORD texId);

#endif // GAMEOS_GRAPHICS_INTERNAL_H
