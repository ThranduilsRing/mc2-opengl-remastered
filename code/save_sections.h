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
    return std::uint32_t(std::uint8_t(a)) | std::uint32_t(std::uint8_t(b)) << 8 |
           std::uint32_t(std::uint8_t(c)) << 16 | std::uint32_t(std::uint8_t(d)) << 24;
}

// Packet-0 stamp of a v2 save. v1 (MC2SAVE.WQT2 / build versionStamp) files still load; old builds refuse v2.
inline constexpr char saveStampV2[] = "MC2SAVE.V2";
inline constexpr std::array<std::uint8_t, 8> sectionMagic{'M', 'C', '2', 'S', 'E', 'C', 'T', 'S'};
inline constexpr std::uint32_t sectionContainerVersion = 2;
inline constexpr std::uint32_t maxSections = 256;
inline constexpr std::uint32_t sectionRequired = 1u;

// Frozen tags. The wire value is the four ASCII bytes in order.
inline constexpr std::uint32_t tagAnimation = fourcc('A', 'N', 'I', 'M');  // ObjectManager animation state (R2 layout v2/v3)
inline constexpr std::uint32_t tagOrders = fourcc('O', 'R', 'D', 'Q');     // TacticalOrderQueue
// Written: BRN1 (B2 brain runtime). Reserved, not yet written: FIX1 (B3 D2 fields), WRLD (M5 entity table + caps), PATH (path queue).
inline constexpr std::uint32_t tagBrain = fourcc('B', 'R', 'N', '1');
inline constexpr std::uint32_t tagFix = fourcc('F', 'I', 'X', '1');
inline constexpr std::uint32_t tagWorld = fourcc('W', 'R', 'L', 'D');
inline constexpr std::uint32_t tagPath = fourcc('P', 'A', 'T', 'H');

// Sections this build reads and the newest payload version of each. A newer version, or an unknown section flagged
// required, rejects the file before the running mission is torn down; an unknown optional section is skipped.
struct KnownSection { std::uint32_t tag; std::uint32_t maxVersion; };
inline constexpr std::array<KnownSection, 3> knownSections{{{tagAnimation, 3}, {tagOrders, 1}, {tagBrain, 2}}};

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

} // namespace mc2::save
