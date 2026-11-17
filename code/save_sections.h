// Save v2 section container: one packet after the legacy END packet holding versioned, tagged sections.
// Design: gamelogic-rewrite-design-2026-10-21 section 3.2 (B1). Adding state (B2 BRN1, B3 FIX1, M5 WRLD, PATH) is one
// tag here, one writer in Mission::save and one apply/default pair in Mission::loadApplySections.
#pragma once

#include "save_archive.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mc2::save {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24;
}

// Tagged value packets stay in their legacy positions; a required section opts
// the entire file into each new codec before any mission state is destroyed.
template<class T> Bytes encodeTaggedRecord(T value, std::uint32_t tag) {
    Archive a;
    std::uint32_t version = 1;
    a(tag, version, value);
    return a.good() ? a.bytes() : Bytes{};
}

template<class T> bool decodeTaggedRecord(std::span<const std::uint8_t> bytes,
                                         T& value, std::uint32_t expected) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0;
    T staged{};
    a(tag, version, staged);
    if (!a.finish() || tag != expected || version != 1) return false;
    value = std::move(staged);
    return true;
}

// Packet-0 stamp of a v2 save. v1 (MC2SAVE.WQT2 / build versionStamp) files still load; old builds refuse v2.
inline constexpr char saveStampV2[] = "MC2SAVE.V2";
inline constexpr std::array<std::uint8_t, 8> sectionMagic{'M', 'C', '2', 'S', 'E', 'C', 'T', 'S'};
inline constexpr std::uint32_t sectionContainerVersion = 2;
inline constexpr std::uint32_t maxSections = 256;
inline constexpr std::uint32_t sectionRequired = 1u;

// Frozen tags. The wire value is the four ASCII bytes in order.
inline constexpr std::uint32_t tagAnimation = fourcc('A', 'N', 'I', 'M');  // ObjectManager animation state (R2 layout v2-v5)
inline constexpr std::uint32_t tagOrders = fourcc('O', 'R', 'D', 'Q');     // TacticalOrderQueue
// Written: BRN1 (brain runtime), WRLD (mover pool/list order), FIX1 (script music, jump backoff, pending path requests; save_fix_section.h). Reserved: PATH.
inline constexpr std::uint32_t tagBrain = fourcc('B', 'R', 'N', '1');
inline constexpr std::uint32_t tagFix = fourcc('F', 'I', 'X', '1');
inline constexpr std::uint32_t tagWorld = fourcc('W', 'R', 'L', 'D');
inline constexpr std::uint32_t tagPath = fourcc('P', 'A', 'T', 'H');
inline constexpr std::uint32_t tagCommander = fourcc('C', 'M', 'D', 'R');
inline constexpr std::uint32_t tagTeam = fourcc('T', 'E', 'A', 'M');
inline constexpr std::uint32_t tagObjectManager = fourcc('O', 'B', 'M', 'H');
inline constexpr std::uint32_t tagWatchList = fourcc('W', 'A', 'T', 'C');
inline constexpr std::uint32_t tagMapCells = fourcc('M', 'C', 'E', 'L');
inline constexpr std::uint32_t tagMapAreas = fourcc('G', 'A', 'R', 'E');
inline constexpr std::uint32_t tagMapDoors = fourcc('G', 'D', 'O', 'R');
inline constexpr std::uint32_t tagDoorLinks = fourcc('G', 'L', 'N', 'K');
inline constexpr std::uint32_t tagWarriorStatic = fourcc('W', 'S', 'T', 'A');
inline constexpr std::uint32_t tagWarrior = fourcc('W', 'A', 'R', 'R');
inline constexpr std::uint32_t tagAblEnvironment = fourcc('A', 'B', 'L', 'E');
inline constexpr std::uint32_t tagObjectSlots = fourcc('O', 'B', 'J', 'S');
// OBJS v3 permits MECH v2 heat extensions; v2 added counted terrain coverage.
inline constexpr std::uint32_t objectSlotsVersion = 3;
// Empty required marker: ANIM carries weapon-node tables beyond the legacy cap.
// Older readers must refuse these saves before entering the legacy object loader.
inline constexpr std::uint32_t tagWeaponNodes = fourcc('W', 'N', 'O', 'D');

