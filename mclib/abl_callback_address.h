#pragma once
#include <cstddef>
#include <cstdint>
#ifndef _WIN32
#include <link.h>
#endif

// Keep the historical Windows guard; ELF PIE code has no fixed address floor.
// Null slots use the dispatcher's separate undefined-routine diagnostic.
inline bool ablCallbackIsBogus(void (*callback)())
{
    const auto address = reinterpret_cast<std::uintptr_t>(callback);
    if (!address) return false;
#ifdef _WIN32
    return address < std::uintptr_t{0x00007F0000000000ULL};
#else
    struct Query { std::uintptr_t address; bool executable = false; } query{address};
    dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* context) {
        auto& query = *static_cast<Query*>(context);
        for (unsigned i = 0; i < image->dlpi_phnum; ++i) {
            const auto& segment = image->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) continue;
            const auto begin = image->dlpi_addr + segment.p_vaddr;
            if (query.address >= begin && query.address - begin < segment.p_memsz) {
                query.executable = true;
                return 1;
            }
        }
        return 0;
    }, &query);
    return !query.executable;
#endif
}
