#pragma once

#include <filesystem>

namespace editor::tests {
struct TestInputs {
    std::filesystem::path stockDirectory;
    std::filesystem::path byteGradeDirectory;
    std::filesystem::path placementBase, placementMods, placementGoldens, placementOutput;
};

const TestInputs& inputs();
}
