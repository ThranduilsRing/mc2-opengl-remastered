// tests/unit/test_gate.cpp
// GATE-REGISTRY-G1A: the typed gate table's parser, readers, cache and [CONFIG v1] dump.
//
// Two layers:
//   * pure   -- detail::parseRow() on rows built here and on explicit text, so every
//               truthiness rule is checked against the probe vectors of the idioms it replaces
//               (U E Z F O X X10 X25) without touching the process environment;
//   * live   -- the real table: slot caching, invalidate/invalidateAll, lookupName, the
//               per-site Rule override, dumpConfigWith() output.
#include "doctest.h"

#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/gate_parse.h"
#include "Gates/gate_row.h"
#include "Gates/gate_table_dsl.h"
#include "Gates/render_options.h"
#include "Gates/render_options_cfg.h"
#include "mlr/mlr_gate.h"
#include "vk_device_profile.h"   // the Vulkan device profile table (header-only, so the table is tested without a Vulkan link)
#include "vk_validation_line.h"  // the engine-log line of one validation-layer message (header-only, Vulkan-free)

#include <cstdio>
#include <limits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#include <process.h>   // _spawnl: the strict-exit test runs this exe again
#include <windows.h>   // GetModuleFileNameA
#endif

namespace gate = mc2::gate;
using gate::Kind;
using gate::Rule;
using gate::Tri;

namespace {

// Set (or clear, value == nullptr) a process environment variable; restored on destruction.
class EnvVar {
public:
    explicit EnvVar(const char* name) : name_(name) {
        const char* old = std::getenv(name);
        if (old != nullptr) { had_ = true; old_ = old; }
    }
    ~EnvVar() { apply(had_ ? old_.c_str() : nullptr); }
    void set(const char* value) const { apply(value); }

private:
    void apply(const char* value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value != nullptr ? value : "");  // "" removes the variable
#else
        if (value != nullptr) setenv(name_.c_str(), value, 1); else unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool had_ = false;
    std::string old_;
};

// The test binary's main() has started by the time any case runs, so no slot a test resolves
// is a "pre-main" slot; the premain case below fabricates one explicitly.
const bool kMainEntered = (gate::markMainEntered(), true);

gate::Row boolRow(Rule rule, bool dflt) {
    return gate::rows::L(rule, "MC2_UNITTEST_BOOL", Kind::Bool, dflt, "test", "", "unit test row");
}

bool rowValue(const gate::Row& row, const char* text) {
    return (gate::detail::parseRow(row, text) & gate::detail::kRowBit) != 0;
}

float asFloat(std::uint64_t slot) { return __builtin_bit_cast(float, static_cast<std::uint32_t>(slot)); }

bool isBad(const gate::Row& row, const char* text) {
    return (gate::detail::parseRow(row, text) & gate::detail::kBadBit) != 0;
}

// The probe texts every rule is tried on, in the order of the vector strings below: unset, "", "0", "false", "1", "yes", "10", "2.5".
constexpr const char* kProbeText[8] = {nullptr, "", "0", "false", "1", "yes", "10", "2.5"};

struct Vector {
    Rule rule;
    bool dflt;
    const char* bits;  // on(1)/off(0) per probe
};

// One vector per boolean reading rule, plus the two default-carrying
// rules, spelled out so a change of either side shows up here.
constexpr Vector kVectors[] = {
    {Rule::Std, false, "00001111"},      {Rule::Std, true, "11001111"},
    {Rule::Lead0, false, "00011111"},    {Rule::Lead0Kill, true, "11011111"},
    {Rule::Lead0P, false, "01011111"},   {Rule::DefOnLead0, true, "10011111"},
    {Rule::Lead1, false, "00001010"},    {Rule::Exact1, false, "00001000"},
    {Rule::NonEmpty, false, "00111111"}, {Rule::Presence, false, "01111111"},
    {Rule::AtoiNZ, false, "00001011"},   {Rule::DefOnAtoiNZ, true, "10001011"},
    {Rule::Is01, false, "00101000"},     {Rule::Lead0Def, false, "00011111"},
    {Rule::Lead0Def, true, "11011111"},
};

}  // namespace

TEST_SUITE("Gate") {

// ---------------------------------------------------------------------------
// Truthiness: every rule against the probe vectors of the idiom it names
// ---------------------------------------------------------------------------

TEST_CASE("Bool rules reproduce the probe vectors of the idioms they replace") {
    for (const Vector& v : kVectors) {
        const gate::Row row = boolRow(v.rule, v.dflt);
        for (int i = 0; i < 8; ++i) {
            INFO("rule index " << static_cast<int>(v.rule) << " default " << v.dflt << " probe " << i);
            CHECK(rowValue(row, kProbeText[i]) == (v.bits[i] == '1'));
        }
    }
}

TEST_CASE("Std: the documented spellings, case-insensitively") {
    const gate::Row row = boolRow(Rule::Std, true);
    for (const char* off : {"0", "false", "FALSE", "Off", "no", "NO"}) CHECK_FALSE(rowValue(row, off));
    for (const char* on : {"1", "true", "TRUE", "On", "yes", "Yes"}) CHECK(rowValue(row, on));
    CHECK(rowValue(row, "") == true);       // empty gives the default
    CHECK(rowValue(row, nullptr) == true);  // unset gives the default
    for (const char* off : {"0", "false", "no", "off"}) CHECK_FALSE(isBad(row, off));
    for (const char* on : {"1", "true", "yes", "on"}) CHECK_FALSE(isBad(row, on));
}

TEST_CASE("Std: an unknown non-empty string is on and flagged badvalue") {
    const gate::Row row = boolRow(Rule::Std, false);
    for (const char* text : {"banana", "2", "00", "-1", "0 ", " 1", "truee"}) {
        INFO(text);
        CHECK(rowValue(row, text));
        CHECK(isBad(row, text));
    }
    CHECK_FALSE(isBad(row, ""));
    CHECK_FALSE(isBad(row, nullptr));
}

TEST_CASE("Legacy rules never report badvalue") {
    for (const Vector& v : kVectors) {
        if (v.rule == Rule::Std) continue;
        for (const char* text : kProbeText) CHECK_FALSE(isBad(boolRow(v.rule, v.dflt), text));
        CHECK_FALSE(isBad(boolRow(v.rule, v.dflt), "banana"));
    }
}

TEST_CASE("Lead1 reads a leading '1' (\"10\" is on), Exact1 only \"1\"") {
    CHECK(rowValue(boolRow(Rule::Lead1, false), "10"));
    CHECK(rowValue(boolRow(Rule::Lead1, false), "1abc"));
    CHECK_FALSE(rowValue(boolRow(Rule::Exact1, false), "10"));
    CHECK_FALSE(rowValue(boolRow(Rule::Exact1, false), "1 "));
}

TEST_CASE("Lead0Kill turns off on any leading '0' and is on for unset and empty") {
    const gate::Row row = boolRow(Rule::Lead0Kill, true);
    CHECK_FALSE(rowValue(row, "0"));
    CHECK_FALSE(rowValue(row, "05"));
    CHECK(rowValue(row, nullptr));
    CHECK(rowValue(row, ""));
    CHECK(rowValue(row, "false"));
}

TEST_CASE("AtoiNZ accepts a numeric prefix") {
    const gate::Row row = boolRow(Rule::AtoiNZ, false);
    CHECK(rowValue(row, "12abc"));
    CHECK(rowValue(row, "2.0"));
    CHECK_FALSE(rowValue(row, "abc"));
    CHECK_FALSE(rowValue(row, "0x"));
}

TEST_CASE("per-site override applies the requested rule to the same flags") {
    const gate::Row row = boolRow(Rule::Std, false);
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t slot = gate::detail::parseRow(row, kProbeText[i]);
        const std::uint32_t flags = gate::detail::flagsOf(slot);
        for (const Vector& v : kVectors)
            CHECK(gate::detail::applyRule(v.rule, flags, v.dflt) == (v.bits[i] == '1'));
    }
}

// ---------------------------------------------------------------------------
// Numerics, enums, Tri
// ---------------------------------------------------------------------------

TEST_CASE("Int rows: whole-string parse, empty and junk fall back to the default") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row row = G("MC2_UNITTEST_INT", Int, 64, "test", "", "doc");
    auto val = [&](const char* t) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(row, t))); };
    CHECK(val("128") == 128);
    CHECK(val("-5") == -5);
    CHECK(val(nullptr) == 64);
    CHECK(val("") == 64);
    for (const char* junk : {"abc", "12abc", " 5", "2.0", "5 "}) {
        INFO(junk);
        CHECK(val(junk) == 64);
        CHECK(isBad(row, junk));
    }
}

TEST_CASE("Int rows with range: Reject falls back, clamp clamps") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row reject = G("MC2_UNITTEST_RNG", Int, 64, "test", "", "doc", range(16, 256));
    const gate::Row clamp = G("MC2_UNITTEST_CLAMP", Int, 64, "test", "", "doc", clampRange(16, 256));
    auto val = [](const gate::Row& r, const char* t) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(r, t))); };
    CHECK(val(reject, "128") == 128);
    CHECK(val(reject, "300") == 64);
    CHECK(isBad(reject, "300"));
    CHECK(val(reject, "3") == 64);
    CHECK(val(clamp, "300") == 256);
    CHECK(val(clamp, "3") == 16);
    CHECK_FALSE(isBad(clamp, "300"));
}

TEST_CASE("Prefix numerics are atoi/atof compatible") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row i = P("MC2_UNITTEST_PI", Int, 7, "test", "", "doc");
    const gate::Row f = P("MC2_UNITTEST_PF", Float, 1.5, "test", "", "doc");
    auto ival = [&](const char* t) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(i, t))); };
    auto fval = [&](const char* t) { return asFloat(gate::detail::parseRow(f, t)); };
    CHECK(ival("12abc") == 12);
    CHECK(ival("2.0") == 2);
    CHECK(ival(nullptr) == 7);
    CHECK(ival("") == 7);
    CHECK(ival("abc") == 0);  // atoi() says 0; flagged
    CHECK(isBad(i, "abc"));
    CHECK(fval("2.5x") == 2.5f);
    CHECK(fval("abc") == 0.0f);
    CHECK(fval(nullptr) == 1.5f);
}

TEST_CASE("Float rows: whole-string parse, non-finite rejected") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row row = G("MC2_UNITTEST_FLT", Float, 0.5, "test", "", "doc");
    auto val = [&](const char* t) { return asFloat(gate::detail::parseRow(row, t)); };
    CHECK(val("2.25") == 2.25f);
    CHECK(val("1e2") == 100.0f);
    CHECK(val(nullptr) == 0.5f);
    for (const char* bad : {"abc", "2.5x", "nan", "inf"}) {
        INFO(bad);
        CHECK(val(bad) == 0.5f);
        CHECK(isBad(row, bad));
    }
}

TEST_CASE("Enum rows: case-insensitive token, unknown falls back with badvalue") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row row = G("MC2_UNITTEST_ENUM", Enum, "a", "test", "", "doc", tokens("a|b|c"));
    auto val = [&](const char* t) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(row, t))); };
    CHECK(val("b") == 1);
    CHECK(val("C") == 2);
    CHECK(val(nullptr) == 0);
    CHECK(val("") == 0);
    CHECK(val("zzz") == 0);
    CHECK(isBad(row, "zzz"));
    CHECK_FALSE(isBad(row, "B"));
}

TEST_CASE("Tri rows read exact \"1\" and \"0\" only") {
    const gate::Row row = gate::rows::G("MC2_UNITTEST_TRI", Kind::Tri, false, "test", "", "doc");
    auto val = [&](const char* t) { return static_cast<std::uint32_t>(gate::detail::parseRow(row, t) & 0xFFu); };
    CHECK(val("1") == 2);   // On
    CHECK(val("0") == 1);   // Off
    CHECK(val(nullptr) == 0);
    CHECK(val("") == 0);
    CHECK(val("yes") == 0);
    CHECK(val("10") == 0);
}

