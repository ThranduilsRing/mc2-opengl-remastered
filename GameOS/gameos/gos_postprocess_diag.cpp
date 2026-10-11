// gosPostProcess: GL lost-draws query tracker (VK-GL-LOST-DRAWS-1) and the HZB readback probe.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// VK-GL-LOST-DRAWS-1: per-group GL_SAMPLES_PASSED queries over the scene FBO
// phase. Tag stack segments the frame: Push/Pop end the running query and begin
// one for the (new) top tag, so each segment counts only its own draws. Results
// are read 4 frames later (ring) so there is no pipeline stall.
// ---------------------------------------------------------------------------
bool g_gosGlLostDrawsOn = false;
namespace {
struct LostSeg { const char* tag; GLuint q; };
constexpr int kLostRing = 4;
std::vector<LostSeg> s_lostSegs[kLostRing];
std::vector<GLuint> s_lostFreeQ;
std::vector<const char*> s_lostStack;
std::map<std::string, unsigned long long> s_lostTotals;
GLuint s_lostCur = 0;
unsigned long long s_lostFrame = 0;
bool s_lostFrameActive = false;
bool s_lostDestroyed = false;

void lostEndCur()
{
    if (s_lostCur) { glEndQuery(GL_SAMPLES_PASSED); s_lostCur = 0; }
}
void lostBeginCur()
{
    if (s_lostStack.empty() || !s_lostStack.back()) return;   // nullptr tag = no-query (compute-only) span
    GLuint q;
    if (!s_lostFreeQ.empty()) { q = s_lostFreeQ.back(); s_lostFreeQ.pop_back(); }
    else glGenQueries(1, &q);
    glBeginQuery(GL_SAMPLES_PASSED, q);
    s_lostSegs[s_lostFrame % kLostRing].push_back({ s_lostStack.back(), q });
    s_lostCur = q;
}
void lostResolveSlot(int slot)
{
    for (const LostSeg& s : s_lostSegs[slot]) {
        GLuint n = 0;
        glGetQueryObjectuiv(s.q, GL_QUERY_RESULT, &n);
        s_lostTotals[s.tag] += n;
        s_lostFreeQ.push_back(s.q);
    }
    s_lostSegs[slot].clear();
}
void lostEmit(bool final_)
{
    std::string line;
    unsigned groups = 0;
    for (const auto& kv : s_lostTotals) {
        ++groups;
        if (kv.second > 0) line += " " + kv.first + "=" + std::to_string(kv.second);
    }
    mc2::fmt::print_err("[VK_GL_LOST_DRAWS v1] frame={:d} final={:d}{} total_groups={:d}\n", mc2::fmt::as_u64(s_lostFrame), mc2::fmt::as_i32(final_ ? 1 : 0), line.c_str(), mc2::fmt::as_u32(groups));
    std::fflush(stderr);
}
bool lostWanted()
{
    static const bool s_on = sceneGlProducerQueryEnabled()
        && mc2::renderBackend() == mc2::RenderBackend::Vulkan;
    return s_on;
}
} // namespace

void lostFrameBegin()
{
    if (!lostWanted() || s_lostDestroyed) return;
    g_gosGlLostDrawsOn = true;
    lostResolveSlot((int)(s_lostFrame % kLostRing));
    s_lostFrameActive = true;
    s_lostStack.clear();
    s_lostStack.push_back("scene_other");
    lostBeginCur();
}
void lostFrameEnd()
{
    if (!s_lostFrameActive) return;
    lostEndCur();
    s_lostStack.clear();
    s_lostFrameActive = false;
    ++s_lostFrame;
    if (s_lostFrame % 300 == 0) lostEmit(false);
}
void lostShutdown()
{
    if (s_lostDestroyed || !g_gosGlLostDrawsOn) return;
    s_lostDestroyed = true;
    g_gosGlLostDrawsOn = false;
    if (s_lostFrameActive) lostFrameEnd();
    for (int i = 0; i < kLostRing; ++i) lostResolveSlot(i);
    lostEmit(true);
    if (!s_lostFreeQ.empty()) glDeleteQueries((GLsizei)s_lostFreeQ.size(), s_lostFreeQ.data());
    s_lostFreeQ.clear();
}

