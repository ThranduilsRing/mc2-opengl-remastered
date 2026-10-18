// Offline tests for the PURE mod-index cache
// signature kernel (mclib/mod_cache_sig.h): sig-line parse/format round-trip
// and the match rule. The FindFirstFile tree walk + cache read/write live in
// mclib/file.cpp (not built here).

#include "doctest.h"
#include "mod_cache_sig.h"

#include <string>

TEST_CASE("mod-cache-sig: parse/format round-trip") {
    ModCacheSignature sig;
    sig.fileCount = 42;
    sig.maxMtime  = 133456789012345678ULL;   // FILETIME-scale u64

    char line[64];
    formatModCacheSigLine(sig, line, sizeof(line));
    CHECK(std::string(line) == "sig 42 133456789012345678");

    ModCacheSignature back;
    CHECK(parseModCacheSigLine(line, &back));
    CHECK(back.fileCount == sig.fileCount);
    CHECK(back.maxMtime == sig.maxMtime);
    CHECK(modCacheSignatureMatches(sig, back));

    // zero signature (empty mod tree) round-trips too
    ModCacheSignature zero;
    formatModCacheSigLine(zero, line, sizeof(line));
    CHECK(std::string(line) == "sig 0 0");
    CHECK(parseModCacheSigLine(line, &back));
    CHECK(modCacheSignatureMatches(zero, back));
}

TEST_CASE("mod-cache-sig: malformed lines are rejected (stale -> rebuild)") {
    ModCacheSignature out;
    CHECK(!parseModCacheSigLine(nullptr, &out));
    CHECK(!parseModCacheSigLine("", &out));
    CHECK(!parseModCacheSigLine("sig", &out));            // no payload
    CHECK(!parseModCacheSigLine("sig ", &out));           // empty payload
    CHECK(!parseModCacheSigLine("sig 42", &out));         // missing mtime
    CHECK(!parseModCacheSigLine("sig 42 ", &out));        // empty mtime
    CHECK(!parseModCacheSigLine("sig x 5", &out));        // garbage count
    CHECK(!parseModCacheSigLine("sig 42 x", &out));       // garbage mtime
    CHECK(!parseModCacheSigLine("sig 42 5 extra", &out)); // trailing junk
    CHECK(!parseModCacheSigLine("SIG 42 5", &out));       // wrong tag case
    CHECK(!parseModCacheSigLine("count 42", &out));       // v1 line shape
    CHECK(!parseModCacheSigLine("sig 42 5", nullptr));    // null out
}

TEST_CASE("mod-cache-sig: match rule -- any component change invalidates") {
    ModCacheSignature a;
    a.fileCount = 10;
    a.maxMtime  = 1000;

    ModCacheSignature b = a;
    CHECK(modCacheSignatureMatches(a, b));

    b.fileCount = 11;                 // file added/removed
    CHECK(!modCacheSignatureMatches(a, b));

    b = a; b.maxMtime = 1001;         // content edit / rename (dir mtime)
    CHECK(!modCacheSignatureMatches(a, b));

    b = a; b.fileCount = 11; b.maxMtime = 1001;
    CHECK(!modCacheSignatureMatches(a, b));
}