TEST_CASE("parseBool is the Std rule and reports badvalue") {
    bool bad = true;
    CHECK(gate::parseBool(nullptr, true, &bad) == true);
    CHECK_FALSE(bad);
    CHECK(gate::parseBool("", false, &bad) == false);
    CHECK(gate::parseBool("No", true, &bad) == false);
    CHECK_FALSE(bad);
    CHECK(gate::parseBool("ON", false, &bad) == true);
    CHECK_FALSE(bad);
    CHECK(gate::parseBool("banana", false, &bad) == true);
    CHECK(bad);
    CHECK(gate::parseBool("0", true) == false);  // bad pointer is optional
}

// ---------------------------------------------------------------------------
// Reader API: types, names, the live table
// ---------------------------------------------------------------------------

TEST_CASE("Tri is an enum class with no bool conversion") {
    static_assert(!std::is_convertible_v<Tri, bool>);
    static_assert(std::is_enum_v<Tri> && !std::is_convertible_v<Tri, int>);
    static_assert(std::is_same_v<decltype(gate::get<"MC2_ABL_VM_FACTS">()), bool>);
    static_assert(std::is_same_v<decltype(gate::get<"MC2_BOOT_INFO">()), std::int32_t>);
    static_assert(std::is_same_v<decltype(gate::get<"MC2_CONFIG_DUMP">()), gate::Token>);
    static_assert(std::is_enum_v<gate::Token> && !std::is_convertible_v<gate::Token, bool> &&
                  !std::is_convertible_v<gate::Token, int>);
    static_assert(std::is_same_v<decltype(gate::get<"MC2_BOOT_TIMER_SECS">()), float>);
    static_assert(std::is_same_v<decltype(gate::get<"MC2_ABL_VM_FACTS", Rule::Presence>()), bool>);
    CHECK(true);
}

TEST_CASE("table is sorted, unique and every name round-trips through lookupName") {
    const gate::Row* rows = gate::tableData();
    const std::size_t n = gate::tableSize();
    REQUIRE(n == gate::detail::kGateCount);
    // The table is exactly 980 rows (gate_table.json `count`). REQUIRE(n == kGateCount) only ties the table to the
    // generated header, so a bad regeneration that shrinks both passes it; this floor catches that loss. It sits a
    // few rows under the real count and moves down with an amnesty wave that deletes rows on purpose.
    REQUIRE(n >= 975);
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) CHECK((rows[i - 1].name < rows[i].name));
        const auto info = gate::lookupName(rows[i].name);
        REQUIRE(info.has_value());
        CHECK(info->index == i);
        CHECK(info->kind == rows[i].kind);
        CHECK(info->kind == gate::detail::kIndex[i].kind);
        CHECK(info->rule == gate::detail::kIndex[i].rule);
    }
    CHECK_FALSE(gate::lookupName("MC2_NOT_A_GATE").has_value());
    CHECK_FALSE(gate::lookupName("").has_value());
    CHECK_FALSE(gate::lookupName("mc2_boot_info").has_value());
}

TEST_CASE("the gate module's own rows exist") {
    CHECK(gate::lookupName("MC2_CONFIG_DUMP").has_value());
    CHECK(gate::lookupName("MC2_CONFIG_STRICT").has_value());
    CHECK(gate::get<"MC2_CONFIG_STRICT">() == false);
}

TEST_CASE("a read caches until invalidate") {
    EnvVar env("MC2_BOOT_INFO");
    env.set("7");
    gate::invalidate("MC2_BOOT_INFO");
    CHECK(gate::get<"MC2_BOOT_INFO">() == 7);
    env.set("9");
    CHECK(gate::get<"MC2_BOOT_INFO">() == 7);  // cached
    gate::invalidate("MC2_BOOT_INFO");
    CHECK(gate::get<"MC2_BOOT_INFO">() == 9);
    env.set(nullptr);
    gate::invalidateAll();
    CHECK(gate::get<"MC2_BOOT_INFO">() == 0);  // default
}

TEST_CASE("isSet, state and the Rule override read the same cached flags") {
    EnvVar env("MC2_ABL_VM_FACTS");
    env.set(nullptr);
    gate::invalidate("MC2_ABL_VM_FACTS");
    CHECK_FALSE(gate::isSet<"MC2_ABL_VM_FACTS">());
    CHECK(gate::state<"MC2_ABL_VM_FACTS">() == Tri::Unset);
    CHECK_FALSE(gate::get<"MC2_ABL_VM_FACTS", Rule::Presence>());
    env.set("0");
    gate::invalidate("MC2_ABL_VM_FACTS");
    CHECK(gate::isSet<"MC2_ABL_VM_FACTS">());
    CHECK(gate::state<"MC2_ABL_VM_FACTS">() == Tri::Off);
    CHECK(gate::get<"MC2_ABL_VM_FACTS", Rule::Presence>());     // set, so present
    CHECK_FALSE(gate::get<"MC2_ABL_VM_FACTS", Rule::Lead0>());   // leading '0'
    env.set("1");
    gate::invalidate("MC2_ABL_VM_FACTS");
    CHECK(gate::state<"MC2_ABL_VM_FACTS">() == Tri::On);
    CHECK(gate::get<"MC2_ABL_VM_FACTS", Rule::Exact1>());
    env.set(nullptr);
    gate::invalidate("MC2_ABL_VM_FACTS");
}

TEST_CASE("Enum rows through the live table") {
    EnvVar en("MC2_CONFIG_DUMP");
    en.set("ALL");
    gate::invalidate("MC2_CONFIG_DUMP");
    CHECK(gate::is<"MC2_CONFIG_DUMP", "all">());
    CHECK_FALSE(gate::is<"MC2_CONFIG_DUMP", "none">());
    en.set(nullptr);
    gate::invalidate("MC2_CONFIG_DUMP");
    CHECK(gate::is<"MC2_CONFIG_DUMP", "default">());
    // get<> on an Enum row is the token's index in the row's list, as a Token.
    CHECK(gate::get<"MC2_CONFIG_DUMP">() == gate::Token{0});   // "default"
    en.set("none");
    gate::invalidate("MC2_CONFIG_DUMP");
    CHECK(gate::get<"MC2_CONFIG_DUMP">() == gate::Token{2});
    en.set(nullptr);
    gate::invalidate("MC2_CONFIG_DUMP");
}

TEST_CASE("String rows are re-read on every call") {
    EnvVar env("MC2_LOG");
    env.set(nullptr);
    CHECK(gate::str<"MC2_LOG">().empty());
    env.set("a,b");
    CHECK(gate::str<"MC2_LOG">() == "a,b");
    env.set("c");
    CHECK(gate::str<"MC2_LOG">() == "c");
}

// ---------------------------------------------------------------------------
// [CONFIG v1]
// ---------------------------------------------------------------------------

namespace {
std::string capture(const gate::DumpOptions& opts, gate::DumpSummary* sum = nullptr) {
    std::FILE* f = std::tmpfile();
    REQUIRE(f != nullptr);
    const gate::DumpSummary s = gate::dumpConfigWith(f, opts);
    if (sum != nullptr) *sum = s;
    std::rewind(f);
    std::string out;
    char buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

bool hasLine(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, prefix.size(), prefix) == 0) return true;
        pos = end + 1;
    }
    return false;
}
}  // namespace

TEST_CASE("dumpConfig prints env-set gates and a summary") {
    EnvVar env("MC2_BOOT_INFO");
    env.set("7");
    gate::DumpSummary sum;
    const std::string out = capture({}, &sum);
    CHECK(hasLine(out, "[CONFIG v1] name=MC2_BOOT_INFO kind=int value=7 src=env raw=\"7\" default=0 rule=prefix owner=code"));
    CHECK(hasLine(out, "[CONFIG v1] summary total=" + std::to_string(gate::tableSize()) + " set="));
    CHECK(sum.set >= 1);
    CHECK(sum.nondefault >= 1);
    CHECK_FALSE(hasLine(out, "[CONFIG v1] name=MC2_ABL_VM_FACTS "));  // unset rows stay out of the default dump
}

TEST_CASE("dumpConfig all lists every row, none only the summary") {
    gate::DumpOptions all;
    all.all = true;
    gate::DumpOptions none;
    none.none = true;
    const std::string a = capture(all);
    const std::string n = capture(none);
    CHECK(hasLine(a, "[CONFIG v1] name=MC2_ABL_VM_FACTS kind=bool value=0 src=default raw=\"\" default=0 rule=lead0 owner=code"));
    std::size_t lines = 0;
    for (char c : a) lines += (c == '\n');
    CHECK(lines >= gate::tableSize());
    CHECK(hasLine(n, "[CONFIG v1] summary"));
    CHECK_FALSE(hasLine(n, "[CONFIG v1] name="));
}

TEST_CASE("dumpConfig warns about an unregistered MC2_ variable and a bad value") {
    EnvVar typo("MC2_TERAIN_NOT_A_GATE");
    typo.set("1");
    const gate::Row* rows = gate::tableData();
    std::size_t stdBool = gate::tableSize();
    for (std::size_t i = 0; i < gate::tableSize(); ++i)
        if (rows[i].kind == Kind::Bool && rows[i].rule == Rule::Std) { stdBool = i; break; }
    REQUIRE(stdBool < gate::tableSize());
    const std::string name(rows[stdBool].name);
    EnvVar bad(name.c_str());
    bad.set("banana");
    gate::DumpSummary sum;
    gate::DumpOptions opts;
    opts.exitOnStrict = false;
    opts.strict = true;
    const std::string out = capture(opts, &sum);
    CHECK(hasLine(out, "[CONFIG v1] warn=unregistered name=MC2_TERAIN_NOT_A_GATE"));
    const bool verified = rows[stdBool].verified;
    CHECK(hasLine(out, "[CONFIG v1] warn=badvalue name=" + name + " kind=bool raw=" + std::string(1, '"') + "banana" +
                           std::string(1, '"') + (verified ? "" : " verified=0")));
    CHECK(sum.unregistered >= 1);
    // A bad value is fatal under strict only on a verified row; an unverified row is a warning.
    CHECK((verified ? sum.badvalue : sum.badvalueUnverified) >= 1);
    if (!verified) CHECK(sum.badvalue == 0);
}

TEST_CASE("a bad value on a verified row counts toward strict, with no verified=0 marker") {
    constexpr std::size_t idx = gate::detail::indexOf("MC2_LIGHT_GRID_DIM");
    REQUIRE(gate::tableData()[idx].verified);
    EnvVar env("MC2_LIGHT_GRID_DIM");
    env.set("abc");
    gate::DumpSummary sum;
    gate::DumpOptions opts;
    opts.exitOnStrict = false;
    opts.strict = true;
    const std::string out = capture(opts, &sum);
    CHECK(hasLine(out, "[CONFIG v1] warn=badvalue name=MC2_LIGHT_GRID_DIM kind=int raw=\"abc\""));
    CHECK(sum.badvalue >= 1);
}

TEST_CASE("an Enum row's default must be one of its tokens") {
    using namespace gate::rows;
    using gate::Row;
    CHECK(rowIsSane(G("MC2_UNITTEST_ENUM", Enum, "a", "test", "", "doc", tokens("a|b"))));
    CHECK_FALSE(rowIsSane(G("MC2_UNITTEST_ENUM", Enum, "z", "test", "", "doc", tokens("a|b"))));
    CHECK(rowIsSane(G("MC2_UNITTEST_ENUM", Enum, "", "test", "", "doc", tokens("|a|b"))));  // empty token listed
    CHECK_FALSE(rowIsSane(G("MC2_UNITTEST_ENUM", Enum, "", "test", "", "doc", tokens("a|b"))));
}

TEST_CASE("a reread row is parsed on every read, never cached") {
    constexpr std::size_t idx = gate::detail::indexOf("MC2_GLTF_AXIS");
    REQUIRE(gate::tableData()[idx].reread);
    EnvVar env("MC2_GLTF_AXIS");
    env.set("3");
    CHECK(gate::get<"MC2_GLTF_AXIS">() == 3);
    env.set("5");   // no invalidate
    CHECK(gate::get<"MC2_GLTF_AXIS">() == 5);
    CHECK((gate::detail::g_slot[idx].load() & gate::detail::kResolvedBit) == 0);
}

