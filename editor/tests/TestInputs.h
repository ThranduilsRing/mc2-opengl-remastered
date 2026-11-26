#pragma once

#include <filesystem>
#include <cstdint>
#include <vector>

namespace editor::tests {
struct TestInputs {
    std::filesystem::path repositoryRoot;
    std::filesystem::path stockDirectory;
    std::filesystem::path byteGradeDirectory;
    std::filesystem::path placementBase, placementMods, placementGoldens, placementOutput;
    std::vector<std::uint32_t> markerObjects;
};

const TestInputs& inputs();
}
