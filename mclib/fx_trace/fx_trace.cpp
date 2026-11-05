//==========================================================================//
// File:    fx_trace.cpp                                                     //
// Contents: Neutral fx invocation counter implementation.                   //
//           Plan v6 §1.                                                     //
//===========================================================================//

#include "mc2_czstring.h"
#include "fx_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace mc2 {
namespace fx_trace {

namespace {

// Latch the env value once at first query (per debug_instrumentation_rule.md
// idiom). Three independent counter tables. We deliberately allocate the
// tables on the heap and NEVER destroy them — this avoids the well-known
// static-dtor ordering trap where one TU's static dtor calls into another
// TU whose statics have already been destroyed (and would crash the
// process during atexit, suppressing the [SMOKE v1] summary). Leaking the
// tables at process exit is the correct trade.
using CounterMap = std::map<std::string, unsigned long long>;
struct ClassEntry {
    unsigned long long count = 0;
    std::string first_seen_name;  // first spec name observed for this class_id
};
using ClassMap = std::map<unsigned int, ClassEntry>;
struct CounterTables {
    CounterMap spawn;
    CounterMap draw;
    CounterMap mlr_enqueue;
    ClassMap   per_class;  // C10
    std::mutex m;
};

bool g_enabled_initialized = false;
bool g_enabled_value = false;
bool g_atexit_registered = false;
CounterTables* g_tables = nullptr;
std::mutex g_init_mutex;

// C10: per-ClassID distribution dump. Raw numeric class_id printed; the
// caller cross-references against the enum in mclib/gosfx/gosfx.hpp. One-way
// dependency rule (fx_trace never includes gosfx/mlr/particles) preserved.
void dump_class_table(const ClassMap& tbl) {
    if (tbl.empty()) {
        mc2::fmt::print_err("[FX_TRACE v1] class: (empty)\n");
        return;
    }
    unsigned long long total = 0;
    for (const auto& [id, st] : tbl) total += st.count;
    mc2::fmt::print_err("[FX_TRACE v1] class total={:d} unique={:d}\n", mc2::fmt::as_u64(total), mc2::fmt::as_size(tbl.size()));
    for (const auto& [classId, stats] : tbl) {
        mc2::fmt::print_err("[FX_TRACE v1] class id={:d} count={:d} first_name={}\n", mc2::fmt::as_u32(classId), mc2::fmt::as_u64(stats.count), stats.first_seen_name.empty()
                         ? "(none)"
                         : stats.first_seen_name.c_str());
    }
}

void dump_table(mc2::czstring label, const CounterMap& tbl) {
    if (tbl.empty()) {
        mc2::fmt::print_err("[FX_TRACE v1] {}: (empty)\n", mc2::fmt::safe(label));
        return;
    }
    unsigned long long total = 0;
    for (const auto& [key, cnt] : tbl) total += cnt;
    mc2::fmt::print_err("[FX_TRACE v1] {} total={:d} unique={:d}\n", mc2::fmt::safe(label), mc2::fmt::as_u64(total), mc2::fmt::as_size(tbl.size()));
    for (const auto& [name, count] : tbl) {
        mc2::fmt::print_err("[FX_TRACE v1] {} name={} count={:d}\n", mc2::fmt::safe(label), name.c_str(), mc2::fmt::as_u64(count));
    }
}

void atexit_dump() {
    if (g_enabled_value && g_tables) {
        dump_and_reset("atexit");
    }
}

void initialize_locked() {
    if (g_enabled_initialized) return;
    g_enabled_value = mc2::gate::get<"MC2_FX_TRACE">();
    g_tables = new CounterTables();  // intentionally leaked
    if (!g_atexit_registered) {
        std::atexit(&atexit_dump);
        g_atexit_registered = true;
    }
    g_enabled_initialized = true;
    if (g_enabled_value) {
        mc2::fmt::print_err("[INSTR v1] enabled: fx_trace\n");
        mc2::fmt::print_err("[FX_TRACE v1] counters: spawn/draw/mlr_enqueue\n");
    }
}

void ensure_initialized() {
    if (g_enabled_initialized) return;
    std::lock_guard<std::mutex> lock(g_init_mutex);
    initialize_locked();
}

} // namespace

bool is_enabled() {
    if (!g_enabled_initialized) ensure_initialized();
    return g_enabled_value;
}

void record_spawn(mc2::czstring name) {
    if (!g_tables) ensure_initialized();
    if (!name) name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->spawn[name];
}

void record_draw(mc2::czstring name) {
    if (!g_tables) ensure_initialized();
    if (!name) name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->draw[name];
}

void record_mlr_enqueue(mc2::czstring leaf_name) {
    if (!g_tables) ensure_initialized();
    if (!leaf_name) leaf_name = "(null)";
    std::lock_guard<std::mutex> lock(g_tables->m);
    ++g_tables->mlr_enqueue[leaf_name];
}

void record_class(unsigned int class_id, mc2::czstring name) {
    if (!g_tables) ensure_initialized();
    std::lock_guard<std::mutex> lock(g_tables->m);
    ClassEntry& e = g_tables->per_class[class_id];
    ++e.count;
    if (e.first_seen_name.empty() && name) {
        e.first_seen_name = name;
    }
}

void dump_and_reset(mc2::czstring reason) {
    if (!g_tables) return;
    std::lock_guard<std::mutex> lock(g_tables->m);
    mc2::fmt::print_err("[FX_TRACE v1] === dump ({}) ===\n", mc2::fmt::safe(reason ? reason : "?"));
    dump_table("spawn", g_tables->spawn);
    dump_table("draw", g_tables->draw);
    dump_table("mlr_enqueue", g_tables->mlr_enqueue);
    dump_class_table(g_tables->per_class);
    mc2::fmt::print_err("[FX_TRACE v1] === end dump ===\n");
    g_tables->spawn.clear();
    g_tables->draw.clear();
    g_tables->mlr_enqueue.clear();
    g_tables->per_class.clear();
}

} // namespace fx_trace
} // namespace mc2
