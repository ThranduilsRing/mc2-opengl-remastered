// Environment metadata, separate from recompiled code and transient VM frames.
#pragma once
#include "../code/save_sections.h"

namespace mc2::save {
struct AblEnvironmentRecord {
    std::uint32_t libraries = 0, modules = 0, eternals = 0;
    std::vector<std::string> files;
    void serialize(Archive& a) {
        a(libraries, modules, eternals);
        a.sequence(files, maxSequenceLength, [](Archive& archive, std::string& file) {
            Text text{1, file, 1023};
            archive(text);
            if (archive.reading() && archive.good()) file = std::move(text.bytes);
        });
        if (libraries > files.size() || libraries > modules ||
            modules > maxSequenceLength || eternals > maxSequenceLength)
            a.reject("abl_environment_counts");
        for (const auto& file : files)
            if (file.empty() || file.size() >= 1024 || file.find('\0') != std::string::npos)
                a.reject("abl_source_name");
    }
};
}
