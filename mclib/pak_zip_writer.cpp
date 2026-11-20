// Deterministic .mc2pak writer (ZIP container, Zstandard method 93). Compiled only where mc2_zstd_compress is linked.
#include "pak_zip.h"
#include "vfs.h"
#include "../3rdparty/zstd/zstd_prefix.h"
#include "../3rdparty/zstd/zstd.h"
#include <zlib.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>

namespace mc2::vfs {
namespace {
const char kComment[] = "MechCommander 2 asset pack (ZIP container, Zstandard entries). List/extract/pack with tools/mc2pak.py or the asset viewer.";

// Portable SHA-256 (content identity for dedupe).
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64], v[8];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[4*i]) << 24 | static_cast<uint32_t>(p[4*i+1]) << 16 | static_cast<uint32_t>(p[4*i+2]) << 8 | p[4*i+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3), s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            uint32_t t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h[i] += v[i];
    }
    static std::string hex(std::span<const uint8_t> in) {
        const uint8_t* data = in.data(); const size_t n = in.size();
        Sha256 s; size_t i = 0;
        for (; i + 64 <= n; i += 64) s.block(data + i);
        uint8_t tail[128] = {}; size_t rest = n - i; if (rest) std::memcpy(tail, data + i, rest);
        tail[rest] = 0x80; size_t total = rest + 1 + 8 <= 64 ? 64 : 128; const uint64_t bits = static_cast<uint64_t>(n) * 8;
        for (int b = 0; b < 8; ++b) tail[total - 1 - b] = static_cast<uint8_t>(bits >> (8 * b));
        for (size_t o = 0; o < total; o += 64) s.block(tail + o);
        char out[65]; for (int j = 0; j < 8; ++j) std::snprintf(out + 8 * j, 9, "%08x", s.h[j]);
        return std::string(out, 64);
    }
};

void le(std::vector<uint8_t>& b, uint64_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
bool ascii(std::string_view s) { return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; }); }
std::string folderOf(std::string_view name) {
    auto a = name.find('/');
    if (a == std::string_view::npos) return "(root)";
    if (name.substr(0, a) != "data") return std::string(name.substr(0, a));
    auto b = name.find('/', a + 1);
    return b == std::string_view::npos ? "data" : std::string(name.substr(0, b));
}
std::string jsonStr(std::string_view s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
struct Central { std::string name, source; uint16_t method; uint32_t crc; uint64_t packed, size, offset; };
}

struct PakWriter::Impl {
    std::string out, temp;
    std::vector<PakZip::Source> sources;
    PakWriteOptions opt;
    std::ofstream f;
    uint64_t pos = 0;
    std::vector<Central> central;
    std::map<std::string, std::string> byHash;       // sha256 -> stored name
    std::vector<std::string> aliasLines;
    std::set<std::string> names, sourceNames;
    PakWriteStats stats;
    ZSTD_CCtx* cctx = nullptr;
    bool failed = false, finished = false;

    void put(const void* p, size_t n) { if (n) f.write(static_cast<const char*>(p), std::streamsize(n)); pos += n; }
    void put(const std::vector<uint8_t>& v) { put(v.data(), v.size()); }
    // One local header + payload; `data` is the stored (possibly compressed) payload.
    void entry(std::string_view name, std::string_view source, uint16_t method, std::span<const uint8_t> payload,
               uint64_t size, uint32_t crc) {
        const uint64_t packed = payload.size();
        const bool z64s = opt.forceZip64 || size >= UINT32_MAX || packed >= UINT32_MAX;
        const uint16_t flags = ascii(name) && ascii(source) ? 0 : 0x800;
        std::vector<uint8_t> h;
        le(h, 0x04034b50, 4); le(h, method == 93 ? 63 : 20, 2); le(h, flags, 2); le(h, method, 2); le(h, 0, 2); le(h, 0x21, 2);
        le(h, crc, 4); le(h, z64s ? UINT32_MAX : packed, 4); le(h, z64s ? UINT32_MAX : size, 4);
        le(h, name.size(), 2); le(h, z64s ? 20 : 0, 2);
        h.insert(h.end(), name.begin(), name.end());
        if (z64s) { le(h, 1, 2); le(h, 16, 2); le(h, size, 8); le(h, packed, 8); }
        central.push_back({std::string(name), std::string(source), method, crc, packed, size, pos});
        put(h);
        put(payload.data(), payload.size());
    }
};

