#pragma once
// inifile.h stub
// Provides FitIniFile — only what parseBrainSpecialBody_FitIni calls.
// Implements a minimal bracket-form [Section]/Key=Value parser (no engine deps).

#include "dstd.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef INIFILE_H
#define INIFILE_H

// Error codes matching real inifile.h
#define BLOCK_NOT_FOUND  0xFADA0000
#define ID_NOT_FOUND     0xFADA0001

// Minimal FitIniFile — full bracket-form parser enough for legacy _specials.fit fixtures.
// Supports: [Section], st KEY = "VALUE" lines (FIT format).
class FitIniFile {
public:
    FitIniFile() = default;
    ~FitIniFile() { close(); }

    long open(const char* path);
    long close();
    long seekBlock(const char* blockName);
    long readIdString(const char* id, char* buf, unsigned long bufSize);

private:
    struct Entry { std::string key; std::string value; };
    struct Block { std::string name; std::vector<Entry> entries; };

    std::vector<Block> m_blocks;
    int                m_curBlock = -1;
};

#endif // INIFILE_H
