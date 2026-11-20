// Archive list/extract/pack/verify shared by the `--pak` verbs and the Archives panel.
#include "ArchiveIo.h"
#include "pak_zip.h"
#include "ffile.h"
#include "fastfile.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
bool reserved(const std::string& n) { return n == "mc2pak.json" || n == "mc2pak.aliases"; }

class PakSource final : public ArchiveSource {
public:
    mc2::vfs::PakZip pak;
    std::vector<const mc2::vfs::PakZip::Entry*> entries;
    explicit PakSource(const std::string& path) : pak(path) {
        if (!pak.valid()) return;
        for (const auto& [k, e] : pak.entries()) {
            if (reserved(k)) continue;
            const char* m = e.method == 0 ? "store" : e.method == 8 ? "deflate" : "zstd";
            rows_.push_back({k, e.source, e.aliasOf.empty() ? m : "alias", e.aliasOf, e.size, e.aliasOf.empty() ? e.packed : 0});
            entries.push_back(&e);
        }
    }
    bool read(size_t row, std::vector<uint8_t>& out) const override {
        if (row >= entries.size()) return false;
        auto bytes = pak.read(*entries[row]);
        if (!bytes) return false;
        out = std::move(*bytes);
        return true;
    }
};

class FstSource final : public ArchiveSource {
public:
    mutable FastFile ff;
    explicit FstSource(const std::string& path) {
        if (ff.open(path.c_str()) != 0) return;
        const std::string leaf = fs::path(path).filename().string();
        const FILE_HANDLE* files = ff.getFilesInfo();
        for (long i = 0; i < ff.getNumFiles(); ++i) {
            const FILEENTRY& e = *files[i].pfe;
            rows_.push_back({e.name, leaf, ff.isLZCompressed() ? "lz" : "zlib", "", e.realSize, e.size});
        }
    }
    bool read(size_t row, std::vector<uint8_t>& out) const override {
        if (row >= rows_.size()) return false;
        const FILEENTRY& e = *ff.getFilesInfo()[row].pfe;
        const long h = ff.openFast(e.hash, e.name);
        if (h < 0) return false;
        out.assign(std::max<size_t>(e.realSize, 1), 0);
        const long n = e.realSize ? ff.readFast(static_cast<DWORD>(h), out.data(), e.realSize) : 0;
        ff.closeFast(static_cast<DWORD>(h));
        out.resize(e.realSize);
        return n == static_cast<long>(e.realSize);
    }
};

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}

std::unique_ptr<ArchiveSource> ArchiveSource::open(const std::string& path, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { if (error) *error = "not a file: " + path; return nullptr; }
    const std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".fst") {
        auto s = std::make_unique<FstSource>(path);
        if (s->rows().empty() && s->ff.getNumFiles() == 0 && !s->ff.isOpen()) { if (error) *error = "cannot open FastFile: " + path; return nullptr; }
        return s;
    }
    auto s = std::make_unique<PakSource>(path);
    if (!s->pak.valid()) { if (error) *error = "not a valid mc2pak/zip archive: " + path; return nullptr; }
    return s;
}

bool archiveGlob(const char* p, const char* t) {
    if (!*p) return !*t;
    if (*p == '*') { for (;; ++t) { if (archiveGlob(p + 1, t)) return true; if (!*t) return false; } }
    return *t && (*p == '?' || *p == *t) && archiveGlob(p + 1, t + 1);
}

bool archiveEntrySafe(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return false;
    size_t at = 0;
    while (at <= name.size()) {
        auto end = name.find('/', at); if (end == std::string::npos) end = name.size();
        const auto part = name.substr(at, end - at);
        if (part == ".." || part == ".") return false;
        at = end + 1;
    }
    return true;
}

bool archiveOutputAllowed(const std::string& path) {
    std::error_code ec;
    auto p = lower(fs::weakly_canonical(path, ec).generic_string());
    const auto rel = fs::path(p).relative_path().generic_string();  // owner-protected install on any drive, never an output
    return !(rel == "games/mc2-opengl" || rel.rfind("games/mc2-opengl/", 0) == 0);
}

bool archiveExtract(const ArchiveSource& src, const std::vector<size_t>& rows, const std::string& outDir,
                    size_t* written, size_t* refused, std::string* error) {
    size_t w = 0, r = 0;
    bool ok = true;
    const fs::path root = fs::absolute(outDir).lexically_normal();
    std::vector<uint8_t> bytes;
    for (size_t i : rows) {
        const auto& name = src.rows()[i].name;
        const fs::path target = (root / name).lexically_normal();
        const auto rel = target.lexically_relative(root);
        if (!archiveEntrySafe(name) || rel.empty() || *rel.begin() == "..") {
            std::fprintf(stderr, "refused unsafe entry name: %s\n", name.c_str());
            ++r; continue;
        }
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!src.read(i, bytes)) { if (error) *error = "cannot read entry: " + name; ok = false; break; }
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) { if (error) *error = "cannot write: " + target.string(); ok = false; break; }
        ++w;
    }
    if (written) *written = w;
    if (refused) *refused = r;
    if (r && error && error->empty()) *error = "refused " + std::to_string(r) + " unsafe entry name(s)";
    return ok && r == 0;
}

