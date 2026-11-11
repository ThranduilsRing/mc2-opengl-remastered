// vfs_internal.h -- the mod layer's state as seen by the mc2::vfs index (vfs_index.cpp). Defined in vfs.cpp.
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ModFileEntry {
    std::string path;   // absolute path to real file
    std::string modId;  // owning mod id (for logging)
};

namespace mc2::vfs::internal {
const std::unordered_map<std::string, ModFileEntry>& modIndex();                      // priority-resolved winner per key
const std::unordered_map<std::string, std::vector<std::string>>& modShadowedBy();     // key -> mod ids that lost
const std::unordered_map<std::string, int>& modPriority();
const std::string& modsAbsRoot();                                                     // absolute mods root, trailing '/'
bool shouldSearchMods(std::string_view fileName);
bool isAbsolutePath(std::string_view p);
} // namespace mc2::vfs::internal
