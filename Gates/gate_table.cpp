// Gates/gate_table.cpp -- THE gate table: one row per MC2_* environment variable the engine
// reads. The compiler is its parser (tools/gate_dump emits Gates/gate_table.json from the
// compiled rows); nothing parses this file with a regex.
//
// Row = G(name, Kind, default, owner, retireBy, doc[, options])    Std rule
//       L(Rule, name, Kind, default, owner, retireBy, doc[, options])   legacy rule, kept so a
//                                                                  migrated site reads exactly as before
//       P(...)                                                      numeric row with atoi/atof parsing
// Rows stay one per line, sorted by name, names unique, doc at most 160 characters; long prose
// lives in docs/gate-history.md (see the `// history:` pointers). Adding or renaming a row
// needs `py -3 tools/gate_gen.py --write` so the index follows.
#include "gate_table_dsl.h"

#ifndef GATE_INDEX_BOOTSTRAP
#include "gate_index.gen.h"
#endif

// The table lives in rows::tbl so that the kind names (Bool, Tri, ...) found through
// `using enum Kind` in rows win over the type mc2::gate::Tri.
namespace mc2::gate::rows::tbl {

constexpr Row kTable[] = {
#include "gate_rows_0.inc"
#include "gate_rows_1.inc"
#include "gate_rows_2.inc"
#include "gate_rows_3.inc"
#include "gate_rows_4.inc"
};

constexpr std::size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

static_assert(nameIsSortedUnique(kTable, kCount), "gate_table.cpp: rows must be sorted by name with no duplicates");
static_assert(parentsAreValid(kTable, kCount), "gate_table.cpp: an inherits() parent must be an existing Bool Exact1 row with no parent of its own");
static_assert(firstInsaneRow(kTable, kCount) == kCount, "gate_table.cpp: a row breaks a table rule (see rowIsSane in gate_table_dsl.h)");

#ifndef GATE_INDEX_BOOTSTRAP
constexpr bool matchesIndex() {
    if (kCount != mc2::gate::detail::kGateCount) return false;
    for (std::size_t i = 0; i < kCount; ++i) {
        const Row& r = kTable[i];
        const IndexEntry& e = mc2::gate::detail::kIndex[i];
        if (r.name != e.name || r.kind != e.kind || r.rule != e.rule || r.tokens != e.tokens || r.inherits != e.parent) return false;
    }
    return true;
}
static_assert(matchesIndex(), "gate_table.cpp and gate_index.gen.h disagree: run py -3 tools/gate_gen.py --write");
#endif

}  // namespace mc2::gate::rows::tbl

namespace mc2::gate {
const Row* tableData() noexcept { return rows::tbl::kTable; }
std::size_t tableSize() noexcept { return rows::tbl::kCount; }
}  // namespace mc2::gate
