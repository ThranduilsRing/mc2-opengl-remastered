#include "FileResolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include "Gates/gate.h"

namespace editor::document {
namespace {
constexpr std::size_t fileLimit = 256 * 1024 * 1024;
std::unexpected<LoadError> failure(std::string field, std::string reason)
{
    return std::unexpected(LoadError{std::move(field), std::move(reason), 0});
}
std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i) result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return result;
}
bool relative(std::string_view key)
{
    if (key.empty() || key.front() == '/' || key.find(':') != key.npos || key.find('\0') != key.npos) return false;
    for (const auto& part : std::filesystem::path(key))
        if (part == ".." || part == "." || part.empty()) return false;
    return true;
}
bool modId(std::string_view id)
{
    return relative(id) && id.find('/') == id.npos && id.find('\\') == id.npos;
}
std::uint32_t hash(std::string_view key)
{
    std::uint32_t result{};
    for (const unsigned char c : key) {
        result = (result << 4) + c;
        const auto high = result & 0xf0000000;
        if (high) result ^= high >> 24;
        result &= ~high;
    }
    return result;
}
std::expected<std::vector<std::byte>, LoadError> readBytes(const std::filesystem::path& path,
    std::size_t offset = 0, std::optional<std::size_t> count = {})
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return failure(path.generic_string(), "open_failed");
    const auto end = input.tellg();
    if (end < 0 || offset > static_cast<std::uint64_t>(end)) return failure(path.generic_string(), "file_bounds");
    const auto size = count.value_or(static_cast<std::size_t>(end) - offset);
    if (size > fileLimit || size > static_cast<std::uint64_t>(end) - offset) return failure(path.generic_string(), "file_bounds");
    std::vector<std::byte> bytes(size);
    input.seekg(static_cast<std::streamoff>(offset));
    if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        return failure(path.generic_string(), "read_failed");
    return bytes;
}
std::string text(const std::vector<std::byte>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
bool inflateExact(std::span<const std::byte> input, std::span<std::byte> output)
{
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit(&stream) != Z_OK) return false;
    const auto result = inflate(&stream, Z_FINISH);
    const bool valid = result == Z_STREAM_END && stream.total_in == input.size() && stream.total_out == output.size();
    inflateEnd(&stream);
    return valid;
}
// Bounded dictionaries preserve the current game's LZW decoding without its static tables.
bool inflateFstLzw(std::span<const std::byte> input, std::span<std::byte> output)
{
    std::array<std::uint16_t, 4096> parents{};
    std::array<std::byte, 4096> suffixes{}, stack{};
    std::size_t read{}, written{};
    std::uint32_t bits{};
    unsigned available{}, width = 9, free = 258;
    int previous = -1;
    std::byte first{};
    const auto code = [&]() -> std::optional<unsigned> {
        while (available < width) {
            if (read == input.size()) return {};
            bits |= std::to_integer<std::uint32_t>(input[read++]) << available;
            available += 8;
        }
        const auto result = bits & ((1U << width) - 1);
        bits >>= width;
        available -= width;
        return result;
    };
    for (;;) {
        auto next = code();
        if (!next) return false;
        if (*next == 257) return written == output.size();
        if (*next == 256) {
            width = 9; free = 258; previous = -1;
            continue;
        }
        auto current = *next;
        std::size_t count{};
        if (current == free && previous >= 0) {
            stack[count++] = first;
            current = static_cast<unsigned>(previous);
        } else if (current >= free) return false;
        while (current > 255) {
            if (current >= free || current >= parents.size() || count >= stack.size() - 1 || parents[current] >= current) return false;
            stack[count++] = suffixes[current];
            current = parents[current];
        }
        if (current > 255) return false;
        first = static_cast<std::byte>(current);
        stack[count++] = first;
        if (count > output.size() - written) return false;
        while (count) output[written++] = stack[--count];
        if (previous >= 0 && free < parents.size()) {
            parents[free] = static_cast<std::uint16_t>(previous);
            suffixes[free++] = first;
        }
        previous = static_cast<int>(*next);
        if (free >= (1U << width) && width < 12) ++width;
    }
}
}

