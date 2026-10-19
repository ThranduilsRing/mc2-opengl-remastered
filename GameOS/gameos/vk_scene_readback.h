#pragma once
#include <cstddef>
#include <cstdint>

namespace mc2vk {
enum SceneReadbackChannel : uint32_t { SceneColor = 1, SceneDepth = 2, SceneObjectId = 4, SceneAll = 7 };
struct ScenePixel {
    uint8_t rgba[4]{};
    float depth = 0;
    uint32_t objectId = 0;
};
struct ScenePoint { int x, y; };
// Native scene images retain the legacy scene's bottom-left coordinates. The
// final presentation copy flips Y; picking/probes must not flip it again here.
bool sceneReadbackExtent(int& width, int& height, uint32_t channels);
// Explicit picking and diagnostic reads only. Submits a bounded, synchronous
// copy and restores every image's attachment layout before returning.
bool readScenePixels(int x, int y, int width, int height, uint32_t channels, ScenePixel* out, size_t count);
bool readSceneSamples(const ScenePoint* points, size_t count, uint32_t channels, ScenePixel* out);
bool sceneReadbackSelfTest();
}
