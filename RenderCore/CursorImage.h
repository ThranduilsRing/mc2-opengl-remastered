#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2 {

// Clear RGB as well as alpha: filtered software fallback must not bleed pink.
inline void keyCursorPixels(std::span<uint8_t> rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] == 0 ||
            (rgba[i] == 255 && rgba[i + 1] == 0 && rgba[i + 2] == 255))
            rgba[i] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0;
    }
}

// One native handle per shape/frame; failed creation leaves that slot empty.
template<class Handle> struct CursorShapes {
    std::vector<std::vector<Handle>> shapes;

    template<class Create> void create(std::span<const unsigned long> frames, Create make) {
        shapes.resize(frames.size());
        for (size_t shape = 0; shape < frames.size(); ++shape) {
            shapes[shape].resize(frames[shape] ? frames[shape] : 1);
            for (size_t frame = 0; frame < shapes[shape].size(); ++frame)
                shapes[shape][frame] = make(shape, frame);
        }
    }

    Handle get(long shape, long frame) const {
        if (shape < 0 || static_cast<size_t>(shape) >= shapes.size()) return {};
        const auto& frames = shapes[shape];
        if (frames.empty()) return {};
        return frames[frame < 0 ? 0 : static_cast<size_t>(frame) % frames.size()];
    }
};

} // namespace mc2
