// gos_visual_capture.cpp - Slice S9 v1 implementation. See header for scope.
//
// Determinism notes (the S9 gate):
//   * PNG bytes are produced by a self-contained encoder (stored DEFLATE
//     blocks + our own CRC32/Adler32) so the same pixels always yield the same
//     file bytes -- no external zlib version variance, no timestamp chunk.
//   * The pixel source is the offscreen scene FBO (RGBA16F clamped to 8-bit by
//     the driver on readback), the same target the legacy [SCREENSHOT v1] path
//     reads -- robust to a minimized/occluded window (FBO 0 reads black).
//   * Bookmark replay hard-sets the camera via setPosition(pos,false) /
//     setRotation(...) -- bypassing swoop/goal interpolation -- and settles N
//     frames before reading, flushing chunk-LOD (frame N-1 MVP latency),
//     shadow warmup, and bloom history.
//
// v1.5 DETERMINISM (the sim-freeze fix):
//   * The v1 capture was nondeterministic: scenarioTime kept advancing across
//     the settle window, so water UV scroll, gosFX phase, light flicker, and
//     unit motion differed run-to-run (47-58% of pixels with small deltas).
//   * v1.5 fix: the bookmark sweep now PAUSES the mission (menu-less, no sound,
//     no HUD overlay) BEFORE the first settle frame. mission.cpp:531 gates
//     `scenarioTime += ...` AND all unit/sensor/collision updates behind
//     isPaused(), so a paused mission freezes the exact clock that drives every
//     animated source. Because the freeze is held for the WHOLE sweep,
//     scenarioTime is constant across bookmarks AND identical between two runs
//     that reach the trigger frame with the same frozen state -> water/FX/light
//     phase and unit positions are byte-identical -> captures are byte-stable.
//   * The sweep fires at a fixed trigger frame (MC2_VISUAL_CAPTURE_FRAME,
//     default 120) so both runs trigger with the same frozen sim state. We
//     pause, hold-camera + settle (render-only; sim frozen so settle just
//     flushes texture streaming / chunk-LOD admission / shadow warmup / bloom
//     history), capture, advance. After the sweep we restore the camera pose
//     (position + rotation + ALTITUDE) and the pre-sweep pause flags verbatim,
//     so a continuing sim resumes exactly as before.
//   * Chunk-MVP latency: chunk dispatch uses the frame N-1 MVP. With a frozen
//     camera held for settleFrames >= 2 the N-1 MVP equals the current MVP, so
//     it converges. Default settle 30 is comfortably >= 2.
//
// Single-frame free-capture mode (MC2_VISUAL_CAPTURE_FRAME WITHOUT a bookmark
// file) is EYEBALL MODE: in a panning/animated mission it is inherently
// nondeterministic (the sim is live at that frame). We KEEP it for quick visual
// spot-checks but it is NOT byte-stable. Only the bookmark sweep is the
// deterministic gate path. (We deliberately do not pause-freeze free mode: it
// has no fixed camera pose to hold, so freezing would not make it stable.)
// When a bookmark file IS configured the free-capture path is fully suppressed
// (before/during/after the sweep) so a bookmark run emits ONLY deterministic
// bookmark PNGs and zero frame*.png -- see active() / onPostRenderFrame().
#include "gos_visual_capture.h"

#include "gos_smoke.h"


#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

#include "render_backend_select.h"
#include "build_identity.h"  // Stable build identity declarations
#include "diagnostic_trace.h"

// Game Camera global `eye` + hard-set API (setPosition/setRotation). Use the
// explicit mclib path: the GameOS-local "utils/camera.h" is a different GL
// helper struct, and a bare "camera.h" would be ambiguous on the include path.
#include "../../mclib/camera.h"

// VK-FIELD-VFX-8: fixture-armed fire-event capture frame (defined mclib/tgl.cpp;
// armed by the MC2_FX_FORCE_SPAWN fixture in code/warrior.cpp).
extern uint32_t g_mc2FxFireCaptureFrame;

// Mission sim-freeze hooks for deterministic capture. We do NOT include the
// game's missiongui.h here -- it pulls in mclib.h / mover.h / controlgui.h via
// quote-includes that are not on the GameOS translation unit's include path.
// Instead the game side (missiongui.cpp) exposes three thin extern-C shims that
// drive the MissionInterfaceManager pause flags. isPaused() gates scenarioTime
// and all unit/sensor/collision updates (code/mission.cpp:531), so freezing the
// pause makes every animated source constant -> byte-deterministic capture.
extern "C" {
    // Returns packed (bPaused<<1 | bPausedWithoutMenu), or -1 if no mission
    // interface instance exists yet.
    int  mc2VisualCaptureGetPauseState();
    // Freeze (1) / unfreeze (0) the sim, menu-less, no sound, no HUD overlay.
    void mc2VisualCaptureSetPaused(int paused);
    // Restore the exact pre-sweep pause flag pair captured above.
    void mc2VisualCaptureRestorePauseState(int packed);
    // MC2_CAPTURE_FOCUS: nth live player-team mech position (code/sim_identity.cpp).
    int  mc2CaptureFocusMech(int n, float* xyz);
    int  mc2CaptureFocusMechEx(int n, int sel, float* xyz, int* id, int* team, char* name, int* total, int* contact);
    void mc2CaptureFocusSetShowMovers(int on);
}

// SIM-IDENTITY-HASH-1: per-capture world-state hash. Dependency-free prototype
// header, same layering reason as the pause shims above -- the collector in
// code/sim_identity.cpp is what includes objmgr.h / mover.h / appear.h.
#include "../../code/sim_identity.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options.h"
#include "mc2fmt.h"

// Global-scope game-quit flag (code/mechcmd2.cpp etc.); the mission main loop polls it
// and calls gos_TerminateApplication(). Declared here so MC2_VISUAL_CAPTURE_EXIT_AFTER
// can request a clean shutdown after the bookmark sweep.
extern bool quitGame;

