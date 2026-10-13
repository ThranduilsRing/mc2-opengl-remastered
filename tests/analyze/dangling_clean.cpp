// Clean twin of dangling_seed.cpp for tests/analyze/analyze_dangling.py: the same calls with the owner kept alive. The gate must stay silent here,
// otherwise a "caught" shape in the seeded file proves nothing.
#include <string>

const std::string& member();
std::string make();
void use(const char*);

void okA()
{
    const char* p = member().c_str();      // a reference to a longer-lived owner
    use(p);
    const std::string owned = make();
    use(owned.c_str());                    // the owner outlives the use
    use(make().c_str());                   // a temporary used inside its own full expression
}

struct HolderOk
{
    std::string held;
    void set() { held = make(); }
    const char* get() const { return held.c_str(); }
};

void instantiate()
{
    okA();
    HolderOk h;
    h.set();
    use(h.get());
}