PakWriter::PakWriter(std::string outPath, std::vector<PakZip::Source> sources, PakWriteOptions options)
    : impl_(std::make_unique<Impl>()) {
    auto& i = *impl_;
    i.out = std::move(outPath); i.temp = i.out + ".tmp"; i.sources = std::move(sources); i.opt = options;
    for (const auto& s : i.sources) i.sourceNames.insert(s.name);
    if (i.opt.dryRun) return;
    std::error_code ec;
    if (std::filesystem::path(i.out).has_parent_path()) std::filesystem::create_directories(std::filesystem::path(i.out).parent_path(), ec);
    i.f.open(i.temp, std::ios::binary | std::ios::trunc);
    i.cctx = ZSTD_createCCtx();
    std::string json = "{\"format\":1,\"sources\":[";
    for (size_t k = 0; k < i.sources.size(); ++k) {
        const auto& s = i.sources[k];
        json += (k ? "," : "") + std::string("{\"name\":") + jsonStr(s.name) + ",\"size\":" + std::to_string(s.size) +
                ",\"toc_hash\":" + jsonStr(s.tocHash) + ",\"entries\":" + std::to_string(s.entries) + "}";
    }
    json += "]}";
    i.entry("mc2pak.json", "", 0, {reinterpret_cast<const uint8_t*>(json.data()), json.size()}, json.size(),
            crc32(0, reinterpret_cast<const Bytef*>(json.data()), static_cast<uInt>(json.size())));
}

PakWriter::~PakWriter() {
    if (!impl_) return;
    if (impl_->cctx) ZSTD_freeCCtx(impl_->cctx);
    if (impl_->f.is_open()) impl_->f.close();
    if (!impl_->finished && !impl_->opt.dryRun) { std::error_code ec; std::filesystem::remove(impl_->temp, ec); }
}
const PakWriteStats& PakWriter::stats() const { return impl_->stats; }

