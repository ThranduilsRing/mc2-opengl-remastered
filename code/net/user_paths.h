#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace mc2::net {
// Inline so the asset viewer keeps its normal paths without linking the session.
inline std::filesystem::path userDirectory;
inline std::string userFile(std::string_view name) {
    return userDirectory.empty() ? std::string(name) : (userDirectory / name).string();
}
}
