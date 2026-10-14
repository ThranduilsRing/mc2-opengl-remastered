#pragma once

#include <filesystem>

namespace editor::tests {
struct TestInputs {
    std::filesystem::path stockDirectory;
    std::filesystem::path byteGradeDirectory;
};

const TestInputs& inputs();
}
