#pragma once
#include <string>
#include <vector>

namespace mc2mechanim {
// Main-thread preview scope only; never installed while a mission actor ticks.
struct PreviewAnimation {
    struct Clip { std::string name; float duration; };
    bool playing = true, loop = true;
    float rate = 1, seek = -1, time = 0, duration = 0;
    std::string clip, active;
    int paletteBones = 0;
    std::vector<Clip> clips;
};
inline PreviewAnimation* previewAnimation = nullptr;
}