void gos_GlLostDrawsPush(const char* tag)
{
    if (!s_lostFrameActive) return;
    lostEndCur();
    s_lostStack.push_back(tag);
    lostBeginCur();
}
void gos_GlLostDrawsPop()
{
    if (!s_lostFrameActive || s_lostStack.size() <= 1) return;
    lostEndCur();
    s_lostStack.pop_back();
    lostBeginCur();
}

void gosPostProcess::runHzbProbe()
{
    ZoneScopedN("Render.HZBProbe");

    // HZB-OCCLUSION-PROBE-1: DIAGNOSTIC ONLY. Reads back a parent HZB level and
    // its child level and (a) checks parent == MIN(children) -- the reverse-Z
    // reduction invariant -- and (b) runs the real conservative cull comparison
    // childDepth < parentDepth, which must be wouldKeep for every self-point
    // (a texel inside a tile can never be culled by that tile's min). This
    // exercises the exact cull math + the pyramid before real object bounds are
    // wired (next slice), with ZERO effect on rendering: read-only, no draw
    // suppression. neverAppliedToDraws is always true.
    if (!hzbProbeEnabled_) return;
    if (hzbMipCount_ < 2) return;
    if (hzbBuildCount_ == 0) return;        // need a built pyramid

    // Choose a small parent level (dims <= 64) so the readback is cheap.
    int pLevel = 1;
    {
        int dw = hzbW_, dh = hzbH_, lvl = 0;
        while (lvl < hzbMipCount_ - 1 && (dw > 64 || dh > 64)) {
            dw = (dw + 1) / 2; dh = (dh + 1) / 2; ++lvl;
        }
        pLevel = (lvl < 1) ? 1 : lvl;        // parent must have a child (>=1)
    }
    const int cLevel = pLevel - 1;

    auto dimsAt = [&](int level, int& w, int& h) {
        w = hzbW_; h = hzbH_;
        for (int k = 0; k < level; ++k) { w = (w + 1) / 2; h = (h + 1) / 2; }
        if (w < 1) w = 1; if (h < 1) h = 1;
    };
    int pw, ph, cw, ch;
    dimsAt(pLevel, pw, ph);
    dimsAt(cLevel, cw, ch);

    // Cost-bound readback: only levels at/under 256 px on the long axis (Lmin).
    // Both self-test levels (cLevel/pLevel) are coarser than Lmin, and object
    // LOD selection is CLAMPED to >= Lmin (clamping coarser only ever makes the
    // test MORE conservative -- a bigger tile MIN is smaller, so it keeps more).
    int Lmin = 0;
    {
        int dw = hzbW_, dh = hzbH_, lvl = 0;
        while (lvl < hzbMipCount_ - 1 && (dw > 256 || dh > 256)) {
            dw = (dw + 1) / 2; dh = (dh + 1) / 2; ++lvl;
        }
        Lmin = lvl;
    }
    if (Lmin > cLevel) Lmin = cLevel;   // guarantee the self-test levels are resident

    std::vector<std::vector<float>> hzbCpu(hzbMipCount_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int L = Lmin; L < hzbMipCount_; ++L) {
        int lw, lh; dimsAt(L, lw, lh);
        hzbCpu[L].resize((size_t)lw * lh);
        glBindTexture(GL_TEXTURE_2D, hzbLevelTex_[L]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, hzbCpu[L].data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    const std::vector<float>& parent = hzbCpu[pLevel];
    const std::vector<float>& child  = hzbCpu[cLevel];

    const float eps = 1e-5f;
    unsigned tested = 0, wouldKeep = 0, wouldCull = 0;
    unsigned integrityMismatch = 0, invalidDepth = 0;

    // Parent-centric, using the EXACT footprint hzb_reduce.frag samples: for
    // parent texel (px,py) the shader reads child texels at
    //   floor( ((px+0.5)/pw)*cw +/- 0.5 ),  floor( ((py+0.5)/ph)*ch +/- 0.5 )
    // (NEAREST + clamp). On odd child extents these 2x2 windows overlap at the
    // boundary -- which is why a naive px*2 / cx/2 inverse mapping produces
    // false mismatches. Replicating the sample positions exactly makes the
    // integrity + cull self-tests agree with the GPU when the pyramid is sound.
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    for (int py = 0; py < ph; ++py) {
        const float sv = ((py + 0.5f) / (float)ph) * (float)ch;
        const int cy0 = clampi((int)std::floor(sv - 0.5f), 0, ch - 1);
        const int cy1 = clampi((int)std::floor(sv + 0.5f), 0, ch - 1);
        for (int px = 0; px < pw; ++px) {
            const float su = ((px + 0.5f) / (float)pw) * (float)cw;
            const int cx0 = clampi((int)std::floor(su - 0.5f), 0, cw - 1);
            const int cx1 = clampi((int)std::floor(su + 0.5f), 0, cw - 1);

            const float ca = child[(size_t)cy0 * cw + cx0];
            const float cb = child[(size_t)cy0 * cw + cx1];
            const float cc = child[(size_t)cy1 * cw + cx0];
            const float cd = child[(size_t)cy1 * cw + cx1];
            float m = ca;
            if (cb < m) m = cb;
            if (cc < m) m = cc;
            if (cd < m) m = cd;

            const float pv = parent[(size_t)py * pw + px];
            if (!std::isfinite(pv) || !std::isfinite(m)) { ++invalidDepth; continue; }

            // (a) reduction integrity: GPU parent must equal the CPU min.
            if (std::fabs(pv - m) > eps) ++integrityMismatch;

            // (b) conservative cull self-test: each child the parent covers is a
            // self-point inside the tile, so its depth must be >= parent's MIN
            // (reverse-Z) -> never culled. wouldCull MUST stay 0.
            const float kids[4] = { ca, cb, cc, cd };
            for (float d : kids) {
                ++tested;
                if (d < pv - eps) ++wouldCull; else ++wouldKeep;
            }
        }
    }

    // ---- Real static-prop occlusion probe (HZB-STATICPROP-CULL-RECON-1) ----
    // For each active static prop: build an AABB from its world center +/- the
    // extent radius, project the 8 corners through viewProj_ (the GL-NDC
    // reverse-Z world->clip transform that produced the scene depth, fed by
    // Camera::worldToClipGL), take the screen rect + the object's CLOSEST
    // reverse-Z depth (max over corners), pick the HZB LOD matching the rect
    // size, sample that level's covered texels (MIN = farthest occluder), and
    // run the conservative cull comparison objClosest < hzbMin. DIAGNOSTIC ONLY
    // -- counts would-cull/would-keep; NEVER suppresses a draw. Real props can be
    // genuinely occluded, so objWouldCull > 0 is expected; the safety invariant
    // is only that nothing acts on it. objOnScreen ~ 0 would mean the projection
    // convention is wrong (axis-swap) -- a loud red flag, not a silent failure.
    // ---- HZB-CAMERA-DISCONTINUITY-GUARD-1 ----------------------------------
    // Derive the camera pose this frame by unprojecting the NDC near/far centers
    // through inverseViewProj_ (set every frame alongside viewProj_, BEFORE the
    // scene depth render -> same-frame coherent with the HZB-source depth). A
    // near-instant pose change (e.g. mc2_17's intro 180deg snaps) makes a single
    // frame's screen-space occlusion test unreliable; we FLAG such frames as
    // unsafe-for-cull and split the raw vs guarded would-cull counts. This is
    // DIAGNOSTIC: it changes no rendering and suppresses no draw.
    auto unproject = [&](float nx, float ny, float nz, float out[3]) {
        const float v[4] = { nx, ny, nz, 1.0f };
        float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                w[r] += inverseViewProj_[c * 4 + r] * v[c];
        const float iw = (std::fabs(w[3]) > 1e-12f) ? 1.0f / w[3] : 0.0f;
        out[0] = w[0] * iw; out[1] = w[1] * iw; out[2] = w[2] * iw;
    };
    float camPos[3], camFar[3], camFwd[3] = { 0.0f, 0.0f, 0.0f };
    unproject(0.0f, 0.0f, 1.0f, camPos);   // reverse-Z near center (z=1)
    unproject(0.0f, 0.0f, 0.0f, camFar);   // far center (z=0)
    {
        float dx = camFar[0] - camPos[0], dy = camFar[1] - camPos[1], dz = camFar[2] - camPos[2];
        float fl = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (fl > 1e-6f) { camFwd[0] = dx / fl; camFwd[1] = dy / fl; camFwd[2] = dz / fl; }
    }
    bool  unsafeForCull = false;
    float camFwdAngleDeg = 0.0f, camPosDelta = 0.0f;
    if (hzbPrevCamValid_) {
        float px = camPos[0] - hzbPrevCamPos_[0], py = camPos[1] - hzbPrevCamPos_[1], pz = camPos[2] - hzbPrevCamPos_[2];
        camPosDelta = std::sqrt(px * px + py * py + pz * pz);
        float dot = camFwd[0] * hzbPrevCamFwd_[0] + camFwd[1] * hzbPrevCamFwd_[1] + camFwd[2] * hzbPrevCamFwd_[2];
        if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
        camFwdAngleDeg = std::acos(dot) * (180.0f / 3.14159265f);
        // Thresholds: a smooth pan is a few deg/frame; a 180deg snap is unmistakable.
        // Position guard only when the map extent is known (one-frame teleport).
        const float kAngleThreshDeg = 30.0f;
        const float kPosThresh = (mapHalfExtent_ > 0.0f) ? (0.25f * mapHalfExtent_) : 1e30f;
        unsafeForCull = (camFwdAngleDeg > kAngleThreshDeg) || (camPosDelta > kPosThresh);
    }
    if (unsafeForCull) ++hzbCamDiscontinuityFrames_;
    hzbPrevCamPos_[0] = camPos[0]; hzbPrevCamPos_[1] = camPos[1]; hzbPrevCamPos_[2] = camPos[2];
    hzbPrevCamFwd_[0] = camFwd[0]; hzbPrevCamFwd_[1] = camFwd[1]; hzbPrevCamFwd_[2] = camFwd[2];
    hzbPrevCamValid_ = true;

    // ---- Real static-prop occlusion probe (HZB-STATICPROP-CULL-RECON-1 +
    //      HZB-CULL-READINESS-COUNTERS-1) -- DIAGNOSTIC ONLY, neverAppliedToDraws.
    // objWouldCullRaw            = raw conservative cull decisions (always counted)
    // objWouldCullGuarded        = raw culls on SAFE (non-discontinuous) frames
    // objSkippedCameraDiscont    = raw culls suppressed because the frame is unsafe
    //   (objWouldCullRaw == objWouldCullGuarded + objSkippedCameraDiscont)
    unsigned objActive = 0, objScanned = 0, objTested = 0;
    unsigned objWouldKeep = 0, objWouldCullRaw = 0, objWouldCullGuarded = 0;
    unsigned objSkippedCameraDiscont = 0, objOffscreen = 0, objNearClippedKeep = 0;
    unsigned objInvalidRect = 0;
    unsigned lodHist[kHzbMaxLevels] = { 0 };

    // HZB-CULL-MARGIN-SWEEP-1: how many GUARDED would-cull candidates survive
    // increasingly conservative depth-gap margins. gap = objClosest - hzbMin
    // (reverse-Z: gap < 0 == behind the occluder). A real cull path should
    // require gap < -margin, not just gap < 0; this quantifies the marginal
    // (near-zero) candidates so we can pick that margin. Accumulated over SAFE
    // (non-discontinuous) frames only, matching the guarded semantics.
    static const float kMargins[] = { 0.00000f, 0.00005f, 0.00010f, 0.00025f, 0.00050f, 0.00100f };
    const int kNumMargins = (int)(sizeof(kMargins) / sizeof(kMargins[0]));
    unsigned objWouldCullAtMargin[6] = { 0 };   // size matches kMargins
    float minGap = 1e9f, maxGap = -1e9f, closestToZeroNegGap = -1e9f;
    unsigned numMarginalCandidates = 0;          // -0.00010 < gap < 0

    // Bounded sample of the GUARDED would-cull candidates CLOSEST to zero (the
    // most marginal / highest false-positive risk), so a human can verify they
    // are truly hidden rather than grazing false positives.
    struct CullCand { int idx; float r0, r1, r2, r3, closest, hmin, gap; int lod; };
    const int kCandMax = 8;
    CullCand cand[kCandMax];
    int candCount = 0;
    {
        auto projectClip = [&](float wx, float wy, float wz, float out[4]) {
            const float w4[4] = { wx, wy, wz, 1.0f };
            float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    clip[r] += viewProj_[c * 4 + r] * w4[c];
            out[0] = clip[0]; out[1] = clip[1]; out[2] = clip[2]; out[3] = clip[3];
        };

        objActive = GpuStaticPropRegistry::getActiveCount();
        const int      kScanCap = 1 << 16;   // backstop vs sparse indices
        const unsigned kObjCap  = 4096;       // bound per-frame cost
        for (int idx = 0; idx < kScanCap && objScanned < objActive && objScanned < kObjCap; ++idx) {
            float mtx[16];
            if (!GpuStaticPropRegistry::staticPropGetModelMatrix(idx, mtx)) continue; // tombstone/gap
            ++objScanned;

            float radius = 0.0f;
            GpuStaticPropRegistry::staticPropGetExtentRadius(idx, &radius);
            if (!(radius > 0.0f)) radius = 1.0f;
            const float cx = -mtx[3], cy = mtx[11], cz = mtx[7];  // MC2 east/north/elev

            float uvMinX = 1e9f, uvMaxX = -1e9f, uvMinY = 1e9f, uvMaxY = -1e9f;
            float objClosest = -1e9f;  // reverse-Z: closest = MAX depth
            bool crossesNear = false;
            for (int ci = 0; ci < 8; ++ci) {
                const float wx = cx + ((ci & 1) ? radius : -radius);
                const float wy = cy + ((ci & 2) ? radius : -radius);
                const float wz = cz + ((ci & 4) ? radius : -radius);
                float clip[4];
                projectClip(wx, wy, wz, clip);
                if (clip[3] <= 1e-6f) { crossesNear = true; break; }
                const float inv = 1.0f / clip[3];
                const float ndcx = clip[0] * inv, ndcy = clip[1] * inv;
                const float depth = clip[2] * inv;            // reverse-Z [0,1]
                const float ux = ndcx * 0.5f + 0.5f, uy = ndcy * 0.5f + 0.5f;
                if (ux < uvMinX) uvMinX = ux; if (ux > uvMaxX) uvMaxX = ux;
                if (uy < uvMinY) uvMinY = uy; if (uy > uvMaxY) uvMaxY = uy;
                if (depth > objClosest) objClosest = depth;
            }
            if (crossesNear) { ++objNearClippedKeep; continue; }   // conservative: keep
            if (uvMaxX < 0.0f || uvMinX > 1.0f || uvMaxY < 0.0f || uvMinY > 1.0f) {
                ++objOffscreen; continue;
            }

            const float cMinX = uvMinX < 0.0f ? 0.0f : uvMinX;
            const float cMaxX = uvMaxX > 1.0f ? 1.0f : uvMaxX;
            const float cMinY = uvMinY < 0.0f ? 0.0f : uvMinY;
            const float cMaxY = uvMaxY > 1.0f ? 1.0f : uvMaxY;
            float rpw = (cMaxX - cMinX) * (float)hzbW_;
            float rph = (cMaxY - cMinY) * (float)hzbH_;
            if (rpw < 1.0f) rpw = 1.0f; if (rph < 1.0f) rph = 1.0f;
            int L = (int)std::ceil(std::log2((rpw > rph ? rpw : rph)));
            if (L < Lmin) L = Lmin;
            if (L > hzbMipCount_ - 1) L = hzbMipCount_ - 1;

            int lw, lh; dimsAt(L, lw, lh);
            const std::vector<float>& lvl = hzbCpu[L];
            int tx0 = clampi((int)std::floor(cMinX * lw), 0, lw - 1);
            int tx1 = clampi((int)std::floor(cMaxX * lw), 0, lw - 1);
            int ty0 = clampi((int)std::floor(cMinY * lh), 0, lh - 1);
            int ty1 = clampi((int)std::floor(cMaxY * lh), 0, lh - 1);
            if (tx1 - tx0 > 3) tx1 = tx0 + 3;                  // bound the inner loop
            if (ty1 - ty0 > 3) ty1 = ty0 + 3;
            float hzbMin = 1e9f;
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) {
                    const float d = lvl[(size_t)ty * lw + tx];
                    if (d < hzbMin) hzbMin = d;
                }
            if (!std::isfinite(hzbMin) || !std::isfinite(objClosest)) { ++objInvalidRect; continue; }

            ++objTested;
            if (L >= 0 && L < kHzbMaxLevels) ++lodHist[L];
            const float gap = objClosest - hzbMin;   // reverse-Z: <0 == behind

            if (!unsafeForCull) {
                if (gap < minGap) minGap = gap;
                if (gap > maxGap) maxGap = gap;
                if (gap < 0.0f && gap > closestToZeroNegGap) closestToZeroNegGap = gap;
                if (gap > -0.00010f && gap < 0.0f) ++numMarginalCandidates;
                for (int mi = 0; mi < kNumMargins; ++mi)
                    if (gap < -kMargins[mi]) ++objWouldCullAtMargin[mi];
            }

            // Conservative reverse-Z cull: object's nearest point is behind the
            // farthest occluder in its footprint -> fully occluded.
            if (objClosest < hzbMin - eps) {
                ++objWouldCullRaw;
                if (unsafeForCull) {
                    ++objSkippedCameraDiscont;       // guard suppresses this frame
                } else {
                    ++objWouldCullGuarded;
                    // Keep the kCandMax candidates with gap CLOSEST to zero (most
                    // marginal). If full, replace the least-marginal (smallest gap).
                    if (candCount < kCandMax) {
                        cand[candCount] = { idx, cMinX, cMinY, cMaxX, cMaxY,
                                            objClosest, hzbMin, gap, L };
                        ++candCount;
                    } else {
                        int worst = 0;
                        for (int k = 1; k < kCandMax; ++k)
                            if (cand[k].gap < cand[worst].gap) worst = k;  // most negative
                        if (gap > cand[worst].gap)
                            cand[worst] = { idx, cMinX, cMinY, cMaxX, cMaxY,
                                            objClosest, hzbMin, gap, L };
                    }
                }
            } else {
                ++objWouldKeep;
            }
        }
    }

    static unsigned long long s_probeFrame = 0;
    ++s_probeFrame;
    const bool logTick = (s_probeFrame <= 5 || (s_probeFrame % 600) == 0);

    if (s_probeFrame <= 3 || (s_probeFrame % 600) == 0 || wouldCull || integrityMismatch) {
        MC2_DIAGFE(stderr,
            "[HZB_PROBE v1] parentL=%d(%dx%d) childL=%d(%dx%d) tested=%u "
            "wouldKeep=%u wouldCull=%u integrityMismatch=%u invalidDepth=%u "
            "neverAppliedToDraws=1\n",
            pLevel, pw, ph, cLevel, cw, ch, tested,
            wouldKeep, wouldCull, integrityMismatch, invalidDepth);
    }
    // Log object readiness counters on the tick OR on any unsafe (discontinuity)
    // frame, so camera snaps are always visible in the trace.
    if (logTick || unsafeForCull) {
        mc2::fmt::print_err("[HZB_PROBE_OBJ v2] staticProps active={:d} scanned={:d} tested={:d} wouldKeep={:d} wouldCullRaw={:d} wouldCullGuarded={:d} skippedCameraDiscont={:d} nearClippedKeep={:d} offscreen={:d} invalidRect={:d} Lmin={:d} cameraDiscontinuity={:d} fwdAngleDeg={:.1f} posDelta={:.1f} discontFramesCumulative={:d} unsafeForCull={:d} neverAppliedToDraws=1\n", mc2::fmt::as_u32(objActive), mc2::fmt::as_u32(objScanned), mc2::fmt::as_u32(objTested), mc2::fmt::as_u32(objWouldKeep), mc2::fmt::as_u32(objWouldCullRaw), mc2::fmt::as_u32(objWouldCullGuarded), mc2::fmt::as_u32(objSkippedCameraDiscont), mc2::fmt::as_u32(objNearClippedKeep), mc2::fmt::as_u32(objOffscreen), mc2::fmt::as_u32(objInvalidRect), mc2::fmt::as_i32(Lmin), mc2::fmt::as_i32(unsafeForCull ? 1 : 0), camFwdAngleDeg, camPosDelta, mc2::fmt::as_u64((unsigned long long)hzbCamDiscontinuityFrames_), mc2::fmt::as_i32(unsafeForCull ? 1 : 0));
    }
    if (logTick && objTested > 0) {
        char hist[256]; int n = 0;
        n += mc2::fmt::to_buf_n(hist + n, sizeof(hist) - n, "[HZB_PROBE_LOD v1] selectedLod");
        for (int L = 0; L < hzbMipCount_ && L < kHzbMaxLevels && n < (int)sizeof(hist) - 16; ++L)
            if (lodHist[L]) n += mc2::fmt::to_buf_n(hist + n, sizeof(hist) - n, " L{:d}={:d}", mc2::fmt::as_i32(L), mc2::fmt::as_u32(lodHist[L]));
        mc2::fmt::print_err("{}\n", mc2::fmt::safe(hist));
    }
    // Depth-gap margin sweep (safe-frame guarded candidates). guard@0.00000
    // equals wouldCullGuarded by gap<0; higher margins show how many survive a
    // more conservative gap<-margin requirement.
    if (logTick && objTested > 0) {
        mc2::fmt::print_err("[HZB_PROBE_MARGIN v1] guardCull@{{0={:d},5e-5={:d},1e-4={:d},2.5e-4={:d},5e-4={:d},1e-3={:d}}} minGap={:.6f} maxGap={:.6f} closestToZeroNegGap={:.6f} numMarginal(-1e-4<gap<0)={:d} neverAppliedToDraws=1\n", mc2::fmt::as_u32(objWouldCullAtMargin[0]), mc2::fmt::as_u32(objWouldCullAtMargin[1]), mc2::fmt::as_u32(objWouldCullAtMargin[2]), mc2::fmt::as_u32(objWouldCullAtMargin[3]), mc2::fmt::as_u32(objWouldCullAtMargin[4]), mc2::fmt::as_u32(objWouldCullAtMargin[5]), (minGap <= 1e8f ? minGap : 0.0f), (maxGap >= -1e8f ? maxGap : 0.0f), (closestToZeroNegGap >= -1e8f ? closestToZeroNegGap : 0.0f), mc2::fmt::as_u32(numMarginalCandidates));
    }
    // Bounded sample of guarded would-cull candidates CLOSEST to zero (safe
    // frames only) -- the highest false-positive risk to eyeball.
    if (logTick && candCount > 0) {
        for (int i = 0; i < candCount; ++i) {
            const CullCand& c = cand[i];
            mc2::fmt::print_err("[HZB_PROBE_CULLCAND v1] idx={:d} rectUV=({:.3f},{:.3f},{:.3f},{:.3f}) objClosest={:.5f} hzbMin={:.5f} gap={:.6f} L={:d} neverAppliedToDraws=1\n", mc2::fmt::as_i32(c.idx), c.r0, c.r1, c.r2, c.r3, c.closest, c.hmin, c.gap, mc2::fmt::as_i32(c.lod));
        }
    }
}
