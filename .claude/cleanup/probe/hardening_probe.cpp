#include <cstdio>
#include <span>
#include <vector>
int main(int argc, char**) {
    std::vector<int> v(3, 7);
    std::span<int> s(v);
    int i = argc + 5;
    int r = (argc > 1) ? s[i] : v[i];
    std::printf("no trap, read %d\n", r);
    return 0;
}
