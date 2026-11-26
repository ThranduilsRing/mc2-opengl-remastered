"""Execute both production static allocation paths with a returning fatal callback."""
import subprocess
from pathlib import Path

import pytest
import console_child
from smoke_lib import nofocus

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('method', ['init', 'read'])
def test_static_allocation_refusal_never_reads_null(tmp_path, method):
    source = (ROOT / 'mclib/ablenv.cpp').read_text()
    start = source.index('long ABLModule::init' if method == 'init' else 'void ABLModule::read')
    start = source.index('\tif (numStatics)', start)
    end = source.index('\n\tif (ModuleRegistry[handle].numOrderCalls)', start)
    body = source[start:end]
    result_type, success = ('long', 'return 0;') if method == 'init' else ('void', '')
    probe = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <new>
union StackItem { char* address; long integer; };
using StackItemPtr = StackItem*;
enum class ABLPool { Stack };
static int operation, failAt, fatals, reads;
static bool g_ablValueReadFailed = false;
static void* ABL_alloc(ABLPool, size_t bytes) {
    return ++operation == failAt ? nullptr : std::calloc(1, bytes);
}
static void ABL_Fatal(int, const char*) { ++fatals; }
namespace mc2::fmt {
static int as_i32(int v) { return v; }
template<class... A> void to_buf(char*, const char*, A...) {}
}
static void readAblVariable(void*, StackItem& item, long bytes) {
    assert(item.address); std::memset(item.address, 1, bytes); ++reads;
}
static void clearScalarSlotPointerArm(StackItem*) {}
struct File {
    long read(unsigned char* target, size_t bytes) {
        assert(target); std::memset(target, 1, bytes); ++reads; return bytes;
    }
};
struct Registry { long* sizeStaticVars; };
static long sizes[] = {8, 16};
static Registry ModuleRegistry[] = {{sizes}};
struct Module {
    StackItemPtr staticData = nullptr;
    long numStatics = 2;
    int handle = 0, id = 0;
    bool fresh = true;
    ~Module() {
        if (staticData) {
            for (int i = 0; i < numStatics; ++i) std::free(staticData[i].address);
            std::free(staticData);
        }
    }
''' + f'{result_type} run(File* moduleFile, bool values) {{\n{body}\n{success}\n}}\n' + r'''
};
int main() {
    for (bool values : {false, true}) {
        for (int failure = 1; failure <= 4; ++failure) {
            Module module; File file;
            operation = fatals = reads = 0; failAt = failure;
            bool refused = false;
            try { module.run(&file, values); }
            catch (const std::bad_alloc&) { refused = true; }
            assert(refused == (failure <= 3));
            assert(fatals == (failure <= 3 ? 1 : 0));
            // A failed first array allocation must not enter either read path.
            if (failure <= 2) assert(reads == 0);
        }
    }
}
'''
    probe = '#include <initializer_list>\n' + probe
    cpp = tmp_path / 'abl_alloc.cpp'
    cpp.write_text(probe)
    exe = tmp_path / 'abl_alloc.exe'
    result = subprocess.run(['clang++', '-std=c++20', str(cpp), '-o', str(exe)],
                            capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_returning_fatal_callback_always_unwinds(tmp_path):
    source = (ROOT / 'mclib/ablerr.cpp').read_text()
    start = source.index('void ABL_Fatal (')
    body = source[start:source.index('\n}', start) + 2]
    probe = r'''
#include "ablerr.h"
#include <cassert>
#include <string_view>
static int calls;
static void returningCallback(long, mc2::czstring) { ++calls; }
static auto ABLFatalCallback = returningCallback;
bool g_ablProfileFrame = true;   // fatals unwind only inside a profile frame
''' + body + r'''
int main() {
    const char* messages[] = {"unsupported activation instruction", nullptr};
    for (const char* message : messages) {
        bool unwound = false;
        try { ABL_Fatal(86, message); }
        catch (const ABLRuntimeFailure& error) {
            unwound = true;
            assert(std::string_view(error.what()) == (message ? message : "ABL fatal error"));
        }
        assert(unwound);
    }
    assert(calls == 2);
}
'''
    cpp = tmp_path / 'abl_fatal.cpp'
    cpp.write_text(probe)
    exe = tmp_path / 'abl_fatal.exe'
    result = subprocess.run(['clang++', '-std=c++20', '-I' + str(ROOT / 'mclib'),
                             '-I' + str(ROOT / 'GameOS/gameos/utils'), str(cpp), '-o', str(exe)],
                            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
