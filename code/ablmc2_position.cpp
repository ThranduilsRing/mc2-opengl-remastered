// ABL native bindings: a position relative to a point or to an object.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp. A TU of its own so the unit tier can run the real
// bindings against a scripted ABL stack (tests/unit/test_abl_position.cpp).
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include <algorithm>
#include <span>

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
	const auto output = ABLi_popRealBuf();
	float relPos[3] = {};

	if (!pos) {
		abl::reportBadArgument("getrelativepositiontopoint", "pos");
		return;
	}
	glm::vec3 curPos;
	curPos.x = pos[0];
	curPos.y = pos[1];
	curPos.z = 0;
	glm::vec3 newPos;
	newPos = relativePositionToPoint(curPos, angle, distance, flags);
	relPos[0] = newPos.x;
	relPos[1] = newPos.y;
	relPos[2] = newPos.z;
	abl::copyRealArray(output, relPos, "getrelativepositiontopoint_truncated");
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
	const auto output = ABLi_popRealBuf();
	float relPos[3] = {};

	// Missing or unknown output extents are rejected even with the optional argument guard disabled.
	if (output.data() == nullptr) {
		abl::reportBadArgument("getRelativePositionToObject", "relPos");
		return;
	}

	// Any object that is not a mover (or an unknown id) answers the zero vector, a chosen answer ((0,0,0) is also a valid position) that
	// keeps the script's real[3] from holding a stale or uninitialised value.
	GameObjectPtr object = getObject(objectId);
	if (object && object->isMover()) {
		glm::vec3 newPos;
		newPos = ((MoverPtr)object)->relativePosition(angle, distance, flags);
		relPos[0] = newPos.x;
		relPos[1] = newPos.y;
		relPos[2] = newPos.z;
	} else {
		relPos[0] = relPos[1] = relPos[2] = 0.0f;
	}
	abl::copyRealArray(output, relPos, "getrelativepositiontoobject_truncated");
}
