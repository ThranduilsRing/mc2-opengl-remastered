<!-- doc-status: current | reviewed:2026-10-11 -->
# C++ Coding Rules (C++23)

> **Standard: C++23** (default since commit `9493e60bf`; `MC2_CXX_STANDARD=23`, MSVC `/std:c++latest`).
> The file name and the sections below date from the C++17 flip (CXX17-1, 2026-05-24). They are kept as the
> C++ coding rules for C++23 under the owner-approved modernization override. Mentions of C++17 in the body
> describe feature origin or flip history, not the current standard.

## 1. Purpose

This document defines which C++ features (C++17 and later, currently C++23) the MC2 OpenGL codebase
allows, which need extra care, and which are off-limits for now. It is
forward-looking: it does not audit existing code; it sets expectations
for every future contributor (and every future agent) touching this
tree.

The codebase has been MSVC C++14 implicit for most of its life. A small
C++17 island (RenderCore, RenderWorld, GameAdapters, the unit-test
tree) coexisted with C++14 legacy until the CXX17-1 flip. From that
point forward, the project is uniformly C++17, now C++23. This document exists so
that the unification does not become an excuse for indiscriminate
modernization.

## 2. Status

-   **Project standard:** C++23 (`MC2_CXX_STANDARD=23`, default since
    `9493e60bf`; MSVC `/std:c++latest`).
-   **C++17 flip date (history):** 2026-05-24.
-   **C++17 flip commit (history):** `5c03835` (CXX17-1 -- root CMakeLists.txt sets
    project-wide C++17; three modules previously on `cxx_std_17`
    PUBLIC declarations become redundant-but-load-bearing min-version
    contracts).
-   **Audit recon:**
    `docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`.
-   **Toolchain:** MSVC 19.44.35225.0 (VS 2022 BuildTools, fully
    C++17/C++20/C++23 capable). No third-party pin on a specific
    standard.

## 3. The cardinal rule

> **Upgrade the language mode first. Use the new features later, on
> purpose, with measurable wins.**

A language-mode flip does not authorize a feature spree. CLAUDE.md
"Code rules" already says: *don't touch what you don't have
to (every touch has blast radius); when you must, bring it to modern
standard.* Neither the C++17 flip nor the move to C++23 changes that calculus.

Concrete corollaries:

-   Do not open a slice titled "modernize X with C++23". Open a slice
    that fixes a bug or reduces measurable debt, and *if* a modern feature is the cleanest local tool for that fix, use it.
-   Standalone cleanup slices still need a blocking or debt
    justification (per `memory/minimal_touch_modern_when_touched.md`).
    "It's nicer C++23" is not a justification.
-   The render path is hot and carefully audited. Modernization for its
    own sake there is a regression risk with no upside. Default
    answer: **leave the runtime renderer alone unless you are already
    editing the surrounding code for an unrelated reason.**

## 4. Allowed immediately

### 4.1 `if constexpr`

Compile-time branching inside a single function. Kills the
SFINAE-or-tag-dispatch overhead that C++14 needed for the same job.

```cpp
template <class T>
void serialize(T const& value, std::ostream& os) {
    if constexpr (std::is_arithmetic_v<T>) {
        os.write(reinterpret_cast<char const*>(&value), sizeof(T));
    } else {
        value.writeTo(os);
    }
}
```

When to use: any place where C++14 code would have written two
overloads selected by `std::enable_if` or by tag dispatch, and the
branching is genuinely on a compile-time property.

When not to use: don't reach for a template just so you can use
`if constexpr`. Plain runtime `if` on a `constexpr bool` config flag is
already free under optimization.

### 4.2 Structured bindings

```cpp
auto [key, value] = *iter;          // destructure a pair
auto const& [hit, recipe] = result; // by reference, const
```

When to use: anywhere a name appears once and you currently write
`pair.first` / `pair.second` or `std::get<0>(tuple)`. Especially
welcome in test code, tools, and offline data-munging where
readability dominates.

When not to use: do not bind the result of a temporary by reference
and then store it across a yield/coroutine/await point -- the
underlying object's lifetime is the temporary's, not the binding's.
Inside hot per-frame code, structured bindings compile to the same
code as manual destructuring; pick whichever reads better at the
call site.

### 4.3 `inline constexpr` constants

