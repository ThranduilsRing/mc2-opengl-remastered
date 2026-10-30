#pragma once

#include <cstdint>

namespace mc2vk::present_thread {
// Only the game opts in. Editors/viewers keep the same present function inline.
void start();
void stop();
bool enabled();
bool onWorker();
void waitSealed();
void waitIdle();
// The packet owns no game objects. Draw/HUD data belong to EnginePresentVk until sealed.
void enqueue(uint32_t width, uint32_t height, bool verify, uint32_t renderFrame,
             uint32_t fireFrame);
void pace(uint64_t startTick, int fps);
// Called after submission, readback and frame retirement, before entering WSI.
void seal();
void forgetTexture(uint32_t handle);
void requestInline();
}
