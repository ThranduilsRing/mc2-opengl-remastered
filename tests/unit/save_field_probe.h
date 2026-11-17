// Independent member census: never derive this list from serialize().
#pragma once
#include "doctest.h"
#include "save_archive.h"
#include <cstring>
#include <tuple>
#include <type_traits>

// A new field can consume existing padding without changing sizeof. Structured
// binding arity still changes, so the census fails to compile in that case too.
#define SAVE_FIELD_SHAPE(value, ...) \
    (void)static_cast<void (*)(std::remove_cvref_t<decltype(value)>&)>( \
        [](auto& object) { auto& [__VA_ARGS__] = object; (void)std::tie(__VA_ARGS__); })

namespace save_probe {
// Members of #pragma pack(1) structs can be misaligned and binding a reference to one is undefined:
// run fn on an aligned copy of a trivially copyable member and write the copy back.
template<class T, class M, class F> void viaCopy(T& object, M member, F&& fn) {
    using V = std::remove_cvref_t<decltype(object.*member)>;
    if constexpr (std::is_trivially_copyable_v<V>) {
        V copy{};
        std::memcpy(static_cast<void*>(&copy), static_cast<const void*>(&(object.*member)), sizeof(V));
        fn(copy);
        if constexpr (!std::is_const_v<T>)
            std::memcpy(static_cast<void*>(&(object.*member)), static_cast<const void*>(&copy), sizeof(V));
    } else fn(object.*member);
}
template<class T> bool equal(const T& a, const T& b) {
    if constexpr (std::is_array_v<T>) {
        for (size_t i = 0; i < std::size(a); ++i) if (!save_probe::equal(a[i], b[i])) return false;
        return true;
    } else return a == b;
}
template<class T> void pattern(T& value, unsigned char seed) {
    if constexpr (std::is_same_v<T, bool>) value = true;
    else if constexpr (std::is_floating_point_v<T>) std::memset(&value, seed, sizeof(value));
    else if constexpr (std::is_enum_v<T>) value = static_cast<T>(1);
    // `long` is the 32-bit wire width the codecs write (in_range<int32_t>); on LP64 a full-width memset
    // would be out of range, so pattern it with the 32-bit value the Windows 4-byte long gets.
    else if constexpr (std::is_same_v<T, long>) value = static_cast<std::int32_t>(0x01010101u * seed);
    else if constexpr (std::is_same_v<T, unsigned long>) value = static_cast<std::uint32_t>(0x01010101u * seed);
    else if constexpr (std::is_integral_v<T>) std::memset(&value, seed, sizeof(value));
    else if constexpr (std::is_same_v<T, std::string>) value.assign(3, char(seed));
    else if constexpr (requires { value.flip(); value.set(0); }) {
        for (size_t i = 0; i < value.size(); ++i) value[i] = (i + seed) % 3 != 0;
    }
    else if constexpr (requires { value.resize(2); }) {
        value.resize(2);
        for (auto& item : value) pattern(item, seed);
    } else if constexpr (requires { value.x; value.y; value.z; }) {
        pattern(value.x, seed); pattern(value.y, seed); pattern(value.z, seed);
    } else if constexpr (std::is_array_v<T>) {
        for (auto& item : value) pattern(item, seed);
    } else static_assert(sizeof(T) == 0, "add an independent pattern for this nested value");
}

template<class T, class... M> T patterned(M... members) {
    T value{};
    unsigned char seed = 1;
    (viaCopy(value, members, [&](auto& field) { pattern(field, seed++); }), ...);
    return value;
}

template<class T> void zero(T& value) {
    if constexpr (requires { value.clear(); }) value.clear();
    else if constexpr (requires { value.reset(); }) value.reset();
    else if constexpr (std::is_array_v<T>) { for (auto& item : value) save_probe::zero(item); }
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) value = T{};
    else if constexpr (requires { value.x; value.y; value.z; }) {
        save_probe::zero(value.x); save_probe::zero(value.y); save_probe::zero(value.z);
    }
    else if constexpr (std::is_trivially_copyable_v<T>) std::memset(&value, 0, sizeof(value));
    else static_assert(sizeof(T) == 0, "zero this nested value independently of its defaults");
}

template<class T, class Codec, class... M>
bool roundtrip(T sample, Codec codec, M... members) {
    const T expected = sample;
    mc2::save::Archive writer;
    codec(writer, sample);
    REQUIRE(writer.good());
    T decoded{};
    // Default member initializers can equal the pattern and hide an omission.
    // Zero each independently listed field before invoking the actual decoder.
    (save_probe::viaCopy(decoded, members, [](auto& field) { save_probe::zero(field); }), ...);
    mc2::save::Archive reader(writer.bytes());
    codec(reader, decoded);
    REQUIRE(reader.finish());
    auto same = [&](auto member) {
        bool equalFields = false;
        save_probe::viaCopy(expected, member, [&](const auto& e) {
            save_probe::viaCopy(decoded, member, [&](const auto& d) { equalFields = save_probe::equal(e, d); });
        });
        return equalFields;
    };
    return (same(members) && ...);
}

// Pin native padding separately; a new member must update this independent census.
template<class T, class... M> constexpr size_t memberBytes(M... members) {
    return (sizeof(std::declval<T>().*members) + ... + 0);
}
}
