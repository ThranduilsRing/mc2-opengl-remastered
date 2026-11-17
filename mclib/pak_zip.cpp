#include "pak_zip.h"
#include "vfs.h"
#include "mc2fmt.h"
#include "../3rdparty/basisu/zstd/zstd.h"
#include <zlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mc2::vfs {
PakIo pakIo;
void resetPakIo() {
    pakIo.pakOpens = 0; pakIo.bytesIn = 0; pakIo.bytesOut = 0; pakIo.decodeNs = 0; pakIo.crcNs = 0;
    pakIo.fstOpens = 0; pakIo.fstIn = 0; pakIo.fstOut = 0; pakIo.fstNs = 0;
}
void reportPakIo() {
    mc2::fmt::print_out("[PAK_IO v1] pak_opens={} pak_bytes_in={} pak_bytes_out={} pak_decode_ms={:.3f} pak_crc_ms={:.3f} fst_opens={} fst_bytes_in={} fst_bytes_out={} fst_read_ms={:.3f}\n",
        static_cast<unsigned long long>(pakIo.pakOpens.load()), static_cast<unsigned long long>(pakIo.bytesIn.load()),
        static_cast<unsigned long long>(pakIo.bytesOut.load()), pakIo.decodeNs.load()/1e6, pakIo.crcNs.load()/1e6,
        static_cast<unsigned long long>(pakIo.fstOpens.load()), static_cast<unsigned long long>(pakIo.fstIn.load()),
        static_cast<unsigned long long>(pakIo.fstOut.load()), pakIo.fstNs.load()/1e6);
}
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("invalid ZIP structure or manifest"); }
// All integer loads and slices are checked before pointer arithmetic.
struct View {
    const uint8_t* p;
    uint64_t n;
    void bounds(uint64_t at, uint64_t len) const { require(at <= n && len <= n - at); }
    uint64_t get(uint64_t at, unsigned width) const {
        bounds(at, width);
        uint64_t v = 0;
        for (unsigned i = 0; i < width; ++i) v |= static_cast<uint64_t>(p[at + i]) << (8 * i);
        return v;
    }
    std::string str(uint64_t at, uint64_t len) const {
        bounds(at, len);
        return std::string(reinterpret_cast<const char*>(p + at), static_cast<size_t>(len));
    }
};
// ZIP64 fields occur only for their corresponding sentinel, in the prescribed order.
void zip64(View extra, uint64_t& size, uint64_t& packed, uint64_t& offset, uint64_t& disk) {
    bool found = false;
    for (uint64_t at = 0; at < extra.n;) {
        auto id = extra.get(at, 2), len = extra.get(at + 2, 2);
        at += 4; extra.bounds(at, len);
        if (id == 1) {
            require(!found); found = true;
            View z{extra.p + at, len}; uint64_t pos = 0;
            auto field = [&](uint64_t& value, uint64_t sentinel, unsigned width) {
                if (value == sentinel) { value = z.get(pos, width); pos += width; }
            };
            field(size, UINT32_MAX, 8); field(packed, UINT32_MAX, 8);
            field(offset, UINT32_MAX, 8); field(disk, UINT16_MAX, 4);
        }
        at += len;
    }
    require(size != UINT32_MAX && packed != UINT32_MAX && offset != UINT32_MAX && disk != UINT16_MAX);
}
}
struct PakZip::Mapping {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    explicit Mapping(std::string_view path) {
        const std::string owned(path);
        file = CreateFileA(owned.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER length{};
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) && length.QuadPart > 0) {
            size = static_cast<uint64_t>(length.QuadPart);
            map = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (map) data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        }
    }
    ~Mapping() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
#else
    int fd = -1;
    explicit Mapping(std::string_view path) {
        const std::string owned(path);
        fd = ::open(owned.c_str(), O_RDONLY);
        struct stat st{};
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
            size = static_cast<uint64_t>(st.st_size);
            void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) data = static_cast<const uint8_t*>(p);
        }
    }
    ~Mapping() { if (data) munmap(const_cast<uint8_t*>(data), size); if (fd >= 0) ::close(fd); }
