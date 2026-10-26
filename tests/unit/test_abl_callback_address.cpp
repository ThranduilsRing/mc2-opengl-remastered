#include "doctest.h"
#include "../../mclib/abl_callback_address.h"

namespace { void registeredCallback() {} }

TEST_CASE("ABL callback address accepts code and rejects garbage")
{
    CHECK_FALSE(ablCallbackIsBogus(registeredCallback));
    CHECK_FALSE(ablCallbackIsBogus(nullptr));
    CHECK(ablCallbackIsBogus(reinterpret_cast<void (*)()>(std::uintptr_t{0x1000})));
#ifndef _WIN32
    static int data = 0;
    CHECK(ablCallbackIsBogus(reinterpret_cast<void (*)()>(&data)));
#endif
}
