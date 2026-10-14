// Save schema codec; design: .claude/cleanup/cxx23-step4-save-schema-design.md.
// Not wired yet; explicit fields keep saves independent of runtime ownership and layout.
#include "save_archive.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mc2::save {
namespace {
void defaultDiagnostic(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[i++]);
        if (lead < 0x80) continue;
        std::uint32_t scalar = 0, minimum = 0;
        std::size_t count = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { scalar = lead & 0x1f; count = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { scalar = lead & 0x0f; count = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { scalar = lead & 7; count = 3; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const auto next = static_cast<std::uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return true;
}

bool validateHeader(Archive& a, const Header& h) {
    if (h.magic != Header{}.magic) a.reject("magic");
    else if (h.schemaVersion != 1) a.reject("schema_version");
    else if (h.releaseLine.bytes.empty() || h.releaseLine.bytes.size() > 128) a.reject("release_line");
    else if (h.originBuildSha.bytes.size() != 40 ||
             !std::all_of(h.originBuildSha.bytes.begin(), h.originBuildSha.bytes.end(),
                          [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) a.reject("build_sha");
    else if (h.saveKind != 1 && h.saveKind != 2) a.reject("save_kind");
    else if (h.sectionCount > 15 || h.payloadBytes > maxFileBytes) a.reject("payload_bounds");
    return a.good();
}

bool validateSections(Archive& a, const Document& document) {
    std::uint32_t previous = 0, present = 0;
    for (const auto& section : document.sections) {
        if (section.tag <= previous || section.tag > 15) { a.reject("section_tag"); break; }
        if (section.required != (section.tag != 12)) { a.reject("section_required"); break; }
        present |= 1u << section.tag;
        previous = section.tag;
    }
    const std::uint32_t required = document.header.saveKind == 1 ? 0xeffe : ((1u << 2) | (1u << 11));
    if ((present & required) != required) a.reject("missing_section");
    return a.good();
}
} // namespace

Archive::Archive(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}
Archive::Archive(std::span<const std::uint8_t> input, Diagnostic diagnostic)
    : reading_(true), input_(input), diagnostic_(std::move(diagnostic)) {
    if (input.size() > maxFileBytes) reject("file_bounds");
}

void Archive::reject(std::string_view reason) {
    if (!good()) return;
    error_ = reason;
    const std::string line = "[SAVE v1] reject reason=" + error_;
    if (diagnostic_) diagnostic_(line); else defaultDiagnostic(line);
}

bool Archive::finish() {
    if (reading_ && good() && remaining() != 0) reject("trailing_bytes");
    return good();
}

void Archive::raw(std::span<std::uint8_t> value) {
    if (!good()) return;
    if (reading_) {
        if (value.size() > remaining()) { reject("truncated"); return; }
        if (!value.empty()) std::memcpy(value.data(), input_.data() + cursor_, value.size());
        cursor_ += value.size();
    } else {
        if (value.size() > maxFileBytes - output_.size()) { reject("file_bounds"); return; }
        output_.insert(output_.end(), value.begin(), value.end());
    }
}

bool Archive::allocate(std::size_t count, std::size_t width) {
    if (!good()) return false;
    if (width != 0 && count > allocationBudget_ / width) { reject("decoded_budget"); return false; }
    allocationBudget_ -= count * width;
    return true;
}

void Text::serialize(Archive& a) {
    a(encoding);
    if (!a.good()) return;
    if (encoding != 1 && encoding != 2) { a.reject("text_encoding"); return; }
    const auto cap = std::min(limit, maxTextBytes);
    if (!a.reading() && bytes.size() > cap) { a.reject("text_length"); return; }
    std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
    a(length);
    if (!a.good()) return;
    if (length > cap) { a.reject("text_length"); return; }
    if (a.reading()) {
        if (length > a.remaining()) { a.reject("truncated_text"); return; }
        if (!a.allocate(length, 1)) return;
        bytes.resize(length);
    }
    a.raw({reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()});
    if (a.good() && encoding == 1 && !validUtf8(bytes)) a.reject("utf8");
}

void Part::validate(Archive& a) const {
    // Control tags 0..3 are the schema's frozen base/player/AI/net and base/mech/vehicle/elemental values.
    if (controlType > 3 || controlDataType > 3) { a.reject("part_control_tag"); return; }
    const auto& key = profileName.bytes;
    if (key.find(':') != std::string::npos || key.find('\0') != std::string::npos ||
        key.starts_with('/') || key.starts_with('\\')) { a.reject("asset_key"); return; }
    std::size_t begin = 0;
    while (begin < key.size()) {
        auto end = key.find_first_of("/\\", begin);
        if (end == std::string::npos) end = key.size();
        if (key.substr(begin, end - begin) == "..") { a.reject("asset_key"); return; }
        begin = end + 1;
    }
}

Digest sha256(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t, 8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const std::size_t blocks = bytes.size() / 64 + (bytes.size() % 64 < 56 ? 1 : 2);
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 64; ++i) {
            const std::size_t offset = block * 64 + i;
            std::uint8_t byte = 0;
            if (offset < bytes.size()) byte = bytes[offset];
            else if (offset == bytes.size()) byte = 0x80;
            if (offset >= blocks * 64 - 8) byte = static_cast<std::uint8_t>(bits >> ((blocks * 64 - 1 - offset) * 8));
            words[i / 4] |= static_cast<std::uint32_t>(byte) << (24 - (i % 4) * 8);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            auto x = words[i - 15], y = words[i - 2];
            words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) +
                       words[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
        }
        auto [a,b,c,d,e,f,g,h] = state;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto t1 = h + (std::rotr(e,6) ^ std::rotr(e,11) ^ std::rotr(e,25)) +
                            ((e & f) ^ (~e & g)) + constants[i] + words[i];
            const auto t2 = (std::rotr(a,2) ^ std::rotr(a,13) ^ std::rotr(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        const std::array<std::uint32_t, 8> result{a,b,c,d,e,f,g,h};
        for (std::size_t i = 0; i < 8; ++i) state[i] += result[i];
    }
    Digest result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(state[i / 4] >> (24 - (i % 4) * 8));
    return result;
}

bool encode(Document document, Bytes& output, Diagnostic diagnostic) {
    Archive a(std::move(diagnostic));
    try {
        document.header.sectionCount = static_cast<std::uint32_t>(document.sections.size());
        if (!validateHeader(a, document.header) || !validateSections(a, document)) return false;
        Archive payload([&a](std::string_view) { a.reject("section_payload"); });
        for (auto& section : document.sections) {
            SectionHeader h{section.tag, section.required, section.payload.size(), sha256(section.payload)};
            payload(h);
            payload.raw(section.payload);
        }
        if (!a.good()) return false;
        document.header.payloadBytes = payload.bytes().size();
        document.header.payloadSha256 = sha256(payload.bytes());
        a(document.header);
        Bytes wirePayload = payload.bytes();
        a.raw(wirePayload);
        if (!a.finish()) return false;
        output = a.bytes();
        return true;
    } catch (const std::bad_alloc&) { a.reject("allocation"); }
      catch (const std::length_error&) { a.reject("length"); }
    return false;
}

bool decode(std::span<const std::uint8_t> bytes, const Compatibility& compatibility,
            Document& output, Diagnostic diagnostic) {
    Archive a(bytes, std::move(diagnostic));
    try {
        Document candidate;
        a(candidate.header);
        if (!a.good() || !validateHeader(a, candidate.header)) return false;
        if (candidate.header.releaseLine.bytes != compatibility.releaseLine) { a.reject("release_line"); return false; }
        if (candidate.header.dataFingerprint != compatibility.dataFingerprint ||
            candidate.header.simRulesFingerprint != compatibility.simRulesFingerprint) {
            a.reject("fingerprint"); return false;
        }
        const auto payload = bytes.last(a.remaining());
        if (candidate.header.payloadBytes != payload.size()) { a.reject("payload_length"); return false; }
        if (candidate.header.payloadSha256 != sha256(payload)) { a.reject("payload_hash"); return false; }
        for (std::uint32_t i = 0; i < candidate.header.sectionCount && a.good(); ++i) {
            SectionHeader h;
            a(h);
            if (!a.good()) break;
            if (h.payloadBytes > a.remaining()) { a.reject("section_length"); break; }
            const auto sectionBytes = bytes.subspan(bytes.size() - a.remaining(), static_cast<std::size_t>(h.payloadBytes));
            if (h.payloadSha256 != sha256(sectionBytes)) { a.reject("section_hash"); break; }
            if (!a.allocate(static_cast<std::size_t>(h.payloadBytes), 1)) break;
            Section section{h.tag, h.required, Bytes(sectionBytes.begin(), sectionBytes.end())};
            a.raw(section.payload);
            candidate.sections.push_back(std::move(section));
        }
        if (!a.finish() || !validateSections(a, candidate)) return false;
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { a.reject("allocation"); }
      catch (const std::length_error&) { a.reject("length"); }
    return false;
}

bool writeFile(const std::filesystem::path& path, Document document, Diagnostic diagnostic) {
    Archive errors(diagnostic);
    Bytes bytes;
    if (!encode(std::move(document), bytes, diagnostic)) return false;
    std::filesystem::path temporary;
    int fd = -1;
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    // A crash leaves only its own staging file; exclusive creation isolates concurrent saves.
    for (int attempt = 0; attempt < 64; ++attempt) {
        temporary = path;
        temporary += ".pending." + std::to_string(process) + "." + std::to_string(stamp) + "." + std::to_string(attempt);
#ifdef _WIN32
        fd = _wopen(temporary.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { errors.reject("file_create"); return false; }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef _WIN32
        const auto written = _write(fd, bytes.data() + offset, static_cast<unsigned int>(bytes.size() - offset));
#else
        const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
#endif
        if (written <= 0) { errors.reject("file_write"); break; }
        offset += static_cast<std::size_t>(written);
    }
#ifdef _WIN32
    const int flushed = _commit(fd);
    const int closed = _close(fd);
#else
    const int flushed = fsync(fd);
    const int closed = close(fd);
#endif
    if (flushed != 0 || closed != 0) errors.reject("file_flush");
    if (errors.good()) {
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            errors.reject("file_replace");
#else
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec) errors.reject("file_replace");
#endif
    }
    if (!errors.good()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return errors.good();
}

bool readFile(const std::filesystem::path& path, const Compatibility& compatibility,
              Document& output, Diagnostic diagnostic) {
    Archive errors(diagnostic);
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate); // path-audit: loose-only; user save files are outside the game-data VFS.
        if (!file) { errors.reject("file_open"); return false; }
        const auto length = file.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > maxFileBytes) {
            errors.reject("file_bounds"); return false;
        }
        Bytes bytes(static_cast<std::size_t>(length));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file || file.peek() != std::char_traits<char>::eof()) { errors.reject("file_read"); return false; }
        return decode(bytes, compatibility, output, diagnostic);
    } catch (const std::bad_alloc&) { errors.reject("allocation"); }
      catch (const std::filesystem::filesystem_error&) { errors.reject("file_path"); }
      catch (const std::length_error&) { errors.reject("length"); }
    return false;
}
} // namespace mc2::save
