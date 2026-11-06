// gameos_graphics: text breaking and drawing, the HUD batch flush and the text C API.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include <utility>
#include "mc2fmt.h"
#include "mc2_diag_verbose.h"
#include "utils/mc2log.h"


static int get_next_break(const char* text) {
    const char* start = text;
    do {
        char c = *text;
        if(c==' ' || c=='\n')
            return static_cast<int32_t>((text - start));
    } while(*text++);

    return static_cast<int32_t>((text - start - 1));
}

int findTextBreak(const char* text, const int count, const gosFont* font, const int region_width, int* out_str_width) {

    int width = 0;
    int pos = 0;

    int space_adv = font->getCharAdvance(' ');

    while(text[pos]) {

        int break_pos = get_next_break(text + pos);

        int cur_width = 0;
        for(int j=0;j<break_pos;++j) {
            cur_width += font->getCharAdvance(text[pos + j]);
        }

        if(width + cur_width >= region_width) {

            if(pos == 0) { // handle case when only one word in line and it does not fit it, just return whole line
                width = cur_width;
                pos = break_pos;
            }
            break;
        } else {
            width += cur_width;
            pos += break_pos;

            if(text[pos] == '\n') {
                pos++;
                break;
            }

            if(text[pos] == ' ') {
                width += space_adv;
                pos++;
            }
        }
    }

    if(out_str_width)
        *out_str_width = width;
    return pos;
}
int calcTextHeight(const char* text, const int count, const gosFont* font, int region_width)
{
    int pos = 0;
    int num_lines = 0;
    while(pos < count) {

        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, nullptr);
        pos += num_chars;
        num_lines++;
    }
    return num_lines;
}

void addCharacter(gosMesh* text_, const float u, const float v, const float u2, const float v2, const float x, const float y, const float x2, const float y2) {

    gos_VERTEX tr, tl, br, bl;

    tl.x = x;
    tl.y = y;
    tl.z = 0;
    tl.u = u;
    tl.v = v;
    tl.argb = 0xffffffff;
    tl.frgb = 0xff000000;

    tr.x = x2;
    tr.y = y;
    tr.z = 0;
    tr.u = u2;
    tr.v = v;
    tr.argb = 0xffffffff;
    tr.frgb = 0xff000000;

    bl.x = x;
    bl.y = y2;
    bl.z = 0;
    bl.u = u;
    bl.v = v2;
    bl.argb = 0xffffffff;
    bl.frgb = 0xff000000;

    br.x = x2;
    br.y = y2;
    br.z = 0;
    br.u = u2;
    br.v = v2;
    br.argb = 0xffffffff;
    br.frgb = 0xff000000;

    text_->addVertices(&tl, 1);
    text_->addVertices(&tr, 1);
    text_->addVertices(&bl, 1);

    text_->addVertices(&tr, 1);
    text_->addVertices(&br, 1);
    text_->addVertices(&bl, 1);

}

void gosRenderer::drawText(const char* text) {
    ZoneScopedN("DrawText");
    gosASSERT(text);

    if(beforeDrawCall()) return;

    const int count = static_cast<int>(strlen(text));


    // TODO: take text region into account!!!!
    
    gosASSERT(text_->getNumVertices() + 6 * count <= text_->getVertexCapacity());

    int ix, iy;
    getTextPos(ix, iy);
	float x = static_cast<float>(ix), y = static_cast<float>(iy);
    const float start_x = x;

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    if(!font) return;   // no font could be loaded (see gos_LoadFont); draw nothing

    const DWORD tex_id = font->getTextureId();
    const gosTexture* tex = getTexture(tex_id);
    gosTextureInfo ti;
    tex->getTextureInfo(&ti);
    const float oo_tex_width = 1.0f / static_cast<float>(ti.width_);
    const float oo_tex_height = 1.0f / static_cast<float>(ti.height_);
    
    const int font_height = font->getMaxCharHeight();
    const int font_ascent = font->getFontAscent();

    const int region_width = getTextRegionWidth();
    const int region_height = getTextRegionHeight();

    const int num_lines = calcTextHeight(text, count, font, region_width);
    if(ta.WrapType == 3) { // center in Y direction as well
        y += (region_height - num_lines * font_height) / 2;
    }
   
    int pos = 0;
    int str_width = 0;
    while(pos < count) {

        x = start_x;    
        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, &str_width);

        // WrapType		- 0=Left aligned, 1=Right aligned, 2=Centered, 3=Centered in region (X and Y)
        switch(ta.WrapType) {
            case 0: break;
            case 1: x += region_width - str_width; break;
            case 2: x += (region_width - str_width) / 2; break;
            case 3: // see vertical centering above
                    x += (region_width - str_width) / 2;
                    break;
        }

        for(int i=0; i<num_chars; ++i) {

            const char c = text[i + pos];

            // Skip non-printable control chars. findTextBreak still
            // includes the trailing '\n' in num_chars to drive line
            // advance via `y += font_height` below; we just must not
            // emit a quad for it. Legacy .glyph files had a zero-width
            // entry at index 10, so the old path drew nothing visible;
            // D3F atlases ship a placeholder glyph there that would
            // appear as a square at end-of-line and on \n\n blank lines.
            if(static_cast<unsigned char>(c) < 0x20) {
                continue;
            }

            const gosGlyphMetrics& gm = font->getGlyphMetrics(c);
            int char_off_x = gm.minx;
            int char_off_y = font_ascent - gm.maxy;
            int char_w = gm.maxx - gm.minx;
            int char_h = gm.maxy - gm.miny;

            // u/v are the actual atlas sample origin under the post-load
            // contract — char_off_x/y drive screen quad position only,
            // not the atlas read. The legacy .glyph loader pre-folds its
            // metrics so this path stays pixel-identical for that format.
            uint32_t iu0 = gm.u;
            uint32_t iv0 = gm.v;
            uint32_t iu1 = iu0 + char_w;
            uint32_t iv1 = iv0 + char_h;

            float u0 = static_cast<float>(iu0) * oo_tex_width;
            float v0 = static_cast<float>(iv0) * oo_tex_height;
            float u1 = static_cast<float>(iu1) * oo_tex_width;
            float v1 = static_cast<float>(iv1) * oo_tex_height;

            addCharacter(text_, u0, v0, u1, v1, (x + char_off_x), (y + char_off_y), (x + char_off_x + char_w), (y + char_off_y + char_h));

            x += font->getCharAdvance(c);
        }
        y += font_height;
        pos += num_chars;
    }
    if (renderStates_[gos_State_IsHUD]) {
        const int n = text_->getNumVertices();
        if (n > 0 && !hudFlushed_) {
            HudDrawCall call;
            call.kind = kHudTextQuadBatch;
            call.vertices.assign(text_->getVertices(), text_->getVertices() + n);
            memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
            call.projection = projection_;
            call.fontTexId = tex_id;
            call.foregroundColor = ta.Foreground;
            call.scaleExempt = s_hud_scale_exempt;
        call.canvasExempt = (s_hud_canvas_exempt_mode < 0)
            ? s_hud_scale_exempt : (s_hud_canvas_exempt_mode != 0);
            hudBatch_.push_back(std::move(call));
        } else if (hudFlushed_) {
            MC2_LOG(Render, Debug, "[HUD] Late drawText discarded (after flushHUDBatch)");
        }
        text_->rewind();
        return;
    }

    text_->rewind();
}