namespace {

// ---------------------------------------------------------------------------
// Minimal deterministic PNG writer (8-bit RGB, no external deps).
// ---------------------------------------------------------------------------

uint32_t crc32_buf(const unsigned char* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void put_u32_be(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>((v >> 24)));
    out.push_back(static_cast<unsigned char>((v >> 16)));
    out.push_back(static_cast<unsigned char>((v >> 8)));
    out.push_back(static_cast<unsigned char>((v)));
}

void write_chunk(std::vector<unsigned char>& out, const char tag[4],
                 const unsigned char* data, size_t len) {
    put_u32_be(out, static_cast<uint32_t>(len));
    size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (len) out.insert(out.end(), data, data + len);
    uint32_t crc = crc32_buf(out.data() + start, 4 + len, 0);
    put_u32_be(out, crc);
}

// DEFLATE "stored" (uncompressed) wrapping for a raw byte stream.
void deflate_stored(const std::vector<unsigned char>& raw,
                    std::vector<unsigned char>& z) {
    // zlib header: CM=8, CINFO=7, no dict, default level bits -> 0x78 0x01.
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    const size_t MAXBLK = 65535;
    while (off < raw.size() || raw.empty()) {
        size_t blk = std::min(MAXBLK, raw.size() - off);
        bool last = (off + blk) >= raw.size();
        z.push_back(last ? 1 : 0);              // BFINAL, BTYPE=00 (stored)
        z.push_back(static_cast<unsigned char>((blk & 0xFF)));
        z.push_back(static_cast<unsigned char>(((blk >> 8) & 0xFF)));
        z.push_back(static_cast<unsigned char>(((~blk) & 0xFF)));
        z.push_back(static_cast<unsigned char>((((~blk) >> 8) & 0xFF)));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + blk);
        off += blk;
        if (last) break;
    }
    // Adler-32 of the raw stream.
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_u32_be(z, (b << 16) | a);
}

// Write an 8-bit RGB PNG. `rgb` is row-major, top-to-bottom, w*h*3 bytes.
bool write_png_rgb(const char* path, int w, int h,
                   const std::vector<unsigned char>& rgb) {
    // Build the filtered scanline stream (filter byte 0 = None per row).
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const unsigned char* row = rgb.data() + static_cast<size_t>(y) * w * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3);
    }
    std::vector<unsigned char> idat;
    deflate_stored(raw, idat);

    std::vector<unsigned char> out;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    unsigned char ihdr[13];
    ihdr[0] = static_cast<unsigned char>((w >> 24)); ihdr[1] = static_cast<unsigned char>((w >> 16));
    ihdr[2] = static_cast<unsigned char>((w >> 8));  ihdr[3] = static_cast<unsigned char>((w));
    ihdr[4] = static_cast<unsigned char>((h >> 24)); ihdr[5] = static_cast<unsigned char>((h >> 16));
    ihdr[6] = static_cast<unsigned char>((h >> 8));  ihdr[7] = static_cast<unsigned char>((h));
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 2;    // color type RGB
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter
    ihdr[12] = 0;   // interlace
    write_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    write_chunk(out, "IDAT", idat.data(), idat.size());
    write_chunk(out, "IEND", nullptr, 0);

    FILE* f = fopen(path, "wb");
    if (!f) {
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] ERROR fopen {}\n", mc2::fmt::safe(path));
        return false;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Capture tuple sidecar JSON.
// ---------------------------------------------------------------------------
void json_escape(std::string_view s, std::string& out) {
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') { out += "\\n"; }
        else out.push_back(c);
    }
}

// Sorted list of MC2_* env var NAMES currently set (the gate-env set).
void collect_gate_env(std::vector<std::string>& names) {
#if defined(_WIN32)
    // _environ is declared by <stdlib.h>; do not re-declare (a namespace-scoped
    // redeclaration would acquire internal linkage and fail to link on MSVC).
    char** e = _environ;
#else
    char** e = environ;
#endif
    for (; e && *e; ++e) {
        const char* eq = strchr(*e, '=');
        if (!eq) continue;
        size_t nlen = static_cast<size_t>((eq - *e));
        if (nlen >= 4 && strncmp(*e, "MC2_", 4) == 0)
            names.emplace_back(*e, nlen);
    }
    std::sort(names.begin(), names.end());
}

// VK-FIRST-BOOKMARK-POSE-1: the pose the just-rendered frame actually used,
// stashed each settle frame BEFORE the per-settle re-apply and emitted into the
// sidecar (see onPostRenderFrame). File-scope so write_sidecar can read it.
glm::vec3 g_renderedPos{0, 0, 0};
float g_renderedProj = 0.0f;
float g_renderedRot  = 0.0f;
float g_renderedAlt  = 0.0f;
// MC2_CAPTURE_FOCUS=mech[:N]: capture/dev knob. Read once; unset => off (one bool test per pose).
// The sim is frozen during the sweep so the resolved mover position is stable.
struct FocusState {
    bool parsed = false;
    bool requested = false;
    int  index = 0;
    float zoom = 200.0f;
    bool found = false;
    float pos[3] = {0, 0, 0};
    bool logged = false;
    int  walk = -1;          // -1 = single mech:N; 0 = player team, 1 = other teams, 2 = any (mech:home|enemy|all walk: bookmark k frames mech k)
    int  id = -1, team = -1, total = 0, contact = 0;
    bool fog = false;        // "mech:all,fog": walk WITHOUT the ShowMovers override, so fogged enemies are labelled and not drawn
    char name[64] = {0};
} g_focus;


struct VkCaptureReq { bool pending = false; std::string png; int w = 0, h = 0; unsigned long long frame = 0; unsigned heldFrames = 0; };
// A requested bookmark capture is delivered by a later present readback. The sweep holds the camera on that bookmark until it is delivered (or this
// many frames passed with no readback), so whichever readback serves the request shows the bookmark's pose, never the next bookmark's.
constexpr unsigned kMaxCaptureHold = 600;
VkCaptureReq g_vkReq;
const char* g_readbackSource = "vk_present";   // VK-VISUAL-CAPTURE-1: sidecar "readback_source"

