#include "ObjectCatalog.h"
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>

using namespace editor::document;
int main(int argc, char** argv)
{
    if (argc != 5) { std::cerr << "usage: editor_catalog_probe BASE MODS ACTIVE_OR_- OUTPUT\n"; return 2; }
    auto resolver = FileResolver::mount({argv[1], argv[2], std::string(argv[3]) == "-" ? "" : argv[3]});
    if (!resolver) { std::cerr << resolver.error().field << ':' << resolver.error().reason << '\n'; return 1; }
    std::vector<std::string> logs;
    auto names = resourceNames(*resolver, [&](const auto& line) { std::cout << line << '\n'; });
    const auto catalog = ObjectCatalog::load(*resolver, [&](const auto& line) { logs.push_back(line); std::cout << line << '\n'; }, std::move(names));
    nlohmann::json report;
    report["rejections"] = logs;
    report["entries"] = nlohmann::json::array();
    report["files"] = nlohmann::json::object();
    report["counts"] = nlohmann::json::object();
    const std::filesystem::path output(argv[4]);
    std::filesystem::create_directories(output);
    const auto record = [&](const std::string& key) {
        if (report["files"].contains(key)) return;
        auto file = resolver->read(key);
        if (!file) return;
        report["files"][key] = {{"layer", file->layer}, {"source", file->source.generic_string()}};
        const auto path = output / key;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(file->bytes.data()), static_cast<std::streamsize>(file->bytes.size()));
    };
    record("data/art/buildings.csv"); record("data/objects/object2.pak");
    std::map<std::string, std::size_t> counts;
    for (const auto& entry : catalog.entries()) {
        ++counts[std::string(familyName(entry.family))];
        report["entries"].push_back({{"id", entry.id}, {"stem", entry.stem}, {"family", familyName(entry.family)},
            {"runtimeClass", entry.runtimeClass}, {"name", entry.displayName}, {"appearance", entry.appearance},
            {"definitionLayer", entry.definition.layer}, {"enumerationLayer", entry.enumeration.layer}, {"variantCount", entry.variants.size()}});
        if (entry.variantFile) record(entry.variantFile->key);
    }
    report["counts"] = counts;
    std::ofstream(output / "catalog.json") << report.dump(2) << '\n';
    std::cout << "[EDITOR_CATALOG v1] counts=" << report["counts"].dump() << " rejections=" << logs.size() << '\n';
    return catalog.entries().empty() ? 1 : 0;
}
