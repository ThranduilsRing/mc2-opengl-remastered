// Mech3DAppearance: debugUpdate.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mech3d_internal.h"


//-----------------------------------------------------------------------------
void Mech3DAppearance::debugUpdate (long whichOne)
{
	if (!inDebugMoveMode)
		return;

	//----------------------------------------
	// Adjust mechDebugAngle based on Input
	if (userInput->getKeyDown(KEY_LEFT) && userInput->ctrl())
	{
		mechDebugAngle[whichOne] += 11.25;
		if (mechDebugAngle[whichOne] > 180.0)
			mechDebugAngle[whichOne] -= 360;
	}

	if (userInput->getKeyDown(KEY_RIGHT) && userInput->ctrl())
	{
		mechDebugAngle[whichOne] -= 11.25;
		if (mechDebugAngle[whichOne] < -180.0)
			mechDebugAngle[whichOne] += 360.0;
	}

	//----------------------------------------
	// Adjust torsoDebugAngle based on Input
	if (userInput->getKeyDown(KEY_UP) && userInput->ctrl())
	{
		torsoDebugAngle[whichOne] += 11.25;
		if (torsoDebugAngle[whichOne] > 180.0)
			torsoDebugAngle[whichOne] -= 360;
	}

	if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl())
	{
		torsoDebugAngle[whichOne] -= 11.25;
		if (torsoDebugAngle[whichOne] < -180.0)
			torsoDebugAngle[whichOne] += 360.0;
	}

	//----------------------------------------
	// Adjust GestureGoal based on Input
	if (userInput->getKeyDown(KEY_0) && userInput->ctrl())
	{
		setGestureGoal(0);		//PARK
	}

	if (userInput->getKeyDown(KEY_1) && userInput->ctrl())
	{
		setGestureGoal(1);		//STAND
	}

	if (userInput->getKeyDown(KEY_2) && userInput->ctrl())
	{
		setGestureGoal(2);		//WALK
	}

	if (userInput->getKeyDown(KEY_3) && userInput->ctrl())
	{
		setGestureGoal(3);		//RUN
	}

	if (userInput->getKeyDown(KEY_4) && userInput->ctrl())
	{
		setGestureGoal(4);		//REVERSE
	}

	if (userInput->getKeyDown(KEY_5) && userInput->ctrl())
	{
		setGestureGoal(5);		//LIMP
	}

	if (userInput->getKeyDown(KEY_6) && userInput->ctrl())
	{
		setGestureGoal(6);		//JUMP
	}

	if (userInput->getKeyDown(KEY_7) && userInput->ctrl())
	{
		setGestureGoal(7);		//FALL FORWARD
	}

	if (userInput->getKeyDown(KEY_8) && userInput->ctrl())
	{
		setGestureGoal(8);		//FALL BACKWARD
	}

	//------------------------------------------------------------------
	// Adjust position based on mech Velocity which is based on gesture
	Stuff::Vector3D velocity;
	velocity.x = 0.7071f;
	velocity.z = 0.0;
	velocity.y = -0.7071f;

	Rotate(velocity,-mechDebugAngle[whichOne]);

	float velMag = getVelocityMagnitude();

	//-----------------------------------------
	// Take slope being walked on into account.
	// Use for ground vehicles for sure.
	/*
	Stuff::Vector3D currentNormal = land->getTerrainNormal(debugMechActorPosition);
	float angle = angle_from(velocity,currentNormal);
	if (angle != 90.0)
	{
		float hillFactor = cos(angle * DEGREES_TO_RADS) * velMag;
		velMag += hillFactor;
	}
	*/

	velocity *= velMag * worldUnitsPerMeter;

	velocity *= frameLength;

	debugMechActorPosition[whichOne] += velocity;
	debugMechActorPosition[whichOne].z = land->getTerrainElevation(debugMechActorPosition[whichOne]);

	setObjectParameters(debugMechActorPosition[whichOne],mechDebugAngle[whichOne],true,0,0);
	update();
	recalcBounds();
	updateFootprints();
}
