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
        uint64_t offset = 0, packed = 0, size = 0; // offset = local header; validated lazily on first read
        uint64_t limit = 0; // start of the next local header (or central directory): data must end before it
        uint16_t flags = 0, nameLen = 0;
        mutable std::atomic<uint64_t> dataOff{0}; // validated data offset cache (0 = not yet validated)
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
    uint64_t dataOffset(const Entry& e) const; // throws on a bad local header
};
} // namespace mc2::vfs