#ifdef MC2_VULKAN
static bool vkHudDrawRepresentable(const mc2vk::VkHudDraw& draw)
{
    // AlphaMode 1 (OneZero/opaque), 3 (AlphaInvAlpha) and 4 (OneInvAlpha, the
    // effect-sprite blend) are all rasterizable by the tail. AlphaMode 4 now binds a
    // dedicated OneInvAlpha textured pipe (ONE, 1-srcA) in engine_present_vk.cpp that
    // matches GL's glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) — the additive-glow
    // effect blend — so beams/tracers/explosions composite at GL brightness.
    // VK-FIELD-VFX-7: AlphaMode 2 (OneOne, pure additive — muzzle flashes / tracer
    // sprites) now binds a dedicated (ONE,ONE) pipe in engine_present_vk.cpp.
    if (draw.alphaMode != 1 && draw.alphaMode != 2 && draw.alphaMode != 3 && draw.alphaMode != 4)
        return false;
    const bool textured = draw.textureHandle != 0 && draw.textureHandle != INVALID_TEXTURE_ID;
    if (textured) {
        if (draw.textureWidth == 0 || draw.textureHeight == 0 ||
            draw.textureSerial == 0 || !draw.textureRgba || draw.textureRgba->empty())
            return false;
        if (draw.kind == std::to_underlying(kHudQuadBatch))
            return draw.vertices.size() >= 4 && (draw.vertices.size() % 4) == 0;
        if (draw.kind == std::to_underlying(kHudTextQuadBatch))
            return draw.vertices.size() >= 4 &&
                   (((draw.vertices.size() % 4) == 0) || ((draw.vertices.size() % 3) == 0));
        if (draw.kind == std::to_underlying(kHudTriBatch))
            return !draw.vertices.empty() && (draw.vertices.size() % 3) == 0;
        return false;
    }
    if (draw.kind == std::to_underlying(kHudQuadBatch))
        return draw.vertices.size() >= 4 && (draw.vertices.size() % 4) == 0;
    if (draw.kind == std::to_underlying(kHudLineBatch))
        return !draw.vertices.empty() && (draw.vertices.size() % 2) == 0;
    if (draw.kind == std::to_underlying(kHudTriBatch))
        return !draw.vertices.empty() && (draw.vertices.size() % 3) == 0;
    return false;
}
#endif

