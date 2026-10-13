// Seeded negative for tests/analyze/analyze_dangling.py: the three shapes in which a pointer into a std::string outlives the string,
// the hazard EString's copy-on-write buffer hid (step 4 batch B1, .claude/cleanup/cxx23-step4-b1-notes.md section 5). The gate script
// compiles this file and FAILS if a shape produces no diagnostic; the same file doubles as the list of shapes the gate is trusted for.
#include <string>

std::string make();
void use(const char*);

// shape A: a local holds the pointer into a temporary that died at the end of the declaration
void shapeA()
{
    const char* p = make().c_str();  // SHAPE:A
    use(p);
}

// shape B: the pointer is returned out of a function whose temporary dies at the return
const char* shapeB()
{
    return make().c_str();  // SHAPE:B
}

// shape C: a member stores the pointer into a temporary
struct HolderC
{
    const char* held = nullptr;
    void set() { held = make().c_str(); }  // SHAPE:C
};

// shape B2: a pointer into a local string leaves the function through the return value
const char* shapeB2()
{
    std::string local = make();
    return local.c_str();  // SHAPE:B2
}

void instantiate()
{
    shapeA();
    (void)shapeB();
    (void)shapeB2();
    HolderC h;
    h.set();
}
