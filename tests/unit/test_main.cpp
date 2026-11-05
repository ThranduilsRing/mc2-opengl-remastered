// tests/unit/test_main.cpp
// doctest entry-point translation unit. The macro must appear in exactly
// ONE TU; every other test*.cpp just `#include "doctest.h"`.
// Own main (instead of DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN): the process leaves through _Exit after the run and a flush, because
// on the Linux box the test binary hung in static teardown after "Status: SUCCESS" and held the GPU lock for hours.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int rc = context.run();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}