void gosRenderer::flushHUDBatch()
{
    if (hudBatch_.empty()) {
        hudFlushed_ = true;
        return;
    }

    // VISUAL-CAPTURE-CLEAN-1: explicit capture flag (default OFF) drops the whole
    // HUD/UI batch on both backends so GL/VK A/B captures are HUD-free.
    if (gos::visual_capture::cleanFrame()) {
        hudBatch_.clear();
        hudFlushed_ = true;
        return;
    }

    render_contract::beginPassScope(render_contract::PassIdentity::UI,
                                    "gosRenderer_flushHUDBatch");

    // UI-SAME-ORDER-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical OFF). PIN INVARIANT: additive validate counters only —
    // no GL state change, no reorder, no draw change. The flush body (hudBatch_ replay, the
    // memcpy state save/restore at :7519) runs UNCHANGED between begin and end. Placed after the
    // empty-batch early-return so begin only fires on non-empty frames; the RAII guard's dtor
    // fires end on ALL exit paths. FBO-only: Backbuffer (default FBO 0, bound by pp->endScene
    // pre-flush). Ambient is DO_NOT_MODEL — UI ambient is per-draw legacy gos dynamic state.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::UI,
                                              "gosRenderer_flushHUDBatch");
    struct TopLevelUiGuard_ {
        ~TopLevelUiGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::UI,
                                                    "gosRenderer_flushHUDBatch");
        }
    } _tlUiGuard;

    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // to true by mission->start() and false by mission->destroy()). Menus and
    // modal dialogs run through the same HUD buffer but stay at 100%; we skip
    // any call whose centroid is above 60% of the screen height.
    //
    // Anchor is SINGLE — bottom-center (sw/2, sh). Every bottom-band primitive
    // shrinks toward that one point. Previously we used 9-slice L/C/R anchoring
    // which pulled the tacmap/command clusters apart rather than compressing
    // the HUD strip as a coherent whole. Single-anchor keeps the strip
    // contiguous: tacmap drifts slightly inward-right, force-group stays put,
    // command panel drifts slightly inward-left, all without visible gaps.
    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // true by Mission::start and false by Mission::destroy). Menus and modal
    // dialogs run through the same HUD buffer but stay at 100%.
    //
    // Single bottom-center anchor (sw/2, sh). Every qualifying primitive
    // shrinks toward that one point. Centroid-based gate at 60% of screen
    // height — draws whose centroid is above that threshold are left alone
    // (catches dialogs, menus). This was the variant that looked good in
    // iteration; later attempts at max-Y gating / shrink-in-place broke more
    // than they fixed (touched scene/overlay draws, pulled tall panels out
    // of their corners, etc.).
    // HUD-SCALE-SPACE-1: env override for harness A/B (MC2_HUD_SCALE=0.4 etc.);
    // read once, wins over the default until the ImGui slider changes it.
    {
        static bool s_envScaleDone = false;
        if (!s_envScaleDone) {
            s_envScaleDone = true;
            if (mc2::gate::isNonEmpty<"MC2_HUD_SCALE">()) {
                const float v = mc2::gate::get<"MC2_HUD_SCALE">();
                if (v >= 0.2f && v <= 1.0f) s_hud_scale = v;
            }
        }
    }
    const float scale = s_hud_scale;
    // HUD-SCALE-SPACE-1 diagnostic: which coordinate space are the batched HUD
    // verts in vs the band test (width_/height_ = real drawable)? One line per
    // 300 flushes under MC2_LOG_PREVIEW.
    if (mc2::gate::get<"MC2_LOG_PREVIEW">()) {
        static int s_hudDiagTick = 0;
        // Sample every flush while the shrink is supposed to be active (mission),
        // every 300th otherwise — the earlier cadence missed the mission window.
        if ((s_hud_scale_active || (s_hudDiagTick++ % 300) == 0) && !hudBatch_.empty()) {
            float ymin = 1e9f, ymax = -1e9f, xmax = -1e9f;
            size_t nv = 0;
            for (const HudDrawCall& c : hudBatch_)
                for (const gos_VERTEX& v : c.vertices) {
                    ymin = std::min(ymin, v.y);
                    ymax = std::max(ymax, v.y);
                    xmax = std::max(xmax, v.x);
                    ++nv;
                }
            if (bool hf = mc2::log::enabled(mc2::log::Cat::Render, mc2::log::Level::Info)) {
                MC2_LOG(Render, Info, "[HUDSCALE] active={:d} scale={:.2f} rendererWH={:d}x{:d} batch={:d} verts={:d} vertY=[{:.0f}..{:.0f}] vertXmax={:.0f} band={:.0f}", mc2::fmt::as_i32(static_cast<int>(s_hud_scale_active)), scale, mc2::fmt::as_i32(width_), mc2::fmt::as_i32(height_), mc2::fmt::as_size(hudBatch_.size()), mc2::fmt::as_size(nv), ymin, ymax, xmax, static_cast<float>(height_) * 0.60f);

            }
        }
    }
    if (s_hud_scale_active && scale < 0.999f) {
        // HUD-SCALE-SPACE-1: band + anchor must live in the SAME space as the
        // batched verts. ControlGui lays the HUD out in Environment.screenWidth/
        // Height space (real-res on stock nifty, the 800x600 logical canvas
        // under the ui-phase1 HUD-RES-CLAMP) — using the renderer's physical
        // size here put the 60% band at 1296px against 800-space verts, so no
        // HUD centroid ever entered the band and the shrink silently no-opped.
        const float sw = (Environment.screenWidth  > 0) ? static_cast<float>(Environment.screenWidth)  : static_cast<float>(width_);
        const float sh = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) : static_cast<float>(height_);
        const float bottomBand = sh * 0.60f;
        const float ax = sw * 0.5f;
        const float ay = sh;
        int scaled = 0, skippedBand = 0, skippedExempt = 0;
        for (HudDrawCall& call : hudBatch_) {
            if (call.vertices.empty()) continue;
            if (call.scaleExempt) { ++skippedExempt; continue; }   // cursor + modal dialogs: never shrink
            float cy = 0.0f;
            for (const gos_VERTEX& v : call.vertices) cy += v.y;
            cy /= static_cast<float>(call.vertices.size());
            if (cy < bottomBand) { ++skippedBand; continue; }
            ++scaled;
            for (gos_VERTEX& v : call.vertices) {
                v.x = ax + (v.x - ax) * scale;
                v.y = ay + (v.y - ay) * scale;
            }
        }
        if (mc2::gate::get<"MC2_LOG_PREVIEW">()) {
            static int s_hudApplyTick = 0;
            if ((s_hudApplyTick++ % 60) == 0) {
                float sampleY = -1.f;
                for (const HudDrawCall& c : hudBatch_)
                    if (!c.vertices.empty() && !c.scaleExempt) { sampleY = c.vertices[0].y; break; }
                if (bool hf = mc2::log::enabled(mc2::log::Cat::Render, mc2::log::Level::Info)) {
                    MC2_LOG(Render, Info, "[HUDSCALE] apply sw={:.0f} sh={:.0f} band={:.0f} scaled={:d} skipBand={:d} skipExempt={:d} firstVertYafter={:.0f}", sw, sh, bottomBand, mc2::fmt::as_i32(scaled), mc2::fmt::as_i32(skippedBand), mc2::fmt::as_i32(skippedExempt), sampleY);

                }
            }
        }
    }

    // UI-ASPECT-ANCHOR-1: map the logical-space UI verts onto the 16:9 canvas.
    // The full-window stretch maps logical [0,width_] -> [0,drawableW]; the
    // canvas wants [bx, bx+bw] instead, so in LOGICAL space that is a scale of
    // bw/drawableW about origin plus a logical offset of width_*bx/drawableW
    // (same vertex-space technique as the s_hud_scale block above). Applies to
    // every call INCLUDING scaleExempt (cursor must follow the canvas mouse
    // mapping); active only on frames the front-end asserted (menus).
    // Two modes:
    //   front-end (s_uiCanvasAssert): remap EVERY call including scaleExempt —
    //     the cursor must follow the canvas-relative mouse mapping.
    //   mission (s_hudCanvasActive): remap HUD chrome only, SKIP scaleExempt —
    //     the mission cursor tracks the full screen (world pick spans the whole
    //     drawable) and modal dialogs keep their legacy full-stretch space.
    //     Hit-tests for remapped chrome go through gos_HudInverseMousePoint
    //     (getMouseHudX/Y), which inverts this transform.
    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        const bool menuCanvas = gos_ComputeUiCanvasBox(
            Environment.drawableWidth, Environment.drawableHeight, &bx, &by, &bw, &bh);
        const bool hudCanvas = !menuCanvas && gos_ComputeHudCanvasBox(
            Environment.drawableWidth, Environment.drawableHeight, &bx, &by, &bw, &bh);
        if (menuCanvas || hudCanvas)
        {
            const float dw = static_cast<float>(Environment.drawableWidth);
            const float dh = static_cast<float>(Environment.drawableHeight);
            const float fx = static_cast<float>(bw) / dw;
            const float fy = static_cast<float>(bh) / dh;
            const float lx = static_cast<float>(width_)  * (static_cast<float>(bx) / dw);
            const float ly = static_cast<float>(height_) * (static_cast<float>(by) / dh);
            for (HudDrawCall& call : hudBatch_) {
                if (hudCanvas && call.canvasExempt)
                    continue;   // cursor, world-anchored overlays, modals stay full-space
                for (gos_VERTEX& v : call.vertices) {
                    v.x = v.x * fx + lx;
                    v.y = v.y * fy + ly;
                }
            }
        }
    }

    // META-FIX design D (MC2_RES_ASSERT) chokepoint (iii): HUD canvas
    // consistency. (a) the canvas box lies inside the drawable; (b) the
    // forward HUD remap — hud-scale bottom-band shrink THEN canvas, composed
    // exactly as the two vertex blocks above apply it — followed by
    // gos_HudInverseMousePoint is the identity within 0.5 px. Catches the
    // two-file comment-synced drift class between this function and
    // gos_HudInverseMousePoint (gameos_graphics_params.cpp). hudCanvas mode
    // only: the menu canvas path uses the canvas-relative mouse normalize,
    // not gos_HudInverseMousePoint. Default OFF = one static bool test.
    if (mc2res::armed()) {
        int rbx = 0, rby = 0, rbw = 0, rbh = 0;
        const bool resHudCanvas = gos_ComputeHudCanvasBox(
            Environment.drawableWidth, Environment.drawableHeight,
            &rbx, &rby, &rbw, &rbh);
        if (resHudCanvas) {
            char resDetail[192];
            if (rbw <= 0 || rbh <= 0 || rbx < 0 || rby < 0 ||
                rbx + rbw > Environment.drawableWidth ||
                rby + rbh > Environment.drawableHeight) {
                mc2::fmt::to_buf(resDetail, "box={:d},{:d} {:d}x{:d} drawable={:d}x{:d}", mc2::fmt::as_i32(rbx), mc2::fmt::as_i32(rby), mc2::fmt::as_i32(rbw), mc2::fmt::as_i32(rbh), mc2::fmt::as_i32(Environment.drawableWidth), mc2::fmt::as_i32(Environment.drawableHeight));
                mc2res::violation("hud_canvas_bounds", resDetail);
            } else {
                const float rdw = static_cast<float>(Environment.drawableWidth);
                const float rdh = static_cast<float>(Environment.drawableHeight);
                const float rfx = static_cast<float>(rbw) / rdw;
                const float rfy = static_cast<float>(rbh) / rdh;
                const float rlx = static_cast<float>(width_)  * (static_cast<float>(rbx) / rdw);
                const float rly = static_cast<float>(height_) * (static_cast<float>(rby) / rdh);
                // Probe pair: one above the hud-scale bottom band, one inside
                // it (exercises the shrink∘canvas composition when the HUD
                // shrink is active — a canvas-only probe false-passes there).
                const float probePts[2][2] = {
                    { static_cast<float>(width_) * 0.5f, static_cast<float>(height_) * 0.25f },
                    { static_cast<float>(width_) * 0.5f, static_cast<float>(height_) * 0.95f },
                };
                for (const auto & probePt : probePts) {
                    float px = probePt[0];
                    float py = probePt[1];
                    const float hudScale = gos_GetHudScale();
                    if (gos_GetHudScaleActive() && hudScale <= 0.999f) {
                        const float sw = static_cast<float>(width_);
                        const float sh = static_cast<float>(height_);
                        if (py >= sh * 0.60f) {   // bottom band, as above
                            const float ax = sw * 0.5f;
                            const float ay = sh;
                            px = ax + (px - ax) * hudScale;
                            py = ay + (py - ay) * hudScale;
                        }
                    }
                    px = px * rfx + rlx;
                    py = py * rfy + rly;
                    gos_HudInverseMousePoint(px, py);
                    if (fabsf(px - probePt[0]) > 0.5f ||
                        fabsf(py - probePt[1]) > 0.5f) {
                        mc2::fmt::to_buf(resDetail, "probe=({:.1f},{:.1f}) roundtrip=({:.2f},{:.2f}) box={:d},{:d} {:d}x{:d} scale={:.3f}", static_cast<double>(probePt[0]), static_cast<double>(probePt[1]), static_cast<double>(px), static_cast<double>(py), mc2::fmt::as_i32(rbx), mc2::fmt::as_i32(rby), mc2::fmt::as_i32(rbw), mc2::fmt::as_i32(rbh), static_cast<double>(gos_GetHudScale()));
                        mc2res::violation("hud_roundtrip", resDetail);
                    }
                }
            }
        }
    }

    // pp->endScene() binds FB 0 and sets the full-screen viewport for us,
    // but leaves VAO 0 bound — rebind our VAO so glVertexAttribPointer works.

    static const bool s_uiPassMeasure = false;
    if (s_uiPassMeasure) {
        // Publish the prior frame's measured CPU draw counts.
        mc2_ui_pass_begin_measure();
    }

    if (s_uiPassMeasure) {
        unsigned calls[4] = {};
        unsigned textured = 0;
        unsigned alphaModes[8] = {};
        unsigned totalVertices = 0;
        for (const HudDrawCall& call : hudBatch_) {
            const unsigned kind = static_cast<unsigned>(call.kind);
            if (kind < 4) ++calls[kind];
            if (call.stateSnapshot[gos_State_Texture] != 0 || call.kind == kHudTextQuadBatch)
                ++textured;
            const unsigned alpha = call.stateSnapshot[gos_State_AlphaMode];
            if (alpha < 8) ++alphaModes[alpha];
            totalVertices += static_cast<unsigned>(call.vertices.size());
        }

        static unsigned s_hudMeasureFrames = 0;
        ++s_hudMeasureFrames;
        if (s_hudMeasureFrames <= 10 || (s_hudMeasureFrames % 120) == 0) {
            mc2::fmt::print_err("[VK_HUD_AUDIT v1] frame={:d} calls={:d} quads={:d} lines={:d} tris={:d} text={:d} textured={:d} vertices={:d} alpha0={:d} alpha1={:d} alpha2={:d} alpha3={:d}\n", mc2::fmt::as_u32(s_hudMeasureFrames), mc2::fmt::as_u32(static_cast<unsigned>(hudBatch_.size())), mc2::fmt::as_u32(calls[0]), mc2::fmt::as_u32(calls[1]), mc2::fmt::as_u32(calls[2]), mc2::fmt::as_u32(calls[3]), mc2::fmt::as_u32(textured), mc2::fmt::as_u32(totalVertices), mc2::fmt::as_u32(alphaModes[0]), mc2::fmt::as_u32(alphaModes[1]), mc2::fmt::as_u32(alphaModes[2]), mc2::fmt::as_u32(alphaModes[3]));
        }
    }

