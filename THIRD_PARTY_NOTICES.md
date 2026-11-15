# Third-Party Notices

## ENet v1.3.18
MIT License, copyright (c) 2002-2024 Lee Salzman.
Source: https://github.com/lsalzman/enet/tree/v1.3.18
Unmodified C sources, headers and LICENSE vendored at 3rdparty/enet/.
CMake selects only the platform's socket implementation.
Windows links ws2_32 and winmm; Unix uses ENet's POSIX implementation.

## OpenLibm v0.8.7
Derived from https://github.com/JuliaMath/openlibm/tree/v0.8.7/src.
Vendored binary64 fdlibm kernels at 3rdparty/detmath/openlibm.cpp, with declarations in openlibm.h.
Copyright Sun Microsystems, Inc. (1993, 2004) and other upstream contributors;
per-file permissive notices are retained with the kernels. Permission to use, copy,
modify, and distribute is granted provided the original notices are preserved.
Adapted for C++ namespaces, bit_cast word access and deterministic simulation math.

## nlohmann/json v3.11.3
MIT License. Source: https://github.com/nlohmann/json
Single-header, vendored at 3rdparty/include/nlohmann/json.hpp.

## mimalloc v3.6.1
MIT License, copyright Microsoft Corporation, Daan Leijen. Source: https://github.com/microsoft/mimalloc/tree/v3.6.1
Vendored unmodified at 3rdparty/mimalloc/ (LICENSE, VERSION.txt alongside); built static, backs global operator new/delete and gos_Malloc/gos_Free.

## unordered_dense v5.3.3
MIT License, copyright Martin Leitner-Ankerl. Source: https://github.com/martinus/unordered_dense/tree/v5.3.3
Single header vendored at 3rdparty/unordered_dense/include/ankerl/unordered_dense.h; LICENSE alongside.

## FastNoiseLite v1.1.1
MIT License, copyright Jordan Peck and contributors. Source: https://github.com/Auburn/FastNoiseLite/tree/v1.1.1
C++ header and GLSL port (trailing whitespace removed) vendored at 3rdparty/fastnoiselite/; full LICENSE and SHA-256 provenance alongside them.

## meshoptimizer v1.3
MIT License, copyright (c) 2016-2026 Arseny Kapoulkine. Source: https://github.com/zeux/meshoptimizer/tree/v1.3 (commit 9e1f07b159d3cb777f1c67ed31fc11fd117986f4)
Vendored at 3rdparty/meshoptimizer/ (LICENSE.md alongside). Engine links a five-file subset (allocator, indexanalyzer, vcacheoptimizer, overdrawoptimizer, vfetchoptimizer) as static mc2_meshopt for static-prop/mech index+vertex ordering; the rest of the tree is not built.
