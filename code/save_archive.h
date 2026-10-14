#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mc2::save {

using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;
using Diagnostic = std::function<void(std::string_view)>;
inline constexpr std::size_t maxFileBytes = 64 * 1024 * 1024;
inline constexpr std::uint32_t maxSequenceLength = 65536;
inline constexpr std::uint32_t maxTextBytes = 1024 * 1024;

// One field list defines both directions; allocations and wire bytes have separate budgets.
class Archive {
public:
    explicit Archive(Diagnostic diagnostic = {});
    explicit Archive(std::span<const std::uint8_t> input, Diagnostic diagnostic = {});
    bool reading() const { return reading_; }
    bool good() const { return error_.empty(); }
    std::string_view error() const { return error_; }
    std::size_t remaining() const { return input_.size() - cursor_; }
    const Bytes& bytes() const { return output_; }
    bool finish();
    void reject(std::string_view reason);
    void raw(std::span<std::uint8_t> value);
    bool allocate(std::size_t count, std::size_t width);

    template<class... T> void operator()(T&... fields) {
        try { (field(fields), ...); }
        catch (const std::bad_alloc&) { reject("allocation"); }
        catch (const std::length_error&) { reject("length"); }
    }

private:
    template<class T> requires (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    void field(T& value) {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
        using U = std::make_unsigned_t<T>;
        std::array<std::uint8_t, sizeof(T)> encoded{};
        if (!reading_) {
            U bits = std::bit_cast<U>(value);
            for (std::size_t i = 0; i < encoded.size(); ++i)
                encoded[i] = static_cast<std::uint8_t>(bits >> (i * 8));
        }
        raw(encoded);
        if (reading_ && good()) {
            U bits = 0;
            for (std::size_t i = 0; i < encoded.size(); ++i)
                bits |= static_cast<U>(static_cast<U>(encoded[i]) << (i * 8));
            value = std::bit_cast<T>(bits);
        }
    }
    void field(bool& value) {
        std::uint8_t encoded = value ? 1 : 0;
        field(encoded);
        if (good() && encoded > 1) reject("boolean");
        if (good() && reading_) value = encoded != 0;
    }
    template<class T> requires std::is_floating_point_v<T>
    void field(T& value) {
        static_assert(std::numeric_limits<T>::is_iec559);
        using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        static_assert(sizeof(T) == sizeof(U));
        if (!reading_ && !std::isfinite(value)) { reject("non_finite"); return; }
        U bits = std::bit_cast<U>(value);
        field(bits);
        if (reading_ && good()) {
            T decoded = std::bit_cast<T>(bits);
            if (!std::isfinite(decoded)) reject("non_finite");
            else value = decoded;
        }
    }
    template<class T, std::size_t N> void field(std::array<T, N>& values) {
        for (auto& value : values) { if (!good()) break; field(value); }
    }
    template<class T> void field(std::vector<T>& values) {
        if (!good()) return;
        if (!reading_ && values.size() > maxSequenceLength) { reject("sequence_count"); return; }
        std::uint32_t count = static_cast<std::uint32_t>(values.size());
        field(count);
        if (!good()) return;
        if (count > maxSequenceLength) { reject("sequence_count"); return; }
        if (reading_) {
            if (!allocate(count, sizeof(T))) return;
            values.resize(count);
        }
        for (auto& value : values) { if (!good()) break; field(value); }
    }
    void field(std::vector<bool>& values) {
        if (!good()) return;
        if (!reading_ && values.size() > maxSequenceLength) { reject("sequence_count"); return; }
        std::uint32_t count = static_cast<std::uint32_t>(values.size());
        field(count);
        if (!good()) return;
        if (count > maxSequenceLength) { reject("sequence_count"); return; }
        if (reading_) {
            if (count > remaining()) { reject("truncated_sequence"); return; }
            if (!allocate(count, 1)) return;
            values.resize(count);
        }
        for (std::uint32_t i = 0; i < count && good(); ++i) {
            bool value = values[i];
            field(value);
            if (reading_ && good()) values[i] = value;
        }
    }
    template<class T> void field(std::optional<T>& value) {
        bool present = value.has_value();
        field(present);
        if (!good()) return;
        if (reading_) {
            if (present) value.emplace(); else value.reset();
        }
        if (present) field(*value);
    }
    template<std::size_t I = 0, class... T>
    void variantArm(std::uint8_t tag, std::variant<T...>& value) {
        if constexpr (I < sizeof...(T)) {
            if (tag == I + 1) {
                if (reading_) value.template emplace<I>();
                field(std::get<I>(value));
            } else variantArm<I + 1>(tag, value);
        } else reject("variant_tag");
    }
    template<class... T> void field(std::variant<T...>& value) {
        static_assert(sizeof...(T) < 256);
        if (!good()) return;
        // Variant alternatives are schema declarations with frozen, one-based tags.
        std::uint8_t tag = static_cast<std::uint8_t>(value.index() + 1);
        field(tag);
        if (good()) variantArm(tag, value);
    }
    template<class T> requires requires(T& value, Archive& a) { value.serialize(a); }
    void field(T& value) {
        if (!good()) return;
        if (depth_ == 64) { reject("nesting"); return; }
        ++depth_;
        value.serialize(*this);
        if constexpr (requires { value.validate(*this); }) {
            if (good()) value.validate(*this);
        }
        --depth_;
    }

    bool reading_ = false;
    std::span<const std::uint8_t> input_;
    std::size_t cursor_ = 0;
    std::size_t allocationBudget_ = maxFileBytes;
    std::size_t depth_ = 0;
    Bytes output_;
    std::string error_;
    Diagnostic diagnostic_;
};

struct Text {
    std::uint8_t encoding = 1;
    std::string bytes;
    std::uint32_t limit = maxTextBytes;
    void serialize(Archive& a);
    bool operator==(const Text& other) const { return encoding == other.encoding && bytes == other.bytes; }
};

template<std::uint32_t Limit> struct BoundedText : Text {
    BoundedText(std::uint8_t encoding = 1, std::string bytes = {})
        : Text{encoding, std::move(bytes), Limit} {}
    void serialize(Archive& a) { limit = Limit; Text::serialize(a); }
};

struct Header {
    std::array<std::uint8_t, 8> magic{'M','C','2','S','A','V','E','1'};
    std::uint32_t schemaVersion = 1;
    BoundedText<128> releaseLine;
    BoundedText<40> originBuildSha;
    Digest dataFingerprint{};
    Digest simRulesFingerprint{};
    std::uint8_t saveKind = 1;
    std::uint32_t sectionCount = 0;
    std::uint64_t payloadBytes = 0;
    Digest payloadSha256{};
    void serialize(Archive& a) {
        a(magic, schemaVersion, releaseLine, originBuildSha, dataFingerprint,
          simRulesFingerprint, saveKind, sectionCount, payloadBytes, payloadSha256);
    }
    bool operator==(const Header&) const = default;
};

struct SectionHeader {
    std::uint32_t tag = 0;
    bool required = true;
    std::uint64_t payloadBytes = 0;
    Digest payloadSha256{};
    void serialize(Archive& a) { a(tag, required, payloadBytes, payloadSha256); }
};

struct Section {
    std::uint32_t tag = 0;
    bool required = true;
    Bytes payload;
    bool operator==(const Section&) const = default;
};

struct Document {
    Header header;
    std::vector<Section> sections;
    bool operator==(const Document&) const = default;
};

struct Compatibility {
    std::string_view releaseLine;
    Digest dataFingerprint{};
    Digest simRulesFingerprint{};
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    void serialize(Archive& a) { a(x, y, z); }
    bool operator==(const Vec3&) const = default;
};

// The Part value schema excludes CSV import hints and all native object ownership.
struct Part {
    std::int32_t objectWID = 0;
    std::uint32_t objNumber = 0, baseColor = 0, highlightColor1 = 0, highlightColor2 = 0;
    std::int32_t active = 0, exists = 0;
    bool destroyed = false;
    Vec3 position;
    float velocity = 0, rotation = 0;
    std::uint32_t gestureId = 0;
    std::int8_t alignment = 0, teamId = 0;
    std::int32_t commanderID = 0, squadId = 0;
    std::int8_t myIcon = 0;
    std::uint32_t controlType = 0, controlDataType = 0;
    BoundedText<8> profileName{2};
    std::uint32_t pilot = 0;
    bool captureable = false;
    std::uint32_t variantNum = 0;
    void serialize(Archive& a) {
        a(objectWID, objNumber, baseColor, highlightColor1, highlightColor2, active,
          exists, destroyed, position, velocity, rotation, gestureId, alignment,
          teamId, commanderID, squadId, myIcon, controlType, controlDataType,
          profileName, pilot, captureable, variantNum);
    }
    void validate(Archive& a) const;
    bool operator==(const Part&) const = default;
};

Digest sha256(std::span<const std::uint8_t> bytes);
bool encode(Document document, Bytes& output, Diagnostic diagnostic = {});
bool decode(std::span<const std::uint8_t> bytes, const Compatibility& compatibility,
            Document& output, Diagnostic diagnostic = {});
bool writeFile(const std::filesystem::path& path, Document document, Diagnostic diagnostic = {});
bool readFile(const std::filesystem::path& path, const Compatibility& compatibility,
              Document& output, Diagnostic diagnostic = {});

template<class T> bool decodeRecord(std::span<const std::uint8_t> bytes, T& output,
                                    Diagnostic diagnostic = {}) {
    Archive archive(bytes, std::move(diagnostic));
    T candidate{};
    archive(candidate);
    if (!archive.finish()) return false;
    output = std::move(candidate);
    return true;
}

} // namespace mc2::save
