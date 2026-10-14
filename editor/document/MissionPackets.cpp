#include "MissionDocument.h"

#include <array>
#include <zlib.h>

namespace editor::document {
namespace {
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}

bool inflatePacket(std::span<const std::byte> input, std::vector<std::byte>& output)
{
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit(&stream) != Z_OK) return false;
    const auto result = inflate(&stream, Z_FINISH);
    const bool complete = result == Z_STREAM_END && stream.total_out == output.size() && stream.total_in == input.size();
    inflateEnd(&stream);
    return complete;
}

// Legacy LZW is an on-disk format; its bounded dictionary belongs to this decoding operation.
bool inflateLegacy(std::span<const std::byte> input, std::vector<std::byte>& output)
{
    if (inflatePacket(input, output)) return true;
    std::array<std::uint16_t, 4096> parents{};
    std::array<std::byte, 4096> suffixes{}, stack{};
    std::size_t read{}, written{};
    std::uint32_t buffer{};
    unsigned available{}, width = 9, free = 258;
    int previous = -1;
    std::byte previousFirst{};
    for (;;) {
        while (available < width) {
            if (read == input.size()) return false;
            buffer |= std::to_integer<std::uint32_t>(input[read++]) << available;
            available += 8;
        }
        const unsigned code = buffer & ((1U << width) - 1);
        buffer >>= width;
        available -= width;
        if (code == 257) return written == output.size();
        if (code == 256) { width = 9; free = 258; previous = -1; continue; }
        unsigned current = code;
        std::size_t count{};
        if (current == free && previous >= 0) { stack[count++] = previousFirst; current = static_cast<unsigned>(previous); }
        else if (current >= free) return false;
        while (current >= 258) {
            if (current >= free || count >= stack.size() - 1 || parents[current] >= current) return false;
            stack[count++] = suffixes[current];
            current = parents[current];
        }
        if (current > 255) return false;
        const auto first = static_cast<std::byte>(current);
        stack[count++] = first;
        if (count > output.size() - written) return false;
        while (count) output[written++] = stack[--count];
        if (previous >= 0 && free < 4096) {
            parents[free] = static_cast<std::uint16_t>(previous);
            suffixes[free++] = first;
            if (free >= (1U << width) && width < 12) ++width;
        }
        previous = static_cast<int>(code);
        previousFirst = first;
    }
}
}

std::expected<PacketArchive, LoadError> decodePak(std::vector<std::byte> bytes)
{
    const auto fail = [](std::string field, std::string reason, std::size_t offset) {
        return std::unexpected(LoadError{std::move(field), std::move(reason), offset});
    };
    if (bytes.size() < 28 || bytes.size() > pakByteLimit) return fail("PAK", "file_size_limit", 0);
    const std::span<const std::byte> source(bytes);
    const auto header = u32(source, 0), tableEnd = u32(source, 4);
    if (tableEnd < 28 || tableEnd > bytes.size() || tableEnd % 4 != 0 || tableEnd / 4 - 2 > nodeLimit)
        return fail("PAK", "invalid_packet_table", 4);
    if (header != 0xfeedfaceU) {
        std::uint32_t checksum{};
        for (std::size_t i = 4; i < bytes.size(); ++i) checksum += std::to_integer<unsigned>(bytes[i]);
        if (header != checksum) return fail("PAK", "checksum_mismatch", 0);
    }
    PacketArchive archive;
    const auto count = tableEnd / 4 - 2;
    std::size_t totalDecoded{};
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = u32(source, 8 + i * 4);
        const auto begin = entry & 0x1fffffffU;
        const auto end = i + 1 == count ? bytes.size() : u32(source, 12 + i * 4) & 0x1fffffffU;
        const auto type = static_cast<std::uint8_t>(entry >> 29);
        const auto field = "Packet" + std::to_string(i);
        if (begin < tableEnd || end < begin || end > bytes.size() || (i == 0 && begin != tableEnd))
            return fail(field, "packet_bounds", 8 + i * 4);
        Packet packet{type, {begin, end}, {}};
        const auto encoded = source.subspan(begin, end - begin);
        if (type == 7) {
            if (!encoded.empty()) return fail(field, "nonempty_null_packet", begin);
        } else if (type == 0 || type == 1) {
            if (encoded.size() > packetByteLimit || encoded.size() > decodedPakByteLimit - totalDecoded) return fail(field, "packet_size_limit", begin);
            packet.decoded.assign(encoded.begin(), encoded.end());
        } else if (type == 2 || type == 4) {
            if (encoded.size() < 5) return fail(field, "truncated_compressed_packet", begin);
            const auto length = u32(encoded, 0);
            if (length == 0 || length > packetByteLimit || length > decodedPakByteLimit - totalDecoded) return fail(field, "packet_size_limit", begin);
            packet.decoded.resize(length);
            if (!(type == 4 ? inflatePacket(encoded.subspan(4), packet.decoded) : inflateLegacy(encoded.subspan(4), packet.decoded)))
                return fail(field, "invalid_compressed_packet", begin);
        } else return fail(field, "unsupported_compression", begin);
        totalDecoded += packet.decoded.size();
        archive.packets.push_back(std::move(packet));
    }
    archive.originalBytes = std::move(bytes);
    return archive;
}
}
