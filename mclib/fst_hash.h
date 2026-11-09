//===========================================================================//
// mclib/fst_hash.h
//
// Leaf-TU declarations for the FST hash + key-normalization primitives.
// No transitive engine dependencies; safe to include from a doctest-driven
// unit test target.
//
// Background:
//   - elfHash() is the standard ELF dictionary hash used by FastFileFind
//     (see mclib/fastfile.cpp historical home; definition relocated here
//     so the tests/unit target can link it without pulling heap.h / ffile.h
//     / the full mclib graphics stack).
//   - fst_normalize_key() collapses backslashes to forward slashes. This
//     mirrors the inline loop in mclib/file.cpp around the elfHash() call
//     (memory/fst_forward_slash_invariant.md). Several callers normalize
//     in place; this leaf version writes to a separate dst.
//
// Engine code keeps using elfHash() via the existing fastfile.h decl;
// fastfile.h is left untouched.
//===========================================================================//

#ifndef MCLIB_FST_HASH_H
#define MCLIB_FST_HASH_H

#include "mc2_czstring.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ELF dictionary hash. Byte-stream-deterministic; case-sensitive.
// Matches the historical definition in mclib/fastfile.cpp 1:1.
uint32_t elfHash(mc2::czstring name);

// Write a slash-normalized copy of src into dst (null-terminated).
// Every '\\' byte becomes '/'; all other bytes pass through.
// dst must have room for strlen(src)+1 bytes. dst == src is allowed
// (strict forward byte-by-byte pass — in-place use is safe).
void fst_normalize_key(char* dst, mc2::czstring src);

// META-FIX design E: in-place CASE+SLASH canonical-key fold — the char*
// sibling of fst_normalize_loose_key() below (lowercase + '\\'->'/', one
// pass). This is THE chokepoint the ladder callers funnel through
// (File::open, fileExists, FastFileFind) so every spelling of a logical
// path hashes/compares as one canonical key (issue #8 family).
void fst_normalize_loose_key_inplace(char* s);

#ifdef __cplusplus
}

#include <string>
#include <string_view>

// Loose-file / mod-overlay index key normalizer: lowercase + forward-slash in
// one pass (the canonical key used by mclib/file.cpp's loose-file index, which
// now delegates here). Distinct from fst_normalize_key(): that one is the FST
// elfHash path and is slash-only (the engine lowercases separately via
// S_strlwr); this one folds case AND slashes together so a mod path and its
// case/slash variants resolve to ONE index key. tolower follows the existing
// engine behavior (C locale; ASCII asset paths fold deterministically).
std::string fst_normalize_loose_key(std::string_view src);
// Explicit NUL-terminated adapter for legacy callers, including null inputs.
inline std::string fst_normalize_loose_key(mc2::czstring src)
{
    return fst_normalize_loose_key(src ? std::string_view(src) : std::string_view{});
}
#endif

#endif // MCLIB_FST_HASH_H
