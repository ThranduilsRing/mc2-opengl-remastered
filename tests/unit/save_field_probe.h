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
    (void)[](auto& object) { auto& [__VA_ARGS__] = object; return std::tie(__VA_ARGS__); }(value)

namespace save_probe {
template<class T> bool equal(const T& a, const T& b) {
    if constexpr (std::is_array_v<T>) {
        for (size_t i = 0; i < std::size(a); ++i) if (!save_probe::equal(a[i], b[i])) return false;
        return true;
    } else return a == b;
}
template<class T> void pattern(T& value, unsigned char seed) {
    if constexpr (std::is_same_v<T, bool>) value = true;
    else if constexpr (std::is_floating_point_v<T>) value = T(seed) + T(0.25);
    else if constexpr (std::is_enum_v<T>) value = static_cast<T>(1);
    else if constexpr (std::is_integral_v<T>) std::memset(&value, seed, sizeof(value));
    else if constexpr (std::is_same_v<T, std::string>) value.assign(3, char(seed));
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
    (pattern(value.*members, seed++), ...);
    return value;
}

template<class T, class Codec, class... M>
bool roundtrip(T sample, Codec codec, M... members) {
    mc2::save::Archive writer;
    codec(writer, sample);
    REQUIRE(writer.good());
    T decoded{};
    mc2::save::Archive reader(writer.bytes());
    codec(reader, decoded);
    REQUIRE(reader.finish());
    return (save_probe::equal(sample.*members, decoded.*members) && ...);
}

// Pin native padding separately; a new member must update this independent census.
template<class T, class... M> constexpr size_t memberBytes(M... members) {
    return (sizeof(std::declval<T>().*members) + ... + 0);
}
}