The C++14 idiom for header-defined integer/enum constants was
`static constexpr` at file scope (one copy per TU) or
`extern constexpr` in a header + a definition in one TU. Both have
sharp edges: the first risks ODR confusion if the symbol ever leaks,
the second is fragile boilerplate.

C++17's `inline constexpr` gives you a single defined symbol visible
in every TU that includes the header, with no .cpp definition.

```cpp
// In a public header (e.g. RenderCore/HandleBases.h):
namespace mc2 {
inline constexpr uint32_t kMechHandleBase     = 0x00010000u;
inline constexpr uint32_t kTerrainHandleBase  = 0x00040000u;
inline constexpr uint32_t kVfxHandleBase      = 0x00080000u;
}
```

When to use: any header-defined constant -- handle bases, magic
numbers, table sizes, version tags. Today's RenderWorld code has
several `static constexpr` constants at .cpp file scope; those are
fine *while local*, but the moment a second TU needs the same value,
promote it to `inline constexpr` in a shared header rather than
duplicating the literal.

When not to use: function-local constants -- a plain `constexpr` local
is already fine.

### 4.4 `std::optional<T>`

A type-safe "maybe-T". Replaces sentinel return values (-1, empty
string, "did you remember to check the bool out-param") in non-hot
APIs.

```cpp
std::optional<RecipeIndex> findRecipeForType(uint32_t typeId) const;

if (auto idx = registry.findRecipeForType(t)) {
    use(*idx);
}
```

When to use: offline tools, schema parsing, lookup APIs in cold
paths, anywhere "missing" is a meaningful answer rather than an
error. Particularly nice in `tools/` and `tests/`.

When not to use: per-frame hot paths. `std::optional` has a discriminant
byte plus alignment padding, and accessing it via `*opt` requires a
runtime check (or you skip the check and risk UB). For a per-pixel /
per-quad / per-instance value, design the data layout instead.

### 4.5 `std::string_view`

Non-owning read-only view over a contiguous run of `char`. Replaces
`char const*` + length parameter pairs, and replaces `std::string
const&` for parameters that never need to outlive the call.

```cpp
bool isShaderName(std::string_view name) {
    return name.size() > 5 && name.substr(name.size() - 5) == ".frag";
}
```

When to use: read-only name / key / path parameters in API signatures.
Logging helpers. Anywhere the caller has a `std::string`, a
`char const*`, OR a literal and the callee just wants to look at the
bytes.

When not to use: storing it in a struct/class member is a footgun --
the view is only as alive as the backing buffer. If the type's
lifetime exceeds the call, store `std::string` (owning) instead. Never
return `std::string_view` from a function that builds the string
internally.

## 5. Use cautiously

### 5.1 `std::variant<...>`

A tagged union. Replaces hand-rolled `enum + union` for sum types.

Caveats:

-   `std::get<T>(v)` and `std::get_if<T>(v)` are the two access modes.
    The `std::get<T>` form **throws `std::bad_variant_access`** on
    type mismatch. Throwing inside the render loop is a runtime fault
    the project's hot paths are not built to recover from.
-   `std::visit` over a variant generates one function-pointer indirect
    call per dispatch. Fine for cold code, measurable in a per-element
    loop.

Use it only in cold code, and only via `std::get_if<T>` (returns a
pointer or `nullptr`) unless you have proven via testing that the
input cannot be the wrong alternative.

### 5.2 `std::filesystem`

Path manipulation, directory iteration, file metadata. Lovely API for
asset-pipeline tools, makefst, pak, shader_reflect, anything that
walks a directory tree.

Caveats:

-   On Windows under MSVC, `std::filesystem::path` defaults to
    `wchar_t` internally and converts at the boundary. The
    `path.string()` -> UTF-8 round-trip can mangle non-ASCII paths.
    If you are reading a path from the OS, prefer `path.u8string()`.
-   The runtime engine itself uses the in-tree path conventions
    (`PATH_SEPARATOR` is `/`). Do not mix `std::filesystem::path` with the in-tree
    string-based path APIs in the same function -- pick one.

Allowed in offline tools (`data_tools/`, `tools/shader_reflect/`,
`text_tool/`, the unit-test tree). Discouraged in the runtime engine
unless you can show the legacy path API genuinely can't do the job.

### 5.3 Parallel algorithms (`std::execution::par`)