// Sections this build reads and the newest payload version of each. A newer version, or an unknown section flagged
// required, rejects the file before the running mission is torn down; an unknown optional section is skipped.
struct KnownSection { std::uint32_t tag; std::uint32_t maxVersion; };
inline constexpr std::array<KnownSection, 18> knownSections{{{tagAnimation, 5}, {tagOrders, 2}, {tagBrain, 2}, {tagWorld, 2}, {tagCommander, 1}, {tagTeam, 1}, {tagObjectManager, 1}, {tagWatchList, 1}, {tagMapCells, 1}, {tagMapAreas, 1}, {tagMapDoors, 1}, {tagDoorLinks, 1}, {tagWarriorStatic, 1}, {tagWarrior, 1}, {tagAblEnvironment, 1}, {tagObjectSlots, objectSlotsVersion}, {tagWeaponNodes, 1}, {tagFix, 1}}};

// Handles retain the compacted pool prefixes, free-slot order and mover-list order.
// Object records themselves remain in stable handle order for legacy readers.
struct SavedMoverLists {
    std::vector<std::uint32_t> mechs, vehicles, movers, friendly, hostile;
    std::int32_t turn = 0;
    bool sameMoverMembership(const SavedMoverLists& rebuilt) const;
    void serialize(Archive& archive, std::uint32_t version = 2) {
        archive.sequence(mechs, maxSequenceLength);
        archive.sequence(vehicles, maxSequenceLength);
        archive.sequence(movers, 1024);
        archive.sequence(friendly, 1024);
        archive.sequence(hostile, 1024);
        if (version >= 2) archive(turn);
    }
    bool operator==(const SavedMoverLists&) const = default;
};

struct SavedSection {
    std::uint32_t tag = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    Bytes payload;
    bool operator==(const SavedSection&) const = default;
};

struct SectionContainer {
    std::vector<SavedSection> sections;  // strictly ascending tag order on the wire
    const SavedSection* find(std::uint32_t tag) const;
    void add(std::uint32_t tag, std::uint32_t version, bool required, Bytes payload);
    bool operator==(const SectionContainer&) const = default;
};

// Wire: magic[8], containerVersion u32, reserved u32[2] (0), count u32, then per section tag u32, version u32,
// flags u32, reserved u32 (0), payloadBytes u32, payload. Sections are written sorted by tag, so equal state gives equal bytes.
bool encodeSections(const SectionContainer& container, Bytes& output, std::string* why = nullptr);
// Rejects bad magic/version, truncation, trailing bytes, unsorted or duplicate tags, a known tag newer than this build,
// and unknown required tags. Unknown optional sections are dropped from `output`.
bool decodeSections(std::span<const std::uint8_t> bytes, SectionContainer& output, std::string* why = nullptr);

// v1 -> v2: the legacy R2 packet that followed END (tag 'R2AN', version 2/3, animation blob, order queue) becomes
// ANIM + ORDQ sections. An R2 version above 3 is a soft reject (returns false, the caller refuses the file); it used to be fatal.
// Re-saving drops unknown optional sections (decodeSections discards them), so a save round-tripped by an older build loses them.
// An empty span (a v1 file without the packet) gives an empty container: every section defaults.
bool upgradeLegacyR2(std::span<const std::uint8_t> r2, SectionContainer& output, std::string* why = nullptr);
inline constexpr std::uint32_t legacyR2Tag = 0x4E413252u;

// Parts block format. A save writes PartsFormat after NumParts so the loader never has to guess. Saves without the key are
// either older (blank Part1, every later part shifted by one) or written between the Parts 1..N fix and this key; for those the
// content heuristic stays: a first part with no WID key, or WID 0 and ObjectNumber 0, is the legacy blank slot.
inline constexpr unsigned long kPartsFormatCurrent = 2;

constexpr bool partsAreShiftedLegacy(bool haveFormatTag, bool haveFirstWid, int firstWid, unsigned long firstObjectNumber) noexcept
{
    if (haveFormatTag)
        return false;
    return !haveFirstWid || (firstWid == 0 && firstObjectNumber == 0);
}

} // namespace mc2::save
