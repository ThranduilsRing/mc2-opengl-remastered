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
#include "texture_decode.h"   // mc2tex::decodeBc7 for the pre-compressed RGBA8 mirror
#include "utils/texture_format.h"
#include "utils/Image.h"
#include "utils/mc2log.h"

#include <cstring>

#include <cstdint>
#include <string>
#include <memory>   // VK-HUD-MIRRORCOPY-1: shared_ptr rgba mirror
#include <vector>

class gosRenderer;

struct gosTextureInfo {
    int width_;
    int height_;
    gos_TextureFormat format_;
};

class gosTexture {
    public:
        static inline uint64_t texelGeneration = 0; // render owner only
        gosTexture(gos_TextureFormat fmt, const char* fname, DWORD hints, BYTE* pdata, DWORD size, bool from_memory)
        {


            format_ = fmt;
            if(fname) {
                filename_ = new char[strlen(fname)+1];
                strcpy(filename_, fname);
            } else {
                filename_ = nullptr;
            }
            texname_ = nullptr;

            hints_ = hints;

            plocked_area_ = nullptr;

            size_ = 0;
            pcompdata_ = nullptr;
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

            format_ = fmt;
            if(texname) {
                texname_ = new char[strlen(texname)+1];
                strcpy(texname_, texname);
            } else {
                texname_ = nullptr;
            }
            filename_ = nullptr;
            hints_ = hints;

            plocked_area_ = nullptr;

            size_ = 0;
            pcompdata_ = nullptr;
            tex_.w = w;
            tex_.h = h;

            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 0;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        // Pre-compressed BC7 source: retain its CPU descriptor and immutable
        // level-0 blocks for the native consumer (vkFormat 145 / 146).
        gosTexture(const CpuTextureDesc& prebuilt, gos_TextureFormat fmt, const char* name, uint32_t blockVkFormat,
                   std::shared_ptr<const std::vector<uint8_t>> blocks, uint32_t mipCount = 1)
            : blocks_(std::move(blocks)), block_vk_format_(blockVkFormat), mip_count_(mipCount ? mipCount : 1)
        {
            format_ = fmt;
            if(name) {
                texname_ = new char[strlen(name)+1];
                strcpy(texname_, name);
            } else {
                texname_ = nullptr;
            }
            filename_ = nullptr;
            // A cooked chain exists to be sampled: it asks for mip filtering whatever the legacy caller's hint said.
            hints_ = mip_count_ > 1 ? gosHint_MipmapFilter0 : 0;
            plocked_area_ = nullptr;
            size_ = 0;
            pcompdata_ = nullptr;
            tex_ = prebuilt;
            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 1;
            ++texelGeneration;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        bool prepareSource();

        ~gosTexture() {
            ++texelGeneration;


            gosASSERT(is_locked_ == false);

            if(pcompdata_)
                delete[] pcompdata_;
            if(filename_)
                delete[] filename_;
            if(texname_)
                delete[] texname_;


        }

        uint32_t getContentSerial() const { return content_serial_; }
        const char* sourceName() const { return filename_ ? filename_ : (texname_ ? texname_ : "<unnamed>"); }
        bool wantsMipmaps() const { return mc2tex::wantsMipmaps(hints_); }
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
            ++texelGeneration;
            if(w <= 0 || h <= 0 || pixels == nullptr) {
                rgba_mirror_.reset();
                mirror_w_ = 0;
                mirror_h_ = 0;
                return;
            }
            const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
            auto fresh = std::make_shared<std::vector<uint8_t>>();
            fresh->resize(pixelCount * 4u);
            std::vector<uint8_t>& dst = *fresh;
            mirror_w_ = w;
            mirror_h_ = h;
            if(format == TF_RGBA8) {
                std::memcpy(dst.data(), pixels, dst.size());
            } else if(format == TF_RGB8) {
                const uint8_t* src = static_cast<const uint8_t*>(pixels);
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

        // Publishes already-RGBA8 texels as the mirror without a copy (the decoded upload source).
        void adoptRgbaMirror(int w, int h, std::vector<uint8_t>&& rgba) {
            ++texelGeneration;
            if (w <= 0 || h <= 0 || rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) {
                setRgbaMirror(0, 0, TF_RGBA8, nullptr);
                return;
            }
            mirror_w_ = w;
            mirror_h_ = h;
            rgba_mirror_ = std::make_shared<const std::vector<uint8_t>>(std::move(rgba));
        }

        // The level-0 BC7 blocks of a pre-compressed texture (null for every other flavour) and their VK_FORMAT (145 / 146).
        std::shared_ptr<const std::vector<uint8_t>> getBlocks() const { return blocks_; }
        uint32_t getBlockVkFormat() const { return block_vk_format_; }
        uint32_t getMipCount() const { return mip_count_; }

        // Gives a pre-compressed texture an RGBA8 mirror for the consumers that only take RGBA8 (the Vulkan HUD / VFX tail):
        // its blocks decoded on the CPU, once per content. Every other flavour sets its mirror at upload, so false means the
        // texture has no CPU texels at all.
        bool ensureRgbaMirror() {
            if (hasRgbaMirror())
                return true;
            if (!blocks_ || mirror_decode_serial_ == content_serial_ || tex_.w <= 0 || tex_.h <= 0)
                return false;
            mirror_decode_serial_ = content_serial_;   // a failed decode is not retried per frame
            std::vector<uint8_t> rgba = mc2tex::decodeBc7(*blocks_, static_cast<uint32_t>(tex_.w), static_cast<uint32_t>(tex_.h));
            if (rgba.empty()) {
                MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] {}: BC7 blocks do not decode at {:d}x{:d}; drawn untextured",
                        texname_ ? texname_ : "NO NAME", tex_.w, tex_.h);
                return false;
            }
            adoptRgbaMirror(tex_.w, tex_.h, std::move(rgba));
            return true;
        }

        BYTE* Lock(int mipl_level, bool is_read_only, int* pitch) {
            gosASSERT(is_locked_ == false);
            is_locked_ = true;
            // TODO:
            gosASSERT(pitch);
            *pitch = tex_.w;

            gosASSERT(!plocked_area_);
            // always return rgba8 formatted data
            lock_type_read_only_ = is_read_only;
            const uint32_t ts = tex_.w*tex_.h * getTexFormatPixelSize(TF_RGBA8);
            plocked_area_ = new BYTE[ts];
            mc2tex::writeBgraLockPixels({plocked_area_, ts},
                ensureRgbaMirror() ? std::span<const uint8_t>(*rgba_mirror_) : std::span<const uint8_t>{});
            return plocked_area_;
        }

        void Unlock() {
            gosASSERT(is_locked_ == true);
        
            if(!lock_type_read_only_) {
                for(int y=0;y<tex_.h;++y) {
                    for(int x=0;x<tex_.w;++x) {
                        DWORD bgra = (reinterpret_cast<DWORD*>(plocked_area_))[tex_.w*y + x];
                        DWORD b = bgra&0xff;
                        DWORD g = (bgra&0xff00)>>8;
                        DWORD r = (bgra&0xff0000)>>16;
                        DWORD a = (bgra&0xff000000)>>24;
                        DWORD argb = (a<<24) | (b<<16) | (g<<8) | r;
                        (reinterpret_cast<DWORD*>(plocked_area_))[tex_.w*y + x] = argb;
                    }
                }
                blocks_.reset();
                block_vk_format_ = 0;
                mip_count_ = 1;
                setRgbaMirror(tex_.w, tex_.h, TF_RGBA8, plocked_area_);
                markContentMutated();
            }

            delete[] plocked_area_;
            plocked_area_ = nullptr;

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
        CpuTextureDesc tex_;

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
        std::shared_ptr<const std::vector<uint8_t>> blocks_;   // pre-compressed level 0, immutable
        uint32_t block_vk_format_ = 0;
        uint32_t mip_count_ = 1;   // levels held in blocks_, level 0 first
        uint32_t mirror_decode_serial_ = 0xffffffffu;   // content serial of the last blocks -> RGBA8 decode attempt
};

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
        const char* getName() const { return font_name_.c_str(); }
        const char* getId() const { return font_id_.c_str(); }

        uint32_t getRefCount() { return ref_count_; }
        uint32_t addRef() { return ++ref_count_; }
        uint32_t decRef() { gosASSERT(ref_count_>0); return --ref_count_; }

    private:
        static uint32_t destroy(gosFont* font);
        gosFont():tex_id_(0), ref_count_(1) {};
        ~gosFont();

        std::string font_name_;
        std::string font_id_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
        uint32_t ref_count_;
};
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

// .bmp+.glyph fallback: create the font page gosTexture, prepareSource
// (STOPs on failure, matching the old inline behavior) and register it with
// the renderer. Returns the gos texture handle.
DWORD gosRendererAddFontBmpTexture(const char* textureName);
// MERGE-WAVE-1 (VK-HUD-MIRRORCOPY-1 port): fetch a texture by id so the font
// TU can publish the rgba mirror on the freshly-built atlas.
gosTexture* gosRendererGetTexture(DWORD texId);

void gosRendererDeleteTexture(DWORD texId);

#endif // GAMEOS_GRAPHICS_INTERNAL_H
