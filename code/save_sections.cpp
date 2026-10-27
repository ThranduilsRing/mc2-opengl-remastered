#include "save_sections.h"

#include <algorithm>

namespace mc2::save {
namespace {

bool fail(std::string* why, std::string_view reason) {
    if (why) *why = reason;
    return false;
}

// A length-prefixed byte payload; the generic sequence cap (65536) is too small for animation state.
void blob(Archive& a, Bytes& bytes) {
    if (!a.reading() && bytes.size() > maxFileBytes) { a.reject("section_bytes"); return; }
    auto length = static_cast<std::uint32_t>(bytes.size());
    a(length);
    if (!a.good()) return;
    if (length > maxFileBytes) { a.reject("section_bytes"); return; }
    if (a.reading()) {
        if (length > a.remaining()) { a.reject("section_truncated"); return; }
        if (!a.allocate(length, 1)) return;
        bytes.resize(length);
    }
    a.raw(bytes);
}

const KnownSection* known(std::uint32_t tag) {
    for (const auto& k : knownSections)
        if (k.tag == tag) return &k;
    return nullptr;
}

} // namespace

const SavedSection* SectionContainer::find(std::uint32_t tag) const {
    for (const auto& s : sections)
        if (s.tag == tag) return &s;
    return nullptr;
}

void SectionContainer::add(std::uint32_t tag, std::uint32_t version, bool required, Bytes payload) {
    sections.push_back({tag, version, required ? sectionRequired : 0u, std::move(payload)});
}

bool encodeSections(const SectionContainer& container, Bytes& output, std::string* why) {
    std::vector<const SavedSection*> order;
    for (const auto& s : container.sections) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](auto* l, auto* r) { return l->tag < r->tag; });
    for (std::size_t i = 1; i < order.size(); ++i)
        if (order[i]->tag == order[i - 1]->tag) return fail(why, "duplicate_section");
    if (order.size() > maxSections) return fail(why, "section_count");
    Archive a;
    auto magic = sectionMagic;
    std::uint32_t version = sectionContainerVersion, reserved0 = 0, reserved1 = 0;
    auto count = static_cast<std::uint32_t>(order.size());
    a(magic, version, reserved0, reserved1, count);
    for (const auto* s : order) {
        std::uint32_t tag = s->tag, sv = s->version, flags = s->flags, reserved = 0;
        a(tag, sv, flags, reserved);
        Bytes payload = s->payload;
        blob(a, payload);
    }
    if (!a.good()) return fail(why, a.error());
    output = a.bytes();
    return true;
}

bool decodeSections(std::span<const std::uint8_t> bytes, SectionContainer& output, std::string* why) {
    Archive a(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t version = 0, reserved0 = 0, reserved1 = 0, count = 0;
    a(magic, version, reserved0, reserved1, count);
    if (!a.good()) return fail(why, a.error());
    if (magic != sectionMagic) return fail(why, "section_magic");
    if (version != sectionContainerVersion) return fail(why, "section_container_version");
    if (count > maxSections) return fail(why, "section_count");
    SectionContainer result;
    std::uint32_t previous = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        SavedSection s;
        std::uint32_t reserved = 0;
        a(s.tag, s.version, s.flags, reserved);
        blob(a, s.payload);
        if (!a.good()) return fail(why, a.error());
        if (i > 0 && s.tag <= previous) return fail(why, "section_order");
        previous = s.tag;
        if (const auto* k = known(s.tag)) {
            if (s.version == 0 || s.version > k->maxVersion) return fail(why, "section_version");
            result.sections.push_back(std::move(s));
        } else if (s.flags & sectionRequired) {
            return fail(why, "unknown_required_section");
        }
    }
    if (!a.finish()) return fail(why, a.error());
    output = std::move(result);
    return true;
}

bool upgradeLegacyR2(std::span<const std::uint8_t> r2, SectionContainer& output, std::string* why) {
    SectionContainer result;
    if (!r2.empty()) {
        Archive a(r2);
        std::uint32_t tag = 0, version = 0;
        a(tag, version);
        if (!a.good()) return fail(why, a.error());
        if (tag != legacyR2Tag) return fail(why, "r2_tag");
        if (version != 2 && version != 3) return fail(why, "r2_version");
        Bytes animation;
        blob(a, animation);
        if (!a.good()) return fail(why, a.error());
        const auto rest = r2.subspan(r2.size() - a.remaining());
        result.add(tagAnimation, version, true, std::move(animation));
        result.add(tagOrders, 1, true, Bytes(rest.begin(), rest.end()));
    }
    output = std::move(result);
    return true;
}

} // namespace mc2::save
