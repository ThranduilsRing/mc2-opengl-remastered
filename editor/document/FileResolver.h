#pragma once

#include "MissionDocument.h"

namespace editor::document {

struct ResolvedFile {
    std::vector<std::byte> bytes;
    std::string key, layer;
    std::filesystem::path source;
};

struct ResolverConfig {
    std::filesystem::path base, mods;
    std::string activeMod;
    // Launcher dependencies follow manifest dependencies, highest priority first.
    std::vector<std::string> dependencies;
};

class FileResolver {
public:
    static std::expected<FileResolver, LoadError> mount(ResolverConfig config);
    std::expected<ResolvedFile, LoadError> read(std::string_view key) const;
    static std::string normalize(std::string_view key);
    std::vector<std::string> keys(std::string_view prefix) const;
    const ResolverConfig& config() const { return config_; }
private:
    struct Location {
        std::filesystem::path path;
        std::string layer;
        std::uint32_t offset{}, encoded{}, decoded{};
        bool archive{}, lzw{};
    };
    ResolverConfig config_;
    std::unordered_map<std::string, Location> files_;
};

} // namespace editor::document
