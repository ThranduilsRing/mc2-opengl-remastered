// gameos_graphics_font.cpp — gosFont implementation.
// GAMEOS-GRAPHICS-SPLIT-1 slice 1: moved verbatim from gameos_graphics.cpp
// (the ~9k-line monolith). Class definition lives in
// gameos_graphics_internal.h; renderer access goes through the shim
// functions defined in the main TU (gosRendererAddFontBmpTexture /
// gosRendererDeleteTexture) so the gosRenderer class stays private there.

#include "gameos_graphics_internal.h"

#include "platform_str.h"

#include <GL/glew.h>

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

    delete[] gi_.glyphs_;
    delete[] gi_.ink_top_;
    delete[] gi_.ink_bot_;
    delete[] gi_.ink_valid_;
    delete[] font_name_;
    delete[] font_id_;
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

////////////////////////////////////////////////////////////////////////////////
gosFont* gosFont::load(const char* fontFile) {

    std::string requestedPath;
    int requestedPointSize = 0;
    if (!mc2SplitFontRequest(fontFile, requestedPath, requestedPointSize))
        return NULL;

    char fname[256];
    char dir[256];
    _splitpath(requestedPath.c_str(), NULL, dir, fname, NULL);

    // Retail .d3f wins when present. .bmp + .glyph stays as the
    // permanent fallback for converted fonts and community content.
    {
        const char* d3f_ext = ".d3f";
        const size_t d3fNameSize = strlen(fname) + 1 + strlen(dir) + strlen(d3f_ext) + 1;
        char* d3fName = new char[d3fNameSize];
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
                char* sidecarName = new char[sidecarNameSize];
                memset(sidecarName, 0, sidecarNameSize);
                S_snprintf(sidecarName, sidecarNameSize, "%s/%s%s", dir, fname, glyph_ext);

                FILE* sidecar = fopen(sidecarName, "rb");
                if(sidecar) {
                    uint32_t legacy_globals[5] = {0};
                    if(fread(legacy_globals, sizeof(uint32_t), 5, sidecar) == 5) {
                        gi.max_advance_    = legacy_globals[2];
                        gi.font_line_skip_ = legacy_globals[4];
                    }
                    fclose(sidecar);
                }
                delete[] sidecarName;
            }

            DWORD tex_id = gos_NewEmptyTexture(gos_Texture_Alpha, d3fName,
                                               RECT_TEX(atlas.width, atlas.height), 0);
            if(tex_id != 0) {
                // Expand 8-bit alpha to RGBA8: fan alpha into R for the
                // gos_text shader's .xxxx sample. Other channels also
                // populated so any future shader change still gets sane data.
                const size_t pixel_count = (size_t)atlas.width * (size_t)atlas.height;
                DWORD* rgba = new DWORD[pixel_count];
                for(size_t i = 0; i < pixel_count; ++i) {
                    uint32_t a = atlas.pixels[i];
                    rgba[i] = (a) | (a << 8) | (a << 16) | (a << 24);
                }
                delete[] atlas.pixels;
                atlas.pixels = NULL;

                GLuint gl_id = gos_GetTextureGLId(tex_id);
                glBindTexture(GL_TEXTURE_2D, gl_id);
                // Save/restore GL_UNPACK_ALIGNMENT — it's global state
                // and a later texture upload may rely on a different
                // value (driver default is 4, but other code paths set
                // it to 1 for tightly-packed sources).
                GLint prev_unpack_alignment = 0;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                atlas.width, atlas.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
                glBindTexture(GL_TEXTURE_2D, 0);
                delete[] rgba;

                gosFont* font = new gosFont();
                font->gi_ = gi;
                font->font_name_ = new char[strlen(fname) + 1];
                strcpy(font->font_name_, fname);
                font->font_id_ = new char[strlen(fontFile) + 1];
                strcpy(font->font_id_, fontFile);
                font->tex_id_ = tex_id;

                delete[] d3fName;
                return font;
            }
            // Texture allocation failed — release everything
            // calibrate_vertical allocated, including the per-glyph ink
            // bounds arrays, before falling through to the .bmp+.glyph
            // path.
            delete[] gi.glyphs_;
            delete[] gi.ink_top_;
            delete[] gi.ink_bot_;
            delete[] gi.ink_valid_;
            delete[] atlas.pixels;
        }
        delete[] d3fName;
    }

    const char* tex_ext = ".bmp";
    const char* glyph_ext = ".glyph";

	const size_t textureNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(tex_ext) + 1;
    char* textureName = new char[textureNameSize];
	memset(textureName, 0, textureNameSize);

	const size_t glyphNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(glyph_ext) + 1;
    char* glyphName = new char[glyphNameSize];
	memset(glyphName, 0, glyphNameSize);

    uint32_t formatted_len = S_snprintf(textureName, textureNameSize, "%s/%s%s", dir, fname, tex_ext);
	gosASSERT(formatted_len <= textureNameSize - 1);

    formatted_len = S_snprintf(glyphName, glyphNameSize, "%s/%s%s", dir, fname, glyph_ext);
	gosASSERT(formatted_len <= glyphNameSize - 1);

    // SPLIT-1: gosTexture construction + createHardwareTexture + renderer
    // registration moved behind the main-TU shim (STOPs on failure there,
    // same as the old inline code).
    DWORD tex_id = gosRendererAddFontBmpTexture(textureName);

    gosFont* font = new gosFont();
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        delete font;
        STOP(("Failed to load font glyphs: %s\n", glyphName));
        return NULL;
    }

    font->font_name_ = new char[strlen(fname) + 1];
    strcpy(font->font_name_, fname);

    font->font_id_ = new char[strlen(fontFile) + 1];
    strcpy(font->font_id_, fontFile);

    font->tex_id_ = tex_id;

    delete[] textureName;
    delete[] glyphName;

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
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        *u = *v = 0;
        return;
    }

    *u = gi_.glyphs_[pos].u;
    *v = gi_.glyphs_[pos].v;
}

int gosFont::getCharAdvance(int c) const
{
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        return getMaxCharWidth();
    }

    return gi_.glyphs_[pos].advance;
}

const gosGlyphMetrics& gosFont::getGlyphMetrics(int c) const {
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_)
        pos = 0;

    return gi_.glyphs_[pos];
}
