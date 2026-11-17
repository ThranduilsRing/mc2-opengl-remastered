// tests/unit/abl_exec_shim.cpp
//
// WHAT: the engine symbols code/ablmc2_position.cpp demands at link time, answered from a scripted argument queue (abl_exec_shim.h).
// WHY: the binding is the real TU; only what lies behind it (the interpreter stack, the object table, the mover geometry) is replaced.
// Rules (as heap_shim.cpp): define only what the linker demands; never put engine behaviour here, link the real TU instead.

#include "abl_exec_shim.h"

#include "ablmc2_internal.h"
#include <span>

namespace abl_test {

ScriptedArgs& args() {
	static ScriptedArgs a;
	return a;
}

}  // namespace abl_test

// ABL exec context (mclib/ablxstd.cpp, ablexec.cpp).
StackItem* stack = nullptr;  // the root stack AblExecContext::resetStack reads (mclib/ablexec.cpp)
size_t stackCount = 0;
long eternalOffset = 0;
bool s_ablArgGuard = false;
bool s_ablRuntimeSoftfail = false;
static AblExecContext s_shimExec;  // execLineNumber and CurModule for the softfail message
AblExecContext* g_ablExec = &s_shimExec;
int32_t FileNumber = -1;

char* ABLModule::getSourceFile(int) { return const_cast<char*>(""); }

// Interpreter stack accessors.
int ABLi_popInteger(void) {
	auto& q = abl_test::args().integers;
	const int v = q.front();
	q.pop_front();
	return v;
}

float ABLi_popReal(void) {
	auto& q = abl_test::args().reals;
	const float v = q.front();
	q.pop_front();
	return v;
}

float* ABLi_popRealPtr(void) {
	auto& q = abl_test::args().realPtrs;
	float* const v = q.front();
	q.pop_front();
	return v;
}

// The object table and the mover geometry.
GameObjectPtr CurObject = nullptr;
GameObjectManager* ObjectManager = nullptr;
GameObject* GameObjectManager::findByPartId(long) { return nullptr; }  // the tests name the current object (id -1), never a part id

glm::vec3 Mover::relativePosition(float, float, unsigned long) { return glm::vec3(); }

glm::vec3 relativePositionToPoint(glm::vec3, float, float, unsigned long) { return glm::vec3(); }

std::span<float>* queuedOutput = nullptr;

// Existing position tests explicitly model real[3]; new calls carry their declared extent.
std::span<float> ABLi_popRealBuf(void) {
	if (queuedOutput) return *queuedOutput;
	auto& queue = abl_test::args().realPtrs;
	auto* address = queue.front();
	queue.pop_front();
	return address ? std::span<float>{address, 3} : std::span<float>{};
}
