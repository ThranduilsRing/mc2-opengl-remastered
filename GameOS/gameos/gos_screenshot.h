// gos_screenshot.h - TGA capture of the frame the Vulkan present path shows.
//
// WHAT: a caller queues one TGA request; the present path's readback of the finished frame hands its pixels to deliver(), which writes
// the file and tells the caller through a completion function.
// WHY: the Vulkan readback is the single pixel source the capture harness already trusts and there is no GL framebuffer read, so a run
// on the GL backend rejects the request with a log line and writes no file.
#pragma once

#include "gos_screenshot_slot.h"   // DoneFn

namespace gos { namespace screenshot {

// Queue a TGA of the next presented frame. Returns false (and logs why) when a request is already pending, the path is empty, or the
// Vulkan backend is not the one drawing. `done` may be null.
bool request(const char* path, DoneFn done = nullptr);

// True while a request waits for the present readback; the present path reads the frame back when this is true.
bool pending();

// Once per presented frame: a request still waiting after a bounded number of frames is dropped (one log line, `done` called with !ok)
// so a present that never reads back cannot block every later request.
void tick();

// The present readback's pixels: RGBA8, bottom-up rows (GL orientation). Writes the pending request's file as an uncompressed 24-bit TGA.
void deliver(unsigned int w, unsigned int h, const unsigned char* rgbaBottomUp);

}}  // namespace gos::screenshot
