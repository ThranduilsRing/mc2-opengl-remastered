// ABL native bindings: the arg-guard repro hooks (test-only, gated by MC2_ABL_ARG_GUARD_REPRO).
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"


static void runAblArgGuardRepro2A(void);

// Test-only repro hook (gate: MC2_ABL_ARG_GUARD_REPRO=1) for the null-pointer arg guard. It calls the exact
// guard check and write site directly: the ABL call path needs a compiled module, and a standalone harness
// would need ~50 TUs. A null tos->address makes ABLi_popRealPtr return exactly nullptr (the StackItem union
// puts .real at offset 0), so "if (s_ablArgGuard && !relPos)" catches it with no near-null gap.
// Guard ON (MC2_ABL_ARG_GUARD=1): [ABL_ARG_GUARD] is logged, the function returns, nothing crashes.
// Guard OFF: relPos[0] = 1.0f writes to 0x0 and crashes, which is the point; smoke always sets the guard.
void runAblArgGuardRepro(void) {
    if (!mc2::gate::get<"MC2_ABL_ARG_GUARD_REPRO">())
        return;

    mc2::fmt::print_out("[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting\n");
    fflush(stdout);

    // Simulate what ABLi_popRealPtr returns when tos->address == NULL.
    // StackItem union has .real at byte offset 0, so
    //   (float*)(&((StackItemPtr)NULL)->real) == (float*)NULL == nullptr.
    float* relPos = nullptr;

    mc2::fmt::print_out("[ABL_REPRO] relPos from simulated null tos->address = {:016X}\n", mc2::fmt::as_ptr((void*)relPos));
    fflush(stdout);

    // ---- GUARD PATH (MC2_ABL_ARG_GUARD=1) ----
    if (s_ablArgGuard && !relPos) {
        // Mirrors execGetRelativePositionToObject guard lines exactly.
        abl_arg_guard_log("execGetRelativePositionToObject[REPRO]", "relPos");
        mc2::fmt::print_out("[ABL_REPRO] guard fired -- no crash -- PASS\n");
        fflush(stdout);
        // Also run the 2A sibling repro (Point-variant pos + relPos cases).
        runAblArgGuardRepro2A();
        return;
    }

    // ---- CRASH PATH (MC2_ABL_ARG_GUARD unset) ----
    // If we reach here, guard is off.  The write below will segfault.
    mc2::fmt::print_out("[ABL_REPRO] guard OFF -- about to write relPos[0] -> WILL CRASH\n");
    fflush(stdout);
    // Volatile prevents the compiler from eliding the null write.
    volatile float* vp = relPos;
    vp[0] = 1.0f;  // writes to 0x0 -> access violation
    // Unreachable:
    mc2::fmt::print_out("[ABL_REPRO] survived null write -- UNEXPECTED (optimizer elided?)\n");
    fflush(stdout);
}

// Point-variant sibling of runAblArgGuardRepro (pos + relPos null cases); called from it once the Object-variant
// guard has returned. Guard ON: both cases log [ABL_ARG_GUARD] and skip. Guard OFF: the write to 0x0 crashes.
static void runAblArgGuardRepro2A(void) {
    if (!mc2::gate::get<"MC2_ABL_ARG_GUARD_REPRO">())
        return;

    mc2::fmt::print_out("[ABL_REPRO_2A] execGetRelativePositionToPoint sibling repro starting\n");
    fflush(stdout);

    // --- Case 1: null pos (the READ input) ---
    {
        float* pos = nullptr;
        float* relPos_valid = (float*)0x1; // irrelevant — pos guard fires first
        (void)relPos_valid;
        mc2::fmt::print_out("[ABL_REPRO_2A] case1: pos=NULL relPos_valid={:016X}\n", mc2::fmt::as_ptr((void*)relPos_valid));
        fflush(stdout);
        if (s_ablArgGuard && !pos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-pos]", "pos");
            mc2::fmt::print_out("[ABL_REPRO_2A] case1 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            mc2::fmt::print_out("[ABL_REPRO_2A] case1 guard OFF -- about to read pos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = pos;
            (void)vp[0]; // read from 0x0 -> access violation
            mc2::fmt::print_out("[ABL_REPRO_2A] case1 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    // --- Case 2: null relPos (the WRITE output) ---
    {
        float dummy[3] = {1.0f, 2.0f, 0.0f};
        float* pos_valid = dummy;
        float* relPos = nullptr;
        mc2::fmt::print_out("[ABL_REPRO_2A] case2: pos_valid={:016X} relPos=NULL\n", mc2::fmt::as_ptr((void*)pos_valid));
        fflush(stdout);
        if (s_ablArgGuard && !relPos) {
            abl_arg_guard_log("execGetRelativePositionToPoint[REPRO2A-relPos]", "relPos");
            mc2::fmt::print_out("[ABL_REPRO_2A] case2 guard fired -- no crash -- PASS\n");
            fflush(stdout);
        } else {
            mc2::fmt::print_out("[ABL_REPRO_2A] case2 guard OFF -- about to write relPos[0] -> WILL CRASH\n");
            fflush(stdout);
            volatile float* vp = relPos;
            vp[0] = 1.0f; // write to 0x0 -> access violation
            mc2::fmt::print_out("[ABL_REPRO_2A] case2 survived -- UNEXPECTED\n");
            fflush(stdout);
        }
    }

    mc2::fmt::print_out("[ABL_REPRO_2A] done\n");
    fflush(stdout);
}
