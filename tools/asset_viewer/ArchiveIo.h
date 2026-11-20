// Archive access for the viewer's headless `--pak` verbs and the Archives panel: .mc2pak/.zip (PakZip) and .fst (FastFile).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ArchiveRow {
    std::string name, source, method, aliasOf;
    uint64_t size = 0, packed = 0;
};

class ArchiveSource {
public:
    virtual ~ArchiveSource() = default;
    static std::unique_ptr<ArchiveSource> open(const std::string& path, std::string* error);
    const std::vector<ArchiveRow>& rows() const { return rows_; }   // container metadata (mc2pak.json/.aliases) excluded
    virtual bool read(size_t row, std::vector<uint8_t>& out) const = 0;
protected:
    std::vector<ArchiveRow> rows_;
};

bool archiveGlob(const char* pattern, const char* text);             // '*' (crosses '/') and '?'
bool archiveEntrySafe(const std::string& name);                       // false for .., absolute, drive-letter, backslash names
// Writes matching rows under outDir; refused (unsafe) names are listed on stderr. Returns true only if nothing failed.
bool archiveExtract(const ArchiveSource& src, const std::vector<size_t>& rows, const std::string& outDir,
                    size_t* written, size_t* refused, std::string* error);
bool archiveOutputAllowed(const std::string& path);                   // the protected play install is never an output
int runPakVerb(int argc, char** argv);                                // argv after "--pak": list|extract|pack|verify
