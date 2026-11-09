#include "fx_format.h"

#include <cmath>
#include <cstring>

namespace mc2::fx {

FormatError::FormatError(size_t offset, std::string_view message)
    : std::runtime_error("FX byte " + std::to_string(offset) + ": " + std::string(message)) {}

Reader::Reader(std::span<const uint8_t> bytes) : bytes_(bytes), size_(bytes.size()) {
    if (size_ > 128u * 1024u * 1024u) fail("catalog exceeds 128 MiB");
}

void Reader::fail(std::string_view message) const { throw FormatError(position_, message); }

uint32_t Reader::integer(unsigned width) {
    if (width == 0 || width > 4 || remaining() < width) fail("truncated integer");
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= static_cast<uint32_t>(bytes_[position_++]) << (8 * i);
    return result;
}

uint32_t Reader::peek() const {
    if (remaining() < 4) fail("truncated record");
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= static_cast<uint32_t>(bytes_[position_ + i]) << (8 * i);
    return result;
}

float Reader::scalar() {
    const uint32_t bits = integer();
    float value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) fail("non-finite scalar");
    return value;
}

bool Reader::boolean() {
    const auto value = integer(1);
    if (value > 1) fail("invalid boolean");
    return value != 0;
}

uint32_t Reader::count(size_t minimum_bytes, uint32_t maximum) {
    const auto value = integer();
    if (value > maximum || (minimum_bytes && value > remaining() / minimum_bytes))
        fail("invalid count or truncated array (" + std::to_string(value) + ")");
    return value;
}

std::string Reader::string() {
    const auto size = count(1);
    if (!size) return {};
    if (remaining() <= size || bytes_[position_ + size] != 0) fail("unterminated string");
    const auto* start = reinterpret_cast<const char*>(bytes_.data() + position_);
    if (std::memchr(start, 0, size)) fail("embedded string terminator");
    std::string value(start, size);
    position_ += size + 1;
    return value;
}

void Writer::integer(uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void Writer::scalar(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    integer(bits);
}

void Writer::string(std::string_view value) {
    integer(static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
        bytes.insert(bytes.end(), value.begin(), value.end());
        bytes.push_back(0);
    }
}

MlrVersion read_mlr_version(Reader& reader) {
    if (reader.peek() != mlr_signature) return {-1, false};
    reader.integer();
    const auto value = reader.integer();
    if (value < 1 || value > 9) reader.fail("unsupported MLR version " + std::to_string(value));
    return {static_cast<int>(value), true};
}

void write_mlr_version(Writer& writer, MlrVersion version) {
    if (version.tagged) { writer.integer(mlr_signature); writer.integer(version.value); }
}

const char* class_name(uint32_t ordinal) {
    static constexpr std::array names = {"Effect", "ParticleCloud", "PointCloud", "SpinningCloud",
        "ShardCloud", "PertCloud", "CardCloud", "ShapeCloud", "EffectCloud", "Singleton",
        "Card", "Shape", "Tube", "DebrisCloud", "PointLight"};
    const auto index = ordinal - first_effect_class;
    return index < names.size() ? names[index] : "Unknown";
}
}
