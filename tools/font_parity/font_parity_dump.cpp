// Offline font parity dump. Loads every .glyph / .d3f file named on the command
// line through gos_font (gos_load_glyphs / gos_load_d3f) and prints, per file:
// header fields, every glyph's metrics (advance, bbox, valid, u, v), the ink
// arrays, an FNV-1a64 hash of the atlas pixels, and for a fixed string set the
// summed advance and the vertical ink extent. There is no kerning table in
// gos_font (advance only), so none is dumped. Output is text; diff two runs.
//
// Build (from a repo root, after vcvars64):
//   cl /nologo /std:c++latest /EHsc /O2 /MD /utf-8 /DLINUX_BUILD /DPLATFORM_WINDOWS
//      /DUSE_ASSEMBLER_CODE=0 /D_CRT_SECURE_NO_WARNINGS /I<build>/generated /IGameOS/include
//      /IGameOS/gameos /I3rdparty/cmake/include /I3rdparty/include /Imclib /Imclib/stuff /I.
//      tools/font_parity/font_parity_dump.cpp GameOS/gameos/gos_font.cpp /Fe:out.exe
// Run: out.exe <file> [<file> ...]  (files sorted by the caller)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include "gos_font.h"
#include "gameos.hpp"
#include "utils/mc2log.h"

// gos_font.cpp logs through mc2log; satisfy the one link symbol with a no-op.
namespace mc2::log {
void vlog(Cat, Level, Stream, const std::source_location&, std::string_view, std::format_args) noexcept {}
}

// Base keeps raw pointers, the unique_ptr commit owns them; read both the same way.
template <class P> static auto raw(P& p) {
    if constexpr (std::is_pointer_v<P>) return p; else return p.get();
}

static uint64_t fnv(const void* d, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(d);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static const char* const kStrings[] = {
    "MechCommander 2", "The quick brown fox jumps over the lazy dog 0123456789",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz", "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
    "Mission Objectives: Destroy 12 enemy units", "\xC4\xD6\xDC\xE4\xF6\xFC\xDF\xE9\xE8\xE0",
};

static void dump(const char* path) {
    gosGlyphInfo gi;
    gosD3FAtlas atlas;
    atlas.width = atlas.height = 0;
    size_t len = strlen(path);
    bool d3f = len > 4 && _stricmp(path + len - 4, ".d3f") == 0;
    bool ok = d3f ? gos_load_d3f(path, gi, atlas) : gos_load_glyphs(path, gi);
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    printf("FILE %s ok=%d\n", base, ok ? 1 : 0);
    if (!ok) return;
    printf(" num=%u start=%u maxadv=%u ascent=%u lineskip=%u\n", gi.num_glyphs_, gi.start_glyph_,
           gi.max_advance_, gi.font_ascent_, gi.font_line_skip_);
    const gosGlyphMetrics* g = raw(gi.glyphs_);
    const int8_t* it = raw(gi.ink_top_);
    const int8_t* ib = raw(gi.ink_bot_);
    const uint8_t* iv = raw(gi.ink_valid_);
    printf(" glyphs=%d ink=%d\n", g ? 1 : 0, (it && ib && iv) ? 1 : 0);
    for (uint32_t i = 0; g && i < gi.num_glyphs_; ++i) {
        printf(" g%u adv=%d x=[%d,%d] y=[%d,%d] valid=%u uv=%u,%u", i, g[i].advance, g[i].minx,
               g[i].maxx, g[i].miny, g[i].maxy, g[i].valid, g[i].u, g[i].v);
        if (it && ib && iv) printf(" ink=%u,%d,%d", iv[i], it[i], ib[i]);
        printf("\n");
    }
    printf(" glyphhash=%016llx\n", (unsigned long long)(g ? fnv(g, gi.num_glyphs_ * sizeof(gosGlyphMetrics)) : 0));
    const uint8_t* px = raw(atlas.pixels);
    printf(" atlas=%dx%d hash=%016llx\n", atlas.width, atlas.height,
           (unsigned long long)(px ? fnv(px, (size_t)atlas.width * atlas.height) : 0));
    for (const char* s : kStrings) {
        long adv = 0; int top = 127, bot = -128;
        for (const unsigned char* c = (const unsigned char*)s; *c; ++c) {
            if (*c < gi.start_glyph_ || *c - gi.start_glyph_ >= gi.num_glyphs_ || !g) continue;
            uint32_t k = *c - gi.start_glyph_;
            adv += g[k].advance;
            if (it && iv[k]) { if (it[k] < top) top = it[k]; if (ib[k] > bot) bot = ib[k]; }
        }
        printf(" str advsum=%ld ink=[%d,%d] %s\n", adv, top, bot, s);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) dump(argv[i]);
    return 0;
}
