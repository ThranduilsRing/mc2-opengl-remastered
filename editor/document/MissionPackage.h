#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace mc2::editor_package {
// Export a saved mission as a new, unpacked mc2-package/1 mod; never replace a project.
bool exportMissionPackage(const std::filesystem::path& mission,
    const std::filesystem::path& modsRoot, const std::string& id,
    const std::vector<std::string>& dependencies, std::string& status);
}
