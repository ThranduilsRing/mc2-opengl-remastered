// ABL native bindings: a position relative to a point or to an object.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp. A TU of its own so the unit tier can run the real
// bindings against a scripted ABL stack (tests/unit/test_abl_position.cpp).
#include "ablmc2_internal.h"
#include "abl_out_params.h"
#include "mc2fmt.h"


// A script that passes a wrong-type or missing output argument is rejected with a log line (malformed content never crashes the engine);
// the log stops after a few lines so a looping script cannot flood it.
static void reportBadArgument (const char* builtin, const char* argument) {
	static int s_lines = 0;
	if (s_lines >= 16)
		return;
	++s_lines;
	mc2::fmt::print_err("[ABL v1] event=bad_argument builtin={} argument={} module={} line={:d}\n", mc2::fmt::safe(builtin),
	                    mc2::fmt::safe(argument), mc2::fmt::safe(CurModule ? CurModule->getName() : nullptr),
	                    mc2::fmt::as_i32(execLineNumber));
}

void execGetRelativePositionToPoint (void) {

	//	Get Relative Position To Point function:
	//		Calcs a position relative to the given point
	//		PARAMS:	real[3]				point
	//				real				angle (-180 <= angle <= 180.0, + is left, - is right)
	//				real				distance (in meters)
	//				integer				flags
	//				real[3]				new point
	//		RETURN: NONE

	float* pos = ABLi_popRealPtr();
	float angle = ABLi_popReal();
	float distance = ABLi_popReal();
	long flags = ABLi_popInteger();
	float* relPos = ABLi_popRealPtr();

	// H2: guard sibling of execGetRelativePositionToObject.
	// pos is READ (curPos.x = pos[0..2]); relPos is WRITE (relPos[0..2] = newPos).
	// Both crash on null. Guard both independently so each gets a distinct log line.
	if (s_ablArgGuard && !pos) {
		abl_arg_guard_log("execGetRelativePositionToPoint", "pos");
		return;
	}
	if (s_ablArgGuard && !relPos) {
		abl_arg_guard_log("execGetRelativePositionToPoint", "relPos");
		return;
	}

	Stuff::Vector3D curPos;
	curPos.x = pos[0];
	curPos.y = pos[1];
	curPos.z = 0;
	Stuff::Vector3D newPos;
	newPos = relativePositionToPoint(curPos, angle, distance, flags);
	relPos[0] = newPos.x;
	relPos[1] = newPos.y;
	relPos[2] = newPos.z;
}

void execGetRelativePositionToObject (void) {

	//	Get Relative Position To Object function:
	//		Calcs a position relative to the given object
	//		PARAMS:	integer				object id
	//				real				angle (-180 <= angle <= 180.0, + is left, - is right)
	//				real				distance (in meters)
	//				integer				flags
	//				real[3]				new point
	//		RETURN: NONE

	long objectId = ABLi_popInteger();
	float angle = ABLi_popReal();
	float distance = ABLi_popReal();
	long flags = ABLi_popInteger();
	float* relPos = ABLi_popRealPtr();

	// relPos is NULL when the calling script passes a wrong-type or missing argument. Both branches below write it, so the test is
	// unconditional here (not behind MC2_ABL_ARG_GUARD): a bad script is rejected with a log line, never faults the engine.
	if (!relPos) {
		reportBadArgument("getRelativePositionToObject", "relPos");
		return;
	}

	// Only a mover has a position relative to itself: any other object (or an unknown id) answers the zero vector, so the
	// script's real[3] never keeps a stale or uninitialised value.
	GameObjectPtr object = getObject(objectId);
	if (object && object->isMover()) {
		Stuff::Vector3D newPos;
		newPos = ((MoverPtr)object)->relativePosition(angle, distance, flags);
		abl::writeVec3(std::span<float, 3>(relPos, 3), newPos.x, newPos.y, newPos.z);
	} else {
		abl::writeZeroVec3(std::span<float, 3>(relPos, 3));
	}
}
