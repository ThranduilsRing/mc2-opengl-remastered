// ABL save values. No StackItem object representation or process address is wire data.
#pragma once
#include "../code/save_sections.h"
#include <cstring>

namespace mc2::save {
inline constexpr auto tagAblVariable = fourcc('A', 'V', 'A', 'R');

// Scalar integer/real/byte arms share their low 32 bits (including real NaN
// payloads). The address arm is derived: array storage is allocated from the
// recompiled size table before restore; scalar upper bytes rebuild as zero.
// ABL arrays contain only packed primitive values, never pointers or structs.
struct AblVariableRecord {
    std::uint32_t bits = 0;
    Bytes elements;
    void serialize(Archive& a) {
        a(bits);
        std::uint32_t count = static_cast<std::uint32_t>(elements.size());
        a(count);
        if (!a.good()) return;
        if (count > maxFileBytes || (a.reading() && count > a.remaining())) {
            a.reject("abl_array_size"); return;
        }
        if (a.reading()) {
            if (!a.allocate(count, 1)) return;
            elements.resize(count);
        }
        a.raw(elements);
        if (!elements.empty() && bits != 0) a.reject("abl_array_scalar");
    }
};

// A framed value record's size is valid when it holds at least the tag and version, fits the file cap, and fits what is left of a
// stream that reports its remaining length (remaining < 0: unknown).
inline bool ablRecordSizeValid(std::int64_t size, std::int64_t remaining) {
    return size >= 8 && static_cast<std::uint64_t>(size) <= maxFileBytes && (remaining < 0 || size <= remaining);
}

template<class Slot> AblVariableRecord captureAblVariable(const Slot& slot, std::size_t size) {
    AblVariableRecord value;
    if (size) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(slot.address);
        value.elements.assign(data, data + size);
    } else std::memcpy(&value.bits, &slot, sizeof(value.bits));
    return value;
}

template<class Slot> bool restoreAblVariable(const AblVariableRecord& value, Slot& slot, std::size_t size) {
    if (value.elements.size() != size || (size && value.bits)) return false;
    if (size) std::memcpy(slot.address, value.elements.data(), size);
    else {
        std::memset(&slot, 0, sizeof(slot));
        std::memcpy(&slot, &value.bits, sizeof(value.bits));
    }
    return true;
}
}