bool PakWriter::add(std::string_view name, std::string_view source, const std::vector<uint8_t>& bytes, std::string* error) {
    auto& i = *impl_;
    auto fail = [&](std::string m) { i.failed = true; if (error) *error = std::move(m); return false; };
    const std::string k(name);
    if (k.empty() || k != key(k) || k == "mc2pak.json" || k == "mc2pak.aliases" || k.find('\0') != std::string::npos)
        return fail("unusable entry name: " + k);
    if (!i.sourceNames.contains(std::string(source))) return fail("unknown source for " + k);
    if (const auto [pos, fresh] = i.names.insert(k); !fresh) return fail("duplicate entry name: " + k);
    if (bytes.size() > kMaxInRamImage) return fail("entry too large for the in-RAM reader: " + k);
    auto& total = i.stats.total; auto& folder = i.stats.folders[folderOf(k)];
    for (auto* s : {&total, &folder}) { ++s->files; s->rawBytes += bytes.size(); }
    const auto digest = Sha256::hex(bytes);
    if (auto it = i.byHash.find(digest); it != i.byHash.end()) {
        const auto& [hash, target] = *it;
        i.aliasLines.push_back(k + "\t" + target + "\t" + std::string(source) + "\n");
        for (auto* s : {&total, &folder}) ++s->aliases;
        return true;
    }
    i.byHash.emplace(digest, k);
    uint64_t packedSize = bytes.size();
    uint16_t method = 0;
    std::vector<uint8_t> packed;
    if (!i.opt.dryRun) {
        const bool ktx2 = k.size() >= 5 && k.compare(k.size() - 5, 5, ".ktx2") == 0;
        if (!ktx2 && !bytes.empty()) {
            packed.resize(ZSTD_compressBound(bytes.size()));
            if (!i.cctx || ZSTD_isError(ZSTD_CCtx_setParameter(i.cctx, ZSTD_c_compressionLevel, i.opt.level)) ||
                ZSTD_isError(ZSTD_CCtx_setParameter(i.cctx, ZSTD_c_windowLog, 27)))
                return fail("zstd context or parameter rejected: " + k);
            const auto n = ZSTD_compress2(i.cctx, packed.data(), packed.size(), bytes.data(), bytes.size());
            if (ZSTD_isError(n)) return fail("zstd compress failed: " + k);
            if (static_cast<double>(n) <= static_cast<double>(bytes.size()) * .95) { method = 93; packedSize = n; }
        }
        const std::span<const uint8_t> payload = method == 93 ? std::span<const uint8_t>(packed.data(), packedSize) : std::span<const uint8_t>(bytes);
        i.entry(k, source, method, payload, bytes.size(), crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
        if (!i.f) return fail("write failed: " + i.temp);
    }
    for (auto* s : {&total, &folder}) { s->uniqueBytes += bytes.size(); s->packedBytes += packedSize; }
    return true;
}

bool PakWriter::finish(std::string* error) {
    auto& i = *impl_;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return false; };
    if (i.failed) return fail("writer failed earlier");
    if (i.opt.dryRun) { i.finished = true; return true; }
    if (!i.aliasLines.empty()) {
        std::string text; for (const auto& l : i.aliasLines) text += l;
        i.entry("mc2pak.aliases", "", 0, {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, text.size(),
                crc32(0, reinterpret_cast<const Bytef*>(text.data()), static_cast<uInt>(text.size())));
    }
    const uint64_t cdStart = i.pos;
    std::vector<uint8_t> cd;
    for (const auto& c : i.central) {
        const bool z64s = i.opt.forceZip64 || c.size >= UINT32_MAX || c.packed >= UINT32_MAX;
        const bool z64o = i.opt.forceZip64 || c.offset >= UINT32_MAX;
        const uint16_t flags = ascii(c.name) && ascii(c.source) ? 0 : 0x800;
        const unsigned xl = 4 + 8 * ((z64s ? 2 : 0) + (z64o ? 1 : 0));
        le(cd, 0x02014b50, 4); le(cd, (3 << 8) | 63, 2); le(cd, c.method == 93 ? 63 : 20, 2); le(cd, flags, 2); le(cd, c.method, 2);
        le(cd, 0, 2); le(cd, 0x21, 2); le(cd, c.crc, 4); le(cd, z64s ? UINT32_MAX : c.packed, 4); le(cd, z64s ? UINT32_MAX : c.size, 4);
        le(cd, c.name.size(), 2); le(cd, z64s || z64o ? xl : 0, 2); le(cd, c.source.size(), 2); le(cd, 0, 2); le(cd, 0, 2);
        le(cd, static_cast<uint64_t>(0100644) << 16, 4); le(cd, z64o ? UINT32_MAX : c.offset, 4);
        cd.insert(cd.end(), c.name.begin(), c.name.end());
        if (z64s || z64o) {
            le(cd, 1, 2); le(cd, xl - 4, 2);
            if (z64s) { le(cd, c.size, 8); le(cd, c.packed, 8); }
            if (z64o) le(cd, c.offset, 8);
        }
        cd.insert(cd.end(), c.source.begin(), c.source.end());
    }
    i.put(cd);
    const uint64_t count = i.central.size();
    const bool big = i.opt.forceZip64 || count >= UINT16_MAX || cd.size() >= UINT32_MAX || cdStart >= UINT32_MAX;
    std::vector<uint8_t> end;
    if (big) {
        const uint64_t z = i.pos;
        le(end, 0x06064b50, 4); le(end, 44, 8); le(end, (3 << 8) | 63, 2); le(end, 63, 2); le(end, 0, 4); le(end, 0, 4);
        le(end, count, 8); le(end, count, 8); le(end, cd.size(), 8); le(end, cdStart, 8);
        le(end, 0x07064b50, 4); le(end, 0, 4); le(end, z, 8); le(end, 1, 4);
    }
    const size_t commentLen = sizeof(kComment) - 1;
    le(end, 0x06054b50, 4); le(end, 0, 2); le(end, 0, 2);
    le(end, big ? UINT16_MAX : count, 2); le(end, big ? UINT16_MAX : count, 2);
    le(end, big ? UINT32_MAX : cd.size(), 4); le(end, big ? UINT32_MAX : cdStart, 4); le(end, commentLen, 2);
    end.insert(end.end(), kComment, kComment + commentLen);
    i.put(end);
    i.f.close();
    if (!i.f && i.f.fail()) return fail("write failed: " + i.temp);
    std::error_code ec;
    std::filesystem::rename(i.temp, i.out, ec);
    if (ec) { std::filesystem::remove(i.out, ec); std::filesystem::rename(i.temp, i.out, ec); }
    if (ec) return fail("rename failed: " + ec.message());
    i.finished = true;
    i.stats.archiveBytes = i.pos;
    return true;
}

bool packDirectory(std::string_view dirView, std::string_view outView, const PakWriteOptions& options,
                   PakWriteStats* stats, std::string* error) {
    const std::string dir(dirView), outPath(outView);
    namespace fs = std::filesystem;
    auto fail = [&](std::string m) { if (error) *error = std::move(m); return false; };
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return fail("not a directory: " + dir);
    struct Item { std::string key; fs::path path; uint64_t size; };
    std::vector<Item> items;
    std::set<std::string> seen;
    const auto root = fs::absolute(dir);
    const auto outAbs = fs::weakly_canonical(fs::absolute(outPath), ec).generic_string();
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::error_code pe;
        if (const auto p = fs::weakly_canonical(it->path(), pe).generic_string(); p == outAbs || p == outAbs + ".tmp") continue;  // never pack our own output
        const auto rel = fs::relative(it->path(), root).generic_u8string();
        auto k = key(std::string(reinterpret_cast<const char*>(rel.data()), rel.size()));
        if (const auto [pos, fresh] = seen.insert(k); !fresh) return fail("names collide after lowercasing: " + k);
        items.push_back({k, it->path(), it->file_size(ec)});
    }
    if (ec) return fail("directory walk failed: " + ec.message());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
    std::string tocText; uint64_t totalSize = 0;
    for (const auto& item : items) { tocText += item.key + '\0' + std::to_string(item.size) + '\n'; totalSize += item.size; }
    PakZip::Source src{"loose", Sha256::hex({reinterpret_cast<const uint8_t*>(tocText.data()), tocText.size()}).substr(0, 16), totalSize, items.size()};
    PakWriter w(outPath, {src}, options);
    std::vector<uint8_t> bytes;
    for (const auto& item : items) {
        std::ifstream f(item.path, std::ios::binary); // path-audit: loose-only (pak builder reads the loose source tree it packs)
        bytes.assign(static_cast<size_t>(item.size), 0);
        if (!f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return fail("read failed: " + item.key);
        if (!w.add(item.key, "loose", bytes, error)) return false;
    }
    if (!w.finish(error)) return false;
    if (stats) *stats = w.stats();
    return true;
}
} // namespace mc2::vfs