TEST_CASE("a cached row keeps its value until invalidate") {
    constexpr std::size_t idx = gate::detail::indexOf("MC2_LIGHT_GRID_DIM");
    REQUIRE_FALSE(gate::tableData()[idx].reread);
    EnvVar env("MC2_LIGHT_GRID_DIM");
    gate::invalidate("MC2_LIGHT_GRID_DIM");
    env.set("32");
    CHECK(gate::get<"MC2_LIGHT_GRID_DIM">() == 32);
    env.set("48");
    CHECK(gate::get<"MC2_LIGHT_GRID_DIM">() == 32);
    gate::invalidate("MC2_LIGHT_GRID_DIM");
    CHECK(gate::get<"MC2_LIGHT_GRID_DIM">() == 48);
    env.set(nullptr);
    gate::invalidate("MC2_LIGHT_GRID_DIM");
}

TEST_CASE("invalidateAll keeps pre-main values and the dump flags the ones that went stale") {
    constexpr std::size_t idx = gate::detail::indexOf("MC2_BOOT_INFO");
    EnvVar env("MC2_BOOT_INFO");
    env.set("7");
    // A slot resolved before main() carries the pre-main bit.
    gate::detail::g_slot[idx].store(gate::detail::kResolvedBit | gate::detail::kPreMainBit | 7u);
    gate::invalidateAll();
    CHECK_FALSE((gate::detail::g_slot[idx].load() & gate::detail::kResolvedBit) != 0);
    env.set("9");  // the launcher applied a different value after static init
    gate::DumpSummary sum;
    const std::string out = capture({}, &sum);
    CHECK(hasLine(out, "[CONFIG v1] warn=premain name=MC2_BOOT_INFO premain_value=7 env_now=9"));
    CHECK(sum.premainStale >= 1);
    env.set("7");  // nothing stale once the environment agrees again
    CHECK_FALSE(hasLine(capture({}), "[CONFIG v1] warn=premain name=MC2_BOOT_INFO"));
}

TEST_CASE("a VK gate read before main is reported even when its value did not change") {
    constexpr std::size_t vk = gate::detail::indexOf("MC2_VK_FORCE_FAIL");
    constexpr std::size_t other = gate::detail::indexOf("MC2_BOOT_INFO");
    EnvVar vkEnv("MC2_VK_FORCE_FAIL");
    EnvVar otherEnv("MC2_BOOT_INFO");
    vkEnv.set(nullptr);
    otherEnv.set("5");
    gate::detail::g_slot[vk].store(gate::detail::kResolvedBit | gate::detail::kPreMainBit);  // Bool off, unset env
    gate::detail::g_slot[other].store(gate::detail::kResolvedBit | gate::detail::kPreMainBit | 5u);
    gate::invalidateAll();
    const std::string out = capture({});
    CHECK(hasLine(out, "[CONFIG v1] warn=premain_read name=MC2_VK_FORCE_FAIL premain_value=0"));
    CHECK_FALSE(hasLine(out, "[CONFIG v1] warn=premain name=MC2_VK_FORCE_FAIL"));  // the value did not change
    CHECK_FALSE(hasLine(out, "[CONFIG v1] warn=premain_read name=MC2_BOOT_INFO"));  // only VK gates are flagged
}

// ---------------------------------------------------------------------------
// G1b: the numeric rows of the terrain pilot read exactly what the lambdas they replace read
// ---------------------------------------------------------------------------

TEST_CASE("exclusive lower bound: rangeAbove accepts (lo, hi] and rejects the rest") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row row = P("MC2_UNITTEST_RNG", Float, 0.6, "test", "", "doc", rangeAbove(0.0, 1.0));
    auto val = [&](const char* t) { return asFloat(gate::detail::parseRow(row, t)); };
    CHECK(val("0.25") == 0.25f);
    CHECK(val("1") == 1.0f);
    CHECK(val("0.0001") == 0.0001f);
    for (const char* t : {"0", "0.0", "-1", "1.5", "abc", "1e999", ""}) {
        INFO(t);
        CHECK(val(t) == 0.6f);
    }
}

TEST_CASE("Prefix float rows: +-inf reaches the range test, so a clamp gives the bound") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row row = P("MC2_UNITTEST_RNG", Float, 0.7, "test", "", "doc", clampRange(0.0, 2.0));
    auto val = [&](const char* t) { return asFloat(gate::detail::parseRow(row, t)); };
    CHECK(val("1e999") == 2.0f);
    CHECK(val("-1e999") == 0.0f);
    CHECK(val("3") == 2.0f);
    CHECK(val("-3") == 0.0f);
    CHECK(val("abc") == 0.0f);  // atof("abc") is 0, inside the range
}

namespace g1b {

const gate::Row& rowNamed(const char* name) {
    const auto info = gate::lookupName(name);
    REQUIRE(info.has_value());
    return gate::tableData()[info->index];
}

constexpr const char* kProbes[] = {nullptr, "", "0", "false", "1", "yes", "10", "2.5", "abc", "0.5", "0.0",
                                   "-1", "1.5", "300", "3", "2", "256", "257", "4096", "4097", "1e999",
                                   "-1e999", " 2", "2junk", "0.25", "0.0001", "1.0000000001", "0x10"};

// The reader lambdas of gos_terrain_lighting.cpp, transcribed with the environment text as a parameter.
std::uint32_t oldGridDim(const char* v) {
    if (v && v[0]) { const long n = std::atol(v); if (n >= 2 && n <= 256) return static_cast<std::uint32_t>(n); }
    return 64u;
}
std::uint32_t oldPoolMax(const char* v) {
    if (v && v[0]) { const long n = std::atol(v); if (n > 0 && n <= 4096) return static_cast<std::uint32_t>(n); }
    return 0u;
}
float oldMoverDim(const char* s) {
    if (!s || !s[0]) return 0.25f;
    const float f = static_cast<float>(std::atof(s));
    return (f >= 0.0f && f <= 1.0f) ? f : 0.25f;
}
float oldMoverRadius(const char* s) {
    if (!s || !s[0]) return 0.6f;
    const float f = static_cast<float>(std::atof(s));
    return (f > 0.0f && f <= 1.0f) ? f : 0.6f;
}
float oldObjScale(const char* s) {
    float f = (s && s[0]) ? static_cast<float>(std::atof(s)) : 0.7f;
    if (f < 0.0f) f = 0.0f;
    if (f > 2.0f) f = 2.0f;
    return f;
}
float oldPoolSpill(const char* v) {
    float f = 2.2f;
    if (v && v[0]) f = static_cast<float>(std::atof(v));
    if (f < 0.25f) f = 0.25f;
    if (f > 4.0f) f = 4.0f;
    return f;
}
float oldPoolCore(const char* v) {
    float f = 0.0f;
    if (v && v[0]) f = static_cast<float>(std::atof(v));
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

std::int32_t intOf(const gate::Row& r, const char* t) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(r, t)));
}

}  // namespace g1b

TEST_CASE("pilot Int rows reproduce the old atol readers on every probe") {
    const gate::Row& grid = g1b::rowNamed("MC2_LIGHT_GRID_DIM");
    const gate::Row& pool = g1b::rowNamed("MC2_LIGHT_POOL_MAX");
    for (const char* t : g1b::kProbes) {
        INFO((t ? t : "(unset)"));
        CHECK(static_cast<std::uint32_t>(g1b::intOf(grid, t)) == g1b::oldGridDim(t));
        CHECK(static_cast<std::uint32_t>(g1b::intOf(pool, t)) == g1b::oldPoolMax(t));
    }
}

TEST_CASE("pilot Float rows reproduce the old atof readers on every probe") {
    struct Case { const char* name; float (*old)(const char*); };
    const Case cases[] = {
        {"MC2_LIGHT_POOL_MOVER_DIM", g1b::oldMoverDim},     {"MC2_LIGHT_POOL_MOVER_RADIUS", g1b::oldMoverRadius},
        {"MC2_LIGHT_POOL_OBJ_SCALE", g1b::oldObjScale},     {"MC2_LIGHT_REALISM_POOL_SPILL", g1b::oldPoolSpill},
        {"MC2_LIGHT_REALISM_POOL_CORE", g1b::oldPoolCore},
    };
    for (const Case& c : cases) {
        const gate::Row& row = g1b::rowNamed(c.name);
        for (const char* t : g1b::kProbes) {
            INFO(c.name << " <- " << (t ? t : "(unset)"));
            CHECK(asFloat(gate::detail::parseRow(row, t)) == c.old(t));
        }
    }
}

TEST_CASE("MC2_PATCH_STREAM_FORCE_INIT_FAIL is a presence gate that defaults off") {
    const gate::Row& row = g1b::rowNamed("MC2_PATCH_STREAM_FORCE_INIT_FAIL");
    CHECK(row.rule == Rule::Presence);
    CHECK_FALSE(row.defBool);
    CHECK_FALSE(rowValue(row, nullptr));
    for (const char* t : {"", "0", "false", "1"}) {
        INFO(t);
        CHECK(rowValue(row, t));  // getenv(x) != nullptr
    }
}

// ---------------------------------------------------------------------------
// Parse edges: sign, whitespace, overflow, non-finite
// ---------------------------------------------------------------------------

TEST_CASE("Int rows: Std parse takes the whole string, Prefix is atoi-compatible") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row std_ = G("MC2_UNITTEST_INT", Int, 7, "test", "", "doc");
    const gate::Row pre = P("MC2_UNITTEST_PI", Int, 7, "test", "", "doc");
    auto val = [&](const gate::Row& r, const char* t) { return g1b::intOf(r, t); };
    // from_chars takes a leading '-' but not '+', whitespace or trailing text: those fall back with badvalue.
    CHECK(val(std_, "-1") == -1);
    for (const char* t : {"+1", " 2", "2 ", "2junk", "0x10"}) {
        INFO(t);
        CHECK(val(std_, t) == 7);
        CHECK(isBad(std_, t));
    }
    // strtol skips leading whitespace, accepts '+', stops at junk, saturates at the 32-bit long range.
    CHECK(val(pre, "+1") == 1);
    CHECK(val(pre, " 2") == 2);
    CHECK(val(pre, "-3") == -3);
    CHECK(val(pre, "2junk") == 2);
    CHECK(val(pre, "0x10") == 0);
    CHECK(val(pre, "99999999999") == 2147483647);
    CHECK(val(pre, "-99999999999") == -2147483647 - 1);
}

TEST_CASE("Int row overflow: Std falls back to the default, flagged") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row std_ = G("MC2_UNITTEST_INT", Int, 7, "test", "", "doc");
    CHECK(g1b::intOf(std_, "2147483647") == 2147483647);
    for (const char* t : {"2147483648", "99999999999", "-2147483649"}) {
        INFO(t);
        CHECK(g1b::intOf(std_, t) == 7);
        CHECK(isBad(std_, t));
    }
}

TEST_CASE("Float rows: a Std row rejects 1e999, a Prefix row lets inf reach the range test") {
    using namespace gate::rows;
    using gate::Row;
    const gate::Row std_ = G("MC2_UNITTEST_FLT", Float, 0.5, "test", "", "doc");
    const gate::Row pre = P("MC2_UNITTEST_PF", Float, 0.5, "test", "", "doc");
    const gate::Row preClamp = P("MC2_UNITTEST_PF", Float, 0.5, "test", "", "doc", clampRange(0.0, 2.0));
    const gate::Row preReject = P("MC2_UNITTEST_PF", Float, 0.5, "test", "", "doc", range(0.0, 2.0));
    auto v = [](const gate::Row& r, const char* t) { return asFloat(gate::detail::parseRow(r, t)); };
    CHECK(v(std_, "1e999") == 0.5f);
    CHECK(isBad(std_, "1e999"));
    CHECK(v(std_, "-1e999") == 0.5f);
    CHECK(v(std_, " 1.5") == 0.5f);          // whole-string parse: leading space is junk
    CHECK(v(pre, " 1.5") == 1.5f);           // strtod skips it
    CHECK(v(pre, "+1.5") == 1.5f);
    CHECK(v(pre, "1.5junk") == 1.5f);
    CHECK(v(preClamp, "1e999") == 2.0f);     // inf clamps to the bound, as the old (float)atof readers did
    CHECK(v(preClamp, "-1e999") == 0.0f);
    CHECK(v(preReject, "1e999") == 0.5f);    // inf outside a reject range: the default
    CHECK(v(preClamp, "1e50") == 2.0f);      // a double that overflows float is inf after the cast
    CHECK(v(pre, "nan") == 0.5f);            // NaN never parses
}

