#ifndef FACET_TOKEN_H
#define FACET_TOKEN_H
#include "mc2_czstring.h"
// Pure, engine/IO-free helpers shared by the data-owned facet headers.
namespace mc2 {

// Case-insensitive ASCII equality for a facet token vs a fixed key.
// Null tokens are absent and never compare equal, including to each other.
inline bool facetTokenEquals(czstring a, czstring key) {
    if (!a || !key) return false;
    for (; *a && *key; ++a, ++key) {
        char ca = *a, ck = *key;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (ck >= 'A' && ck <= 'Z') ck = static_cast<char>(ck - 'A' + 'a');
        if (ca != ck) return false;
    }
    return *a == '\0' && *key == '\0';
}

// FNV-1a over the whole name. Empty/null name -> 0 (the "unset" id).
inline unsigned int fnv1aName(czstring name) {
    if (!name || name[0] == '\0') return 0u;
    unsigned int h = 2166136261u;   // FNV offset basis
    for (const char* p = name; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;             // FNV prime
    }
    return h;
}

}  // namespace mc2
#endif