#ifdef MC2_VULKAN
    bool nativeHudQueued = false;
    {
        // Env-token-only memo (no disarm interaction): the env block is
        // process-static, so one getenv per process is exact.
        static const bool s_hudDrawDumpEnv = false;
        std::vector<mc2vk::VkHudDraw> nativeDraws;
        nativeDraws.reserve(hudBatch_.size());
        for (const HudDrawCall& call : hudBatch_) {
            mc2vk::VkHudDraw draw;
            draw.kind = static_cast<uint32_t>(call.kind);
            draw.textureHandle = (call.kind == kHudTextQuadBatch)
                ? static_cast<uint32_t>(call.fontTexId)
                : static_cast<uint32_t>(call.stateSnapshot[gos_State_Texture]);
            if (draw.textureHandle != 0 && draw.textureHandle != INVALID_TEXTURE_ID) {
                if (gosTexture* tex = getTexture(draw.textureHandle)) {
                    gosTextureInfo ti;
                    tex->getTextureInfo(&ti);
                    draw.textureWidth = static_cast<uint32_t>(ti.width_);
                    draw.textureHeight = static_cast<uint32_t>(ti.height_);
                    draw.textureFormat = static_cast<uint32_t>(ti.format_);
                    draw.textureSerial = tex->getContentSerial();
                    // Pre-compressed textures (BC7 .ktx2 sidecars: effect sprites) get their RGBA8 mirror decoded from
                    // their blocks on first use, so textured beams / effects composite instead of self-declining.
                    if (draw.textureWidth != 0 &&
                        draw.textureHeight != 0 && !tex->hasRgbaMirror())
                        tex->ensureRgbaMirror();
                    if (draw.textureWidth != 0 && draw.textureHeight != 0 && tex->hasRgbaMirror())
                        draw.textureRgba = tex->getRgbaMirror();
                }
            }
            draw.alphaMode = call.stateSnapshot[gos_State_AlphaMode];
            // VK-FIELD-VFX-4: an effect tri-batch (AlphaMode 4, the OneInvAlpha
            // additive blend) whose texture STILL has no mirror after the GPU
            // readback attempt falls back to UNTEXTURED vertex-coloured (the
            // round-3 proven beam look) instead of self-declining — a weapon
            // beam must never vanish outright over a missing texture mirror.
            {
                if (draw.kind == static_cast<uint32_t>(kHudTriBatch) &&
                    (draw.alphaMode == 4 || draw.alphaMode == 2) &&
                    draw.textureHandle != 0 && draw.textureHandle != INVALID_TEXTURE_ID &&
                    (!draw.textureRgba || draw.textureRgba->empty()))
                    draw.textureHandle = 0;
            }
            draw.textureAddress = call.stateSnapshot[gos_State_TextureAddress];
            draw.filter = call.stateSnapshot[gos_State_Filter];
            draw.foregroundColor = call.foregroundColor;
            draw.vertices.reserve(call.vertices.size());
            for (const gos_VERTEX& v : call.vertices) {
                mc2vk::VkHudVertex out;
                out.x = v.x; out.y = v.y; out.z = v.z; out.rhw = v.rhw;
                out.argb = v.argb; out.frgb = v.frgb;
                out.u = v.u; out.v = v.v;
                draw.vertices.push_back(out);
            }
            // VK-FIELD-VFX-3: per-draw self-decline instead of the all-or-nothing veto.
            // The VK tail renderer (recordPendingHudColorProof) ALREADY skips each draw
            // it cannot represent (no texture mirror / unsupported kind) individually
            // (`continue`), so one non-representable draw need NOT force the WHOLE HUD to
            // the DISCARDED GL backbuffer. Under the VFX umbrella we always composite the
            // native tail: the representable subset (brackets/bars/beams/HUD/text + mirror-
            // backed effect sprites) survives, and the rest drops per-draw (it would be
            // lost to the discarded GL HUD anyway). This removes the poisoning landmine
            // where any live textured effect killed the entire HUD composite, and lets the
            // effect classes (carnage/muzzle/artillery) route through IsHUD safely.
            const bool drawRepresentable = vkHudDrawRepresentable(draw);
            if (!drawRepresentable) {
                static unsigned s_nativeHudDeclineLogs = 0;
                if (s_nativeHudDeclineLogs < 16) {
                    mc2::fmt::print_err("[VK_HUD_SKIP_DECLINE v1] kind={:d} tex={:d} wh={:d}x{:d} serial={:d} rgba={:d} alpha={:d} verts={:d}\n", mc2::fmt::as_u32(draw.kind), mc2::fmt::as_u32(draw.textureHandle), mc2::fmt::as_u32(draw.textureWidth), mc2::fmt::as_u32(draw.textureHeight), mc2::fmt::as_u32(draw.textureSerial), mc2::fmt::as_size(draw.textureRgba ? draw.textureRgba->size() : static_cast<size_t>(0)), mc2::fmt::as_u32(draw.alphaMode), mc2::fmt::as_size(draw.vertices.size()));
                    ++s_nativeHudDeclineLogs;
                }
            }
            // VK-HUD-GLYPH-CORRUPTION diagnostic: dump per-draw texture handle+serial
            // for the first few frames so an intra-frame same-handle/different-serial
            // collision (mirror keyed by handle -> last-upload-wins -> stale glyphs)
            // is directly visible. Gated by MC2_VK_HUD_DRAW_DUMP (env token
            // memoized once per process: VK-HUD-GATE-HOIST-1).
            if (s_hudDrawDumpEnv) {
                static unsigned s_hudDumpLines = 0;
                if (s_hudDumpLines < 400 && draw.textureHandle != 0 &&
                    draw.textureHandle != INVALID_TEXTURE_ID) {
                    ++s_hudDumpLines;
                    mc2::fmt::print_err("[VK_HUD_DRAW_DUMP] batchseq={:d} kind={:d} handle={:d} serial={:d} wh={:d}x{:d} verts={:d}\n", mc2::fmt::as_u32(s_hudDumpLines), mc2::fmt::as_u32(draw.kind), mc2::fmt::as_u32(draw.textureHandle), mc2::fmt::as_u32(draw.textureSerial), mc2::fmt::as_u32(draw.textureWidth), mc2::fmt::as_u32(draw.textureHeight), mc2::fmt::as_size(draw.vertices.size()));
                }
            }
            nativeDraws.push_back(std::move(draw));
        }
        // HUD-CANVAS-EXTENT-FIX: pass the logical canvas the HUD vertices were authored
        // in (width_/height_ == Environment.screenWidth/Height) so the VK tail maps them
        // to NDC correctly even when the present image (physical drawable) differs.
        nativeHudQueued = mc2vk::getOrCreateEnginePresentVk().queueHudVulkanFrame(
            std::move(nativeDraws), static_cast<uint32_t>(width_), static_cast<uint32_t>(height_));
    }