`std::sort(std::execution::par, v.begin(), v.end(), pred);` and
friends.

Caveats:

-   STL parallel pulls in PPL/TBB-style runtime overhead at compile
    time and at runtime. Compile-time cost is non-trivial.
-   On small inputs the parallel version is slower than serial, and on
    pathological inputs can deadlock or oversubscribe (the engine
    already runs N worker threads for game logic).
-   Tracy + parallel STL is a difficult mix; per-thread spans show up
    inconsistently.

Allowed only if you have measured wins on a representative dataset
(not a synthetic micro-benchmark) and have considered whether the
existing job-system / cull-pipeline parallelism would do the job
better.

### 5.4 Polymorphic memory resources (`std::pmr`)

Allocator wiring for STL containers without the C++03-era allocator
template pain.

Caveats:

-   Useful for arena allocators backing per-frame scratch.
-   Wiring `std::pmr::vector<T>` with a `monotonic_buffer_resource`
    requires every nested container to be PMR-aware. One missed
    `std::vector` in the chain silently leaks to the default
    allocator.
-   The codebase already has custom container patterns (mclib's
    custom `Auto_Ptr`, the various pool/freelist machinery in
    `mclib/stuff/`). Adding `std::pmr` next to them is not free
    cognitive cost.

Allowed for new arena-allocator slices, not for retrofitting existing
container usage.

## 6. Avoid for now

### 6.1 Broad STL rewrites

Do not open a slice that replaces `std::vector` with
`std::array`, or `std::map` with `std::unordered_map`, or any other
"this should be that now" sweep just because the language version
allows it. The existing containers are tuned for their existing
workloads. Replace one at a time, on its own, with profiling data
showing it matters.

### 6.2 Replacing custom containers in hot code

`mclib/stuff/auto_ptr.hpp` is not `std::auto_ptr` (which is gone) and
it is not `std::unique_ptr` either; the codebase's `Auto_Ptr<>` has
specific semantics. Likewise the various custom vector / pool /
freelist types in `mclib/`. They exist because the project's hot
paths had specific allocation, copy, or aliasing requirements that
the standard library did not meet at the time the original author
wrote them.

Before swapping any of them, *investigate why they exist*. A grep on
the type plus a read of the surrounding allocation pattern almost
always reveals a constraint that `std::unique_ptr` / `std::vector` /
`std::deque` does not honor.

### 6.3 Introducing exceptions in TUs where they're disabled

The engine has historically been exception-light. `/EHsc` may not be
on for every legacy TU (verify per-target in the .vcxproj if you are
about to write `throw`). `std::variant`'s `std::get` throws.
`std::optional`'s `value()` throws. Many `std::filesystem`
operations throw on error (the `_ec`-suffixed overloads return an
error code instead).

Default rule: do not introduce a throw site in a TU that is currently
exception-free. Use the non-throwing overload (`get_if`, `value_or`,
`*_ec` filesystem variants) or check beforehand.

### 6.4 Allocator mixing

`new`, `delete` and `gos_Malloc`/`gos_Free` share one engine allocator
(mimalloc; CRT under `MC2_ASAN`). Never `free()` memory from `new` or
`gos_Malloc`, and never `delete`/`gos_Free` memory from `malloc`. Debug and
`MC2_ALLOC_STATS` builds abort with an "allocator mismatch" message on a foreign pointer.

## 7. First useful cleanups (advisory backlog)

These are not assigned. They are the cleanups that, if someone is
*already* editing the surrounding code for an unrelated reason, would
be cheap wins. None of them is allowed as a standalone "modernize"
slice.

-   **`inline constexpr` handle-base constants.** Today,
    `kMechHandleBase`, `kTerrainHandleBase`, `kVfxHandleBase` live as
    `static constexpr` at file scope inside `RenderWorld/RenderWorld.cpp`.
    Promote them to `inline constexpr` in a public header
    (e.g. a new RenderCore handle-bases header) when the next slice needs a
    second TU to reference them. Until then, they're fine where they
    are.
-   **`std::optional<T>` in offline-tool / schema-parsing code.**
    `tools/shader_reflect/`, hypothetical `tools/material_cook/`,
    any future schema-load code. "Missing field" maps naturally to
    `std::optional`.
-   **Structured bindings in tools and tests.** `tests/unit/` and the
    various `data_tools/` executables are exactly the kind of code
    where readability dominates and a destructuring binding is just
    pure win. No reason not to use them in new code there.