void write_sidecar(const std::string& jsonPath, std::string_view mission,
                   std::string_view label, unsigned int frame,
                   int w, int h, long triggerFrame, bool deterministic) {
    FILE* f = fopen(jsonPath.c_str(), "wb");
    if (!f) {
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] ERROR fopen {}\n", jsonPath.c_str());
        return;
    }
    std::vector<std::string> gateEnv;
    collect_gate_env(gateEnv);

    std::string esc;
    std::string buf;
    buf += "{\n";
    buf += "  \"build\": {";
    buf += " \"sha\": \""; esc.clear(); json_escape(MC2_BUILD_GIT_SHA, esc); buf += esc; buf += "\",";
    buf += " \"dirty\": "; buf += (MC2_BUILD_GIT_DIRTY ? "true" : "false"); buf += ",";
    buf += " \"branch\": \""; esc.clear(); json_escape(MC2_BUILD_GIT_BRANCH, esc); buf += esc; buf += "\" },\n";
    buf += "  \"mission\": \""; esc.clear(); json_escape(mission, esc); buf += esc; buf += "\",\n";
    buf += "  \"label\": \""; esc.clear(); json_escape(label, esc); buf += esc; buf += "\",\n";
    char num[64];
    mc2::fmt::to_buf(num, "  \"frame\": {:d},\n", mc2::fmt::as_u32(frame)); buf += num;
    mc2::fmt::to_buf(num, "  \"trigger_frame\": {:d},\n", mc2::fmt::as_i32(triggerFrame)); buf += num;
    // CAPTURE-DETERMINISM-1: `frame` above is the RENDER frame counter
    // (g_mc2FrameCounter, gameosmain.cpp) -- the counter the sweep triggers on.
    // The SIM advances on a separate counter (g_fixedSimFrame, advanced only on
    // frames where the fixed step actually ran). The two are NOT locked: frames
    // rendered while the sim is paused/loading do not advance the sim. Two runs
    // can therefore reach render frame N having executed a DIFFERENT number of
    // sim steps, i.e. genuinely different sim states, while every other field
    // here matches. Record it so that divergence is visible in the artifact
    // instead of being invisible behind a matching `frame` + `deterministic`.
    mc2::fmt::to_buf(num, "  \"sim_frame\": {:d},\n", mc2::fmt::as_i32(static_cast<long>((SmokeMode::fixedClockSeconds() * 30.0 + 0.5)))); buf += num;
    buf += "  \"deterministic\": "; buf += (deterministic ? "true" : "false"); buf += ",\n";
    mc2::fmt::to_buf(num, "  \"seed\": {:d},\n", mc2::fmt::as_u32(static_cast<unsigned>(SmokeMode::state().seed))); buf += num;
    buf += "  \"preset\": \""; esc.clear();
    json_escape(SmokeMode::state().profile, esc); buf += esc; buf += "\",\n";
    mc2::fmt::to_buf(num, "  \"width\": {:d},\n", mc2::fmt::as_i32(w)); buf += num;
    mc2::fmt::to_buf(num, "  \"height\": {:d},\n", mc2::fmt::as_i32(h)); buf += num;
    buf += "  \"readback_source\": \""; buf += g_readbackSource; buf += "\",\n";
    // The backend that DREW this frame, not the one requested: a harness has to tell a GL frame from a Vulkan frame
    // (a Vulkan context that fails to start now exits the process).
    buf += "  \"backend\": \""; buf += "vulkan"; buf += "\",\n";
    // VK-FIRST-BOOKMARK-POSE-1: record the ACTUAL camera pose the JUST-RENDERED
    // frame used (stashed at the rendered_pose_matches point, BEFORE the per-settle
    // re-apply -- so it reflects what update()+render produced, not the value we
    // then re-assert). This is the load-bearing signal the sidecar previously
    // lacked (it only carried the bookmark LABEL, not where the camera really was):
    // it machine-verifies bookmark #1 captured at its named pose rather than the
    // scripted intro pose.
    mc2::fmt::to_buf(num, "  \"cap_pos\": [{:.3f}, {:.3f}, {:.3f}],\n", g_renderedPos.x, g_renderedPos.y, g_renderedPos.z); buf += num;
    mc2::fmt::to_buf(num, "  \"cap_proj\": {:.3f},\n", g_renderedProj); buf += num;
    mc2::fmt::to_buf(num, "  \"cap_rot\": {:.3f},\n", g_renderedRot); buf += num;
    mc2::fmt::to_buf(num, "  \"cap_alt\": {:.3f},\n", g_renderedAlt); buf += num;
    if (g_focus.requested) {
        char fnum[256];
        // Focus target + resolved position (found=false: no live player mech, pose left as bookmarked).
        mc2::fmt::to_buf(fnum, "  \"focus\": {{\"target\": \"mech:{:d}\", \"found\": {}, \"pos\": [{:.3f}, {:.3f}, {:.3f}], \"zoom\": {:.1f}}},\n", mc2::fmt::as_i32(g_focus.index), mc2::fmt::safe(g_focus.found ? "true" : "false"), g_focus.pos[0], g_focus.pos[1], g_focus.pos[2], g_focus.zoom); buf += fnum;
        if (g_focus.walk >= 0) {
            mc2::fmt::to_buf(fnum, "  \"focus_walk\": {{\"mode\": \"{}\", \"index\": {:d}, \"total\": {:d}, \"id\": {:d}, \"team\": {:d}, \"contact\": \"{}\", \"name\": \"{}\"}},\n", mc2::fmt::safe(g_focus.walk == 0 ? "home" : g_focus.walk == 1 ? "enemy" : "all"), mc2::fmt::as_i32(g_focus.index), mc2::fmt::as_i32(g_focus.total), mc2::fmt::as_i32(g_focus.id), mc2::fmt::as_i32(g_focus.team), mc2::fmt::safe(g_focus.contact == 0 ? "home" : g_focus.contact == 1 ? "visual" : g_focus.contact == 2 ? "fogged" : "override"), mc2::fmt::safe(g_focus.name)); buf += fnum;
        }
    }
    // The render options the engine ran with (options.cfg plus MC2_OPTIONS_OVERRIDE): two captures are only comparable when these match.
    buf += "  \"render_options\": "; buf += mc2::options::toJson(mc2::options::current()); buf += ",\n";
    buf += "  \"gate_env\": [";
    for (size_t i = 0; i < gateEnv.size(); ++i) {
        if (i) buf += ", ";
        buf += "\""; esc.clear(); json_escape(gateEnv[i], esc); buf += esc; buf += "\"";
    }
    buf += "]";

    // SIM-IDENTITY-HASH-1: the world-state hash for THIS capture.
    //
    // This is the sim half of the identity the parity harness grades on. The
    // pixel sha answers "do these two frames look the same"; this answers "were
    // these two frames even drawn from the same world". Two arms whose worlds
    // differ are not comparable, and scripts/sim_identity.py turns that into an
    // explicit UNGRADED verdict rather than a pass.
    //
    // The sweep has already called mc2VisualCaptureSetPaused(1) by the time a
    // bookmark is written, so the sim is FROZEN here -- the hash describes a
    // stationary world, not one being sampled mid-step.
    //
    // Heap, not stack: a few hundred movers at ~200 bytes each would be a
    // quarter-megabyte stack frame. A zero return means "emit nothing" -- an
    // absent block reads as "not recorded", a truncated one would read as valid.
    {
        std::vector<char> sib(512 * 1024);
        const double clockSec = SmokeMode::fixedClockSeconds();
        const int wrote = mc2SimIdentityWriteJson(
            sib.data(), static_cast<int>(sib.size()), clockSec,
            static_cast<long>((clockSec * 30.0 + 0.5)));
        if (wrote > 0) {
            buf += ",\n";
            buf += sib.data();
        }
    }
    buf += "\n}\n";
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Shared capture op: PNG + sidecar with deterministic <dir>/<base>.{png,json}.
// ---------------------------------------------------------------------------
void capture_to(std::string_view dir, std::string_view base,
                std::string_view mission, std::string_view label,
                unsigned int frame, int w, int h,
                long triggerFrame, bool deterministic) {
    std::string png = std::string(dir).append("/").append(base).append(".png");
    std::string js  = std::string(dir).append("/").append(base).append(".json");
    {
        // VK-VISUAL-CAPTURE-1: defer the PNG to this frame's VK present
        // readback (vkCaptureDeliver). Sidecar now: it snapshots per-frame
        // globals (rendered pose) that the next bookmark would overwrite.
        g_readbackSource = "vk_present";
        write_sidecar(js, mission, label, frame, w, h, triggerFrame, deterministic);
        g_vkReq.pending = true;
        g_vkReq.heldFrames = 0;
        g_vkReq.frame = frame;   // the sidecar frame
        g_vkReq.png = png;
        g_vkReq.w = w; g_vkReq.h = h;
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] VK readback requested {} ({:d}x{:d})\n", png.c_str(), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
        return;
    }
    // There is no GL readback arm any more: a run on the GL opt-out writes no capture.
    mc2::fmt::print_err("[VISUAL_CAPTURE v1] refused {}: the capture reads the Vulkan present image and the Vulkan backend is not drawing\n", png.c_str());
}

