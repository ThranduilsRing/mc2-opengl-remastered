// gos_screenshot.cpp - see gos_screenshot.h.
#include "gos_screenshot.h"

#include <cstdio>
#include <string>
#include <vector>

#include "mc2fmt.h"
#include "render_backend_select.h"

namespace gos { namespace screenshot {

namespace {

struct Request {
    bool pending = false;
    std::string path;
    DoneFn done = nullptr;
    unsigned int framesWaited = 0;
};
constexpr unsigned int kMaxFramesPending = 300;
Request g_req;

}  // namespace

bool request(const char* path, DoneFn done) {
    if (!path || !path[0]) {
        mc2::fmt::print_err("[SCREENSHOT v1] request rejected: empty path\n");
        return false;
    }
    if (mc2::renderBackend() != mc2::RenderBackend::Vulkan) {
        mc2::fmt::print_err("[SCREENSHOT v1] request rejected: the Vulkan backend is not drawing\n");
        return false;
    }
    if (g_req.pending) {
        mc2::fmt::print_err("[SCREENSHOT v1] request rejected: {} still pending\n", mc2::fmt::safe(g_req.path.c_str()));
        return false;
    }
    g_req.pending = true;
    g_req.path = path;
    g_req.done = done;
    return true;
}

bool pending() { return g_req.pending; }

void tick() {
    if (!g_req.pending) return;
    if (++g_req.framesWaited < kMaxFramesPending) return;
    // The present never reached its readback (a failed or declined present, a fallback): free the slot so
    // the next request is not rejected for the rest of the run.
    const Request req = g_req;
    g_req = Request{};
    mc2::fmt::print_err("[SCREENSHOT v1] request for {} expired after {:d} frames without a readback\n", mc2::fmt::safe(req.path.c_str()), mc2::fmt::as_u32(kMaxFramesPending));
    if (req.done) req.done(req.path.c_str(), false, 0, 0);
}

void deliver(unsigned int w, unsigned int h, const unsigned char* rgba) {
    if (!g_req.pending) return;
    const Request req = g_req;
    g_req = Request{};
    bool ok = false;
    if (!rgba || w == 0 || h == 0 || w > 0xFFFFu || h > 0xFFFFu) {
        mc2::fmt::print_err("[SCREENSHOT v1] readback rejected: {:d}x{:d} pixels={}\n", mc2::fmt::as_u32(w), mc2::fmt::as_u32(h), mc2::fmt::safe(rgba ? "set" : "null"));
    } else if (FILE* f = fopen(req.path.c_str(), "wb")) {
        unsigned char header[18] = {};
        header[2] = 2;   // uncompressed true-colour, origin bottom-left
        header[12] = w & 0xFF;
        header[13] = (w >> 8) & 0xFF;
        header[14] = h & 0xFF;
        header[15] = (h >> 8) & 0xFF;
        header[16] = 24;
        std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            bgr[i * 3 + 0] = rgba[i * 4 + 2];
            bgr[i * 3 + 1] = rgba[i * 4 + 1];
            bgr[i * 3 + 2] = rgba[i * 4 + 0];
        }
        ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) && fwrite(bgr.data(), 1, bgr.size(), f) == bgr.size();
        fclose(f);
    } else {
        mc2::fmt::print_err("[SCREENSHOT v1] cannot open {} for writing\n", mc2::fmt::safe(req.path.c_str()));
    }
    if (ok)
        mc2::fmt::print_err("[SCREENSHOT v1] wrote {} ({:d}x{:d}, vk_present)\n", mc2::fmt::safe(req.path.c_str()), mc2::fmt::as_u32(w), mc2::fmt::as_u32(h));
    if (req.done) req.done(req.path.c_str(), ok, static_cast<int>(w), static_cast<int>(h));
}

}}  // namespace gos::screenshot
