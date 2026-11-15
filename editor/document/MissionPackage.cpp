#include "MissionPackage.h"
#include "../../code/save_archive.h"
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace mc2::editor_package {
bool exportMissionPackage(const std::filesystem::path& mission,
    const std::filesystem::path& modsRoot, const std::string& id,
    const std::vector<std::string>& dependencies, std::string& status)
{
    namespace fs = std::filesystem;
    using nlohmann::json;
    try {
        const std::regex token("[a-z0-9][a-z0-9._-]*");
        if (!std::regex_match(id, token) || id.back() == '.')
            throw std::runtime_error("Use a lowercase mod id (letters, digits, dot, dash, underscore).");
        for (const auto& dependency : dependencies)
            if (!std::regex_match(dependency, token) || dependency == id)
                throw std::runtime_error("Invalid or self-referencing mod dependency.");
        const auto root = fs::absolute(modsRoot / id).lexically_normal();
        if (fs::exists(root)) throw std::runtime_error("Package folder already exists; choose a new mod id.");
        auto source = mission;
        source.replace_extension();
        const auto stem = source.filename().string();
        if (!std::regex_match(stem, std::regex("[A-Za-z0-9][A-Za-z0-9._-]*")) || stem.back() == '.')
            throw std::runtime_error("Invalid mission filename.");
        std::map<std::string, std::string> payload;
        for (const auto* suffix : {".pak", ".fit", "_purchase.fit", ".burnin.jpg", ".foliage.json", "_ai.fit", "_specials.fit"}) {
            const auto path = fs::path(source.string() + suffix);
            if (!fs::exists(path) && std::string_view(suffix) != ".pak" && std::string_view(suffix) != ".fit") continue;
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot read mission file: " + path.string());
            std::string bytes(std::istreambuf_iterator<char>{input}, {});
            if (input.bad()) throw std::runtime_error("Mission read failed: " + path.string());
            payload["data/missions/" + stem + suffix] = std::move(bytes);
        }
        payload["mod.json"] = json{{"id", id}, {"name", id}, {"version", "1.0.0"},
            {"type", "campaign"}, {"dependencies", dependencies}}.dump(2) + '\n';
        payload["data/campaign/" + id + ".fit"] =
            "FITini\n[Campaign]\nst CampaignName = \"" + id + "\"\nl GroupCount = 1\nl CBills = 1000000\n"
            "[Group0]\nl NumberToComplete = 1\nl Tune = 0\nst Video = \"\"\nl MissionCount = 1\n"
            "[Group0Mission0]\nst FileName = \"" + stem + "\"\nb Mandatory = TRUE\nb CompletePrevious = FALSE\n"
            "b Hidden = FALSE\nb PlayLogistics = FALSE\nb PlaySalvage = FALSE\nb PlayPilotPromotion = FALSE\n"
            "b PlayPurchasing = FALSE\nb PlaySelection = FALSE\nst PurchaseFile = \"" +
            (payload.contains("data/missions/" + stem + "_purchase.fit") ? stem + "_purchase" : "purchase_All") + "\"\nFITend\n";
        json manifest{{"schema", "mc2-package/1"}, {"id", id}, {"name", id}, {"version", "1.0.0"},
            {"dependencies", json::array()}, {"files", json::array()}};
        for (const auto& dependency : dependencies)
            manifest["dependencies"].push_back({{"id", dependency}, {"version", "*"}});
        for (const auto& [path, bytes] : payload) {
            std::string hash;
            for (const auto byte : mc2::save::sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()})) {
                hash += "0123456789abcdef"[byte >> 4];
                hash += "0123456789abcdef"[byte & 15];
            }
            manifest["files"].push_back({{"path", path}, {"size", bytes.size()}, {"sha256", hash}});
        }
        payload["package.json"] = manifest.dump(2) + '\n';
        fs::create_directories(root.parent_path());
        if (!fs::create_directory(root)) throw std::runtime_error("Package folder already exists.");
        for (const auto& [path, bytes] : payload) {
            fs::create_directories((root / path).parent_path());
            std::ofstream output(root / path, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) throw std::runtime_error("Package write failed; incomplete folder: " + root.string());
        }
        status = "Package ready: " + root.string() + " (Campaigns & mods)";
        return true;
    } catch (const std::exception& error) {
        status = error.what();
        return false;
    }
}
}