// ---------------------------------------------------------------------------
// Bookmark file (tiny hand-rolled JSON reader; schema owner doc section 2.1).
// ---------------------------------------------------------------------------
struct Bookmark {
    std::string name;
    float pos[3] = {0, 0, 0};
    float rot = 0.0f;       // cameraRotation (deg)
    float proj = 30.0f;     // projectionAngle (deg)
    float alt = 1200.0f;    // camera altitude
    bool hasPos = false;
};

// Read a JSON number array "[a, b, c]" starting at *p (after the key); returns
// count parsed, advances *pp past the closing bracket.
int read_num_array(const char* p, const char** pp, float* out, int maxN) {
    while (*p && *p != '[') ++p;
    if (*p != '[') { *pp = p; return 0; }
    ++p;
    int n = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ']') break;
        char* end = nullptr;
        float v = static_cast<float>(strtod(p, &end));
        if (end == p) break;
        if (n < maxN) out[n] = v;
        ++n;
        p = end;
    }
    if (*p == ']') ++p;
    *pp = p;
    return n;
}

bool read_scalar(const char* obj, const char* key, float* out) {
    std::string pat = std::string("\"") + key + "\"";
    const char* k = strstr(obj, pat.c_str());
    if (!k) return false;
    k += pat.size();
    while (*k && *k != ':') ++k;
    if (*k != ':') return false;
    ++k;
    char* end = nullptr;
    float v = static_cast<float>(strtod(k, &end));
    if (end == k) return false;
    *out = v;
    return true;
}

