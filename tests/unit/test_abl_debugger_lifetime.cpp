// A2: the ABL debugger managers (watch manager, break points) are created and
// initialized in one place. init() and read() both call the helper, so the
// creation order and the ownership cannot drift apart between the two paths.
//
// Source pins only: ABLModule lives in the interpreter, which this offline
// tier does not link. The pins are the contract the two call sites share.

#include "doctest.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MC2_REPO_ROOT
#error "MC2_REPO_ROOT must be defined by the build"
#endif

namespace {

std::string readRepoFile(const char* relPath) {
    std::ifstream in(std::string(MC2_REPO_ROOT) + "/" + relPath, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << relPath);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

size_t countOf(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

} // namespace

TEST_CASE("ABL debugger managers are created by one helper") {
    const std::string header = readRepoFile("mclib/ablenv.h");
    const std::string source = readRepoFile("mclib/ablenv.cpp");

    // Declared on ABLModule, defined once.
    CHECK(header.find("void initDebuggerManagers (void);") != std::string::npos);
    CHECK(countOf(source, "void ABLModule::initDebuggerManagers (void)") == 1);

    // Both lifecycle paths go through it: init() and read().
    CHECK(countOf(source, "initDebuggerManagers();") == 2);

    // Neither path builds a manager on its own any more.
    CHECK(countOf(source, "std::make_unique<WatchManager>") == 1);
    CHECK(countOf(source, "std::make_unique<BreakPointManager>") == 1);

    // Creation order is unchanged: watch manager first, then break points.
    const size_t watch = source.find("std::make_unique<WatchManager>");
    const size_t breaks = source.find("std::make_unique<BreakPointManager>");
    REQUIRE(watch != std::string::npos);
    REQUIRE(breaks != std::string::npos);
    CHECK(watch < breaks);

    // The failure messages are the ones the two call sites used to carry.
    CHECK(source.find(" Unable to AblStackHeap->malloc WatchManager ") != std::string::npos);
    CHECK(source.find(" Unable to AblStackHeap->malloc BreakPointManager ") != std::string::npos);
}
