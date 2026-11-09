#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::fx {

inline constexpr uint32_t gfx_signature = 0x47465823;
inline constexpr uint32_t mlr_signature = 0x4d4c5223;
inline constexpr uint32_t first_effect_class = 1312;

// Only the reader throws this internal exception. read_catalog translates it
// into a diagnostic and never publishes a partially decoded catalog.
class FormatError : public std::runtime_error {
public:
    FormatError(size_t offset, std::string_view message);
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes);
    size_t offset() const { return position_; }
    size_t remaining() const { return size_ - position_; }
    [[noreturn]] void fail(std::string_view message) const;
    uint32_t integer(unsigned width = 4);
    uint32_t peek() const;
    float scalar();
    bool boolean();
    std::string string();
    uint32_t count(size_t minimum_bytes, uint32_t maximum = 1048576);
    template<size_t N> std::array<float, N> scalars() {
        std::array<float, N> values;
        for (auto& value : values) value = scalar();
        return values;
    }
private:
    std::span<const uint8_t> bytes_;
    size_t size_;
    size_t position_ = 0;
};

class Writer {
public:
    std::vector<uint8_t> bytes;
    void integer(uint32_t value, unsigned width = 4);
    void scalar(float value);
    void boolean(bool value) { integer(value ? 1 : 0, 1); }
    void string(std::string_view value);
    template<size_t N> void scalars(const std::array<float, N>& values) {
        for (float value : values) scalar(value);
    }
};

struct MlrVersion {
    int value = 9;
    bool tagged = true;
};
MlrVersion read_mlr_version(Reader& reader);
void write_mlr_version(Writer& writer, MlrVersion version);
const char* class_name(uint32_t ordinal);

}