bool load_bookmarks(const char* path, std::string& missionOut,
                    std::vector<Bookmark>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string txt(static_cast<size_t>(len), '\0');
    size_t rd = fread(&txt[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    txt.resize(rd);

    // mission
    const char* m = strstr(txt.c_str(), "\"mission\"");
    if (m) {
        m = strchr(m, ':');
        if (m) { m = strchr(m, '"'); if (m) { ++m; const char* e = strchr(m, '"');
                 if (e) missionOut.assign(m, e); } }
    }

    // Iterate bookmark objects: find each "name" within the bookmarks array.
    const char* arr = strstr(txt.c_str(), "\"bookmarks\"");
    if (!arr) return false;
    const char* p = arr;
    while ((p = strstr(p, "\"name\"")) != nullptr) {
        Bookmark b;
        const char* k = strchr(p, ':');
        if (k) { k = strchr(k, '"'); if (k) { ++k; const char* e = strchr(k, '"');
                 if (e) b.name.assign(k, e); } }
        // Bookmark objects are flat (scalars and arrays). Include keys before
        // "name": sorted JSON puts "alt" first, otherwise zoom silently falls
        // back to 1200 or reads the following bookmark's altitude.
        const char* begin = p;
        while (begin > txt.c_str() && *begin != '{') --begin;
        const char* end = strchr(p, '}');
        std::string obj(begin, end ? static_cast<size_t>(end - begin + 1) : strlen(begin));
        const char* pos = strstr(obj.c_str(), "\"pos\"");
        if (pos) {
            const char* after = nullptr;
            float v[3] = {0, 0, 0};
            if (read_num_array(pos, &after, v, 3) >= 2) {
                b.pos[0] = v[0]; b.pos[1] = v[1]; b.pos[2] = v[2];
                b.hasPos = true;
            }
        }
        read_scalar(obj.c_str(), "rot", &b.rot);
        read_scalar(obj.c_str(), "proj", &b.proj);
        read_scalar(obj.c_str(), "alt", &b.alt);
        out.push_back(b);
        p += 6;
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Capture-primitive state (MC2_VISUAL_CAPTURE_FRAME / _DIR).
// ---------------------------------------------------------------------------
struct CaptureCfg {
    bool parsed = false;
    long frame = -1;
    const char* dir = nullptr;
    bool done = false;
};
CaptureCfg g_cap;

// ---------------------------------------------------------------------------
// Bookmark-replay state machine. Driven once per frame.
// ---------------------------------------------------------------------------
struct ReplayState {
    bool parsed = false;
    const char* path = nullptr;     // null => feature off
    bool loaded = false;
    bool finished = false;
    int settleFrames = 30;
    std::string mission;
    std::string dir;                // capture dir (reuses MC2_VISUAL_CAPTURE_DIR)
    std::vector<Bookmark> marks;
    size_t idx = 0;
    int settleCount = 0;
    int extendCount = 0;        // pose-verify bounded extra-settle budget used
    bool teleported = false;
    long triggerFrame = 120;        // sweep fires at this fixed frame
    bool triggered = false;         // sweep has begun (sim frozen)
    // Saved camera pose for restore after the sweep.
    bool saved = false;
    glm::vec3 savePos;
    glm::vec3 saveRot;
    float saveAlt = 1200.0f;        // cameraAltitude (getRotation drops z=0)
    // Saved pre-sweep mission pause flags (restored verbatim).
    bool pauseStateSaved = false;
    int savePauseState = 0;
};
ReplayState g_replay;

// MC2_VISUAL_CAPTURE_FRAME as the old reader saw it: unset -> -1, set -> atol() (empty -> 0).
static long captureFrameEnv() {
    if (!mc2::gate::isSet<"MC2_VISUAL_CAPTURE_FRAME">()) return -1L;
    return mc2::gate::isNonEmpty<"MC2_VISUAL_CAPTURE_FRAME">() ? static_cast<long>(mc2::gate::get<"MC2_VISUAL_CAPTURE_FRAME">()) : 0L;
}

void parse_capture_cfg() {
    if (g_cap.parsed) return;
    g_cap.parsed = true;
    // Unset -> -1 (no capture); set-but-empty -> 0, exactly the atol() of the old reader.
    g_cap.frame = captureFrameEnv();
    static const std::string s_capDir = mc2::gate::str<"MC2_VISUAL_CAPTURE_DIR">();   // "" when unset or empty
    g_cap.dir = mc2::gate::isSet<"MC2_VISUAL_CAPTURE_DIR">() ? s_capDir.c_str() : nullptr;
    static bool initLineDone = false;
    if (!initLineDone && g_cap.frame >= 0 && g_cap.dir) {
        initLineDone = true;
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] init frame={:d} dir={}\n", mc2::fmt::as_i32(g_cap.frame), mc2::fmt::safe(g_cap.dir));
    }
}

void parse_replay_cfg() {
    if (g_replay.parsed) return;
    g_replay.parsed = true;
    static const std::string s_replayPath = mc2::gate::str<"MC2_VISUAL_BOOKMARK_CAPTURE">();   // "" when unset or empty
    g_replay.path = mc2::gate::isSet<"MC2_VISUAL_BOOKMARK_CAPTURE">() ? s_replayPath.c_str() : nullptr;
    if (!g_replay.path) return;
    { long s = mc2::gate::get<"MC2_VISUAL_SETTLE">(); if (s > 0) g_replay.settleFrames = static_cast<int>(s); }
    g_replay.dir = mc2::gate::isSet<"MC2_VISUAL_CAPTURE_DIR">() ? mc2::gate::str<"MC2_VISUAL_CAPTURE_DIR">() : std::string(".");
    // Reuse MC2_VISUAL_CAPTURE_FRAME as the deterministic sweep-trigger frame
    // (default 120). Both runs trigger the freeze+sweep at the same frame so
    // scenarioTime is frozen at an identical value across runs.
    if (mc2::gate::isSet<"MC2_VISUAL_CAPTURE_FRAME">()) { long t = captureFrameEnv(); if (t >= 0) g_replay.triggerFrame = t; }
    mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] bookmark-replay init path={} settle={:d} triggerFrame={:d} dir={}\n", mc2::fmt::safe(g_replay.path), mc2::fmt::as_i32(g_replay.settleFrames), mc2::fmt::as_i32(g_replay.triggerFrame), g_replay.dir.c_str());
}

void parse_focus() {
    if (g_focus.parsed) return;
    g_focus.parsed = true;
    if (!mc2::gate::isSet<"MC2_CAPTURE_FOCUS">()) return;
    const std::string spec = mc2::gate::str<"MC2_CAPTURE_FOCUS">();
    if (spec.compare(0, 4, "mech") != 0) return;
    if (spec.size() > 4) {
        if (spec[4] != ':') return;
        std::string argStr = spec.substr(5);
        if (argStr.size() > 4 && argStr.compare(argStr.size() - 4, 4, ",fog") == 0) { g_focus.fog = true; argStr.resize(argStr.size() - 4); }
        const char* arg = argStr.c_str();
        if (!std::strcmp(arg, "home")) g_focus.walk = 0;
        else if (!std::strcmp(arg, "enemy")) g_focus.walk = 1;
        else if (!std::strcmp(arg, "all")) g_focus.walk = 2;
        else {
            g_focus.index = std::atoi(arg);
            if (g_focus.index < 0) g_focus.index = 0;
        }
    }
    g_focus.zoom = static_cast<float>(mc2::gate::get<"MC2_CAPTURE_FOCUS_ZOOM">());
    g_focus.requested = true;
    // The walk frames enemies, which the stock fog-of-war render gate hides unless ShowMovers is set
    // (MC2_FORCE_ALL_VISIBLE does not reach that gate). Capture tool only; ",fog" opts out.
    if (g_focus.walk >= 0) mc2CaptureFocusSetShowMovers(g_focus.fog ? 0 : 1);
}

void apply_bookmark(const Bookmark& b0) {
    if (!eye) return;
    parse_focus();
    Bookmark b = b0;
    if (g_focus.requested) {
        char fnum[256];
        if (g_focus.walk >= 0) {
            g_focus.index = static_cast<int>(g_replay.idx);
            g_focus.name[0] = 0; g_focus.id = -1; g_focus.team = -1;
            g_focus.found = mc2CaptureFocusMechEx(g_focus.index, g_focus.walk, g_focus.pos, &g_focus.id, &g_focus.team, g_focus.name, &g_focus.total, &g_focus.contact) != 0;
        } else {
            g_focus.found = mc2CaptureFocusMech(g_focus.index, g_focus.pos) != 0;
        }
        if (g_focus.found) {
            b.pos[0] = g_focus.pos[0]; b.pos[1] = g_focus.pos[1]; b.pos[2] = g_focus.pos[2];
            b.hasPos = true;
            b.alt = g_focus.zoom;
        }
        if (!g_focus.logged && mc2_diag::tagRequested("CAPTURE_FOCUS")) {
            g_focus.logged = true;
            mc2::fmt::print_err("[CAPTURE_FOCUS v1] mech:{:d} found={:d} pos={:.1f},{:.1f},{:.1f} zoom={:.1f}\n", mc2::fmt::as_i32(g_focus.index), mc2::fmt::as_i32(g_focus.found ? 1 : 0), g_focus.pos[0], g_focus.pos[1], g_focus.pos[2], g_focus.zoom);
        }
    }
    if (b.hasPos) {
        glm::vec3 p;
        p.x = b.pos[0]; p.y = b.pos[1]; p.z = b.pos[2];
        eye->setPosition(p, false);   // hard set, no swoop
    }
    glm::vec3 rot;
    rot.x = b.proj;   // projectionAngle
    rot.y = b.rot;    // cameraRotation
    rot.z = b.alt;    // altitude (setRotation writes cameraAltitude = rot.z)
    eye->setRotation(rot);
    // VK-FIRST-BOOKMARK-POSE-1: the hard setPosition/setRotation above does NOT
    // clear the intro-swoop goal or the object-tracking target, so the NEXT
    // eye->update() (which runs every frame during the frozen sweep) would drag
    // the camera back toward the scripted intro pose -- deterministically
    // stomping the FIRST bookmark while the intro goal is still draining. Pin
    // every scripted override to the just-applied pose so update() is a no-op.
    eye->freezeScriptedCameraForCapture();
}

// VK-FIRST-BOOKMARK-POSE-1: does the camera's CURRENTLY-RENDERED pose match the
// bookmark within epsilon? Called at post-render BEFORE the per-settle re-apply,
// so `eye` still reflects the pose this frame's update()+render actually used.
// getPosition() is the ground-focus point (x,y load-bearing; z is terrain-driven
// so it is not compared). Rotation compares projectionAngle/cameraRotation and
// altitude. Generous epsilons: this is a "did a scripted writer yank us away"
// guard, not a bit-exact check.
bool rendered_pose_matches(const Bookmark& b) {
    if (!eye) return true;
    if (b.hasPos) {
        glm::vec3 p = eye->getPosition();
        if (fabsf(p.x - b.pos[0]) > 2.0f) return false;
        if (fabsf(p.y - b.pos[1]) > 2.0f) return false;
    }
    glm::vec3 rot = eye->getRotation();   // x=proj, y=cameraRotation, z=0
    if (fabsf(rot.x - b.proj) > 0.5f) return false;
    // cameraRotation wraps at 360; compare on the circle.
    float dr = fabsf(rot.y - b.rot);
    if (dr > 180.0f) dr = 360.0f - dr;
    if (dr > 0.5f) return false;
    // NOTE: altitude is deliberately NOT hard-checked. The engine legitimately
    // clamps cameraAltitude to a per-projection-angle max (gamecam.cpp), so a
    // bookmark requesting an unreachable altitude captures at the clamped value
    // -- that is correct, not a scripted-pose stomp. Position (x,y) + projection
    // angle + cameraRotation uniquely identify the "yanked to the wrong pose"
    // failure this guard exists to catch (the SW-corner intro pose differs in
    // all of x, y, and proj). cap_alt is still recorded in the sidecar to eyeball.
    return true;
}

// Restore the full pre-sweep camera pose. Camera::getRotation() leaves z=0
// (it never copies cameraAltitude), so setRotation(saveRot) alone would clamp
// altitude to AltitudeMinimum. We carry the saved cameraAltitude in rot.z so
// setRotation restores it exactly. cameraAltitude is a public member of Camera.
void restore_camera_pose() {
    if (!eye || !g_replay.saved) return;
    eye->setPosition(g_replay.savePos, false);
    glm::vec3 rot = g_replay.saveRot;   // x=projectionAngle, y=cameraRotation
    rot.z = g_replay.saveAlt;                 // restore altitude (was 0 in saveRot)
    eye->setRotation(rot);
}

}  // namespace

namespace gos { namespace visual_capture {

bool active() {
    // Parse env once (cached). Unset path is a single int + 2 pointer checks,
    // no allocation.
    parse_capture_cfg();
    parse_replay_cfg();
    // The free-capture path is fully suppressed whenever a bookmark file is
    // configured (g_replay.path != nullptr) -- not just while the sweep runs.
    // A bookmark run must emit ONLY the deterministic bookmark PNGs (zero
    // frame*.png), even after the sweep finishes.
    const bool bookmarkConfigured = (g_replay.path != nullptr);
    const bool capActive = (!bookmarkConfigured && g_cap.frame >= 0 &&
                            g_cap.dir && !g_cap.done);
    const bool replayActive = (g_replay.path != nullptr && !g_replay.finished);
    return capActive || replayActive;
}

}}  // namespace gos::visual_capture (reopened below)

// VK-MECH-CAPTURE-DETERMINISM-1: capture-mode-only gait-freeze predicate.
// The bookmark sweep freezes the SIM (bPaused via mc2VisualCaptureSetPaused) so
// scenarioTime + all unit updates stop -> byte-deterministic capture. But the
// RENDER path still calls Mech3DAppearance::update(animate=true) once per render
// frame (BattleMech::render, mech.cpp), which advances the stock gait by
// mechFrameRate * frameLength (a WALL-CLOCK global, NOT scenarioTime). So a mech's
// animation pose kept drifting run-to-run during the frozen sweep -> the center
// "mech island" jitter that forced OBJ_BOX masking in the near-oracle.
//
// This shim returns 1 iff a capture is ACTIVE *and* the sim is currently frozen
// for the sweep (bPaused bit of the packed pause state). mech3d.cpp gates the
// gait advance on !this, so:
//   * capture unset            -> shim is 0 -> ZERO behavior change (byte-safe);
//   * capture active, pre-sweep-> bPaused=0 -> gait runs, mechs reach their pose;
//   * capture active, in sweep -> bPaused=1 -> gait frozen -> deterministic.
// Purely capture-mode-only: live game (no capture env) never sees this.
extern "C" int mc2CaptureGaitFreezeActive() {
    if (!gos::visual_capture::active()) return 0;
    extern int mc2VisualCaptureGetPauseState();   // missiongui.cpp shim
    const int packed = mc2VisualCaptureGetPauseState();
    if (packed < 0) return 0;                      // no mission interface yet
    return (packed & 0x2) ? 1 : 0;                 // bPaused bit
}

namespace gos { namespace visual_capture {

void onPostRenderFrame(unsigned int frame, int sceneW, int sceneH) {
    // FIX #1: when a bookmark file is configured the free-capture path is
    // ALWAYS inert -- before, during, AND after the sweep. The deterministic
    // bookmark sweep owns the camera + sim freeze and reuses
    // MC2_VISUAL_CAPTURE_FRAME as its trigger frame; firing the live-camera
    // free capture (e.g. once g_replay.finished flips true) would write a
    // spurious nondeterministic frame*.png. A bookmark run therefore produces
    // ONLY the deterministic bookmark PNGs.
    const bool bookmarkConfigured = (g_replay.path != nullptr);
    const bool capActive = (!bookmarkConfigured && g_cap.frame >= 0 &&
                            g_cap.dir && !g_cap.done);
    const bool replayActive = (g_replay.path != nullptr && !g_replay.finished);
    if (!capActive && !replayActive) return;       // zero-cost unset path

    // Only ride the smoke harness (no new driver). Outside smoke mode, do
    // nothing (mission name / seed determinism is only defined there).
    if (!SmokeMode::state().enabled) return;
    if (sceneW <= 0 || sceneH <= 0) return;

    const std::string mission = SmokeMode::state().mission.empty()
        ? std::string("unknown") : SmokeMode::state().mission;

    // ---- (1) single-frame free-capture primitive (EYEBALL MODE) -----------
    // capActive is already false whenever a bookmark file is configured (see
    // the guard above), so this path runs ONLY when no bookmark sweep exists.
    // It is the nondeterministic eyeball spot-check mode.
    // VK-FIELD-VFX-8: MC2_VISUAL_CAPTURE_ON_FIRE — GL twin of the present-PPM
    // fire-event pin. When set (with MC2_FX_FORCE_SPAWN), the trigger frame is
    // the fixture-armed g_mc2FxFireCaptureFrame (~0.5s after the first volley)
    // instead of the static MC2_VISUAL_CAPTURE_FRAME, so GL-vs-VK weapon-class
    // pairs capture the SAME fire-relative instant on both backends (a static
    // frame pin is volley roulette). Unarmed (0) = wait.
    long trigger = g_cap.frame;
    {
        static const bool s_onFire = mc2::gate::get<"MC2_VISUAL_CAPTURE_ON_FIRE">();
        if (s_onFire) {
            trigger = (::g_mc2FxFireCaptureFrame == 0)
                ? 0x7fffffffL : (long)::g_mc2FxFireCaptureFrame;
        }
    }
    if (capActive && static_cast<long>(frame) >= trigger) {
        char base[256];
        mc2::fmt::to_buf(base, "{}_frame{:d}", mission.c_str(), mc2::fmt::as_u32(frame));
        capture_to(g_cap.dir, base, mission, "frame", frame, sceneW, sceneH,
                   g_cap.frame, /*deterministic=*/false);
        g_cap.done = true;
    }

    // ---- (2) bookmark replay sweep (DETERMINISTIC, sim-frozen) -------------
    if (replayActive) {
        if (!g_replay.loaded) {
            g_replay.loaded = true;
            std::string fileMission;
            if (!load_bookmarks(g_replay.path, fileMission, g_replay.marks)) {
                mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] bookmark load FAILED path={}\n", mc2::fmt::safe(g_replay.path));
                g_replay.finished = true;
                return;
            }
            g_replay.mission = fileMission.empty() ? mission : fileMission;
            mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] loaded {:d} bookmarks (mission={})\n", mc2::fmt::as_size(g_replay.marks.size()), g_replay.mission.c_str());
        }

        // Wait for the deterministic trigger frame. Determinism does NOT come
        // from wall-clock alignment (that was the false premise of the old
        // wall-clock sim and is now moot): it comes from S9D's fixed-timestep
        // clock (scenarioTime = frame/30 under MC2_SMOKE_FIXED_TIMESTEP=1) plus
        // S9E's pinned render-shader clocks. At any given trigger frame both
        // runs therefore share the identical scenarioTime; we then freeze it
        // (and all shader clocks) for the whole sweep, so every animated source
        // is held at the same frozen phase across bookmarks AND across runs.
        if (!g_replay.triggered) {
            if (static_cast<long>(frame) < g_replay.triggerFrame) return;
            g_replay.triggered = true;
            // Save + freeze the sim BEFORE the first settle window. From here on
            // scenarioTime and all unit/sensor/collision updates are frozen, so
            // every animated source (water UV, gosFX, light flicker, motion) is
            // constant across the sweep AND identical between two runs.
            int ps = mc2VisualCaptureGetPauseState();
            if (ps >= 0) {
                g_replay.pauseStateSaved = true;
                g_replay.savePauseState = ps;
                mc2VisualCaptureSetPaused(1);
            }
            // Save the full pre-sweep camera pose, including altitude (which
            // getRotation() does NOT report -- it leaves z=0).
            if (eye) {
                g_replay.saved = true;
                g_replay.savePos = eye->getPosition();
                g_replay.saveRot = eye->getRotation();
                g_replay.saveAlt = eye->cameraAltitude;   // public member
                mc2::fmt::print_err("[ADJUDICATION_POSE v1] pos={:.3f},{:.3f},{:.3f} rot(proj={:.3f},rot={:.3f}) alt={:.3f}\n",
                        g_replay.savePos.x, g_replay.savePos.y, g_replay.savePos.z,
                        g_replay.saveRot.x, g_replay.saveRot.y, g_replay.saveAlt);
            }
            mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] sweep triggered frame={:d} (triggerFrame={:d}) sim frozen, pauseState={:d}\n", mc2::fmt::as_u32(frame), mc2::fmt::as_i32(g_replay.triggerFrame), mc2::fmt::as_i32(g_replay.savePauseState));
        }

        if (g_vkReq.pending) {
            if (++g_vkReq.heldFrames < kMaxCaptureHold) return;
            mc2::fmt::print_err("[VISUAL_CAPTURE v1] WARN no present readback for {} after {:d} frames -- capture dropped\n", g_vkReq.png.c_str(), mc2::fmt::as_u32(kMaxCaptureHold));
            g_vkReq.pending = false;
        }
        if (g_replay.idx >= g_replay.marks.size()) {
            // Sweep done: restore camera (pos+rot+altitude) then the exact
            // pre-sweep pause flags, so a continuing sim resumes unchanged.
            restore_camera_pose();
            if (g_replay.pauseStateSaved)
                mc2VisualCaptureRestorePauseState(g_replay.savePauseState);
            g_replay.finished = true;
            mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] sweep complete, camera + pause-state restored\n");
            // VK-NOINTEROP-STRICT-REVIEW: opt-in clean shutdown after the sweep so the
            // VK island destroy lines (forbidden_* counters) + the final [VK_PRESENT]
            // logHealth are emitted to the capture log. Without this the harness kills
            // the still-running game on timeout and those shutdown-only lines never
            // appear -- the exact reason the prior no-interop proof log lacked island
            // counters. Sets the same quitGame flag a window-close would; the mission
            // main loop polls it and calls gos_TerminateApplication() -> ordered VK
            // teardown (gameosmain.cpp mc2_vk_*_island_destroy). Off by default.
            if (mc2::gate::get<"MC2_VISUAL_CAPTURE_EXIT_AFTER">()) {
                ::quitGame = true;
                mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] MC2_VISUAL_CAPTURE_EXIT_AFTER=1 -> requesting clean shutdown (quitGame=true)\n");
            }
            return;
        }
        if (!g_replay.teleported) {
            apply_bookmark(g_replay.marks[g_replay.idx]);
            g_replay.teleported = true;
            g_replay.settleCount = 0;
            g_replay.extendCount = 0;
            return;   // let the scene render at the new pose before capturing
        }
        // VK-FIRST-BOOKMARK-POSE-1: verify the pose THIS just-rendered frame used
        // (eye still reflects update()+render output; we have NOT re-applied yet
        // this frame) matches the bookmark. With freezeScriptedCameraForCapture()
        // this holds from the first settle frame; the check is a bounded guard
        // against any residual/future scripted writer that still yanks the camera.
        const int kMaxPoseExtend = 30;
        // Stash the pose THIS just-rendered frame used (eye pre-reapply) so the
        // sidecar records the true rendered pose, not the value we re-assert below.
        if (eye) {
            g_renderedPos = eye->getPosition();
            glm::vec3 rr = eye->getRotation();
            g_renderedProj = rr.x;
            g_renderedRot  = rr.y;
            g_renderedAlt  = eye->cameraAltitude;
        }
        const bool renderedOk = rendered_pose_matches(g_replay.marks[g_replay.idx]);
        // Re-apply each settle frame (and re-freeze scripted overrides) so any
        // residual swoop/goal logic cannot drift the pose. Sim is frozen, so
        // settle only flushes render-side streaming / chunk-LOD admission /
        // shadow warmup / bloom history.
        apply_bookmark(g_replay.marks[g_replay.idx]);
        if (g_replay.settleCount < g_replay.settleFrames) {
            ++g_replay.settleCount;
            return;
        }
        // Frame-count settle elapsed. Additionally require the rendered pose to
        // match the bookmark; if a scripted writer still drifted it, extend
        // (bounded) so the re-applied+frozen pose gets a fresh render first.
        if (!renderedOk && g_replay.extendCount < kMaxPoseExtend) {
            if (g_replay.extendCount == 0)
                mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] bookmark '{}' rendered pose off-bookmark; extending settle (pose-verify guard)\n", g_replay.marks[g_replay.idx].name.c_str());
            ++g_replay.extendCount;
            return;
        }
        if (!renderedOk)
            mc2::fmt::print_err("[VISUAL_CAPTURE v1.5] WARN bookmark '{}' still off-pose after {:d} extend frames -- capturing anyway\n", g_replay.marks[g_replay.idx].name.c_str(), mc2::fmt::as_i32(kMaxPoseExtend));
        // Settled: capture this bookmark. The capture is byte-stable ONLY when
        // the deterministic clock (S9D sim + S9E shader, MC2_SMOKE_FIXED_TIMESTEP)
        // is engaged; without it the sweep freeze holds scenarioTime constant
        // within one run but two runs reach the trigger frame at different clock
        // values. Stamp the sidecar honestly so a Baseline-A golden is never
        // mislabeled deterministic when it is not.
        const bool deterministic = SmokeMode::fixedTimestepEnabled();
        const Bookmark& bm = g_replay.marks[g_replay.idx];
        // Flatten the bookmark name into a filename token so a name cannot
        // escape the capture dir (path traversal / separators).
        std::string safeName = bm.name;
        for (char& c : safeName) {
            if (c == '/' || c == '\\' || c == ':' || c == '.')
                c = '_';
        }
        char base[256];
        mc2::fmt::to_buf(base, "{}_{}", g_replay.mission.c_str(), safeName.c_str());
        capture_to(g_replay.dir, base, g_replay.mission, bm.name,
                   frame, sceneW, sceneH,
                   g_replay.triggerFrame, deterministic);
        ++g_replay.idx;
        g_replay.teleported = false;
    }
}

