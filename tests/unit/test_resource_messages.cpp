// Typed resource messages (mclib/resource_messages.h, GameOS/gameos/MC2Strings.{h,cpp}).
//
// The REAL catalog is loaded: MC2Strings.cpp is linked into mc2_tests and reads data/defs/text/en_us from the
// checkout, so every expectation below is the shipped template, not a stub. Only two seams are scripted:
// EnumerateModFitFiles (the mod index, so LoadModFits can be driven) and a one-line gos_FormatResourceMessage
// forwarder (gameos_res.cpp is not linkable here; its catalog branch is exactly this forward).
#include "doctest.h"
#include "resource_messages.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---- scripted seams ----------------------------------------------------------------------------------------
// (the mod index is the real mclib/file.cpp one, empty here; overlays go in through LoadOverlayFile)
std::string gos_FormatResourceMessage(HSTRRES, DWORD id, std::span<const MC2Strings::ArgKind> signature, std::span<const MC2Strings::MessageArg> args)
{
    return MC2Strings::FormatLegacyMessage(static_cast<long>(id), signature, args);
}

namespace {

using MC2Strings::ArgKind;

void useRealCatalog()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const std::string root = MC2_REPO_ROOT;
#ifdef _WIN32
    _putenv_s("MC2_DEFS_ROOT", root.c_str());
#else
    setenv("MC2_DEFS_ROOT", root.c_str(), 1);
#endif
    REQUIRE(MC2Strings::Load());
}

std::string stockTemplate(long id)
{
    const char* text = MC2Strings::GetStockByLegacyId(id);
    REQUIRE(text != nullptr);
    return text;
}