#endif


#ifdef MC2_VULKAN
    if (nativeHudQueued) {
        static unsigned s_nativeHudSkipFrames = 0;
        ++s_nativeHudSkipFrames;
        if (mc2DiagVerbose() && (s_nativeHudSkipFrames <= 10 || (s_nativeHudSkipFrames % 120) == 0)) {
            mc2::fmt::print_err("[VK_HUD_SKIP v1] frame={:d} draws={:d} reason=\"native_full_coverage\"\n", mc2::fmt::as_u32(s_nativeHudSkipFrames), mc2::fmt::as_u32(static_cast<unsigned>(hudBatch_.size())));
        }
        hudFlushed_ = true;
        s_hud_scale_exempt = false;
        render_contract::endPassScope(render_contract::PassIdentity::UI,
                                      "gosRenderer_flushHUDBatch");
        return;
    }
#endif

    // The native tail declined the frame (no Vulkan present path): there is no GL replay, the batch is dropped.
    {
        static unsigned s_hudDeclineLogs = 0;
        if (s_hudDeclineLogs < 4) {
            ++s_hudDeclineLogs;
            mc2::fmt::print_err("[VK_HUD_DECLINE v1] draws={:d} native tail declined the frame; the HUD batch is dropped\n", mc2::fmt::as_u32(static_cast<unsigned>(hudBatch_.size())));
        }
    }
    hudFlushed_ = true;

    // Auto-clear the scale-exemption latch each frame so it can never leak into
    // the next frame's non-exempt HUD recording. Callers (cursor sprite, modal
    // dialogs) set it true around their own draws; this is the single reset.
    s_hud_scale_exempt = false;

    // UI-ASPECT-ANCHOR-1: latch this frame's canvas assert for early-next-frame
    // consumers (mouse normalize runs before any render code), then re-arm.
    s_uiCanvasLatch  = s_uiCanvasAssert;
    s_uiCanvasAssert = false;

    render_contract::endPassScope(render_contract::PassIdentity::UI,
                                  "gosRenderer_flushHUDBatch");
}

