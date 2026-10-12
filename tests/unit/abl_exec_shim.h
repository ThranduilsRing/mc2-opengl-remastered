// tests/unit/abl_exec_shim.h
//
// WHAT: a scripted stand-in for the ABL interpreter stack, so the REAL exec* native bindings (code/ablmc2_*.cpp) can run in the unit tier.
// WHY: an exec* binding pops its arguments from the interpreter, which needs a compiled script and the whole runtime; a test that only
// wants "what does the builtin do with these arguments" queues them here and calls the binding itself.
#pragma once

#include <deque>

namespace abl_test {

// The arguments the next binding call pops, in call order, one queue per accessor.
struct ScriptedArgs {
	std::deque<int> integers;
	std::deque<float> reals;
	std::deque<float*> realPtrs;
	void clear() { integers.clear(); reals.clear(); realPtrs.clear(); }
};

ScriptedArgs& args();

}  // namespace abl_test
