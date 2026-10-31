// Commander values; native layout is retained only for the legacy reader.
#pragma once
#include "unitdesg.h"
#include "save_sections.h"

struct _MoverGroupData {
    long id;
    long numMovers;
    std::int32_t moverWIDs[MAX_MOVERGROUP_COUNT];
    std::int32_t pointWID;
    bool disbandOnNoPoint;

    void serialize(mc2::save::Archive& a) {
        static_assert(sizeof(long) == 4);
        a(id, numMovers);
        for (auto& wid : moverWIDs) a(wid);
        a(pointWID, disbandOnNoPoint);
    }
    bool operator==(const _MoverGroupData&) const = default;
};
using MoverGroupData = _MoverGroupData;

struct _CommanderData {
    long id;
    long teamId;
    MoverGroupData groups[MAX_MOVERGROUPS];

    void serialize(mc2::save::Archive& a) {
        a(id, teamId);
        for (auto& group : groups) a(group);
    }
};
using CommanderData = _CommanderData;

struct _StaticCommanderData {
    long numCommanders;
    long homeCommanderId;
    void serialize(mc2::save::Archive& a) { a(numCommanders, homeCommanderId); }
};
using StaticCommanderData = _StaticCommanderData;

namespace mc2::save {
inline constexpr auto commanderStaticTag = fourcc('C', 'M', 'S', 'T');
inline constexpr auto commanderDataTag = fourcc('C', 'M', 'D', 'R');

template<class T> Bytes commanderRecord(T value, std::uint32_t tag) {
    Archive a;
    std::uint32_t version = 1;
    a(tag, version, value);
    return a.good() ? a.bytes() : Bytes{};
}

template<class T> bool decodeCommanderRecord(std::span<const std::uint8_t> bytes,
                                             T& value, std::uint32_t expected) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    T staged{};
    a(tag, version, staged);
    if (!a.finish() || tag != expected || version != 1) return false;
    value = std::move(staged);
    return true;
}
}
