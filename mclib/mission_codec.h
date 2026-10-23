#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace mc2::mission {

// Mission PAK packet 1 is a count followed by Win32 records, independent of the live object layout.
struct ObjectRecord {
    std::int32_t type;
    std::array<float, 3> position;
    float rotation;
    std::int32_t damage, team;
    std::uint32_t parentId;
    std::array<std::int32_t, 2> padding;
};
static_assert(sizeof(ObjectRecord) == 40);
static_assert(std::endian::native == std::endian::little);

inline std::optional<std::uint32_t> objectCount(std::span<const std::byte> bytes)
{
    if (bytes.size() < 4) return {};
    std::uint32_t count;
    std::memcpy(&count, bytes.data(), 4);
    if (count > (bytes.size() - 4) / sizeof(ObjectRecord) ||
        bytes.size() != 4 + static_cast<std::size_t>(count) * sizeof(ObjectRecord)) return {};
    return count;
}

// Callers validate the packet count once before indexing its records.
inline ObjectRecord objectRecord(std::span<const std::byte> bytes, std::size_t index)
{
    ObjectRecord record;
    std::memcpy(&record, bytes.data() + 4 + index * sizeof(record), sizeof(record));
    return record;
}

inline std::optional<std::array<std::uint32_t, 2>> parentCell(std::uint32_t id)
{
    if (id == 0 || id == 0xffffffffU) return {};
    return std::array{id >> 16, id & 0xffffU};
}

} // namespace mc2::mission
