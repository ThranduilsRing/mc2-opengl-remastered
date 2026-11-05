// gameos_graphics_font.cpp — gosFont implementation.
// GAMEOS-GRAPHICS-SPLIT-1 slice 1: moved verbatim from gameos_graphics.cpp
// (the ~9k-line monolith). Class definition lives in
// gameos_graphics_internal.h; renderer access goes through the shim
// functions defined in the main TU (gosRendererAddFontBmpTexture /
// gosRendererDeleteTexture) so the gosRenderer class stays private there.

#include <memory>
#include "gameos_graphics_internal.h"

#include "platform_str.h"
#include "file.h"
#include "utils/mc2log.h"
#include "mc2fmt.h"


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const DWORD INVALID_TEXTURE_ID = 0;   // matches the main TU's value

gosFont::~gosFont()
{
    if(tex_id_ != INVALID_TEXTURE_ID)
        gosRendererDeleteTexture(tex_id_);

}

// ui-phase1: "fontFile,pointSize" request-splitting helper so gosFont::load()
// can parse an appended ",<pointSize>" suffix off fontFile before calling
// _splitpath. requestedPointSize is currently parsed but unused by the
// D3F-first loader below; reserved for future per-size atlas support.
namespace {

static bool mc2SplitFontRequest(const char* fontFile, std::string& path, int& pointSize)
{
    if (!fontFile || !fontFile[0])
        return false;

    path = fontFile;
    pointSize = 0;

    const std::size_t comma = path.find_last_of(',');
    if (comma != std::string::npos) {
        try {
            pointSize = std::abs(std::stoi(path.substr(comma + 1)));
        } catch (...) {
            pointSize = 0;
        }
        path = path.substr(0, comma);
    }

    return !path.empty();
}

} // namespace