void gosRenderer::flush()
{
}


void __stdcall gos_TextDraw( const char *Message, ... )
{

	if (!Message || !strlen(Message)) {
        MC2_LOG(Render, Debug, "Trying to draw zero legth string");
        return;
    }

	va_list	ap;
    va_start(ap, Message);

    static const int MAX_TEXT_LEN = 4096;
	char text[MAX_TEXT_LEN] = {0};

	vsnprintf(text, MAX_TEXT_LEN - 1, Message, ap);

	size_t len = strlen(text);
	text[len] = '\0';

    va_end(ap);

    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawText(text);
}

void __stdcall gos_TextDrawBackground( int Left, int Top, int Right, int Bottom, DWORD Color )
{
    // TODO: Is it correctly Implemented?
    gosASSERT(g_gos_renderer);


    gos_VERTEX v[4];
    v[0].x = static_cast<float>(Left);
    v[0].y = static_cast<float>(Top);
    v[0].z = 0;
	v[0].argb = Color;
	v[0].frgb = 0;
	v[0].u = 0;	
	v[0].v = 0;	
    memcpy(&v[1], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[2], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[3], &v[0], sizeof(gos_VERTEX));
    v[1].x = static_cast<float>(Right);
    v[1].u = 1.0f;

    v[2].x = static_cast<float>(Right);
    v[2].y = static_cast<float>(Bottom);
    v[2].u = 1.0f;
    v[2].v = 0.0f;

    v[1].y = static_cast<float>(Bottom);
    v[1].v = 1.0f;

    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(v, 4);
}