template <class... A>
std::string printfLike(const std::string& format, A... a)
{
    char stackBuf[256];
    const int n = std::snprintf(stackBuf, sizeof stackBuf, format.c_str(), a...);
    REQUIRE(n >= 0);
    if (static_cast<size_t>(n) < sizeof stackBuf)
        return std::string(stackBuf, static_cast<size_t>(n));
    std::string big(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(big.data(), big.size(), format.c_str(), a...);
    big.resize(static_cast<size_t>(n));
    return big;
}

struct OverlayDir
{
    std::filesystem::path dir;
    OverlayDir()
    {
        dir = std::filesystem::temp_directory_path() / ("mc2_resmsg_" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~OverlayDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string write(const char* name, const std::string& body) const
    {
        const std::filesystem::path path = dir / name;
        std::ofstream(path, std::ios::binary) << body;
        return path.string();
    }
};

std::string overrideFit(const std::string& key, long legacyId, const std::string& text)
{
    return "String {\n    key = \"" + key + "\"\n    text = \"" + text + "\"\n}\n\nLegacyTextAlias {\n    legacyId = " +
           std::to_string(legacyId) + "\n    textKey = \"" + key + "\"\n}\n";
}

bool compiles(const char* text, std::initializer_list<ArgKind> signature)
{
    const std::vector<ArgKind> sig(signature);
    MC2Strings::CompiledMessage out;
    return MC2Strings::CompileMessage(text, sig, out);
}

}  // namespace

TEST_CASE("stock templates format exactly like the old sprintf")
{
    useRealCatalog();

    const float weight = 87.5f;
    CHECK(mc2msg::message<IDS_EN_WEIGHT>(weight) == printfLike(stockTemplate(IDS_EN_WEIGHT), static_cast<double>(weight)));
    CHECK(mc2msg::message<IDS_EN_WEIGHT>(weight) == "WEIGHT : 88 tons");   // %.0lf rounds half to even only on exact halves; 87.5 -> 88

    CHECK(mc2msg::message<IDS_EN_CLASS>(std::string("HEAVY")) == printfLike(stockTemplate(IDS_EN_CLASS), "HEAVY"));
    CHECK(mc2msg::message<IDS_EN_ARMOR>("Light Armor", 42L) == printfLike(stockTemplate(IDS_EN_ARMOR), "Light Armor", 42L));
    CHECK(mc2msg::message<IDS_EN_SPEED>(-1L) == printfLike(stockTemplate(IDS_EN_SPEED), -1L));
    CHECK(mc2msg::message<IDS_EN_WEAPON_RATEOFFIRE>(10.f / 3.f) == printfLike(stockTemplate(IDS_EN_WEAPON_RATEOFFIRE), static_cast<double>(10.f / 3.f)));
    CHECK(mc2msg::message<IDS_DIALOG_CBILLS>(123456L) == printfLike(stockTemplate(IDS_DIALOG_CBILLS), 123456L));
    CHECK(mc2msg::message<IDS_DIALOG_TIME>("1/2/2026", "10:11:12") == printfLike(stockTemplate(IDS_DIALOG_TIME), "1/2/2026", "10:11:12"));
    CHECK(mc2msg::message<IDS_DIALOG_OVERWRITE_PROMPT>(std::string("mission")) == printfLike(stockTemplate(IDS_DIALOG_OVERWRITE_PROMPT), "mission"));

    // unsigned long argument through a %ld template (the MaximumNumberOfPlayers site)
    const unsigned long players = 8;
    CHECK(mc2msg::message<IDS_MP_LM_MAP_LIST_MAX_PLAYERS>(players) == printfLike(stockTemplate(IDS_MP_LM_MAP_LIST_MAX_PLAYERS), static_cast<long>(players)));
    CHECK(mc2msg::message<IDS_MP_LM_MAP_LIST_MAX_PLAYERS>(players) == "8 MAX PLAYERS");
}

TEST_CASE("a mission name holding percent signs is an argument, never a format string")
{
    useRealCatalog();
    CHECK(mc2msg::message<IDS_MP_LM_MAP_LIST_MAP_NAME>("50%s of %n %d") == "MAP NAME : 50%s of %n %d");
    CHECK(mc2msg::message<IDS_PLAYER_LEFT>("100%%") == "100%% has left the game.");
}

TEST_CASE("long strings are not truncated")
{
    useRealCatalog();
    const std::string longName(5000, 'x');
    const std::string out = mc2msg::message<IDS_MP_LM_MAP_LIST_MAP_NAME>(longName);
    CHECK(out == "MAP NAME : " + longName);
    CHECK(out.size() == 11 + 5000);
}

TEST_CASE("the importer accepts the printf subset with width, precision and %%")
{
    CHECK(compiles("%d %i %u %x %X %o", {ArgKind::Int, ArgKind::Int, ArgKind::Int, ArgKind::Int, ArgKind::Int, ArgKind::Int}));
    CHECK(compiles("%-5ld|%05d|%+d|%.3s|%8.2lf|%lld|%hd|%hhu", {ArgKind::Int, ArgKind::Int, ArgKind::Int, ArgKind::Str, ArgKind::Float, ArgKind::Int, ArgKind::Int, ArgKind::Int}));
    CHECK(compiles("100%% done", {}));
    CHECK(compiles("{0} {} {x} braces are text", {}));

    MC2Strings::CompiledMessage out;
    const std::vector<ArgKind> sig = {ArgKind::Int};
    REQUIRE(MC2Strings::CompileMessage("{n}=%d, 100%%, {}", sig, out));
    const MC2Strings::MessageArg arg{ArgKind::Int, 7, 0, {}};
    CHECK(MC2Strings::FormatMessage(out, std::span<const MC2Strings::MessageArg>(&arg, 1)) == "{n}=7, 100%, {}");
}

TEST_CASE("the importer rejects malformed and type-mismatched templates")
{
    std::string why;
    MC2Strings::CompiledMessage out;
    const std::vector<ArgKind> one = {ArgKind::Int};
    CHECK_FALSE(MC2Strings::CompileMessage("%n", one, out, &why));
    CHECK(why.find("%n") != std::string::npos);
    CHECK_FALSE(compiles("%p", {ArgKind::Int}));
    CHECK_FALSE(compiles("%c", {ArgKind::Int}));
    CHECK_FALSE(compiles("%*d", {ArgKind::Int}));
    CHECK_FALSE(compiles("%1$d", {ArgKind::Int}));
    CHECK_FALSE(compiles("%", {}));
    CHECK_FALSE(compiles("%5", {ArgKind::Int}));
    CHECK_FALSE(compiles("%99999d", {ArgKind::Int}));
    CHECK_FALSE(compiles("%d", {ArgKind::Str}));           // type mismatch
    CHECK_FALSE(compiles("%s", {ArgKind::Int}));
    CHECK_FALSE(compiles("%f", {ArgKind::Int}));
    CHECK_FALSE(compiles("%d %d", {ArgKind::Int}));        // more conversions than arguments
    CHECK(compiles("%d", {ArgKind::Int, ArgKind::Int}));   // fewer conversions than arguments is allowed (extra args unused)
    CHECK(out.pieces.empty());
}

TEST_CASE("integer width follows the length modifier like a typed printf")
{
    const long long big = 0x100000001LL;
    const std::vector<ArgKind> sig = {ArgKind::Int};
    const MC2Strings::MessageArg arg{ArgKind::Int, big, 0, {}};
    const auto render = [&](const char* text) {
        MC2Strings::CompiledMessage m;
        REQUIRE(MC2Strings::CompileMessage(text, sig, m));
        return MC2Strings::FormatMessage(m, std::span<const MC2Strings::MessageArg>(&arg, 1));
    };
    CHECK(render("%lld") == "4294967297");
    CHECK(render("%ld") == printfLike("%ld", static_cast<long>(big)));   // 32-bit long on Windows, 64-bit on Linux
    CHECK(render("%d") == "1");
    CHECK(render("%hd") == "1");
    CHECK(render("%hhd") == "1");
    CHECK(render("%lu") == printfLike("%lu", static_cast<unsigned long>(big)));
    CHECK(render("%x") == "1");
    CHECK(render("%08ld") == printfLike("%08ld", static_cast<long>(big)));
    const MC2Strings::MessageArg neg{ArgKind::Int, -5, 0, {}};
    MC2Strings::CompiledMessage m;
    REQUIRE(MC2Strings::CompileMessage("%ld|%5d|%-5d|", sig, m, nullptr) == false);   // three conversions, one argument
}

TEST_CASE("a mod override that is malformed or mistyped falls back to the stock template")
{
    useRealCatalog();
    OverlayDir overlay;
    const std::string stockCBills = mc2msg::message<IDS_DIALOG_CBILLS>(500L);
    REQUIRE(stockCBills == "C-BILLS : 500");

    const unsigned long long before = MC2Strings::Revision();
    MC2Strings::LoadOverlayFile(overlay.write("bad_cbills.fit", overrideFit("test.mod.cbills.bad", IDS_DIALOG_CBILLS, "%n CBILLS %d")).c_str());
    CHECK(MC2Strings::Revision() > before);
    CHECK(std::string(MC2Strings::GetByLegacyId(IDS_DIALOG_CBILLS)) == "%n CBILLS %d");   // raw lookup is unchanged
    CHECK(mc2msg::message<IDS_DIALOG_CBILLS>(500L) == stockCBills);

    MC2Strings::LoadOverlayFile(overlay.write("mistyped_prompt.fit", overrideFit("test.mod.overwrite.bad", IDS_DIALOG_OVERWRITE_PROMPT, "Replace %d?")).c_str());
    CHECK(mc2msg::message<IDS_DIALOG_OVERWRITE_PROMPT>(std::string("m1")) == "Overwrite m1?");

    MC2Strings::LoadOverlayFile(overlay.write("toomany_prompt.fit", overrideFit("test.mod.delete.bad", IDS_DIALOG_DELETE_PROMPT, "Delete %s and %s?")).c_str());
    CHECK(mc2msg::message<IDS_DIALOG_DELETE_PROMPT>(std::string("m2")) == "Delete m2?");
}

TEST_CASE("a valid late override replaces the cached template, through overlay and mod reload")
{
    useRealCatalog();
    OverlayDir overlay;
    CHECK(mc2msg::message<IDS_PLAYER_LEFT>("Ann") == "Ann has left the game.");   // fills the cache

    MC2Strings::LoadOverlayFile(overlay.write("left_a.fit", overrideFit("test.mod.left.a", IDS_PLAYER_LEFT, "{%s} walked away")).c_str());
    CHECK(mc2msg::message<IDS_PLAYER_LEFT>("Ann") == "{Ann} walked away");

    // a later overlay file: its alias wins (last write) and the revision moves
    const unsigned long long before = MC2Strings::Revision();
    MC2Strings::LoadOverlayFile(overlay.write("left_b.fit", overrideFit("test.mod.left.b", IDS_PLAYER_LEFT, "%-6s|gone")).c_str());
    CHECK(MC2Strings::Revision() > before);
    CHECK(mc2msg::message<IDS_PLAYER_LEFT>("Ann") == "Ann   |gone");

    // repeated formatting with no catalog change keeps the same revision (cache is reused)
    const unsigned long long steady = MC2Strings::Revision();
    CHECK(mc2msg::message<IDS_PLAYER_LEFT>("Bob") == "Bob   |gone");
    CHECK(MC2Strings::Revision() == steady);
}

TEST_CASE("UnescapeFitString: the stock backslash break code is a line break, other escapes decode")
{
    CHECK(MC2Strings::UnescapeFitString(R"(LASER SPECIALIST\\\nAbility)") == "LASER SPECIALIST\nAbility");
    CHECK(MC2Strings::UnescapeFitString(R"(a\nb\tc\d\"e)") == "a\nb\tc\d\"e");
    CHECK(MC2Strings::UnescapeFitString(R"(end\)") == "end\\");
}