// CAMERA-PARK-1 (ported from vk branch 4b69ef78): see header. One-shot camera
// pose pin at frame >= @frame (default 300); sim keeps running.
void cameraParkTick(unsigned int frame)
{
    static const std::string s_parkText = mc2::gate::str<"MC2_CAMERA_PARK">();   // "" when unset or empty
    static const char* s_park = mc2::gate::isSet<"MC2_CAMERA_PARK">() ? s_parkText.c_str() : nullptr;
    if (!s_park || !eye)
        return;
    static bool s_parked = false;
    static long s_parkFrame = 300;
    static float pv[5] = {0, 0, 0, 0, 0};
    static bool s_parsed = false;
    if (!s_parsed) {
        s_parsed = true;
        char buf[160];
        strncpy(buf, s_park, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        if (char* at = strchr(buf, '@')) { s_parkFrame = atol(at + 1); *at = 0; }
        int n = sscanf(buf, "%f,%f,%f,%f,%f", &pv[0], &pv[1], &pv[2], &pv[3], &pv[4]);
        if (n < 5) s_park = nullptr;  // malformed -> off
    }
    if (s_park && !s_parked && static_cast<long>(frame) >= s_parkFrame) {
        s_parked = true;
        glm::vec3 p; p.x = pv[0]; p.y = pv[1]; p.z = 0.0f;
        eye->setPosition(p, false);
        glm::vec3 rot; rot.x = pv[3]; rot.y = pv[2]; rot.z = pv[4];
        eye->setRotation(rot);
        eye->freezeScriptedCameraForCapture();
        mc2::fmt::print_err("[CAMERA_PARK v1] frame={:d} pos={:.0f},{:.0f} rot={:.0f} proj={:.0f} alt={:.0f}\n", mc2::fmt::as_u32(frame), pv[0], pv[1], pv[2], pv[3], pv[4]);
    }
}

}}  // namespace gos::visual_capture

