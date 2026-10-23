#include "MissionEnvironmentAdapter.h"
#include "../mclib/file.h"
#include <filesystem>

namespace GameAdapters::Environment {

VisualTuningSource readProfileSource(std::string_view path) {
    const std::string logical(path);
    const auto binding = ResolveModFileSource(path);
    VisualTuningSource source{logical,"base-loose",{},binding.editable};
    if (binding.path) {
        source.path = *binding.path;
        const char* owner = LookupModOwner(logical.c_str());
        source.provider = "mod:" + std::string(owner ? owner : "unknown");
    }
    const long exists = fileExists(logical.c_str());
    if (!exists) return source;
    if (exists == FILE_ON_FST) { source.provider = "base-fst"; source.editable = false; }
    File file;
    if (file.open(logical.c_str()) != NO_ERR) {
        source.error = "unreadable profile: " + logical;
        return source;
    }
    const auto length = file.getLength();
    if (!length || length > 512*1024) {
        source.error = "invalid profile size: " + logical;
        return source;
    }
    source.text.resize(length);
    if (file.read(reinterpret_cast<MemoryPtr>(source.text.data()),static_cast<long>(length)) != static_cast<long>(length)) {
        source.text.clear(); source.error = "short profile read: " + logical;
    }
    return source;
}

const RenderCore::LegacyMissionLighting& publishLighting(const RenderCore::LegacyMissionLighting& lighting) {
    visualTuning_setMissionLighting(lighting);
    return visualTuning_environment().lighting;
}
} // namespace GameAdapters::Environment
