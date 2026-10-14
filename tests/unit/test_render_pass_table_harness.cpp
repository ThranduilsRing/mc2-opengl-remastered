#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <set>
#include <string>

#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"

namespace {
namespace rc = RenderCore;

static const auto& kPasses = rc::kRenderPassContracts;
static constexpr int kPassCount = rc::kRenderPassContractCount;

static uint32_t rid(rc::RenderResourceId r) { return (uint32_t)r; }
static constexpr uint32_t kResUnknown = (uint32_t)rc::RenderResourceId::Unknown;
static constexpr uint32_t kResCount   = (uint32_t)rc::RenderResourceId::Count;

static const std::set<uint32_t> kExternallyProducedReads = {

};

TEST_CASE("render_pass_table_harness/count_matches_enum") {
    CHECK((kPassCount == (int)rc::kRenderPassIdCount));
    return;
}

TEST_CASE("render_pass_table_harness/unique_pass_ids") {
    std::set<uint32_t> seen;
    for (int i = 0; i < kPassCount; ++i) {
        uint32_t id = (uint32_t)kPasses[i].id;
        if (!seen.insert(id).second)
            FAIL_CHECK(("duplicate pass id at row " + std::to_string(i) + ": " + std::to_string(id)));
    }
    return;
}

TEST_CASE("render_pass_table_harness/id_by_value_matches_table_index") {
    for (int i = 0; i < kPassCount; ++i) {
        uint32_t id = (uint32_t)kPasses[i].id;
        CHECK((id == (uint32_t)(i + 1)));
        if (id != (uint32_t)(i + 1))
            FAIL_CHECK(("row " + std::to_string(i) + " id=" + std::to_string(id) +
                   " expected " + std::to_string(i + 1) + " (row/enum order drift)"));
    }

    for (uint32_t want = 1; want <= rc::kRenderPassIdCount; ++want) {
        int n = 0;
        for (int i = 0; i < kPassCount; ++i) if ((uint32_t)kPasses[i].id == want) ++n;
        if (n != 1) FAIL_CHECK(("enum value " + std::to_string(want) + " mapped " +
                           std::to_string(n) + " times (expected 1)"));
    }
    return;
}

TEST_CASE("render_pass_table_harness/pass_names_nonempty_unique") {
    std::set<std::string> seen;
    for (int i = 0; i < kPassCount; ++i) {
        const char* nm = kPasses[i].name;
        if (!nm || nm[0] == '\0') { FAIL_CHECK(("row " + std::to_string(i) + " has empty name")); continue; }
        if (!seen.insert(nm).second) FAIL_CHECK((std::string("duplicate pass name: ") + nm));
    }
    return;
}

TEST_CASE("render_pass_table_harness/required_strings_present") {
    for (int i = 0; i < kPassCount; ++i) {
        const auto& p = kPasses[i];
        const std::string where = std::string("'") + (p.name ? p.name : "?") + "'";
        if (!p.ownerSubsystem || !p.ownerSubsystem[0]) FAIL_CHECK((where + " ownerSubsystem empty"));
        if (!p.inspectorSectionId || !p.inspectorSectionId[0]) FAIL_CHECK((where + " inspectorSectionId empty"));
    }
    return;
}

static bool resourcesInRange(const rc::RenderResourceId arr[4], const char* which, const char* pass) {
    for (int j = 0; j < 4; ++j) {
        uint32_t r = rid(arr[j]);
        if (r == kResUnknown) continue;
        if (r >= kResCount)
            FAIL_CHECK((std::string(pass) + " " + which + "[" + std::to_string(j) + "] id=" +
                   std::to_string(r) + " out of range [1," + std::to_string(kResCount) + ")"));
    }
    return true;
}

TEST_CASE("render_pass_table_harness/read_resource_ids_in_range") {
    for (int i = 0; i < kPassCount; ++i) resourcesInRange(kPasses[i].reads, "reads", kPasses[i].name);
    return;
}

TEST_CASE("render_pass_table_harness/write_resource_ids_in_range") {
    for (int i = 0; i < kPassCount; ++i) resourcesInRange(kPasses[i].writes, "writes", kPasses[i].name);
    return;
}

static bool noMidTerminator(const rc::RenderResourceId arr[4], const char* which, const char* pass) {
    bool seenTerm = false;
    for (int j = 0; j < 4; ++j) {
        bool isUnknown = rid(arr[j]) == kResUnknown;
        if (isUnknown) seenTerm = true;
        else if (seenTerm)
            FAIL_CHECK((std::string(pass) + " " + which + " has a non-Unknown entry after the "
                   "terminator at slot " + std::to_string(j) + " (silent truncation)"));
    }
    return true;
}

TEST_CASE("render_pass_table_harness/no_mid_array_terminator_in_reads") {
    for (int i = 0; i < kPassCount; ++i) noMidTerminator(kPasses[i].reads, "reads", kPasses[i].name);
    return;
}

TEST_CASE("render_pass_table_harness/no_mid_array_terminator_in_writes") {
    for (int i = 0; i < kPassCount; ++i) noMidTerminator(kPasses[i].writes, "writes", kPasses[i].name);
    return;
}

static std::set<uint32_t> collectWrittenResources() {
    std::set<uint32_t> w;
    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].writes[j]);
            if (r != kResUnknown) w.insert(r);
        }
    return w;
}

