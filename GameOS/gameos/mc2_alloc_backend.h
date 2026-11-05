// Allocator backend behind gos_Malloc/gos_Free and the global operator new/delete (GameOS/gameos/gameos.cpp).
//   * default: mimalloc (3rdparty/mimalloc).
//   * MC2_ASAN (the existing CMake option; also any -fsanitize=address build): the CRT malloc/free, so AddressSanitizer
//     intercepts every engine allocation (red zones, use-after-free, leak reports). Routing through mimalloc would hide
//     the whole engine heap from ASan. Reliable on MSVC and clang alike, unlike mimalloc's MI_TRACK_ASAN, which only
//     poisons inside mimalloc's own arenas and needs the sanitizer interface headers per toolchain.
// Rule: never free() a block from new/gos_Malloc and never delete/gos_Free a malloc'd block (docs/cxx17-coding-rules.md).
// mc2a_check_owned() enforces it in MC2_ALLOC_STATS and non-NDEBUG builds.
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string_view>

#if !defined(MC2_ASAN) && !defined(__SANITIZE_ADDRESS__)
#include <mimalloc.h>
#endif

#if defined(MC2_ASAN) || defined(__SANITIZE_ADDRESS__)
#if defined(_WIN32)
inline void* mc2a_malloc(size_t n) { return std::malloc(n); }
inline void* mc2a_aligned(size_t n, size_t al) { return _aligned_malloc(n, al); }
inline void  mc2a_free(void* p) { std::free(p); }
inline void  mc2a_aligned_free(void* p) { _aligned_free(p); }
inline size_t mc2a_usable(void* p) { return _msize(p); }
#else
inline void* mc2a_malloc(size_t n) { return std::malloc(n); }
inline void* mc2a_aligned(size_t n, size_t al) { return std::aligned_alloc(al, (n + al - 1) / al * al); }
inline void  mc2a_free(void* p) { std::free(p); }
inline void  mc2a_aligned_free(void* p) { std::free(p); }
inline size_t mc2a_usable(void* p) { return malloc_usable_size(p); }
#endif
inline bool  mc2a_owns(const void*) { return true; } // ASan itself reports a bad free
#else
inline void* mc2a_malloc(size_t n) { return mi_malloc(n); }
inline void* mc2a_aligned(size_t n, size_t al) { return mi_malloc_aligned(n, al); }
inline void  mc2a_free(void* p) { mi_free(p); }
inline void  mc2a_aligned_free(void* p) { mi_free(p); }
inline size_t mc2a_usable(void* p) { return mi_usable_size(p); }
inline bool  mc2a_owns(const void* p) { return mi_is_in_heap_region(p); }
#endif

#if defined(MC2_ALLOC_STATS) || !defined(NDEBUG)
// Allocator-mismatch guard: a non-null pointer reaching gos_Free/delete must belong to the engine allocator.
inline void mc2a_check_owned(const void* p, std::string_view who)
{
    if (p && !mc2a_owns(p)) {
        std::fprintf(stderr, "allocator mismatch in %.*s: %p is not an engine-allocator block (free()'d/malloc'd memory crossed new/gos_Malloc)\n", static_cast<int>(who.size()), who.data(), p);
        std::fflush(stderr);
        std::abort();
    }
}
#else
inline void mc2a_check_owned(const void*, std::string_view) {}
#endif
