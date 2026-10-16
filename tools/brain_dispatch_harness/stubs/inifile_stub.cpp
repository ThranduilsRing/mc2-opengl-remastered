// FitIniFile stub implementation.
// Minimal bracket-form parser for legacy _specials.fit fixtures.
// Supports: [Section], st KEY = "VALUE"
//
// FIT format sample:
//   FITini
//   [BrainSpecial]
//   [Body]
//   st DO0 = "Brain.CorePower false"
//   ...
#include "inifile.h"
#include <fstream>
#include <sstream>
#include <algorithm>

long FitIniFile::open(const char* path) {
    m_blocks.clear();
    m_curBlock = -1;

    std::ifstream f(path);
    if (!f.is_open())
        return 1; // non-zero = error

    std::string line;
    int curBlock = -1;

    while (std::getline(f, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n' || line.back() == '\t'))
            line.pop_back();

        if (line.empty() || line[0] == ';')
            continue;

        // Block header: [Name]
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                Block b;
                b.name = line.substr(1, end - 1);
                m_blocks.push_back(std::move(b));
                curBlock = (int)m_blocks.size() - 1;
            }
            continue;
        }

        if (curBlock < 0)
            continue;

        // Entry line: st KEY = "VALUE"
        // Skip leading "st " prefix if present
        const char* p = line.c_str();
        if (std::strncmp(p, "st ", 3) == 0) p += 3;
        while (*p == ' ' || *p == '\t') ++p;

        // Find '='
        const char* eq = std::strchr(p, '=');
        if (!eq) continue;

        std::string key(p, eq);
        // trim key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();

        const char* vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') ++vp;

        // Value may be quoted
        std::string value;
        if (*vp == '"') {
            ++vp;
            const char* vend = std::strchr(vp, '"');
            value = vend ? std::string(vp, vend) : std::string(vp);
        } else {
            value = std::string(vp);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
                value.pop_back();
        }

        Entry e;
        e.key   = key;
        e.value = value;
        m_blocks[curBlock].entries.push_back(std::move(e));
    }

    return NO_ERR;
}

long FitIniFile::close() {
    m_blocks.clear();
    m_curBlock = -1;
    return NO_ERR;
}

long FitIniFile::seekBlock(const char* blockName) {
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        if (m_blocks[i].name == blockName) {
            m_curBlock = i;
            return NO_ERR;
        }
    }
    return BLOCK_NOT_FOUND;
}

long FitIniFile::readIdString(const char* id, char* buf, unsigned long bufSize) {
    if (m_curBlock < 0)
        return ID_NOT_FOUND;
    const Block& blk = m_blocks[m_curBlock];
    for (const Entry& e : blk.entries) {
        if (e.key == id) {
            std::strncpy(buf, e.value.c_str(), bufSize);
            buf[bufSize] = '\0';  // safe if caller guarantees bufSize < actual buf
            return NO_ERR;
        }
    }
    return ID_NOT_FOUND;
}
