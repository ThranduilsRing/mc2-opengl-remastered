// gameos_graphics_texture.cpp - gosTexture hardware-texture creation.
// GAMEOS-GRAPHICS-SPLIT-1 slice 2: moved verbatim from gameos_graphics.cpp.
// Class definition lives in gameos_graphics_internal.h.

#include "gameos_graphics_internal.h"

#include "diagnostic_trace.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

// PNG-MAGENTA-KEY-1: MC2 art uses BOTH real alpha and magenta (0xFF00FF)
// colour-keying for transparency. The modder's PNG conversions carry the
// legacy magenta regions baked into opaque pixels, which drew as solid
// magenta boxes. For PNG sources ONLY (stock ships zero PNGs, so stock is
// untouched by construction), map exact-magenta pixels to transparent
// black at decode: transparent so they key out, black so bilinear
// filtering halos dark instead of pink. Runs BEFORE convertIfNecessary, so
// gos_Texture_Detect sees the new alpha and classifies Alpha; an explicit
// gos_Texture_Solid caller still wins (makeKindaSolid re-opaques after).
// Killswitch: MC2_PNG_MAGENTA_KEY=0.
static void keyMagentaToAlphaPNG(Image& img) {
    static const bool s_on = []() {
        const char* v = std::getenv("MC2_PNG_MAGENTA_KEY");
        return !(v && v[0] == '0');
    }();
    if (!s_on || img.getFormat() != FORMAT_RGBA8)
        return;
    DWORD* pixels = (DWORD*)img.getPixels();
    const int n = img.getWidth() * img.getHeight();
    for (int i = 0; i < n; ++i) {
        // RGBA8 in memory: R at byte 0 -> little-endian DWORD 0xAABBGGRR.
        if ((pixels[i] & 0x00FFFFFFu) == 0x00FF00FFu)
            pixels[i] = 0x00000000u;
    }
}

static void makeKindaSolid(Image& img) {
    // have to do this, otherwise texutre with zero alpha could be drawn with alpha blend enabled, evel though logically aplha blend should not be enabled!
    // (happens when drawing terrain, see TerrainQuad::draw() case when no detail and no owerlay bu t isCement is true)
    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            pixels[y*img.getWidth() + x] = pix | 0xff000000;
        }
    }
}

static bool doesLookLikeAlpha(const Image& img) {
    gosASSERT(img.getFormat() == FORMAT_RGBA8);

    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            if((0xFF000000 & pix) != 0xFF000000)
                return true;
        }
    }
    return false;
}

static gos_TextureFormat convertIfNecessary(Image& img, gos_TextureFormat gos_format) {

    const bool has_alpha_channel = FORMAT_RGBA8 == img.getFormat();

    if(gos_format == gos_Texture_Detect) {
        bool has_alpha = has_alpha_channel ? doesLookLikeAlpha(img) : false;
        gos_format = has_alpha ? gos_Texture_Alpha : gos_Texture_Solid;
    }

    if(gos_format == gos_Texture_Solid && has_alpha_channel)
        makeKindaSolid(img);

    return gos_format;
}