void __stdcall gos_TextSetAttributes( HGOSFONT3D FontHandle, DWORD Foreground, float Size, bool WordWrap, bool Proportional, bool Bold, bool Italic, DWORD WrapType/*=0*/, bool DisableEmbeddedCodes/*=0*/)
{
    gosASSERT(g_gos_renderer);

    gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    ta.FontHandle = FontHandle;
    ta.Foreground = Foreground;
    ta.Size = Size;
    ta.WordWrap = WordWrap;
    ta.Proportional = Proportional;
    ta.Bold = Bold;
    ta.Italic = Italic;
    ta.WrapType = WrapType;
    ta.DisableEmbeddedCodes = DisableEmbeddedCodes;
}

void __stdcall gos_TextSetPosition( int XPosition, int YPosition )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextPos(XPosition, YPosition);
}

void __stdcall gos_TextSetRegion( int Left, int Top, int Right, int Bottom )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextRegion(Left, Top, Right, Bottom);
}

void __stdcall gos_TextStringLength( DWORD* Width, DWORD* Height, const char *fmt, ... )
{
    gosASSERT(Width && Height);

    if(!fmt) {
        MC2_LOG(Render, Debug, "No text to calculate length!");
        *Width = 1;
        *Height = 1;
        return;
    }

    const int   MAX_TEXT_LEN = 4096;
	char        text[MAX_TEXT_LEN] = {0};
	va_list	    ap;

    va_start(ap, fmt);
	vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);

	size_t len = strlen(text);
    text[len] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    if(!font) { *Width = 0; *Height = 0; return; }   // no font could be loaded (see gos_LoadFont)

    int num_newlines = 0;
    int max_width = 0;
    int cur_width = 0;
    const char* txtptr = text;

    while(*txtptr) {
        if(*txtptr == '\n') {
            num_newlines++;
            max_width = max_width > cur_width ? max_width : cur_width;
            cur_width = 0;
        } else {
            const int cw = font->getCharAdvance(*txtptr);
            cur_width += cw;
        }
        txtptr++;
    }
    max_width = max_width > cur_width ? max_width : cur_width;

    *Width = max_width;
    *Height = (num_newlines + 1) * font->getMaxCharHeight();
}