-   **EXPLICITLY NOT recommended:** runtime renderer churn. Do not
    "modernize" `mclib/`, `GameOS/gameos/`, `gui/`, or any of the
    other 14 legacy libraries unless you have a non-modernization
    reason to be in that file. The render path is hot, carefully
    audited, and the cost-benefit on "nicer C++" there is firmly
    negative.

## 8. Decision flow

When you are about to write a C++17-or-later feature in a runtime TU, walk
this checklist:

1.  **Is this in a hot path?** (per-frame, per-instance, per-pixel,
    per-quad.) If yes -- skip to step 4.
2.  **Is the feature in §4 ("Allowed immediately")?** If yes -- use
    it.
3.  **Is the feature in §5 ("Use cautiously")?** If yes -- write a
    one-line justification in the commit message explaining why the
    cautioned caveat does not bite here.
4.  **Hot-path branch.** Default answer is *no, do not add a C++17
    feature here just because you can*. The exceptions:
    -   `if constexpr` collapsing two template specializations into
        one function is an unambiguous win (no runtime cost).
    -   `inline constexpr` promoting a duplicated literal into a
        shared header is an unambiguous win.
    -   Everything else: prove the win with a measurement.
5.  **Is the feature in §6 ("Avoid for now")?** Then don't. Or, if you
    truly believe the rule should change, open a discussion (a memory
    file or a planning doc) before writing the code.

## 9. Examples from the codebase

### 9.1 Current `static constexpr` handle base

Today, the handle-base constants live at file scope inside
`RenderWorld/RenderWorld.cpp`:

```cpp
// Inside RenderWorld.cpp (current, pre-cleanup):
namespace {
static constexpr uint32_t kMechHandleBase    = 0x00010000u;
static constexpr uint32_t kTerrainHandleBase = 0x00040000u;
static constexpr uint32_t kVfxHandleBase     = 0x00080000u;
}
```

This is fine *while it lives in one TU*. The moment a second TU --
say a future tool that needs to validate a serialized handle's
kind -- wants the same value, the cleanup is:

```cpp
// In a new public header RenderCore/HandleBases.h:
#pragma once
#include <cstdint>

namespace mc2 {
inline constexpr uint32_t kMechHandleBase    = 0x00010000u;
inline constexpr uint32_t kTerrainHandleBase = 0x00040000u;
inline constexpr uint32_t kVfxHandleBase     = 0x00080000u;
}
```

```cpp
// RenderWorld.cpp now includes the header and drops the local
// namespace { static constexpr ... } block.
#include "RenderCore/HandleBases.h"
```

Net: one source of truth, no ODR risk, no .cpp definition needed,
both TUs see the same value at compile time.

Do not do this preemptively. Do it the day the second consumer
arrives.

### 9.2 `std::optional` in a lookup API

Hypothetical -- the kind of API where `std::optional` reads cleanly:

```cpp
// In an offline tool. Cold path; "not found" is a normal answer.
std::optional<MaterialId> findMaterial(std::string_view name) const {
    auto it = byName_.find(name);
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

// Caller:
if (auto id = catalog.findMaterial("rock_01")) {
    bake(*id);
}
```

Compare to the C++14 pattern: either a sentinel `MaterialId{0}` (which
collides with the legitimate id 0), or a bool out-parameter (which
the caller forgets to check), or an exception (which is off-limits in
exception-free TUs). `std::optional` is strictly better here.

The same API written for the hot path would not use `std::optional`
-- it would use a precomputed dense table indexed by handle, with
the "missing" case handled by a generation check.

## 10. Cross-references

-   `CLAUDE.md` -- "Code rules" section (pointer to this doc).
-   `docs/renderworld_migration_guide.md` -- the RenderWorld
    contributor onboarding doc; the C++ rules cross-reference is in
    the §15 cheat sheet area.
-   `docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`
    -- the audit that preceded the CXX17-1 flip; covers the toolchain,
    per-target standard inventory, and the "features that already
    work" survey.
-   `memory/minimal_touch_modern_when_touched.md` -- the underlying
    discipline this doc operationalizes for the C++ standard specifically.
-   `.claude/skills/greybeard.md` -- META-FIX vs PATCH discipline;
    applies to any "let's modernize this" proposal.
