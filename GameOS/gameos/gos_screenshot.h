// gos_screenshot.h - TGA capture of the frame the Vulkan present path shows.
//
// WHAT: a caller queues one TGA request; the present path's readback of the finished frame hands its pixels to deliver(), which writes
// the file and tells the caller through a completion function.
// WHY: the presented image is the one frame both backends of the renderer end in, and the Vulkan readback is the single pixel source the
// capture harness already trusts; there is no GL framebuffer read.
#pragma once

namespace gos { namespace screenshot {

// Called once the file is written (ok) or the write failed (!ok); w/h are the presented frame size.
using DoneFn = void (*)(const char* path, bool ok, int w, int h);

// Queue a TGA of the next presented frame. Returns false (and logs why) when a request is already pending, the path is empty, or the
// Vulkan backend is not the one drawing. `done` may be null.
bool request(const char* path, DoneFn done = nullptr);

// True while a request waits for the present readback; the present path reads the frame back when this is true.
bool pending();

// The present readback's pixels: RGBA8, bottom-up rows (GL orientation). Writes the pending request's file as an uncompressed 24-bit TGA.
void deliver(unsigned int w, unsigned int h, const unsigned char* rgbaBottomUp);

}}  // namespace gos::screenshot