////////////////////////////////////////////////////////////////////////////////
// Visual ink bounds for the active font + text. Width matches
// gos_TextStringLength (max line width across \n). Top/Bottom are signed
// pixel offsets from the gos_TextSetPosition y coordinate to the topmost
// / bottommost inked pixel that would render — independent of line_skip
// padding. Use for vertically centering labels in fixed frames where
// line-skip-based centering biases ALL CAPS text low.
//
// Negative Top is legitimate for extended glyphs (Ä Ö Ü etc.) that sit
// above the ASCII-trimmed band — those would render slightly clipped at
// the top under the current renderer, but the bounds still reflect
// where ink would conceptually appear.
//
// Multi-line: Top is the first line's top, Bottom is
// (num_lines − 1) * font_line_skip + last line's bottom. Single-line
// callers (buttons / list items) get the obvious answer.
//
// Legacy .glyph fonts (no per-glyph ink data) fall back to
// Top=0, Bottom=line-skip-equivalent — centering math degrades to the
// existing line-skip-based behavior, no improvement but no regression.
void __stdcall gos_TextVisualBounds( DWORD* Width, int* Top, int* Bottom, const char *fmt, ... )
{
    gosASSERT(Width && Top && Bottom);

    const int   MAX_TEXT_LEN = 4096;
    char        text[MAX_TEXT_LEN] = {0};
    va_list     ap;

    va_start(ap, fmt);
    vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);
    text[MAX_TEXT_LEN - 1] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    if(!font) { *Width = 0; *Top = 0; *Bottom = 0; return; }   // no font could be loaded (see gos_LoadFont)

    const gosGlyphInfo& gi = font->getGlyphInfo();
    const int line_skip = font->getMaxCharHeight();

    int max_width = 0;
    int cur_width = 0;
    int line_index = 0;
    int min_top = INT_MAX;
    int max_bot = INT_MIN;
    bool any_ink = false;

    // Per-glyph: project each ink position into the multi-line block's
    // y coordinate as `line_index * line_skip + ink_offset`. Tracking
    // min/max globally avoids a leading-empty-lines bug where
    // first-line-ink bookkeeping would otherwise emit a Top that
    // ignores the line offset.
    for(const char* p = text; ; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if(c == '\n' || c == '\0') {
            max_width = std::max(max_width, cur_width);
            if(c == '\0') break;
            line_index++;
            cur_width = 0;
            continue;
        }
        cur_width += font->getCharAdvance(c);
        if(gi.ink_valid_ && c < gi.num_glyphs_ && gi.ink_valid_[c]) {
            int base = line_index * line_skip;
            int t = base + static_cast<int>(gi.ink_top_[c]);
            int b = base + static_cast<int>(gi.ink_bot_[c]);
            min_top = std::min(min_top, t);
            max_bot = std::max(max_bot, b);
            any_ink = true;
        }
    }

    *Width = static_cast<DWORD>(max_width);

    if(any_ink) {
        *Top    = min_top;
        *Bottom = max_bot;
    } else if(gi.ink_valid_) {
        // Whitespace-only or empty — degenerate, no ink to center on.
        *Top    = 0;
        *Bottom = 0;
    } else {
        // Legacy .glyph fallback — line-skip-based geometry under the
        // inclusive-bounds convention callers use ((Top+Bottom+1)/2).
        // The `- 1` makes (0 + (lines*ls - 1) + 1)/2 = lines*ls/2,
        // matching gos_TextStringLength's height/2 centering result.
        *Top    = 0;
        *Bottom = (line_index + 1) * line_skip - 1;
    }
}