TEST_CASE("render_pass_table_harness/every_read_has_static_writer") {
    const std::set<uint32_t> written = collectWrittenResources();
    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].reads[j]);
            if (r == kResUnknown) continue;
            if (written.count(r) || kExternallyProducedReads.count(r)) continue;
            FAIL_CHECK((std::string(kPasses[i].name) + " reads resource " + std::to_string(r) +
                   " that NO pass writes (and it is not allowlisted external)"));
        }
    return;
}

TEST_CASE("render_pass_table_harness/producer_consumer_closure_for_contract3") {
    const std::set<uint32_t> written = collectWrittenResources();
    const uint32_t shadowDyn = (uint32_t)rc::RenderResourceId::ShadowDynamicMap;

    bool readSomewhere = false;
    for (int i = 0; i < kPassCount && !readSomewhere; ++i)
        for (int j = 0; j < 4; ++j)
            if (rid(kPasses[i].reads[j]) == shadowDyn) { readSomewhere = true; break; }
    if (readSomewhere)
        CHECK((written.count(shadowDyn) > 0));

    for (int i = 0; i < kPassCount; ++i)
        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(kPasses[i].reads[j]);
            if (r == kResUnknown || written.count(r) || kExternallyProducedReads.count(r)) continue;
            FAIL_CHECK(("CONTRACT-3 closure broken: resource " + std::to_string(r) +
                   " is read but never written"));
        }
    return;
}

TEST_CASE("render_pass_table_harness/frame_pass_order_complete") {
    CHECK((rc::kFramePassOrderCount == (int)rc::kRenderPassIdCount));

    std::set<uint32_t> seen;
    for (int i = 0; i < rc::kFramePassOrderCount; ++i) {
        uint32_t id = (uint32_t)rc::kFramePassOrder[i];
        if (id == (uint32_t)rc::RenderPassId::None) {
            FAIL_CHECK(("kFramePassOrder slot " + std::to_string(i) + " is None (not a real pass)"));
            continue;
        }
        if (id < 1 || id > rc::kRenderPassIdCount)
            FAIL_CHECK(("kFramePassOrder slot " + std::to_string(i) + " id=" + std::to_string(id) +
                   " out of range [1," + std::to_string(rc::kRenderPassIdCount) + "]"));
        if (!seen.insert(id).second)
            FAIL_CHECK(("kFramePassOrder duplicate id " + std::to_string(id) +
                   " at slot " + std::to_string(i)));
    }

    for (uint32_t want = 1; want <= rc::kRenderPassIdCount; ++want)
        if (!seen.count(want))
            FAIL_CHECK(("kFramePassOrder missing RenderPassId value " + std::to_string(want)));
    return;
}

TEST_CASE("render_pass_table_harness/ordered_dag_read_before_write") {
    std::set<uint32_t> writtenSoFar = kExternallyProducedReads;
    for (int i = 0; i < rc::kFramePassOrderCount; ++i) {
        const rc::RenderPassContract* p = nullptr;
        for (int k = 0; k < kPassCount; ++k)
            if (kPasses[k].id == rc::kFramePassOrder[i]) { p = &kPasses[k]; break; }
        if (!p) {
            FAIL_CHECK(("kFramePassOrder slot " + std::to_string(i) +
                   " has no matching contract row"));
            continue;
        }

        for (int j = 0; j < 4; ++j) {
            uint32_t r = rid(p->reads[j]);
            if (r == kResUnknown) break;
            if (!writtenSoFar.count(r))
                FAIL_CHECK((std::string(p->name) + " reads " + std::to_string(r) +
                       " but no earlier pass in kFramePassOrder writes it "
                       "(ordered-DAG read-before-write violation)"));
        }

        for (int j = 0; j < 4; ++j) {
            uint32_t w = rid(p->writes[j]);
            if (w == kResUnknown) break;
            writtenSoFar.insert(w);
        }
    }
    return;
}

TEST_CASE("render_pass_table_harness/demo_intentional_fail" * doctest::should_fail()) {
    CHECK((kPassCount == 999));
    return;
}

}
