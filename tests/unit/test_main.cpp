// tests/unit/test_main.cpp
// doctest entry-point translation unit. The macro must appear in exactly
// ONE TU; every other test*.cpp just `#include "doctest.h"`.
//
// Custom main instead of DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN: on the Linux box mc2_tests printed
// "[doctest] Status:" and then hung in exit (futex_wait on a thread that never ran), holding the GPU
// lock for an hour. Static destructors of linked engine code can wait for threads that are never
// joined, so skip them: flush the streams and leave through _Exit with the doctest result.
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
