// Immutable mapped ZIP container. Decoder state belongs to each read, not the archive.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::vfs {
// File/openRead-path costs; legacy direct FST probes are not included.
struct PakIo {
    std::atomic<uint64_t> pakOpens{0}, bytesIn{0}, bytesOut{0}, decodeNs{0}, crcNs{0};
    std::atomic<uint64_t> fstOpens{0}, fstIn{0}, fstOut{0}, fstNs{0};
};
extern PakIo pakIo;
inline uint64_t ioNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void resetPakIo();
void reportPakIo();
class PakZip {
public:
    struct Entry {
        std::string key, source;
        std::string aliasOf;  // non-empty: a mc2pak.aliases name sharing that entry's stored bytes (no second local header)
        uint64_t offset = 0, packed = 0, size = 0;
        uint32_t crc = 0;
        uint16_t method = 0;
        mutable std::atomic<bool> failed{false};
    };
    struct Source { std::string name, tocHash; uint64_t size = 0, entries = 0; };
    explicit PakZip(std::string_view path);
    ~PakZip();
    PakZip(const PakZip&) = delete;
    PakZip& operator=(const PakZip&) = delete;
    bool valid() const { return valid_; }
    const Entry* find(std::string_view key) const;
    std::optional<std::vector<uint8_t>> read(const Entry& entry) const;
    const std::map<std::string, Entry>& entries() const { return entries_; }
    const std::vector<Source>& sources() const { return sources_; }
private:
    struct Mapping;
    std::unique_ptr<Mapping> mapping_;
    std::map<std::string, Entry> entries_;
    std::vector<Source> sources_;
    std::string path_;
    bool valid_ = false;
    void parse();
};

// Writer (mclib/pak_zip_writer.cpp, compiled only into targets that link mc2_zstd_compress: asset viewer, unit tests).
struct PakWriteOptions { int level = 19; bool forceZip64 = false; bool dryRun = false; };
struct PakFolderStat { uint64_t files = 0, aliases = 0, rawBytes = 0, uniqueBytes = 0, packedBytes = 0; };
struct PakWriteStats {
    PakFolderStat total;
    std::map<std::string, PakFolderStat> folders;  // "data/<x>" for data/*, else the first path component
    uint64_t archiveBytes = 0;
};
// Streams one deterministic archive: mc2pak.json first, entries in add() order, mc2pak.aliases last (only when a
// payload repeats). Identical payloads (SHA-256) are stored once; later names become alias-table lines.
class PakWriter {
public:
    PakWriter(std::string outPath, std::vector<PakZip::Source> sources, PakWriteOptions options = {});
    ~PakWriter();
    PakWriter(const PakWriter&) = delete;
    PakWriter& operator=(const PakWriter&) = delete;
    bool add(std::string_view name, std::string_view source, const std::vector<uint8_t>& bytes, std::string* error);
    bool finish(std::string* error);  // writes the central directory and renames the temp file into place
    const PakWriteStats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Packs every file under dir (logical key = lowercase relative path with '/'), one source named "loose".
bool packDirectory(const std::string& dir, const std::string& outPath, const PakWriteOptions& options,
                   PakWriteStats* stats, std::string* error);
} // namespace mc2::vfs
