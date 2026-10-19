//===========================================================================//
// mclib/fst_hash.cpp
//
// Leaf-TU implementation of elfHash() and the FST key normalizer. See
// mclib/fst_hash.h for rationale.
//
// elfHash() body is byte-for-byte the historical mclib/fastfile.cpp:120
// definition; relocated so the tests/unit target can link it without
// dragging in heap.h / ffile.h / the broader engine includes. The decl
// in mclib/fastfile.h still resolves here at link time.
//===========================================================================//

#include "fst_hash.h"

extern "C" unsigned long elfHash(const char* name)
{
    unsigned long h = 0, g;
    while (*name)
    {
        h = (h << 4) + static_cast<unsigned char>((*name++));
        if ((g = h & 0xF0000000UL))
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

extern "C" void fst_normalize_key(char* dst, const char* src)
{
    // memory/fst_forward_slash_invariant.md: backslash inputs must collapse
    // to forward slash before hashing. file.cpp does this in-place around
    // the elfHash() call; this leaf version writes to a separate buffer.
    // dst == src is safe (strict forward pass, byte read before write).
    while (*src)
    {
        char c = *src++;
        *dst++ = (c == '\\') ? '/' : c;
    }
    *dst = '\0';
}

#include <cctype>

extern "C" void fst_normalize_loose_key_inplace(char* s)
{
    // META-FIX design E: canonical-key chokepoint, char* form. Byte-for-byte
    // the fst_normalize_loose_key() fold below (lowercase + slash), operating
    // in place so the File::open / fileExists / FastFileFind hot paths stay
    // allocation-free.
    if (!s) return;
    for (; *s; ++s)
        *s = (*s == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
}

// Byte-for-byte the loop previously inlined in mclib/file.cpp::NormalizeKey:
// backslash -> '/', every other byte -> tolower. Folds case AND slashes so a
// mod-overlay path resolves to one canonical index key regardless of how the
// caller spelled it.
std::string fst_normalize_loose_key(const char* src)
{
    std::string s = src ? src : "";
    for (char& c : s)
        c = (c == '\\') ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