int runPakVerb(int argc, char** argv) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(clock::now() - start).count(); };
    auto die = [](const std::string& m) { std::fprintf(stderr, "pak: %s\n", m.c_str()); return 1; };
    if (argc < 2) return die("usage: --pak list <pak|fst> | extract <pak|fst> <outdir> [--match glob] | pack <dir> <out.mc2pak> [--level N] [--dry-run] | verify <pak>");
    const std::string verb = argv[0];
    std::string err;
    if (verb == "pack") {
        if (argc < 3) return die("usage: --pak pack <dir> <out.mc2pak> [--level N] [--dry-run]");
        mc2::vfs::PakWriteOptions opt;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--dry-run")) opt.dryRun = true;
            else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) {
                char* e = nullptr; const long v = std::strtol(argv[++i], &e, 10);
                if (!e || *e || v < 1 || v > 22) return die(std::string("bad --level (1-22): ") + argv[i]);
                opt.level = static_cast<int>(v);
            }
            else return die(std::string("unknown option: ") + argv[i]);
        }
        if (!archiveOutputAllowed(argv[2])) return die("writes inside the protected install are forbidden");
        mc2::vfs::PakWriteStats st;
        if (!mc2::vfs::packDirectory(argv[1], argv[2], opt, &st, &err)) return die(err);
        auto line = [](const std::string& n, const mc2::vfs::PakFolderStat& s) {
            std::printf("folder %-28s files=%llu aliases=%llu raw=%llu unique=%llu stored=%llu dedupe=%.2f%%\n", n.c_str(),
                        (unsigned long long)s.files, (unsigned long long)s.aliases, (unsigned long long)s.rawBytes,
                        (unsigned long long)s.uniqueBytes, (unsigned long long)s.packedBytes,
                        s.rawBytes ? 100.0 * double(s.rawBytes - s.uniqueBytes) / double(s.rawBytes) : 0.0);
        };
        for (const auto& [n, s] : st.folders) line(n, s);
        line("TOTAL", st.total);
        std::printf("pak pack entries=%llu unique=%llu aliases=%llu raw_bytes=%llu archive_bytes=%llu%s seconds=%.3f\n",
                    (unsigned long long)st.total.files, (unsigned long long)(st.total.files - st.total.aliases),
                    (unsigned long long)st.total.aliases, (unsigned long long)st.total.rawBytes,
                    (unsigned long long)st.archiveBytes, opt.dryRun ? " (dry run: nothing written)" : "", seconds());
        return 0;
    }
    auto src = ArchiveSource::open(argv[1], &err);
    if (!src) return die(err);
    const auto& rows = src->rows();
    if (verb == "list") {
        size_t aliases = 0;
        for (const auto& r : rows) {
            std::printf("%s\t%llu\t%llu\t%s\t%s%s%s\n", r.name.c_str(), (unsigned long long)r.size, (unsigned long long)r.packed,
                        r.method.c_str(), r.source.c_str(), r.aliasOf.empty() ? "" : "\t-> ", r.aliasOf.c_str());
            aliases += !r.aliasOf.empty();
        }
        std::printf("pak list entries=%zu aliases=%zu seconds=%.3f\n", rows.size(), aliases, seconds());
        return 0;
    }
    if (verb == "extract") {
        if (argc < 3) return die("usage: --pak extract <pak|fst> <outdir> [--match glob]");
        std::string match = "*";
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--match") && i + 1 < argc) match = argv[++i];
            else return die(std::string("unknown option: ") + argv[i]);
        }
        if (!archiveOutputAllowed(argv[2])) return die("writes inside the protected install are forbidden");
        std::vector<size_t> pick;
        for (size_t i = 0; i < rows.size(); ++i) if (archiveGlob(match.c_str(), rows[i].name.c_str())) pick.push_back(i);
        size_t written = 0, refused = 0;
        const bool ok = archiveExtract(*src, pick, argv[2], &written, &refused, &err);
        std::printf("pak extract entries=%zu refused=%zu seconds=%.3f\n", written, refused, seconds());
        return ok ? 0 : die(err);
    }
    if (verb == "verify") {
        std::vector<uint8_t> bytes;
        size_t checked = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].aliasOf.empty()) continue;  // shares the target's bytes; the target is read below
            if (!src->read(i, bytes)) return die("corrupt entry: " + rows[i].name);
            ++checked;
        }
        std::printf("pak verify entries=%zu stored=%zu OK seconds=%.3f\n", rows.size(), checked, seconds());
        return 0;
    }
    return die("unknown verb: " + verb);
}
