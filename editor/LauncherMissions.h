#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace editor::launcher {
// Only complete loose campaign pairs are offered; archives remain read-only.
inline std::vector<std::string> campaignMissions(const std::filesystem::path& folder)
{
    std::vector<std::string> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const auto path = it->path();
        const auto name = path.stem().string();
        std::error_code entryError;
        if (name.size() == 6 && name.starts_with("mc2_") && name[4] >= '0' && name[4] <= '9'
            && name[5] >= '0' && name[5] <= '9' && path.extension() == ".pak"
            && it->is_regular_file(entryError) && std::filesystem::is_regular_file(folder / (name + ".fit"), entryError))
            result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

inline std::filesystem::path copyCampaign(const std::filesystem::path& install, const std::string& name,
    std::error_code& error)
{
    const auto source = install / "data/missions";
    const auto missions = campaignMissions(source);
    if (!std::binary_search(missions.begin(), missions.end(), name)) {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }
    const auto root = install / "user-missions";
    std::filesystem::create_directories(root, error);
    if (error) return {};
    std::filesystem::path destination;
    for (int copy = 1; !error; ++copy) {
        destination = root / (name + "-" + std::to_string(copy));
        if (std::filesystem::create_directory(destination, error)) break;
    }
    if (error) return {};
    for (const char* extension : {".pak", ".fit", ".abl"}) {
        const auto from = source / (name + extension);
        if (std::string(extension) == ".abl" && !std::filesystem::exists(from, error) && !error) continue;
        if (!error) std::filesystem::copy_file(from, destination / from.filename(), std::filesystem::copy_options::none, error);
        // copy_file preserves read-only attributes; the new mission must be editable.
        if (!error) std::filesystem::permissions(destination / from.filename(), std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add, error);
        if (error) {
            std::error_code cleanup;
            std::filesystem::remove_all(destination, cleanup);
            return {};
        }
    }
    return destination / (name + ".pak");
}
}