// ---------------------------------------------------------------------------
// MC2_CONFIG_STRICT: the process ends with exit code 78
// ---------------------------------------------------------------------------

#ifdef _WIN32
// The child half of the strict test: does nothing unless the parent spawned this exe with GATE_TEST_STRICT_CHILD
// set. It prints the dump and, under MC2_CONFIG_STRICT=1 with an unregistered variable in the environment, must
// never return.
TEST_CASE("strict_exit_child") {
    if (std::getenv("GATE_TEST_STRICT_CHILD") == nullptr) return;
    gate::dumpConfig(stderr);
    if (std::getenv("MC2_CONFIG_STRICT") != nullptr) FAIL("dumpConfig returned under strict mode");
}

TEST_CASE("an unregistered MC2_ variable under MC2_CONFIG_STRICT=1 ends the process with exit code 78") {
    char exe[MAX_PATH] = {0};
    REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0);
    EnvVar strict("MC2_CONFIG_STRICT");
    EnvVar typo("MC2_TERAIN_NOT_A_GATE");
    EnvVar child("GATE_TEST_STRICT_CHILD");
    child.set("1");
    strict.set("1");
    typo.set("1");
    const intptr_t rc = _spawnl(_P_WAIT, exe, exe,
                                "-tc=strict_exit_child", nullptr);
    CHECK(rc == 78);
    strict.set(nullptr);
    typo.set(nullptr);
    // Without strict the same environment only warns: the child's dump returns and it exits 0.
    const intptr_t rc2 = _spawnl(_P_WAIT, exe, exe,
                                 "-tc=strict_exit_child", nullptr);
    CHECK(rc2 == 0);
}
#endif

// ---------------------------------------------------------------------------
// G3: terrain float rows (ranges), the resample Enum and the intro presence gate
// ---------------------------------------------------------------------------

TEST_CASE("G3 float rows carry the bounds of the readers they replace") {
    struct Want { const char* name; bool clamp; double lo; double hi; };
    constexpr double kFltMax = 3.4028234663852886e38;
    const Want want[] = {
        {"MC2_MOVER_CONTACT_SHADOW", false, 0.0, 1.0},
        {"MC2_TERRAIN_LOD_CHUNK_DIST_SCALE", false, static_cast<double>(0.05f), kFltMax},
        {"MC2_TERRAIN_LOD_MORPH_START", true, 0.0, 0.95},
        {"MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH", true, 0.0, 1.0},
        {"MC2_TERRAIN_SHORELINE_EDGE_JITTER", true, 0.0, 32.0},
        {"MC2_TERRAIN_SHORELINE_STRENGTH", true, 0.0, 2.0},
        {"MC2_TERRAIN_VISUAL_DISPLACE_FAR", true, 0.0, 1.0},
        {"MC2_WATER_SHORE_FEATHER", false, 0.0, kFltMax},
        {"MC2_WATER_SHORE_MEANDER_FREQ", false, 0.0, kFltMax},
    };
    for (const Want& w : want) {
        INFO(w.name);
        const gate::Row& r = g1b::rowNamed(w.name);
        CHECK(r.kind == Kind::Float);
        REQUIRE(r.range.has);
        CHECK(r.range.clamp == w.clamp);
        CHECK(r.range.lo == w.lo);
        CHECK(r.range.hi == w.hi);
    }
    CHECK(g1b::rowNamed("MC2_WATER_SHORE_MEANDER_FREQ").defFloat == 1.0f / 384.0f);
    // The old reader tested `s > 0.05f` in float: exactly 0.05 is rejected, the next float up is accepted.
    const gate::Row& dist = g1b::rowNamed("MC2_TERRAIN_LOD_CHUNK_DIST_SCALE");
    CHECK(asFloat(gate::detail::parseRow(dist, "0.05")) == 1.0f);
    CHECK(asFloat(gate::detail::parseRow(dist, "0.050001")) == 0.050001f);
    // A clamp row clamps; a reject row falls back to its default.
    const gate::Row& clamp = g1b::rowNamed("MC2_TERRAIN_SHORELINE_STRENGTH");
    const gate::Row& reject = g1b::rowNamed("MC2_MOVER_CONTACT_SHADOW");
    CHECK(asFloat(gate::detail::parseRow(clamp, "9")) == 2.0f);
    CHECK(asFloat(gate::detail::parseRow(reject, "99")) == 0.0f);
    CHECK(asFloat(gate::detail::parseRow(reject, "0.5")) == 0.5f);
}

TEST_CASE("isNonEmpty is the old v && v[0] test, whatever the row kind") {
    EnvVar env("MC2_TERRAIN_AMBIENT_FLOOR");
    gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");
    CHECK_FALSE(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">());
    CHECK_FALSE(gate::isSet<"MC2_TERRAIN_AMBIENT_FLOOR">());
    CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.35f);     // unset reads the default
    env.set("0.5");
    gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");
    CHECK(gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">());
    CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 0.5f);
    env.set("9");
    gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");
    CHECK(gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">() == 2.0f);      // clampRange(0, 2)
    env.set(nullptr);
    gate::invalidate("MC2_TERRAIN_AMBIENT_FLOOR");
}

TEST_CASE("MC2_WATER_SHORE_MODE tokens 0..3 resolve, anything else is the first (empty) token") {
    EnvVar env("MC2_WATER_SHORE_MODE");
    for (const char* v : {"0", "1", "2", "3"}) {
        env.set(v);
        gate::invalidate("MC2_WATER_SHORE_MODE");
        INFO(v);
        CHECK(gate::get<"MC2_WATER_SHORE_MODE">() == gate::Token{static_cast<std::int32_t>(v[0] - '0') + 1});
    }
    for (const char* v : {"4", "01", "x"}) {
        env.set(v);
        gate::invalidate("MC2_WATER_SHORE_MODE");
        INFO(v);
        CHECK(gate::get<"MC2_WATER_SHORE_MODE">() == gate::Token{0});
    }
    env.set(nullptr);
    gate::invalidate("MC2_WATER_SHORE_MODE");
}

TEST_CASE("the table rules reject a Tri row without a parent, an inherits on a non-Tri row, and a bad parent") {
    using namespace gate::rows;
    using gate::Row;
    constexpr Row umb = G("MC2_UNITTEST_UMB", Bool, false, "t", "", "d");  // Std, not Exact1
    constexpr Row umb1 = L(Exact1, "MC2_UNITTEST_UMB1", Bool, false, "t", "", "d");
    constexpr Row okSub = G("MC2_UNITTEST_SUB", Kind::Tri, false, "t", "", "d", inherits("MC2_UNITTEST_UMB1"));
    constexpr Row noParent = G("MC2_UNITTEST_SUB", Kind::Tri, false, "t", "", "d");
    constexpr Row boolWithParent = G("MC2_UNITTEST_BOOL", Bool, false, "t", "", "d", inherits("MC2_UNITTEST_UMB1"));
    constexpr Row badParentSub = G("MC2_UNITTEST_SUB", Kind::Tri, false, "t", "", "d", inherits("MC2_UNITTEST_UMB"));
    constexpr Row grandchild = G("MC2_UNITTEST_SUB2", Kind::Tri, false, "t", "", "d", inherits("MC2_UNITTEST_SUB"));
    constexpr Row good[] = {umb1, okSub};
    constexpr Row missing[] = {okSub};
    constexpr Row wrongRule[] = {badParentSub, umb};   // the parent is a Std Bool row, not Exact1
    constexpr Row chain[] = {umb1, okSub, grandchild};  // a parent with a parent of its own
    CHECK(rowIsSane(okSub));
    CHECK_FALSE(rowIsSane(noParent));
    CHECK_FALSE(rowIsSane(boolWithParent));
    CHECK(parentsAreValid(good, 2));
    CHECK_FALSE(parentsAreValid(missing, 1));
    CHECK_FALSE(parentsAreValid(wrongRule, 2));
    CHECK_FALSE(parentsAreValid(chain, 3));
}

TEST_CASE("the VK opt-in and kill-switch rows read like the envTriState expressions they replace") {
    // Verbatim old reader: -1 unset or any text other than exactly "0"/"1".
    auto old = [](const char* v) {
        if (!v || v[0] == '\0' || v[1] != '\0') return -1;
        return v[0] == '1' ? 1 : v[0] == '0' ? 0 : -1;
    };
    constexpr const char* kProbe[] = {nullptr, "", "0", "false", "1", "yes", "10", "2.5", "00", "0x", "01"};
    const char* exact1[] = {"MC2_VK_MECH_FEED_AUDIT", "MC2_VK_POST_HZB", "MC2_VK_POST_SCREENFX", "MC2_VK_POST_SSAO",
                            "MC2_VK_TERRAIN_LODFADE",
                            "MC2_VK_TERRAIN_VISUALDISPLACE_FAR"};
    const char* killSwitch[] = {"MC2_VK_LIGHTFEED_DEDUP", "MC2_VK_MECH_FENCE_DEFER", "MC2_VK_POST_SCREENFX_CLOUD",
                                "MC2_VK_POST_SCREENFX_SHADOW", "MC2_VK_POST_SCREENFX_SHORELINE",
                                "MC2_VK_PROP_FENCE_DEFER", "MC2_VK_TERRAIN_FENCE_DEFER", "MC2_VK_WATER_FENCE_DEFER"};
    for (const char* name : exact1) {
        const gate::Row& row = g1b::rowNamed(name);
        for (const char* v : kProbe) {
            INFO(name << " = " << (v != nullptr ? v : "<unset>"));
            CHECK(rowValue(row, v) == (old(v) == 1));
        }
    }
    for (const char* name : killSwitch) {
        const gate::Row& row = g1b::rowNamed(name);
        CHECK(row.defBool);
        for (const char* v : kProbe) {
            INFO(name << " = " << (v != nullptr ? v : "<unset>"));
            const bool oldOn = old(v) != 0;
            // The one documented edge: a leading '0' followed by more text ("00", "0x") now also turns it off.
            const bool edge = v != nullptr && v[0] == '0' && v[1] != '\0';
            CHECK(rowValue(row, v) == (edge ? false : oldOn));
        }
    }
    // MC2_VK_MECH_FEED_NATIVE: "was it set to exactly 0 or 1".
    for (const char* name : {"MC2_VK_MECH_FEED_NATIVE"}) {
        const gate::Row& row = g1b::rowNamed(name);
        for (const char* v : kProbe) {
            INFO(name << " = " << (v != nullptr ? v : "<unset>"));
            CHECK(rowValue(row, v) == (old(v) != -1));
        }
    }
}

TEST_CASE("MC2_FX_FORCE_SPAWN is an Enum row whose first token is the unset/unknown default") {
    // The other four level gates are covered by "G6 level-valued gates are Enum rows".
    const gate::Row& row = g1b::rowNamed("MC2_FX_FORCE_SPAWN");
    CHECK(row.kind == Kind::Enum);
    CHECK(row.defStr.empty());
    for (const char* t : {"0", "1", "2", "9", "loop"})
        CHECK(gate::rows::tokenListHas(row.tokens, t));
    const auto idx = [&](const char* v) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(row, v)));
    };
    CHECK(idx(nullptr) == 0);
    CHECK(idx("loop") > 0);
    CHECK(idx("zz") == 0);
}

// ---------------------------------------------------------------------------
// G6: mclib rows. The old readers are transcribed with the environment text as a parameter.
// ---------------------------------------------------------------------------

namespace g6 {
float oldMoverFwd(const char* v) {
    float f = 24.0f;
    if (v && v[0]) f = static_cast<float>(std::atof(v));
    if (f < 0.0f) f = 0.0f;
    if (f > 120.0f) f = 120.0f;
    return f;
}
float oldCullMargin(const char* v) {
    const float m = (v && v[0]) ? static_cast<float>(std::atof(v)) : 0.25f;
    return (m >= 0.0f) ? m : 0.25f;
}
long oldNightFloor(const char* v) {
    if (!v || !v[0]) return 0L;
    long f = std::atol(v);
    if (f < 0) f = 0;
    if (f > 64) f = 64;
    return f;
}
float oldClampKnob(const char* v, float dflt, float lo, float hi) {
    float f = dflt;
    if (v && v[0]) f = static_cast<float>(std::atof(v));
    if (f < lo) return lo;
    if (f > hi) return hi;
    return f;
}
}  // namespace g6

