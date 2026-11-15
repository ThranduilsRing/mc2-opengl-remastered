#ifndef MC2_MODEL_CREDIT_H
#define MC2_MODEL_CREDIT_H
#include "inifile.h"
#include <string>

// Model authorship belongs to the appearance manifest, independently of the pack author.
inline std::string readModelCredit(FitIniFile& manifest)
{
    char author[256] = {}, source[256] = {};
    if (manifest.seekBlock("ModelCredit") != NO_ERR ||
        manifest.readIdString("Author", author, sizeof(author) - 1) != NO_ERR ||
        manifest.readIdString("Source", source, sizeof(source) - 1) != NO_ERR ||
        !author[0] || !source[0])
        return {};
    return std::string("Model by: ") + author + " (" + source + ")";
}
#endif
