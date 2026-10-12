// tests/unit/abl_exec_shim.cpp
//
// WHAT: the engine symbols code/ablmc2_position.cpp demands at link time, answered from a scripted argument queue (abl_exec_shim.h).
// WHY: the binding is the real TU; only what lies behind it (the interpreter stack, the object table, the mover geometry) is replaced.
// Rules (as heap_shim.cpp): define only what the linker demands; never put engine behaviour here, link the real TU instead.

#include "abl_exec_shim.h"

#include "ablmc2_internal.h"

namespace abl_test {

ScriptedArgs& args() {
	static ScriptedArgs a;
	return a;
}

}  // namespace abl_test

// ABL exec context (mclib/ablxstd.cpp, ablexec.cpp).
bool s_ablArgGuard = false;
bool s_ablRuntimeSoftfail = false;
int execLineNumber = 0;
ABLModulePtr CurModule = nullptr;
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

Stuff::Vector3D Mover::relativePosition(float, float, unsigned long) { return Stuff::Vector3D(); }

Stuff::Vector3D relativePositionToPoint(Stuff::Vector3D, float, float, unsigned long) { return Stuff::Vector3D(); }