TEST_CASE("G6 numeric rows reproduce the old mclib readers on every probe") {
    const gate::Row& fwd = g1b::rowNamed("MC2_LIGHT_POOL_MOVER_FWD");
    const gate::Row& margin = g1b::rowNamed("MC2_SHADOW_CASTER_CULL_MARGIN");
    const gate::Row& floor = g1b::rowNamed("MC2_NIGHT_AMBIENT_FLOOR");
    for (const char* t : g1b::kProbes) {
        INFO((t ? t : "(unset)"));
        if (t && std::string(t) == "1e999") continue;   // inf: listed EDGE for the float rows (no harness sets it)
        if (t && std::string(t) == "-1e999") continue;
        CHECK(asFloat(gate::detail::parseRow(fwd, t)) == g6::oldMoverFwd(t));
        CHECK(asFloat(gate::detail::parseRow(margin, t)) == g6::oldCullMargin(t));
        CHECK(static_cast<long>(g1b::intOf(floor, t)) == g6::oldNightFloor(t));
    }
}

TEST_CASE("G6 clamped float knobs reproduce the old lambda readers (defaults and bounds included)") {
    struct Knob { const char* name; float dflt; float lo; float hi; };
    const Knob knobs[] = {
        {"MC2_CAMERA_SMOOTH", 100.0f, 0.0f, 2000.0f},
        {"MC2_CONE_POOL_REACH", 4.0f, 1.0f, 16.0f},
        {"MC2_LIGHT_CONE_GLOW_ALPHA", 40.0f, 0.0f, 255.0f},
        {"MC2_LIGHT_CONE_SPILL", 1.0f, 0.0f, 3.0f},
        {"MC2_LIGHT_REALISM_CONE_OPACITY", 0.40f, 0.0f, 4.0f},
        {"MC2_LIGHT_REALISM_CONE_FEATHER", 2.5f, 0.0f, 8.0f},
        {"MC2_LIGHT_REALISM_BEACON", 70.0f, 0.0f, 255.0f},
        {"MC2_LIGHT_REALISM_BEACON_RADIUS", 40.0f, 1.0f, 200.0f},
    };
    for (const Knob& k : knobs) {
        const gate::Row& row = g1b::rowNamed(k.name);
        CHECK(row.kind == Kind::Float);
        CHECK(row.range.clamp);
        for (const char* t : g1b::kProbes) {
            INFO(k.name << " <- " << (t ? t : "(unset)"));
            CHECK(asFloat(gate::detail::parseRow(row, t)) == g6::oldClampKnob(t, k.dflt, k.lo, k.hi));
        }
    }
}

TEST_CASE("G6 String rows keep path and text values verbatim") {
    for (const char* name : {"MC2_ABL_COMPILE_FATAL_REPRO", "MC2_OVERLAY_TILE_HIRES", "MC2_MECH_IMPORT_FORCE_CLIP",
                             "MC2_MECH_SKEL_BONE_DUMP", "MC2_LOG_FILE_RESOLVE", "MC2_RESOLVE_TRACE_FILE"}) {
        INFO(name);
        const gate::Row& row = g1b::rowNamed(name);
        CHECK(row.kind == Kind::String);
        CHECK(std::string(row.defStr).empty());
    }
    CHECK(std::string(g1b::rowNamed("MC2_MECH_IMPORT_FORCE_BONE").defStr) == "j_LThigh");
}

TEST_CASE("G6 the mod-cache refresh flag is re-read after an in-process putenv") {
    const gate::Row& row = g1b::rowNamed("MC2_REBUILD_MOD_CACHE");
    CHECK(row.reread);
    CHECK(row.rule == Rule::Presence);
}

TEST_CASE("G6 predicate enums accept their documented spellings without a badvalue") {
    struct Spell { const char* name; const char* word; };
    const Spell spell[] = {
        {"MC2_OBJECT_ADMISSION_PREDICATE", "modern"}, {"MC2_EFFECT_ADMISSION_PREDICATE", "modern"},
        {"MC2_LIGHTING_SHADOW_PREDICATE_MODE", "Modern"}, {"MC2_DEBUG_OVERLAY_PREDICATE_MODE", "Modern"},
        {"MC2_SELECTION_PICKING_PREDICATE_MODE", "Modern"}, {"MC2_SCREENXY_PREDICATE_MODE", "Modern"},
        {"MC2_PROJECTZ_BYPASS_MODE", "Bypass"},
    };
    for (const Spell& s : spell) {
        INFO(s.name);
        const gate::Row& r = g1b::rowNamed(s.name);
        CHECK(r.kind == Kind::Enum);
        CHECK((gate::detail::parseRow(r, s.word) & gate::detail::kBadBit) == 0);
        CHECK(g1b::intOf(r, s.word) != 0);   // a named level, not the default token
    }
}

TEST_CASE("G6 level-valued gates are Enum rows with the level tokens first") {
    struct Want { const char* name; const char* tokens; const char* def; };
    const Want want[] = {
        {"MC2_SHADOW_DYNAMIC_PROP_CASTERS", "1|0|2", "1"},
        {"MC2_SPOTLIGHT_DIRECTION_FIX", "0|1|2", "0"},
    };
    for (const Want& w : want) {
        INFO(w.name);
        const gate::Row& r = g1b::rowNamed(w.name);
        CHECK(r.kind == Kind::Enum);
        CHECK(std::string(r.tokens) == w.tokens);
        CHECK(std::string(r.defStr) == w.def);
        // unset reads the default token (index 0); an unknown token reads the default and is flagged bad.
        CHECK(g1b::intOf(r, nullptr) == 0);
        CHECK(g1b::intOf(r, "9") == 0);
    }
    CHECK(g1b::intOf(g1b::rowNamed("MC2_SHADOW_DYNAMIC_PROP_CASTERS"), "0") == 1);
}

TEST_CASE("G6 string and presence rows") {
    const gate::Row& color = g1b::rowNamed("MC2_LIGHT_POOL_MOVER_COLOR");
    CHECK(color.kind == Kind::String);
    CHECK(std::string(color.defStr).empty());
    const gate::Row& burnin = g1b::rowNamed("MC2_BURNIN_NO_JPG");
    CHECK(burnin.rule == Rule::Presence);
    CHECK_FALSE(rowValue(burnin, nullptr));
    CHECK(rowValue(burnin, "0"));
    CHECK(rowValue(burnin, ""));
}

TEST_CASE("raw<> echoes the environment text, nullptr when unset, without caching") {
    EnvVar env("MC2_BOX_DECAL");
    env.set(nullptr);
    CHECK(gate::raw<"MC2_BOX_DECAL">() == nullptr);
    env.set("");
    gate::invalidate("MC2_BOX_DECAL");
    CHECK(gate::get<"MC2_BOX_DECAL">() == false);
    env.set("yes");
    CHECK(std::string(gate::raw<"MC2_BOX_DECAL">()) == "yes");   // fresh even though get<> cached
    env.set(nullptr);
    gate::invalidate("MC2_BOX_DECAL");
}

// ---------------------------------------------------------------------------
// G5: gos_postprocess.cpp rows
// ---------------------------------------------------------------------------

TEST_CASE("G5 postprocess clamp rows reproduce the clamping lambdas they replaced") {
    auto fl = [&](const char* name, const char* text) { return asFloat(gate::detail::parseRow(g1b::rowNamed(name), text)); };
    auto in = [&](const char* name, const char* text) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(g1b::rowNamed(name), text)));
    };
    CHECK(in("MC2_SHADOW_CSM_COUNT", nullptr) == 3);
    CHECK(in("MC2_SHADOW_CSM_COUNT", "0") == 1);
    CHECK(in("MC2_SHADOW_CSM_COUNT", "9") == 3);
    CHECK(in("MC2_SHADOW_CSM_COUNT", "2") == 2);
    CHECK(fl("MC2_SHADOW_CSM_LAMBDA", "-1") == 0.0f);
    CHECK(fl("MC2_SHADOW_CSM_LAMBDA", "7") == 1.0f);
    CHECK(fl("MC2_SHADOW_CSM_LAMBDA", "") == 0.5f);
    CHECK(fl("MC2_SHADOW_CSM_SOFTNESS", "0.01") == 0.1f);      // the old test was `s < 0.1f`
    CHECK(fl("MC2_SHADOW_CSM_SOFTNESS", "99") == 6.0f);
    CHECK(fl("MC2_SHADOW_DIST_FADE", "1e9") == 100000.0f);
    CHECK(fl("MC2_SHADOW_DIST_FADE_MAX", "2") == 1.0f);
    CHECK(fl("MC2_SHADOW_OBJ_NORMAL_BIAS", "30") == 16.0f);
    CHECK(fl("MC2_SHADOW_MECH_SOFT", "-3") == 0.0f);
    // the reader fell back to 2500 for <= 0
    CHECK(fl("MC2_SHADOW_BOUNDED_NEAR_RADIUS", "0") == 2500.0f);
    CHECK(fl("MC2_SHADOW_BOUNDED_NEAR_RADIUS", "-5") == 2500.0f);
    CHECK(fl("MC2_SHADOW_BOUNDED_NEAR_RADIUS", "800") == 800.0f);
    // snapped and lower-bound-only knobs keep unranged rows: the site does the snapping / flooring
    CHECK(in("MC2_SHADOW_MAP_SIZE", "5000") == 5000);
    CHECK(in("MC2_SHADOW_MAP_SIZE", nullptr) == 8192);
    CHECK(fl("MC2_SHADOW_CSM_R0", "10") == 10.0f);
}

TEST_CASE("G5 postprocess text rows and the backend selector") {
    const gate::Row& be = g1b::rowNamed("MC2_POSTPROCESS_BACKEND");
    CHECK(be.kind == Kind::Enum);
    CHECK(gate::rows::tokenListHas(be.tokens, "vk"));
    CHECK(gate::rows::tokenListHas(be.tokens, "vulkan"));
    for (const char* name : {"MC2_SHADOW_DEBUG_MODE", "MC2_VIEW_MODE", "MC2_OOB_FOG_COLOR"})
        CHECK(g1b::rowNamed(name).kind == Kind::String);
}

// ---------------------------------------------------------------------------
// G5: gos_static_prop_batcher.cpp rows
// ---------------------------------------------------------------------------

TEST_CASE("G5 static-prop clamp rows reproduce the lambdas they replaced") {
    auto fl = [&](const char* name, const char* text) { return asFloat(gate::detail::parseRow(g1b::rowNamed(name), text)); };
    auto in = [&](const char* name, const char* text) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(g1b::rowNamed(name), text)));
    };
    CHECK(fl("MC2_FAR_BACKDROP_LIT_NEAR", nullptr) == 700.0f);
    CHECK(fl("MC2_FAR_BACKDROP_LIT_FAR", nullptr) == 960.0f);
    CHECK(fl("MC2_OBJECT_POLY_OFFSET_FACTOR", nullptr) == -1.0f);
    // texture tier and the billboard mode keep their snapping / first-character logic at the site
    CHECK(in("MC2_STATICPROP_TEX_TIER", nullptr) == 512);
    CHECK(in("MC2_STATICPROP_TEX_TIER", "300") == 300);
    CHECK(g1b::rowNamed("MC2_LIGHT_BILLBOARD_FACING").kind == Kind::String);
    CHECK(g1b::rowNamed("MC2_FAR_BACKDROP_LIT_TINT").kind == Kind::String);
}

// ---------------------------------------------------------------------------
// G5: gameos_graphics.cpp rows
// ---------------------------------------------------------------------------