namespace gos { namespace visual_capture {

bool cleanFrame() {
    static const bool s_clean = mc2::gate::get<"MC2_VISUAL_CAPTURE_CLEAN">();
    return s_clean;
}

bool vkCapturePending() { return g_vkReq.pending; }
unsigned long long vkCapturePendingSince() { return g_vkReq.frame; }

void vkCaptureDeliver(unsigned int w, unsigned int h, const unsigned char* rgba) {
    if (!g_vkReq.pending) return;
    g_vkReq.pending = false;
    if (!rgba || w == 0 || h == 0) return;
    if (static_cast<int>(w) != g_vkReq.w || static_cast<int>(h) != g_vkReq.h)
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] WARN VK present {:d}x{:d} != requested {:d}x{:d} (PNG uses present size)\n", mc2::fmt::as_u32(w), mc2::fmt::as_u32(h), mc2::fmt::as_i32(g_vkReq.w), mc2::fmt::as_i32(g_vkReq.h));
    // sharedImg rows are bottom-up (GL orientation); PNG wants top-down.
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (unsigned y = 0; y < h; ++y) {
        const unsigned char* src = rgba + static_cast<size_t>((h - 1 - y)) * w * 4;
        unsigned char* dst = rgb.data() + static_cast<size_t>(y) * w * 3;
        for (unsigned x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    if (write_png_rgb(g_vkReq.png.c_str(), static_cast<int>(w), static_cast<int>(h), rgb)) {
        mc2::fmt::print_err("[VISUAL_CAPTURE v1] wrote {} ({:d}x{:d}) src=vk_present\n", g_vkReq.png.c_str(), mc2::fmt::as_u32(w), mc2::fmt::as_u32(h));
        fflush(stderr);
    }
}

}}  // namespace gos::visual_capture