gosFont* gosFont::load(const char* fontFile) {

    std::string requestedPath;
    int requestedPointSize = 0;
    if (!mc2SplitFontRequest(fontFile, requestedPath, requestedPointSize))
        return nullptr;

    char fname[256];
    char dir[256];
    _splitpath(requestedPath.c_str(), nullptr, dir, fname, nullptr);

    // Retail .d3f wins when present. .bmp + .glyph stays as the
    // permanent fallback for converted fonts and community content.
    {
        const char* d3f_ext = ".d3f";
        const size_t d3fNameSize = strlen(fname) + 1 + strlen(dir) + strlen(d3f_ext) + 1;
        auto d3fNameOwner = std::make_unique_for_overwrite<char[]>(d3fNameSize);
        char* d3fName = d3fNameOwner.get();
        memset(d3fName, 0, d3fNameSize);
        uint32_t d3f_len = S_snprintf(d3fName, d3fNameSize, "%s/%s%s", dir, fname, d3f_ext);
        gosASSERT(d3f_len <= d3fNameSize - 1);

        gosGlyphInfo gi;
        gosD3FAtlas atlas;
        if(gos_load_d3f(d3fName, gi, atlas)) {
            // Legacy .glyph sidecar bridge — when a same-basename
            // .glyph exists alongside the .d3f, adopt its line spacing
            // and max-advance globals. UI widgets were authored against
            // those values; D3F's dwFontHeight em-box would pack lines
            // ~2x denser than retail. font_ascent_ is intentionally
            // left at the calibrated value (visible band height) so
            // the per-glyph maxy/miny set by calibrate_vertical stay
            // consistent with the renderer's char_off_y math.
            //
            // .glyph header layout (matches gos_load_glyphs):
            //   u32 num_glyphs, start_glyph, max_advance, ascent, line_skip
            {
                const char* glyph_ext = ".glyph";
                const size_t sidecarNameSize = strlen(fname) + 1 + strlen(dir) + strlen(glyph_ext) + 1;
                auto sidecarNameOwner = std::make_unique_for_overwrite<char[]>(sidecarNameSize);
                char* sidecarName = sidecarNameOwner.get();
                memset(sidecarName, 0, sidecarNameSize);
                // _splitpath leaves the trailing separator on dir; "dir//name" would
                // miss FST/mod keys (the key fold does not collapse slashes).
                // A non-empty dir from _splitpath already ends in a separator.
                S_snprintf(sidecarName, sidecarNameSize, dir[0] ? "%s%s%s" : "%s/%s%s", dir, fname, glyph_ext);

                // Game file layer (mod overlay, loose file, FastFile), not raw fopen.
                File sidecar;
                if(sidecar.open(sidecarName) == NO_ERR) {
                    uint32_t legacy_globals[5] = {0};
                    if(sidecar.read(reinterpret_cast<MemoryPtr>(legacy_globals), sizeof(legacy_globals)) == static_cast<long>(sizeof(legacy_globals))) {
                        gi.max_advance_    = legacy_globals[2];
                        gi.font_line_skip_ = legacy_globals[4];
                    }
                    sidecar.close();
                }
                sidecarNameOwner.reset();
            }

            DWORD tex_id = gos_NewEmptyTexture(gos_Texture_Alpha, d3fName,
                                               RECT_TEX(atlas.width, atlas.height), 0);
            if(tex_id != 0) {
                // Expand 8-bit alpha to RGBA8: fan alpha into R for the
                // gos_text shader's .xxxx sample. Other channels also
                // populated so any future shader change still gets sane data.
                const size_t pixel_count = static_cast<size_t>(atlas.width) * static_cast<size_t>(atlas.height);
                auto rgbaOwner = std::make_unique_for_overwrite<DWORD[]>(pixel_count);
                DWORD* rgba = rgbaOwner.get();
                for(size_t i = 0; i < pixel_count; ++i) {
                    uint32_t a = atlas.pixels[i];
                    rgba[i] = (a) | (a << 8) | (a << 16) | (a << 24);
                }
                atlas.pixels.reset();

                if(gosTexture* tex = gosRendererGetTexture(tex_id)) {
                    tex->setRgbaMirror(atlas.width, atlas.height, TF_RGBA8, rgba);
                    tex->markContentMutated();
                }
                rgbaOwner.reset();

                gosFont* font = new gosFont();   // ref-counted, private dtor: stays raw (destroy())
                font->gi_ = std::move(gi);
                font->font_name_ = fname;
                font->font_id_ = fontFile;
                font->tex_id_ = tex_id;

                d3fNameOwner.reset();
                return font;
            }
            // Texture allocation failed — release everything
            // calibrate_vertical allocated, including the per-glyph ink
            // bounds arrays, before falling through to the .bmp+.glyph
            // path.
        }
        d3fNameOwner.reset();
    }

    const char* tex_ext = ".bmp";
    const char* glyph_ext = ".glyph";

	const size_t textureNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(tex_ext) + 1;
    auto textureNameOwner = std::make_unique<char[]>(textureNameSize);
    char* textureName = textureNameOwner.get();

	const size_t glyphNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(glyph_ext) + 1;
    auto glyphNameOwner = std::make_unique<char[]>(glyphNameSize);
    char* glyphName = glyphNameOwner.get();

    uint32_t formatted_len = S_snprintf(textureName, textureNameSize, "%s/%s%s", dir, fname, tex_ext);
	gosASSERT(formatted_len <= textureNameSize - 1);

    formatted_len = S_snprintf(glyphName, glyphNameSize, "%s/%s%s", dir, fname, glyph_ext);
	gosASSERT(formatted_len <= glyphNameSize - 1);

    DWORD tex_id = gosRendererAddFontBmpTexture(textureName);

    gosFont* font = new gosFont();   // ref-counted, private dtor: stays raw (destroy())
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        delete font;
        STOP(("Failed to load font glyphs: %s\n", glyphName));
        return nullptr;
    }

    font->font_name_ = fname;
    font->font_id_ = fontFile;

    font->tex_id_ = tex_id;

    return font;

}

uint32_t gosFont::destroy(gosFont* font) {
    uint32_t rc = font->decRef();
    if(0 == rc) {
        delete font;
    }

    return rc;
}

void gosFont::getCharUV(int c, uint32_t* u, uint32_t* v) const {

    gosASSERT(u && v);

    int32_t pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= static_cast<int>(gi_.num_glyphs_)) {
        *u = *v = 0;
        return;
    }

    *u = gi_.glyphs_[pos].u;
    *v = gi_.glyphs_[pos].v;
}

int gosFont::getCharAdvance(int c) const
{
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= static_cast<int>(gi_.num_glyphs_)) {
        return getMaxCharWidth();
    }

    return gi_.glyphs_[pos].advance;
}

const gosGlyphMetrics& gosFont::getGlyphMetrics(int c) const {
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= static_cast<int>(gi_.num_glyphs_))
        pos = 0;

    return gi_.glyphs_[pos];
}