TEST_CASE("G5 graphics rows reproduce the lambdas and helpers they replaced") {
    auto fl = [&](const char* name, const char* text) { return asFloat(gate::detail::parseRow(g1b::rowNamed(name), text)); };
    // `v > 0 ? v : default` readers: a non-positive value falls back to the default
    CHECK(fl("MC2_WATER_SHORE_FEATHER_WU", nullptr) == 7.0f);
    CHECK(fl("MC2_WATER_SHORE_FEATHER_WU", "0") == 7.0f);
    CHECK(fl("MC2_WATER_SHORE_FEATHER_WU", "-3") == 7.0f);
    CHECK(fl("MC2_WATER_SHORE_FEATHER_WU", "12") == 12.0f);
    CHECK(fl("MC2_WATER_EDGE_FEATHER_STRENGTH", "0") == 1.0f);
    CHECK(fl("MC2_WATER_EDGE_FEATHER_STRENGTH", "2.5") == 2.5f);
    CHECK(fl("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN", nullptr) == 0.78f);
    CHECK(fl("MC2_TERRAIN_DETAIL_FADE_START", nullptr) == 1000.0f);
    CHECK(fl("MC2_TERRAIN_DETAIL_FADE_END", nullptr) == 3000.0f);
    CHECK(fl("MC2_TERRAIN_DETAIL_FADE_END", "") == 3000.0f);
    // out of range -> default (the old `f >= 0 && f <= 1 ? f : 0.25f`)
    CHECK(fl("MC2_LIGHT_POOL_MOVER_DIM", "2") == 0.25f);
    CHECK(fl("MC2_LIGHT_POOL_MOVER_DIM", "0.5") == 0.5f);
    CHECK(g1b::rowNamed("MC2_WATER_SHINE").kind == Kind::String);
}

// ---------------------------------------------------------------------------
// G5: gameosmain.cpp rows
// ---------------------------------------------------------------------------

TEST_CASE("G5 gameosmain rows and the runtime-name reader") {
    auto in = [&](const char* name, const char* text) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(gate::detail::parseRow(g1b::rowNamed(name), text)));
    };
    CHECK(in("MC2_SCREENSHOT_AT_FRAME", nullptr) == -1);      // the reader's "no screenshot" sentinel
    CHECK(in("MC2_SCREENSHOT_AT_FRAME", "300") == 300);
    CHECK(in("MC2_FPS_CAP", nullptr) == 90);
    CHECK(in("MC2_FPS_CAP", "0") == 0);
    CHECK(in("MC2_WIDTH", nullptr) == 0);
    CHECK_FALSE(g1b::rowNamed("MC2_STATIC_UPDATE_TRACE").defBool);
    CHECK(g1b::rowNamed("MC2_SCREENSHOT_PATH").kind == Kind::String);
    CHECK(g1b::rowNamed("MC2_LOAD_RACE_PROBE_FRAMES").kind == Kind::String);
    CHECK(gate::rows::tokenListHas(g1b::rowNamed("MC2_SCREENSHOT_SOURCE").tokens, "backbuffer"));

    EnvVar env("MC2_BOX_DECAL");   // envText reads any name at run time, uncached
    env.set(nullptr);
    CHECK(gate::envText("MC2_BOX_DECAL") == nullptr);
    env.set("1");
    CHECK(std::string(gate::envText("MC2_BOX_DECAL")) == "1");
    env.set(nullptr);
}

TEST_CASE("boolState answers by the row's own rule for a gate named at run time") {
    CHECK_FALSE(gate::boolState("MC2_NOT_A_GATE").has_value());
    EnvVar kill("MC2_SHADOW_ENABLE");          // Lead0Kill, default ON
    kill.set(nullptr);
    gate::invalidate("MC2_SHADOW_ENABLE");
    auto st = gate::boolState("MC2_SHADOW_ENABLE");
    REQUIRE(st.has_value());
    CHECK_FALSE(st->set);
    CHECK(st->on);
    kill.set("0");
    gate::invalidate("MC2_SHADOW_ENABLE");
    st = gate::boolState("MC2_SHADOW_ENABLE");
    CHECK(st->set);
    CHECK_FALSE(st->on);
    kill.set(nullptr);
    gate::invalidate("MC2_SHADOW_ENABLE");
    EnvVar str("MC2_VULKAN_SPV_DIR");          // not a Bool row: on = set and not a leading 0
    str.set("shaders/x");
    st = gate::boolState("MC2_VULKAN_SPV_DIR");
    CHECK((st->set && st->on));
    str.set(nullptr);
}

}  // TEST_SUITE

TEST_CASE("MC2_VFX_DISTORT_AMP accepts (0, 0.5) like the `a > 0 && a < 0.5f` reader it replaced") {
    using namespace gate::rows;
    const gate::Row row = P("MC2_UNITTEST_DAMP", Float, 0.04, "test", "", "doc", rangeAbove(0.0, 0.4999999701976776));
    auto val = [&](const char* t) { return asFloat(gate::detail::parseRow(row, t)); };
    CHECK(val("0.25") == 0.25f);
    CHECK(val("0.49999997") == 0.49999997f);
    for (const char* t : {"0.5", "0.51", "0", "-0.1", "abc"}) {
        INFO(t);
        CHECK(val(t) == 0.04f);
    }
}

// mc2::mlr_gate: the MC2_DISABLE_GOSFX latch is a thread-safe function-local static (first use in this process is here).
TEST_CASE("mlr_gate latches the env value on first use and every thread sees it")
{
    _putenv_s("MC2_DISABLE_GOSFX", "1");
    mc2::gate::invalidateAll();

    CHECK(mc2::mlr_gate::is_disabled());

    _putenv_s("MC2_DISABLE_GOSFX", "0");            // after the first call the value no longer moves
    mc2::gate::invalidateAll();
    CHECK(mc2::mlr_gate::is_disabled());

    std::vector<std::thread> threads;
    std::vector<int> seen(8, 0);
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&seen, i] { seen[i] = mc2::mlr_gate::is_disabled() ? 1 : 0; });
    for (auto &t : threads) t.join();
    for (int v : seen) CHECK(v == 1);

    _putenv_s("MC2_DISABLE_GOSFX", "");
    mc2::gate::invalidateAll();
}

// ---------------------------------------------------------------------------
// options.cfg render options (Gates/render_options.h): defaults, bounds, garbage rejection and the override channel
// ---------------------------------------------------------------------------

TEST_CASE("render options default to what the gate rows they replaced defaulted to, except StaticPropAmbient, which is ON") {
    const mc2::options::RenderOptions d{};
    CHECK_FALSE(d.postCas);
    CHECK(d.postCasSharpness == 0.5f);
    CHECK(d.tonemapMode == 0);
    CHECK(d.shadowCsm);
    CHECK(d.shadowBoundedNearFit);
    CHECK(d.propShadowReceive == 0);
    CHECK(d.shorelineLegacySuppress);
    CHECK(d.cpuPhongRetire);
    CHECK(d.staticPropAmbient);        // the flat fill is ON; the PBR specular below is OFF (owner ruling 2026-10-13: most props carry no PBR data)
    CHECK(d.staticPropIblSh);
    CHECK_FALSE(d.staticPropIblSunRotate);
    CHECK(d.staticPropIblSunRotateFloor == 0.45f);
    CHECK_FALSE(d.staticPropPbr);
    CHECK_FALSE(d.sunAxisFix);
    CHECK_FALSE(d.sunElevationFix);
    CHECK_FALSE(d.vfxSoftParticles);
    CHECK_FALSE(d.vfxLitParticles);
}

TEST_CASE("render option keys are unique and findable") {
    namespace mo = mc2::options;
    for (std::size_t i = 0; i < mo::kOptionCount; ++i) {
        REQUIRE(mo::find(mo::kOptions[i].key) == &mo::kOptions[i]);
    }
    CHECK(mo::find("NotAnOption") == nullptr);
    CHECK(mo::find("shadowcsm") == mo::find("ShadowCSM"));   // keys are case-insensitive like options.cfg
    CHECK(mo::find("PostCAS") != mo::find("PostCASSharpness"));
    CHECK(mo::kOptionCount == 17);
}

TEST_CASE("render option values outside their bounds, NaN and garbage are rejected and change nothing") {
    namespace mo = mc2::options;
    mo::RenderOptions o{};
    const mo::Option& sharp = *mo::find("PostCASSharpness");
    CHECK(mo::setFloat(o, sharp, 0.8f) == mo::Verdict::Ok);
    CHECK(o.postCasSharpness == 0.8f);
    CHECK(mo::setFloat(o, sharp, 1.5f) == mo::Verdict::OutOfRange);
    CHECK(mo::setFloat(o, sharp, -0.1f) == mo::Verdict::OutOfRange);
    CHECK(mo::setFloat(o, sharp, std::numeric_limits<float>::quiet_NaN()) == mo::Verdict::Garbage);
    CHECK(mo::setFloat(o, sharp, std::numeric_limits<float>::infinity()) == mo::Verdict::Garbage);
    CHECK(o.postCasSharpness == 0.8f);
    const mo::Option& tone = *mo::find("TonemapMode");
    CHECK(mo::setLong(o, tone, 3) == mo::Verdict::Ok);
    CHECK(mo::setLong(o, tone, 4) == mo::Verdict::OutOfRange);
    CHECK(mo::setLong(o, tone, -1) == mo::Verdict::OutOfRange);
    CHECK(o.tonemapMode == 3);
    const mo::Option& floor = *mo::find("StaticPropIblSunRotateFloor");
    CHECK(mo::setFloat(o, floor, 2.0f) == mo::Verdict::Ok);
    CHECK(mo::setFloat(o, floor, 9.0f) == mo::Verdict::OutOfRange);
    CHECK(o.staticPropIblSunRotateFloor == 2.0f);
    const mo::Option& recv = *mo::find("PropShadowReceive");
    CHECK(mo::setLong(o, recv, 2) == mo::Verdict::OutOfRange);
    CHECK(o.propShadowReceive == 0);
}

TEST_CASE("the MC2_OPTIONS_OVERRIDE grammar applies good entries and reports every bad one") {
    namespace mo = mc2::options;
    mo::RenderOptions o{};
    std::vector<mo::Reject> rej;
    const int n = mo::applyOverrides(o, "shadowcsm=0, STATICPROPPBR=TRUE,PostCASSharpness=nan,TonemapMode=9,Bogus=1,SunAxisFix,VfxLitParticles=maybe", rej);
    CHECK(n == 2);
    CHECK_FALSE(o.shadowCsm);
    CHECK(o.staticPropPbr);
    CHECK(o.postCasSharpness == 0.5f);
    CHECK(o.tonemapMode == 0);
    CHECK_FALSE(o.vfxLitParticles);
    REQUIRE(rej.size() == 5);
    CHECK(rej[0].key == "PostCASSharpness");
    CHECK(rej[1].key == "TonemapMode");
    CHECK(rej[1].why == mo::Verdict::OutOfRange);
    CHECK(rej[2].key == "Bogus");
    CHECK(rej[2].why == mo::Verdict::UnknownKey);   // an unknown key is named as such, not as garbage
    CHECK(rej[3].key == "SunAxisFix");
    CHECK(rej[3].why == mo::Verdict::Garbage);      // a known key without a value
    CHECK(rej[4].key == "VfxLitParticles");
    std::vector<mo::Reject> none;
    CHECK(mo::applyOverrides(o, "", none) == 0);
    CHECK(none.empty());
}

TEST_CASE("render options read before the publish are counted, and publish replaces the values") {
    namespace mo = mc2::options;
    // Start from a process that has not published yet, whatever ran before, and put the shared state back afterwards.
    const mo::RenderOptions savedCurrent = mo::g_current;
    const bool savedPublished = mo::g_published, savedChanged = mo::g_publishChanged;
    const int savedEarly = mo::g_earlyReads;
    const char* const savedFile = mo::g_firstEarlyFile;
    const unsigned savedLine = mo::g_firstEarlyLine;
    mo::g_published = false;
    mo::g_publishChanged = false;
    mo::g_earlyReads = 0;

    (void)mo::current();
    CHECK(mo::earlyReads() == 1);
    CHECK(std::string(mo::firstEarlyFile()).find("test_gate.cpp") != std::string::npos);
    mo::RenderOptions o{};
    o.sunAxisFix = true;
    mo::publish(o);
    CHECK(mo::current().sunAxisFix);
    CHECK(mo::earlyReads() == 1);                 // reads after the publish are not early
    CHECK_FALSE(mo::publishChangedValues());      // the first publish is not a change
    mo::publish(mo::RenderOptions{});
    CHECK(mo::publishChangedValues());            // a second publish with other values is

    mo::g_current = savedCurrent;
    mo::g_published = savedPublished;
    mo::g_publishChanged = savedChanged;
    mo::g_earlyReads = savedEarly;
    mo::g_firstEarlyFile = savedFile;
    mo::g_firstEarlyLine = savedLine;
}

