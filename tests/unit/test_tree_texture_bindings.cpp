#include "doctest.h"
#include "tree_texture_bindings.h"
#include <array>
#include <cstring>
#include <string>

namespace {
struct Shape {
    std::array<const char*, 4> names{"solid", "absent", "A_leaf", "sidecar"};
    std::array<std::uint32_t, 4> handles{};
    std::array<bool, 4> alpha{true, true, false, true};
    int nameReads = 0;
    long GetNumTextures() const { return names.size(); }
    void GetTextureName(long i, char* out, long) { ++nameReads; std::strcpy(out, names[i]); }
    void SetTextureHandle(long i, std::uint32_t h) { handles[i] = h; }
    void SetTextureAlpha(long i, bool a) { alpha[i] = a; }
};
}

TEST_CASE("tree_texture_bindings/present absent alpha sidecar and interleaved missions") {
    TreeTextureBindings a, b;
    int resolves = 0, mission = 1;
    std::string order;
    auto resolve = [&](const char* name, bool alpha) {
        ++resolves;
        order += std::string(name) + ";";
        TreeTextureBindings::Binding result;
        // The production resolver accepts either a source or its KTX sidecar.
        const bool source = std::strcmp(name, "solid") == 0 || std::strcmp(name, "A_leaf") == 0;
        const bool sidecar = std::strcmp(name, "sidecar") == 0;
        if (source || sidecar) {
            result.handle = mission * 100 + resolves;
            result.present = true;
            result.alpha = alpha;
        }
        return result;
    };
    Shape first, other, further;
    a.apply(first, resolve);
    order += "register-a;";
    b.apply(other, resolve);
    order += "register-b;";
    a.apply(further, resolve);
    order += "register-a;";
    CHECK(resolves == 8);
    CHECK(first.nameReads == 4);
    CHECK(further.nameReads == 0);
    CHECK(first.handles == further.handles);
    CHECK(first.handles[0] == 101);
    CHECK(first.handles[1] == 0xffffffffu);
    CHECK(first.handles[2] == 103);
    CHECK(first.handles[3] == 104);
    CHECK(first.alpha == std::array<bool, 4>{false, true, true, false});
    CHECK(first.alpha == further.alpha);
    CHECK(other.handles[0] == 105);
    CHECK(order == "solid;absent;A_leaf;sidecar;register-a;solid;absent;A_leaf;sidecar;register-b;register-a;");
    mission = 2;
    a.clear(); b.clear();
    Shape next;
    a.apply(next, resolve);
    CHECK(resolves == 12);
    CHECK(next.handles[0] == 209);
    CHECK(next.handles[0] != first.handles[0]);
    CHECK(next.handles[1] == 0xffffffffu);
    CHECK(next.alpha[1]);
}
