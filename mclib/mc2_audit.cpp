//===========================================================================//
// mclib/mc2_audit.cpp — META-FIX INFRA-1: gated audit emitter.
// See mc2_audit.h for the contract. Leaf TU: <c*> headers + STL only.
//===========================================================================//

#include "mc2_czstring.h"
#include "mc2_audit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "mc2fmt.h"

namespace {

// Per-tag dedup capacity — kLogLineCap precedent (GameOS/gameos/mc2_verify.cpp:
// cap full emissions so a hot-loop fire cannot flood stdout).
const size_t kAuditOnceCapPerTag = 64;

Mc2AuditSink g_sink = nullptr;

std::unordered_map<std::string, std::unordered_set<unsigned long long>>&
onceSets()
{
    static std::unordered_map<std::string,
                              std::unordered_set<unsigned long long>> s;
    return s;
}

} // namespace

void mc2AuditLine(mc2::czstring line)
{
    if (!line) return;
    mc2::fmt::print_out("{}\n", mc2::fmt::safe(line));
    fflush(stdout);
    if (g_sink) g_sink(line);
}

bool mc2AuditOnce(mc2::czstring tag, unsigned long long key)
{
    if (!tag) tag = "";
    auto& set = onceSets()[tag];
    if (set.find(key) != set.end())
        return false;                      // already emitted for this key
    if (set.size() >= kAuditOnceCapPerTag)
        return false;                      // tag budget exhausted — rate-limit
    set.insert(key);
    return true;
}

void mc2AuditSetSink(Mc2AuditSink sink)
{
    g_sink = sink;
}
