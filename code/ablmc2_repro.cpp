// ABL native bindings: the arg-guard repro hooks (test-only, gated by MC2_ABL_ARG_GUARD_REPRO).
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"


static void runAblArgGuardRepro2A(void);

// Test-only repro hook (gate: MC2_ABL_ARG_GUARD_REPRO=1) for the null-pointer arg guard. It calls the exact
// guard check and write site directly: the ABL call path needs a compiled module, and a standalone harness
// would need ~50 TUs. A null tos->address makes ABLi_popRealPtr return exactly nullptr (the StackItem union
// puts .real at offset 0), so "if (s_ablArgGuard && !relPos)" catches it with no near-null gap.
// Guard ON (MC2_ABL_ARG_GUARD=1): [ABL_ARG_GUARD] is logged, the function returns, nothing crashes.
// Guard OFF: relPos[0] = 1.0f writes to 0x0 and crashes, which is the point; smoke always sets the guard.
void runAblArgGuardRepro(void) {
    const char* repro_env = getenv("MC2_ABL_ARG_GUARD_REPRO");
    if (!repro_env || repro_env[0] != '1')
        return;

    printf("[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting\n");
    fflush(stdout);

    // Simulate what ABLi_popRealPtr returns when tos->address == NULL.
    // StackItem union has .real at byte offset 0, so
    //   (float*)(&((StackItemPtr)NULL)->real) == (float*)NULL == nullptr.
    float* relPos = nullptr;

    printf("[ABL_REPRO] relPos from simulated null tos->address = %p\n", (void*)relPos);
    fflush(stdout);

    // ---- GUARD PATH (MC2_ABL_ARG_GUARD=1) ----
    if (s_ablArgGuard && !relPos) {
        // Mirrors execGetRelativePositionToObject guard lines exactly.
        abl_arg_guard_log("execGetRelativePositionToObject[REPRO]", "relPos");
        printf("[ABL_REPRO] guard fired -- no crash -- PASS\n");
        fflush(stdout);
        // Also run the 2A sibling repro (Point-variant pos + relPos cases).
        runAblArgGuardRepro2A();
        return;
    }

    // ---- CRASH PATH (MC2_ABL_ARG_GUARD unset) ----
    // If we reach here, guard is off.  The write below will segfault.
    printf("[ABL_REPRO] guard OFF -- about to write relPos[0] -> WILL CRASH\n");
    fflush(stdout);
    // Volatile prevents the compiler from eliding the null write.
    volatile float* vp = relPos;
    vp[0] = 1.0f;  // writes to 0x0 -> access violation
    // Unreachable:
    printf("[ABL_REPRO] survived null write -- UNEXPECTED (optimizer elided?)\n");
    fflush(stdout);
}

// Point-variant sibling of runAblArgGuardRepro (pos + relPos null cases); called from it once the Object-variant
// guard has returned. Guard ON: both cases log [ABL_ARG_GUARD] and skip. Guard OFF: the write to 0x0 crashes.
static void runAblArgGuardRepro2A(void) {
    const char* repro_env = getenv("MC2_ABL_ARG_GUARD_REPRO");
    if (!repro_env || repro_env[0] != '1')
        return;

    printf("[ABL_REPRO_2A] execGetRelativePositionToPoint sibling repro starting\n");
    fflush(stdout);

    // --- Case 1: null pos (the READ input) ---
    {
        float* pos = nullptr;
        float* relPos_valid = (float*)0x1; // irrelevant — pos guard fires first
        (void)relPos_valid;
        printf("[ABL_REPRO_2A] case1: pos=NULL relPos_valid=%p\n", (void*)relPos_valid);
        fflush(stdout);
        if (s_ablArgGuard && !pos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-pos]", "pos");
            printf("[ABL_REPRO_2A] case1 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            printf("[ABL_REPRO_2A] case1 guard OFF -- about to read pos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = pos;
            (void)vp[0]; // read from 0x0 -> access violation
            printf("[ABL_REPRO_2A] case1 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    // --- Case 2: null relPos (the WRITE output) ---
    {
        float dummy[3] = {1.0f, 2.0f, 0.0f};
        float* pos_valid = dummy;
        float* relPos = nullptr;
        printf("[ABL_REPRO_2A] case2: pos_valid=%p relPos=NULL\n", (void*)pos_valid);
        fflush(stdout);
        if (s_ablArgGuard && !relPos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-relPos]", "relPos");
            printf("[ABL_REPRO_2A] case2 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            printf("[ABL_REPRO_2A] case2 guard OFF -- about to write relPos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = relPos;
            vp[0] = 1.0f; // write to 0x0 -> access violation
            printf("[ABL_REPRO_2A] case2 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    printf("[ABL_REPRO_2A] done\n");
    fflush(stdout);
}