#endif
};
PakZip::PakZip(std::string_view path) : mapping_(std::make_unique<Mapping>(path)), path_(path) {
    try { parse(); valid_ = true; }
    catch (const std::exception& e) {
        entries_.clear(); sources_.clear();
        mc2::fmt::print_out("[PAK_REJECT] file={} reason={}\n", path, e.what());
    }
}
PakZip::~PakZip() = default;
void PakZip::parse() {
    require(mapping_->data && mapping_->size >= 22);
    View v{mapping_->data, mapping_->size};
    uint64_t end = v.n - 22, floor = v.n > 65557 ? v.n - 65557 : 0;
    for (;;) {
        if (v.get(end, 4) == 0x06054b50 && v.get(end + 20, 2) == v.n - end - 22) break;
        require(end > floor); --end;
    }
    require(v.get(end + 4, 2) == 0 && v.get(end + 6, 2) == 0);
    uint64_t count = v.get(end + 10, 2), bytes = v.get(end + 12, 4), cd = v.get(end + 16, 4);
    require(v.get(end + 8, 2) == count);
    uint64_t tail = end;
    if (count == UINT16_MAX || bytes == UINT32_MAX || cd == UINT32_MAX) {
        require(end >= 20 && v.get(end - 20, 4) == 0x07064b50);
        require(v.get(end - 16, 4) == 0 && v.get(end - 4, 4) == 1);
        auto z = v.get(end - 12, 8); v.bounds(z, 56);
        require(v.get(z, 4) == 0x06064b50 && v.get(z + 4, 8) >= 44);
        require(z <= end - 20 && end - 20 - z >= 56 && v.get(z + 4, 8) == end - 20 - z - 12);
        require(v.get(z + 16, 4) == 0 && v.get(z + 20, 4) == 0);
        count = v.get(z + 32, 8); require(count == v.get(z + 24, 8));
        bytes = v.get(z + 40, 8); cd = v.get(z + 48, 8); tail = z;
    }
    require(cd <= tail && bytes == tail - cd && count <= bytes / 46);
    std::set<std::string> names;
    uint64_t at = cd;
    std::map<uint64_t, size_t> skipped;
    std::map<uint64_t, uint64_t> ranges;
    for (uint64_t i = 0; i < count; ++i) {
        v.bounds(at, 46); require(v.get(at, 4) == 0x02014b50);
        auto flags = v.get(at + 8, 2), method = v.get(at + 10, 2);
        require((flags & ~uint64_t{0x806}) == 0); // no encryption, descriptors or patched data
        auto crc = v.get(at + 16, 4), packed = v.get(at + 20, 4), size = v.get(at + 24, 4);
        auto nl = v.get(at + 28, 2), xl = v.get(at + 30, 2), cl = v.get(at + 32, 2);
        auto disk = v.get(at + 34, 2), local = v.get(at + 42, 4);
        v.bounds(at + 46, nl + xl + cl); require(at + 46 + nl + xl + cl <= tail);
        auto name = v.str(at + 46, nl), k = key(name);
        require(!k.empty() && name.find('\0') == std::string::npos && names.insert(k).second);
        zip64({v.p + at + 46 + nl, xl}, size, packed, local, disk);
        require(disk == 0 && size <= kMaxInRamImage && packed <= kMaxInRamImage);
        v.bounds(local, 30); require(local < cd && v.get(local, 4) == 0x04034b50);
        require(v.get(local + 6, 2) == flags && v.get(local + 8, 2) == method);
        require(v.get(local + 14, 4) == crc && v.get(local + 26, 2) == nl);
        auto lx = v.get(local + 28, 2);
        v.bounds(local + 30, nl + lx); require(v.str(local + 30, nl) == name);
        auto lp = v.get(local + 18, 4), ls = v.get(local + 22, 4);
        uint64_t unusedOffset = 0, unusedDisk = 0;
        zip64({v.p + local + 30 + nl, lx}, ls, lp, unusedOffset, unusedDisk);
        require(lp == packed && ls == size);
        auto data = local + 30 + nl + lx;
        require(data <= cd && packed <= cd - data);
        auto next = ranges.lower_bound(local);
        if (next != ranges.end()) { const auto& [start, stop] = *next; require(data + packed <= start); }
        if (next != ranges.begin()) { const auto& [start, stop] = *std::prev(next); require(stop <= local); }
        const auto [rangeIt, inserted] = ranges.emplace(local, data + packed); require(inserted);
        if (method == 0 || method == 8 || method == 93) {
            const auto [it, added] = entries_.try_emplace(k);
            auto& [entryKey, e] = *it;
            e.key = k; e.source = v.str(at + 46 + nl + xl, cl);
            e.offset = data; e.packed = packed; e.size = size;
            e.crc = static_cast<uint32_t>(crc); e.method = static_cast<uint16_t>(method);
        } else ++skipped[method];
        if (i == 0) require(name == "mc2pak.json" && method == 0 && local == 0);
        at += 46 + nl + xl + cl;
    }
    require(at == tail);
    for (const auto& [method, n] : skipped)
        mc2::fmt::print_out("[PAK] skipped entries={} method={}\n", n, method);
    auto manifest = find("mc2pak.json"); require(manifest != nullptr);
    auto raw = read(*manifest); require(raw.has_value());
    auto j = nlohmann::json::parse(raw->begin(), raw->end());
    require(j.at("format") == 1 && j.at("sources").is_array());
    std::set<std::string> sourceNames;
    for (const auto& s : j.at("sources")) {
        Source src{s.at("name").get<std::string>(), s.at("toc_hash").get<std::string>(),
                   s.at("size").get<uint64_t>(), s.at("entries").get<uint64_t>()};
        require(src.name == key(src.name) && src.name.find('/') == std::string::npos &&
                src.tocHash.size() == 16 && src.tocHash.find_first_not_of("0123456789abcdef") == std::string::npos &&
                sourceNames.insert(src.name).second);
        sources_.push_back(std::move(src));
    }
    for (const auto& [k, e] : entries_) require(k == "mc2pak.json" || sourceNames.count(e.source) == 1);
}
const PakZip::Entry* PakZip::find(std::string_view k) const {
    auto it = entries_.find(std::string(k));
    if (it == entries_.end()) return nullptr;
    const auto& [entryKey, e] = *it;
    return e.failed.load() ? nullptr : &e;
}
std::optional<std::vector<uint8_t>> PakZip::read(const Entry& e) const {
    if (e.failed.load()) return std::nullopt;
    const auto begin = ioNow();
    ++pakIo.pakOpens; pakIo.bytesIn += e.packed;
    try {
        require(e.size <= kMaxInRamImage && e.packed <= kMaxInRamImage);
        std::vector<uint8_t> out(static_cast<size_t>(std::max<uint64_t>(e.size, 1)));
        const auto* input = mapping_->data + e.offset;
        if (e.method == 0) { require(e.size == e.packed); std::memcpy(out.data(), input, e.size); }
        else if (e.method == 8) {
            z_stream z{};
            z.next_in = const_cast<Bytef*>(input); z.avail_in = static_cast<uInt>(e.packed);
            z.next_out = out.data(); z.avail_out = static_cast<uInt>(out.size());
            require(inflateInit2(&z, -MAX_WBITS) == Z_OK);
            auto result = inflate(&z, Z_FINISH); inflateEnd(&z);
            require(result == Z_STREAM_END && z.total_out == e.size && z.total_in == e.packed);
        } else {
            auto size = ZSTD_getFrameContentSize(input, e.packed);
            require(size != ZSTD_CONTENTSIZE_ERROR && (size == ZSTD_CONTENTSIZE_UNKNOWN || size == e.size));
            auto result = ZSTD_decompress(out.data(), e.size, input, e.packed);
            require(!ZSTD_isError(result) && result == e.size);
        }
        const auto crcBegin = ioNow(); pakIo.decodeNs += crcBegin - begin;
        const auto crc = crc32(0, out.data(), static_cast<uInt>(e.size));
        pakIo.crcNs += ioNow() - crcBegin;
        require(crc == e.crc); pakIo.bytesOut += e.size;
        out.resize(static_cast<size_t>(e.size)); return out;
    } catch (const std::exception&) {
        if (!e.failed.exchange(true)) mc2::fmt::print_out("[PAK_CORRUPT] key={} file={}\n", e.key, path_);
        return std::nullopt;
    }
}
} // namespace mc2::vfs