std::string FileResolver::normalize(std::string_view key)
{
    std::string result(key);
    for (char& c : result) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::vector<std::string> FileResolver::keys(std::string_view prefix) const
{
    std::vector<std::string> result;
    const auto folded = normalize(prefix);
    for (const auto& [key, location] : files_) if (key.starts_with(folded)) result.push_back(key);
    std::ranges::sort(result);
    return result;
}

std::expected<FileResolver, LoadError> FileResolver::mount(ResolverConfig config)
{
    FileResolver resolver;
    resolver.config_ = std::move(config);
    auto& settings = resolver.config_;
    if (settings.mods.empty()) settings.mods = settings.base / "mods";
    if (!settings.activeMod.empty() && !modId(settings.activeMod)) return failure("activeMod", "invalid_mod_id");
    std::string activeId = settings.activeMod;
    std::vector<std::string> dependencies;
    if (!settings.activeMod.empty()) {
        const auto manifest = settings.mods / settings.activeMod / "mod.json";
        std::error_code ec;
        if (std::filesystem::exists(manifest, ec)) {
            auto bytes = readBytes(manifest);
            if (!bytes) return std::unexpected(bytes.error());
            try {
                const auto json = nlohmann::json::parse(text(*bytes));
                dependencies = json.value("dependencies", std::vector<std::string>{});
                activeId = json.value("id", settings.activeMod);
                if (activeId.empty()) activeId = settings.activeMod;
            } catch (const nlohmann::json::exception&) { return failure("mod.json", "invalid_manifest"); }
        }
    }
    for (const auto& id : settings.dependencies)
        if (std::ranges::find(dependencies, id) == dependencies.end()) dependencies.push_back(id);
    for (const auto& id : dependencies) if (!modId(id)) return failure("dependencies", "invalid_mod_id");
    settings.dependencies = dependencies;
    const auto indexLoose = [&](const std::filesystem::path& root, const std::string& layer) -> std::expected<void, LoadError> {
        std::error_code ec;
        if (!std::filesystem::exists(root / "data", ec)) return {};
        std::filesystem::recursive_directory_iterator it(root / "data", ec), end;
        while (!ec && it != end) {
            const auto name = it->path().filename().string();
            const bool hidden = name.starts_with('.');
            if (hidden && it->is_directory(ec)) it.disable_recursion_pending();
            if (!hidden && it->is_regular_file(ec)) {
                const auto key = normalize(it->path().lexically_relative(root).generic_string());
                resolver.files_.try_emplace(key, Location{it->path(), layer});
            }
            it.increment(ec);
        }
        if (ec) return failure(root.generic_string(), "index_failed");
        return {};
    };
    if (!settings.activeMod.empty()) {
        if (mc2::gate::get<"MC2_SCRATCH">()) {   // row default ON; =0 is the kill-switch
            auto indexed = indexLoose(settings.mods / settings.activeMod / ".scratch",
                                      "mod:" + activeId + "#scratch");
            if (!indexed) return std::unexpected(indexed.error());
        }
        auto indexed = indexLoose(settings.mods / settings.activeMod, "mod:" + activeId);
        if (!indexed) return std::unexpected(indexed.error());
    }
    for (const auto& id : dependencies) {
        auto indexed = indexLoose(settings.mods / id, "mod:" + id);
        if (!indexed) return std::unexpected(indexed.error());
    }
    auto indexed = indexLoose(settings.base, "base-loose");
    if (!indexed) return std::unexpected(indexed.error());
    auto system = readBytes(settings.base / "system.cfg");
    if (!system) return std::unexpected(system.error());
    auto syntax = decodeFit(text(*system));
    if (!syntax) return std::unexpected(syntax.error());
    const auto field = [&](std::string_view name) -> const Field* {
        for (const auto& section : syntax->sections) if (identifier(section.name) == "fastfiles")
            for (const auto id : section.fields) for (const auto& f : syntax->fields)
                if (f.id == id && identifier(f.name) == identifier(name)) return &f;
        return nullptr;
    };
    const auto* countField = field("NumFastFiles");
    if (!countField || countField->value.size() != 1 || !std::holds_alternative<std::int64_t>(countField->value[0]))
        return failure("FastFiles", "invalid_count");
    const auto count = std::get<std::int64_t>(countField->value[0]);
    if (count < 0 || count > 256) return failure("FastFiles", "invalid_count");
    for (std::int64_t i = 0; i < count; ++i) {
        const auto* f = field("File" + std::to_string(i));
        if (!f || f->value.size() != 1 || !std::holds_alternative<std::string>(f->value[0])) return failure("FastFiles", "missing_file");
        const auto name = normalize(std::get<std::string>(f->value[0]));
        if (!relative(name)) return failure("FastFiles", "invalid_path");
        const auto path = settings.base / name;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        auto header = readBytes(path, 0, 8);
        if (!header) return std::unexpected(header.error());
        const auto magic = word(*header, 0), entries = word(*header, 4);
        if ((magic != 0xfaddecaf && magic != 0xcaddecaf) || entries > nodeLimit) return failure(name, "invalid_fst_header");
        auto table = readBytes(path, 8, static_cast<std::size_t>(entries) * 266);
        if (!table) return std::unexpected(table.error());
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) return failure(name, "file_size_failed");
        for (std::size_t j = 0; j < entries; ++j) {
            const auto p = j * 266;
            const auto offset = word(*table, p), encoded = word(*table, p + 4), decoded = word(*table, p + 8);
            const auto* start = reinterpret_cast<const char*>(table->data() + p + 16);
            const auto* nul = std::find(start, start + 250, '\0');
            if (nul == start + 250 || offset < 8 + table->size() || offset > size || encoded > size - offset || decoded > fileLimit)
                return failure(name, "invalid_fst_entry");
            const auto key = normalize(std::string_view(start, static_cast<std::size_t>(nul - start)));
            if (!relative(key)) return failure(name, "invalid_fst_key");
            if (word(*table, p + 12) != hash(key)) continue;
            resolver.files_.try_emplace(key, Location{path, "fastfile", offset, encoded, decoded, true, magic == 0xfaddecaf});
        }
    }
    return resolver;
}

std::expected<ResolvedFile, LoadError> FileResolver::read(std::string_view input) const
{
    const auto key = normalize(input);
    if (!relative(key)) return failure(key, "invalid_key");
    const auto it = files_.find(key);
    if (it == files_.end()) return failure(key, "missing_file");
    const auto& location = it->second;
    auto bytes = location.archive ? readBytes(location.path, location.offset, location.encoded) : readBytes(location.path);
    if (!bytes) return std::unexpected(bytes.error());
    if (location.archive && location.encoded == location.decoded && bytes->size() > 2 &&
        (*bytes)[0] == std::byte{0x78} && (0x7800 + std::to_integer<unsigned>((*bytes)[1])) % 31 == 0) {
        std::vector<std::byte> decoded(location.decoded);
        if (inflateExact(*bytes, decoded)) bytes = std::move(decoded);
    }
    if (location.archive && location.encoded != location.decoded) {
        std::vector<std::byte> decoded(location.decoded);
        if (!inflateExact(*bytes, decoded) && !(location.lzw && inflateFstLzw(*bytes, decoded))) return failure(key, "invalid_compression");
        bytes = std::move(decoded);
    }
    return ResolvedFile{std::move(*bytes), key, location.layer, location.path};
}
} // namespace editor::document