bool gosTexture::createHardwareTexture() {

    // Opt-in to mipmaps via gosHint_MipmapFilter0. MC2's original convention
    // was "absence of DisableMipmap means mipmaps on," but in this port many
    // HUD/GUI/tacmap loads pass hints=0 without DisableMipmap and must stay
    // non-mipmapped for pixel-perfect sampling. We use MipmapFilter0 as a
    // positive opt-in instead -- no existing code sets this bit, so only
    // explicitly-updated load sites enable mipmaps. DisableMipmap wins if
    // both are set (defensive).
    const bool wantMipmaps = (hints_ & gosHint_MipmapFilter0) != 0
                          && (hints_ & gosHint_DisableMipmap) == 0;

    if(!is_from_memory_) {

        gosASSERT(filename_);

        Image img;
        if(!img.loadFromFile(filename_)) {
            SPEW(("DBG", "failed to load texture from file: %s\n", filename_));
            return false;
        }

        // PNG-MAGENTA-KEY-1 (file path): key magenta only for .png sources.
        {
            const size_t fl = strlen(filename_);
            if (fl >= 4 && _stricmp(filename_ + fl - 4, ".png") == 0)
                keyMagentaToAlphaPNG(img);
        }

        // check for only those formats, because lock.unlock may incorrectly work with different channes size (e.g. 16 or 32bit or floats)
        FORMAT img_fmt = img.getFormat();
        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        return tex_.isValid();

    } else if(pcompdata_ && size_ > 0) {

        // The texture cache stores raw file bytes; pick the decoder by
        // signature instead of assuming TGA.  data/defs UI Editor pages
        // reference .png art that flows through this from-memory path.
        const bool looksLikePNG = size_ >= 8 &&
            pcompdata_[0] == 0x89 && pcompdata_[1] == 'P' &&
            pcompdata_[2] == 'N'  && pcompdata_[3] == 'G';

        Image img;
        bool decoded = looksLikePNG
            ? img.loadPNG(pcompdata_, size_)
            : img.loadTGA(pcompdata_, size_);
        if(!decoded) {
            SPEW(("DBG", "failed to load texture from data, filename: %s, texname: %s\n", filename_? filename_ : "NO FILENAME", texname_?texname_:"NO TEXNAME"));
            return false;
        }

        // PNG-MAGENTA-KEY-1 (from-memory path): same rule, keyed off the PNG
        // signature sniff above.
        if (looksLikePNG)
            keyMagentaToAlphaPNG(img);

        FORMAT img_fmt = img.getFormat();

        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        return tex_.isValid();
    } else {
        gosASSERT(tex_.w >0 && tex_.h > 0);

        TexFormat tf = TF_RGBA8; // TODO: check format_ and do appropriate stuff
        DWORD* pdata = new DWORD[tex_.w*tex_.h];
        for(int i=0;i<tex_.w*tex_.h;++i)
            pdata[i] = 0xFF00FFFF;

        // OVERLAY-MAGENTA-TEXTURE-RECON-1 (Source A): this texture object has w/h but
        // NO source path and NO compressed data -> filled solid magenta. Emit WHICH
        // texture resolved to nothing (the highest-value magenta probe). Gated
        // MC2_OVERLAY_MAGENTA_TRACE + MC2_DIAG_TAGS=OVERLAY_MAGENTA; read via
        // get_diagnostic_events("OVERLAY_MAGENTA"). No behavior change.
        {
            static const bool s_magentaTrace = (std::getenv("MC2_OVERLAY_MAGENTA_TRACE") != nullptr);
            if (s_magentaTrace && mc2_diag::tagEnabled("OVERLAY_MAGENTA")) {
                char _mg_fn[256]; char _mg_tn[256];
                const char* _mg_sfn = filename_ ? filename_ : "";
                const char* _mg_stn = texname_  ? texname_  : "";
                size_t _mg_k;
                for (_mg_k=0; _mg_k<sizeof(_mg_fn)-1 && _mg_sfn[_mg_k]; ++_mg_k)
                    _mg_fn[_mg_k] = (_mg_sfn[_mg_k]=='\\') ? '/' : _mg_sfn[_mg_k];
                _mg_fn[_mg_k] = '\0';
                for (_mg_k=0; _mg_k<sizeof(_mg_tn)-1 && _mg_stn[_mg_k]; ++_mg_k)
                    _mg_tn[_mg_k] = (_mg_stn[_mg_k]=='\\') ? '/' : _mg_stn[_mg_k];
                _mg_tn[_mg_k] = '\0';
                char _mg_buf[600];
                snprintf(_mg_buf, sizeof(_mg_buf),
                         "{\"site\":\"fallback_fill\",\"filename\":\"%s\",\"texname\":\"%s\",\"w\":%d,\"h\":%d}",
                         _mg_fn, _mg_tn, (int)tex_.w, (int)tex_.h);
                mc2_diag::writeEvent("OVERLAY_MAGENTA", 1, 0, _mg_buf);
            }
        }
        tex_ = create2DTexture(tex_.w, tex_.h, tf, (const uint8_t*)pdata, wantMipmaps);
        delete[] pdata;
        return tex_.isValid();
    }

}