TEST_CASE("options.cfg line editor reads, replaces in place and adds inside the [MechCommander2] block") {
    namespace mo = mc2::options;
    const std::string head = "FITini \r\n\r\n[MechCommander2]\r\nl ResolutionX = 3840\r\nb FullScreen = TRUE\r\nb ShadowCSM = TRUE\r\n";
    const std::string tail = "\r\n[Keys]\r\nl Foo = 1\r\n";
    const std::string file = head + tail;
    CHECK(*mo::cfg::get(file, "ResolutionX") == "3840");
    CHECK(*mo::cfg::get(file, "shadowcsm") == "TRUE");
    CHECK_FALSE(mo::cfg::get(file, "Foo").has_value());   // another block
    CHECK_FALSE(mo::cfg::get(file, "PostCAS").has_value());

    // an existing line is replaced in place, every other byte stays
    const mo::Option& csm = *mo::find("ShadowCSM");
    CHECK(mo::cfg::set(file, csm, "FALSE") == "FITini \r\n\r\n[MechCommander2]\r\nl ResolutionX = 3840\r\nb FullScreen = TRUE\r\nb ShadowCSM = FALSE\r\n" + tail);

    // a missing key lands after the block's last line, before the blank line and the next block
    const mo::Option& pbr = *mo::find("StaticPropPbr");
    CHECK(mo::cfg::set(file, pbr, "TRUE") == head + "b StaticPropPbr = TRUE\r\n" + tail);

    // value types: a float key carries the f prefix, a long key l; a key that is a prefix of another is not confused with it
    const mo::Option& sharp = *mo::find("PostCASSharpness");
    const std::string withSharp = mo::cfg::set(file, sharp, "0.25");
    CHECK(*mo::cfg::get(withSharp, "PostCASSharpness") == "0.25");
    CHECK(withSharp.find("f PostCASSharpness = 0.25") != std::string::npos);
    CHECK_FALSE(mo::cfg::get(withSharp, "PostCAS").has_value());
    CHECK(mo::cfg::set(file, *mo::find("TonemapMode"), "2").find("l TonemapMode = 2") != std::string::npos);

    // LF files stay LF; a last line without a line break still gets the new line on its own line
    const std::string lf = "[MechCommander2]\nb ShadowCSM = TRUE";
    CHECK(mo::cfg::set(lf, pbr, "TRUE") == "[MechCommander2]\nb ShadowCSM = TRUE\nb StaticPropPbr = TRUE\n");

    // no [MechCommander2] block: unchanged (the launcher never invents a file whose resolution lines are missing)
    const std::string other = "[Other]\nb X = TRUE\n";
    CHECK(mo::cfg::set(other, pbr, "TRUE") == other);
    CHECK(mo::cfg::set(std::string{}, pbr, "TRUE").empty());

    // the whole-file load the engine runs before the window exists: valid keys apply, an out-of-range one is skipped, other blocks are ignored
    mo::RenderOptions loaded{};
    const std::string cfgText = "[MechCommander2]\r\nb ShadowCSM = FALSE\r\nl TonemapMode = 7\r\nf PostCASSharpness = 0.75\r\nb SunAxisFix = TRUE\r\n[Keys]\r\nb SunElevationFix = TRUE\r\n";
    CHECK(mo::cfg::loadFromText(loaded, cfgText) == 3);
    CHECK_FALSE(loaded.shadowCsm);
    CHECK(loaded.tonemapMode == 0);
    CHECK(loaded.postCasSharpness == 0.75f);
    CHECK(loaded.sunAxisFix);
    CHECK_FALSE(loaded.sunElevationFix);

    // what the editor writes is what the table's validation accepts
    mo::RenderOptions o{};
    CHECK(mo::setFromText(o, sharp, *mo::cfg::get(withSharp, "PostCASSharpness")) == mo::Verdict::Ok);
    CHECK(o.postCasSharpness == 0.25f);
}

// One strict grammar for options.cfg: CPrefs::load and the pre-window publish both call cfg::loadFromText, so the values the early readers see and
// the lines CPrefs logs come from the same call. Each case is one line of the [MechCommander2] block.
TEST_CASE("options.cfg values follow one strict grammar and every reader sees the same result") {
    namespace mo = mc2::options;
    struct Case { const char* line; const char* key; mo::Verdict want; };
    const Case cases[] = {
        {"b ShadowCSM = banana",           "ShadowCSM",        mo::Verdict::Garbage},
        {"b ShadowCSM = F",                "ShadowCSM",        mo::Verdict::Garbage},
        {"b ShadowCSM = yes",              "ShadowCSM",        mo::Verdict::Garbage},
        {"b ShadowCSM =",                  "ShadowCSM",        mo::Verdict::Garbage},
        {"b ShadowCSM = FALSE ; off",      "ShadowCSM",        mo::Verdict::Garbage},
        {"b ShadowCSM = TRUE TRUE",        "ShadowCSM",        mo::Verdict::Garbage},
        {"l TonemapMode = abc",            "TonemapMode",      mo::Verdict::Garbage},
        {"l TonemapMode = 2 ; AgX",        "TonemapMode",      mo::Verdict::Garbage},
        {"l TonemapMode = 2 // AgX",       "TonemapMode",      mo::Verdict::Garbage},
        {"l TonemapMode = 1.5",            "TonemapMode",      mo::Verdict::Garbage},
        {"l TonemapMode = 0x2",            "TonemapMode",      mo::Verdict::Garbage},
        {"l TonemapMode = 9",              "TonemapMode",      mo::Verdict::OutOfRange},
        {"f PostCASSharpness = 0,45",      "PostCASSharpness", mo::Verdict::Garbage},
        {"f PostCASSharpness = 0.5abc",    "PostCASSharpness", mo::Verdict::Garbage},
        {"f PostCASSharpness = nan",       "PostCASSharpness", mo::Verdict::Garbage},
        {"f PostCASSharpness = inf",       "PostCASSharpness", mo::Verdict::Garbage},
        {"f PostCASSharpness = 7",         "PostCASSharpness", mo::Verdict::OutOfRange},
    };
    for (const Case& c : cases) {
        CAPTURE(c.line);
        const std::string text = std::string("[MechCommander2]\r\n") + c.line + "\r\n";
        mo::RenderOptions viaCPrefs{};
        std::vector<mo::Reject> rejects;
        CHECK(mo::cfg::loadFromText(viaCPrefs, text, &rejects) == 0);
        REQUIRE(rejects.size() == 1);
        CHECK(rejects[0].key == c.key);
        CHECK(rejects[0].why == c.want);
        CHECK(viaCPrefs == mo::RenderOptions{});          // the default is kept
        mo::RenderOptions viaEarlyReader{};               // the pre-window publish passes no reject list
        CHECK(mo::cfg::loadFromText(viaEarlyReader, text) == 0);
        CHECK(viaEarlyReader == viaCPrefs);
    }

    // the accepted spellings
    mo::RenderOptions o{};
    std::vector<mo::Reject> rejects;
    const std::string good = "[MechCommander2]\r\nb ShadowCSM = false\r\nb SunAxisFix = TRUE\r\nb VfxLitParticles = 1\r\nb VfxSoftParticles = 0\r\n"
                             "l TonemapMode = 3\r\nf PostCASSharpness = 0.25\r\nb StaticPropAmbient =   False  \r\n";
    CHECK(mo::cfg::loadFromText(o, good, &rejects) == 7);
    CHECK(rejects.empty());
    CHECK_FALSE(o.shadowCsm);
    CHECK(o.sunAxisFix);
    CHECK(o.vfxLitParticles);
    CHECK_FALSE(o.vfxSoftParticles);
    CHECK(o.tonemapMode == 3);
    CHECK(o.postCasSharpness == 0.25f);
    CHECK_FALSE(o.staticPropAmbient);

    // a duplicate key is read at its first line; a rejected value does not clobber a value the struct already holds
    mo::RenderOptions dup{};
    CHECK(mo::cfg::loadFromText(dup, "[MechCommander2]\nb ShadowCSM = FALSE\nb ShadowCSM = TRUE\n") == 1);
    CHECK_FALSE(dup.shadowCsm);
    mo::RenderOptions keep{};
    keep.tonemapMode = 2;
    std::vector<mo::Reject> keepRejects;
    CHECK(mo::cfg::loadFromText(keep, "[MechCommander2]\nl TonemapMode = abc\n", &keepRejects) == 0);
    CHECK(keep.tonemapMode == 2);
    CHECK(keepRejects.size() == 1);
}

TEST_CASE("the render options serialize as one JSON object of all 17 values for the state dump and the capture sidecar") {
    namespace mo = mc2::options;
    mo::RenderOptions o{};
    const std::string d = mo::toJson(o);
    CHECK(d.rfind("{\"PostCAS\": false, \"PostCASSharpness\": 0.5, \"TonemapMode\": 0, \"ShadowCSM\": true", 0) == 0);
    CHECK(d.find("\"StaticPropIblSunRotateFloor\": 0.45") != std::string::npos);
    CHECK(d.find("\"StaticPropPbr\": false") != std::string::npos);
    CHECK(d.back() == '}');
    CHECK(std::count(d.begin(), d.end(), ':') == 17);
    o.tonemapMode = 3;
    o.staticPropPbr = true;
    o.postCasSharpness = 0.25f;
    const std::string c = mo::toJson(o);
    CHECK(c.find("\"TonemapMode\": 3") != std::string::npos);
    CHECK(c.find("\"StaticPropPbr\": true") != std::string::npos);
    CHECK(c.find("\"PostCASSharpness\": 0.25") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Vulkan device profile (GameOS/gameos/vk_device_profile.h): every item of the table is required, so a device that lacks one is refused and gets nothing enabled
// ---------------------------------------------------------------------------

namespace {
namespace vkp = mc2vk::profile;

// A device that has everything: 1.4 device under a 1.3 instance, every feature bit, the swapchain and memory-budget extensions, roomy limits.
vkp::DeviceCaps fullCaps() {
    vkp::DeviceCaps c;
    c.deviceApi = vkp::makeApi(1, 4);
    c.instanceApi = vkp::makeApi(1, 3);
    c.extensions = {"VK_KHR_swapchain", "VK_EXT_memory_budget"};
    c.timelineSemaphore = c.bufferDeviceAddress = c.descriptorIndexing = c.runtimeDescriptorArray = true;
    c.shaderSampledImageArrayNonUniformIndexing = c.descriptorBindingPartiallyBound = true;
    c.dynamicRendering = c.synchronization2 = true;
    c.independentBlend = true;
    c.maxImageDimension2D = 16384;
    c.maxBoundDescriptorSets = 32;
    c.maxPushConstantsSize = 256;
    c.maxStorageBufferRange = 0xFFFFFFFFull;
    return c;
}

// A 1.2-only device: the 1.3 features exist only behind their KHR extensions.
vkp::DeviceCaps device12Caps(bool withExtensions) {
    vkp::DeviceCaps c = fullCaps();
    c.deviceApi = vkp::makeApi(1, 2);
    c.instanceApi = vkp::makeApi(1, 2);
    if (withExtensions) {
        c.extensions.emplace_back(vkp::kExtDynamicRendering);
        c.extensions.emplace_back(vkp::kExtSynchronization2);
    } else {
        c.dynamicRendering = c.synchronization2 = false;
    }
    return c;
}

bool hasId(const std::vector<std::string>& v, std::string_view id) {
    for (const std::string& s : v)
        if (s == id) return true;
    return false;
}
}  // namespace

TEST_CASE("vk profile: a device with everything is accepted and every facility takes its core route") {
    const vkp::Evaluation e = vkp::evaluate(fullCaps(), true);
    CHECK(e.accepted);
    CHECK(e.missingRequired.empty());
    CHECK(e.missingWanted.empty());
    REQUIRE(e.items.size() == sizeof(vkp::kItemIds) / sizeof(vkp::kItemIds[0]));
    for (size_t i = 0; i < e.items.size(); ++i) CHECK((e.items[i].id == vkp::kItemIds[i]));   // the line lists the table in one order
    CHECK(vkp::routeOf(e, "dynamic_rendering") == vkp::Route::Core);
    CHECK(vkp::routeOf(e, "synchronization2") == vkp::Route::Core);
    CHECK(vkp::routeOf(e, "limits") == vkp::Route::Limit);
    CHECK(vkp::routeOf(e, "no_such_item") == vkp::Route::None);
    CHECK((vkp::routeName(vkp::Route::Core) == "core" && vkp::routeName(vkp::Route::Ext) == "ext" && vkp::routeName(vkp::Route::Limit) == "limit" &&
           vkp::routeName(vkp::Route::None) == "-"));
    CHECK(vkp::routeOf(e, "memory_budget") == vkp::Route::Ext);
    // core: no KHR extension is named; the swapchain of the present window and the memory budget are, in table order
    REQUIRE(e.deviceExtensions.size() == 2);
    CHECK(e.deviceExtensions[0] == "VK_KHR_swapchain");
    CHECK(e.deviceExtensions[1] == "VK_EXT_memory_budget");
    CHECK(vkp::featureEnabled(e, "timelineSemaphore"));
    CHECK(vkp::featureEnabled(e, "bufferDeviceAddress"));
    CHECK(vkp::featureEnabled(e, "descriptorIndexing"));
    CHECK(vkp::featureEnabled(e, "dynamicRendering"));
    CHECK(vkp::featureEnabled(e, "synchronization2"));
}

TEST_CASE("vk profile: on a 1.2 device dynamic rendering and synchronization2 come from their KHR extensions") {
    const vkp::Evaluation e = vkp::evaluate(device12Caps(true), true);
    CHECK(e.accepted);
    CHECK(vkp::routeOf(e, "dynamic_rendering") == vkp::Route::Ext);
    CHECK(vkp::routeOf(e, "synchronization2") == vkp::Route::Ext);
    CHECK(hasId(e.deviceExtensions, vkp::kExtDynamicRendering));
    CHECK(hasId(e.deviceExtensions, vkp::kExtSynchronization2));
    CHECK(e.deviceExtensions.size() == 4);                              // swapchain, the two KHR routes, memory budget
    CHECK(vkp::featureEnabled(e, "dynamicRendering"));
    CHECK(vkp::routeOf(e, "timeline_semaphore") == vkp::Route::Core);   // core in 1.2: no extension to name
}

TEST_CASE("vk profile: a 1.2 device without the KHR extensions is refused and gets nothing enabled") {
    vkp::DeviceCaps c = device12Caps(false);
    const vkp::Evaluation e = vkp::evaluate(c, true);
    CHECK_FALSE(e.accepted);
    CHECK(hasId(e.missingRequired, "dynamic_rendering"));
    CHECK(hasId(e.missingRequired, "synchronization2"));
    CHECK(e.missingWanted.empty());
    CHECK_FALSE(vkp::featureEnabled(e, "dynamicRendering"));
    CHECK_FALSE(vkp::featureEnabled(e, "timelineSemaphore"));           // a refused device is enabled for nothing, not even the bits it has
    CHECK(e.enabledFeatures.empty());
    CHECK(e.deviceExtensions.empty());
    // A feature bit with no extension and no core version behind it is not a route either.
    c.dynamicRendering = true;
    const vkp::Evaluation bit = vkp::evaluate(c, true);
    CHECK(hasId(bit.missingRequired, "dynamic_rendering"));
    CHECK_FALSE(bit.accepted);
}

TEST_CASE("vk profile: each of the five facilities of the owner ruling, and independent blend, is required") {
    struct Row { const char* id; void (*drop)(vkp::DeviceCaps&); };
    const Row rows[] = {
        {"timeline_semaphore", [](vkp::DeviceCaps& c) { c.timelineSemaphore = false; }},
        {"buffer_device_address", [](vkp::DeviceCaps& c) { c.bufferDeviceAddress = false; }},
        {"descriptor_indexing", [](vkp::DeviceCaps& c) { c.descriptorIndexing = false; }},
        {"dynamic_rendering", [](vkp::DeviceCaps& c) { c.dynamicRendering = false; }},
        {"synchronization2", [](vkp::DeviceCaps& c) { c.synchronization2 = false; }},
        {"independent_blend", [](vkp::DeviceCaps& c) { c.independentBlend = false; }},
    };
    for (const Row& r : rows) {
        vkp::DeviceCaps c = fullCaps();
        r.drop(c);
        const vkp::Evaluation e = vkp::evaluate(c, true);
        CHECK_FALSE(e.accepted);
        REQUIRE(e.missingRequired.size() == 1);
        CHECK(e.missingRequired[0] == r.id);
        CHECK(e.missingWanted.empty());
        const std::string line = vkp::formatLine("gpu", c, e);
        CHECK(line.find(std::string(" required_missing=") + r.id + " ") != std::string::npos);
        CHECK(line.find("verdict=refused") != std::string::npos);
        CHECK(line.find(" enabled=- extensions=-") != std::string::npos);
    }
}

TEST_CASE("vk profile: a device or instance below 1.2 is refused") {
    vkp::DeviceCaps c = fullCaps();
    c.deviceApi = vkp::makeApi(1, 1);
    vkp::Evaluation e = vkp::evaluate(c, true);
    CHECK_FALSE(e.accepted);
    CHECK(hasId(e.missingRequired, "api_1_2"));
    CHECK_FALSE(vkp::featureEnabled(e, "timelineSemaphore"));           // no 1.2 feature struct is valid on a 1.1 device
    c = fullCaps();
    c.instanceApi = vkp::makeApi(1, 1);                                 // the application asked for 1.1: 1.2 functionality is off limits
    e = vkp::evaluate(c, true);
    CHECK_FALSE(e.accepted);
    CHECK(hasId(e.missingRequired, "api_1_2"));
    CHECK(vkp::formatLine("old gpu", c, e).rfind("[VK_PROFILE v1] verdict=refused device=\"old gpu\" ", 0) == 0);
}

TEST_CASE("vk profile: the swapchain is required only when the context drives a present window") {
    vkp::DeviceCaps c = fullCaps();
    c.extensions.clear();
    const vkp::Evaluation headless = vkp::evaluate(c, false);
    CHECK(headless.accepted);
    CHECK_FALSE(hasId(headless.deviceExtensions, vkp::kExtSwapchain));    // a context with no window never names the swapchain
    CHECK(hasId(headless.missingWanted, "swapchain"));                  // reported, never a refusal
    const vkp::Evaluation e = vkp::evaluate(c, true);
    CHECK_FALSE(e.accepted);
    CHECK(hasId(e.missingRequired, "swapchain"));
}

TEST_CASE("vk profile: each limit is required") {
    vkp::DeviceCaps c = fullCaps();
    c.maxImageDimension2D = 4096;                                       // below the 8192 shadow map
    CHECK(hasId(vkp::evaluate(c, true).missingRequired, "limits"));
    c = fullCaps();
    c.maxBoundDescriptorSets = 3;
    CHECK(hasId(vkp::evaluate(c, true).missingRequired, "limits"));
    c = fullCaps();
    c.maxPushConstantsSize = 64;
    CHECK(hasId(vkp::evaluate(c, true).missingRequired, "limits"));
    c = fullCaps();
    c.maxStorageBufferRange = 1u << 20;
    CHECK(hasId(vkp::evaluate(c, true).missingRequired, "limits"));
    CHECK(vkp::evaluate(fullCaps(), true).missingRequired.empty());
}

TEST_CASE("vk profile: descriptor indexing needs the umbrella bit and every individual bit the descriptor table uses") {
    for (int missing = 0; missing < 4; ++missing) {
        vkp::DeviceCaps c = fullCaps();
        if (missing == 0) c.descriptorIndexing = false;
        if (missing == 1) c.runtimeDescriptorArray = false;
        if (missing == 2) c.shaderSampledImageArrayNonUniformIndexing = false;
        if (missing == 3) c.descriptorBindingPartiallyBound = false;
        const vkp::Evaluation e = vkp::evaluate(c, true);
        CHECK_FALSE(e.accepted);                                        // the bindless table needs the whole group
        CHECK(hasId(e.missingRequired, "descriptor_indexing"));
        CHECK_FALSE(vkp::featureEnabled(e, "runtimeDescriptorArray"));  // none of the group is enabled when one is missing
    }
}

TEST_CASE("vk profile: the memory-budget extension is optional: reported when missing, enabled when present") {
    vkp::DeviceCaps c = fullCaps();
    c.extensions = {"VK_KHR_swapchain"};
    const vkp::Evaluation e = vkp::evaluate(c, true);
    CHECK(e.accepted);
    CHECK(hasId(e.missingWanted, "memory_budget"));
    CHECK(e.missingRequired.empty());
    CHECK(vkp::routeOf(e, "memory_budget") == vkp::Route::None);
    CHECK_FALSE(hasId(e.deviceExtensions, vkp::kExtMemoryBudget));
    const std::string line = vkp::formatLine("gpu", c, e);
    CHECK(line.find(" wanted_missing=memory_budget ") != std::string::npos);
    CHECK(line.find(" extensions=VK_KHR_swapchain") != std::string::npos);
    CHECK(vkp::evaluate(fullCaps(), true).missingWanted.empty());
}

TEST_CASE("vk profile: the line names the device, both versions, what is missing and what is enabled") {
    const vkp::DeviceCaps c = fullCaps();
    const vkp::Evaluation e = vkp::evaluate(c, true);
    const std::string line = vkp::formatLine("AMD Radeon RX 7900 XTX", c, e);
    CHECK(line == "[VK_PROFILE v1] verdict=ok device=\"AMD Radeon RX 7900 XTX\" api=1.4.0 instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- "
                  "items=api_1_2:core,swapchain:ext,limits:limit,timeline_semaphore:core,buffer_device_address:core,descriptor_indexing:core,"
                  "dynamic_rendering:core,synchronization2:core,independent_blend:core,memory_budget:ext enabled=timelineSemaphore,bufferDeviceAddress,"
                  "descriptorIndexing,runtimeDescriptorArray,shaderSampledImageArrayNonUniformIndexing,descriptorBindingPartiallyBound,dynamicRendering,"
                  "synchronization2,independentBlend "
                  "extensions=VK_KHR_swapchain,VK_EXT_memory_budget");
    CHECK(vkp::floorText() == "1.2");                                   // the line states the floor of the table it was evaluated against
    const vkp::Evaluation lacking = vkp::evaluate(device12Caps(false), true);
    CHECK(vkp::formatLine("gpu", device12Caps(false), lacking).find(" required_missing=dynamic_rendering,synchronization2 wanted_missing=- ") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Vulkan validation-layer line (GameOS/gameos/vk_validation_line.h): one message is always one line the smoke parser can read
// ---------------------------------------------------------------------------

TEST_CASE("vk validation line: a message becomes one line with a clean id and a quote-free text") {
    using mc2vk::formatValidationLine;
    CHECK(formatValidationLine("error", "VUID-vkCmdDraw-None-02699", "vkCmdDraw(): set 0 not updated") ==
          "[VK_VALIDATION v1] severity=error id=VUID-vkCmdDraw-None-02699 text=\"vkCmdDraw(): set 0 not updated\"");
    // a multi-line message with quotes and tabs stays on one line and keeps its closing quote unambiguous
    CHECK(formatValidationLine("warning", "UNASSIGNED id with spaces", "line one\nline \"two\"\r\n\tline three") ==
          "[VK_VALIDATION v1] severity=warning id=UNASSIGNED_id_with_spaces text=\"line one line 'two'   line three\"");
    CHECK(formatValidationLine("info", "", "x") == "[VK_VALIDATION v1] severity=info id=- text=\"x\"");
}

TEST_CASE("vk validation line: a very long message is capped") {
    const std::string line = mc2vk::formatValidationLine("error", "id", std::string(5000, 'a'));
    CHECK(line.size() == std::string("[VK_VALIDATION v1] severity=error id=id text=\"\"").size() + mc2vk::kValidationTextCap);
    CHECK(line.back() == '"');
}
