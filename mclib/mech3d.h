//-------------------------------------------------------------------------------
//
// Mech 3D layer.  Controls how the mech moves through animations
//
// For MechCommander 2
//
// Replace Mactor for better looking mechs!
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MECH3D_H
#define MECH3D_H
//-------------------------------------------------------------------------------
// Include Files
#ifndef APPRTYPE_H
#include"apprtype.h"
#endif

#ifndef OBJECTAPPEARANCE_H
#include"objectappearance.h"
#endif

#ifndef MSL_H
#include"msl.h"
#endif

#include"memfunc.h"

#include<gosfx/gosfxheaders.hpp>
#include <vector>  // T1.6: Mech3DAppearance::spotlight{Lights,SlotIds,NodeIds}_
// M2 RenderWorld handle storage (RenderCore is pure types; no GL, no game headers).
#include "../RenderCore/Handle.h"
// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: renderer-facing per-mech visual state
// (pure POD; same firewall rationale as Handle.h above).
#include "../RenderCore/MechVisualState.h"
//-------------------------------------------------------------------------------
// Structs used by layer.
//
typedef struct _GestureData
{
	float			startVel;				//Gesture Starts at this speed
	float			endVel;					//Gesture Ends at this speed
	long			frameStart;				//Which frame does gesture start at.
	bool			reverse;				//Should we play backwards?
	long			rightFootDownFrame0;	//When should the right foot make a poof and print?
	long			rightFootDownFrame1;	//Sometimes the foot's down twice.
	long			leftFootDownFrame0;		//When should the left foot make a poof and print?
	long			leftFootDownFrame1;		//Sometimes the foot's down twice.
} GestureData;

typedef struct _PaintSchemata
{
	DWORD			redColor;				//Replace all Reds in texture with this color scaled by intensity of red!
	DWORD			greenColor;				//Replace all Greens in texture with this color scaled by intensity of green!
	DWORD			blueColor;				//Replace all Blues in texture with this color scaled by intensity of blue!
} PaintSchemata;

typedef PaintSchemata *PaintSchemataPtr;
//-------------------------------------------------------------------------------
// Macro Definitions
#define GesturePark						0
#define GestureStandToPark				1
#define	GestureStand					2
#define GestureStandToWalk				3			//OBSOLETE!
#define GestureWalk						4
#define GestureParkToStand 				5
#define GestureWalkToRun				6			//OBSOLETE!
#define GestureRun						7
#define GestureRunToWalk 				8			//OBSOLETE!
#define GestureReverse					9
#define GestureStandToReverse			10			//OBSOLETE!
#define GestureLimpLeft					11
#define GestureLimpRight				12
#define GestureIdle						13
#define GestureFallBackward 			14
#define GestureFallForward 				15
#define GestureHitFront					16
#define GestureHitBack					17
#define GestureHitLeft					18
#define GestureHitRight					19
#define GestureJump						20
#define GestureRollover					21
#define GestureGetUp					22
#define GestureFallenForward			23
#define	GestureFallenBackward			24
#define MaxGestures						25

#define MAX_MECH_ANIMATIONS				MaxGestures

#define	NUM_MECH_STATES					10
#define MECH_STATE_PARKED				0
#define	MECH_STATE_STANDING				1
#define	MECH_STATE_WALKING				2
#define	MECH_STATE_RUNNING				3
#define	MECH_STATE_REVERSE				4
#define	MECH_STATE_LIMPING_LEFT			5
#define MECH_STATE_JUMPING				6
#define MECH_STATE_FALLEN_FORWARD		7
#define	MECH_STATE_FALLEN_BACKWARD		8
#define MECH_STATE_LIMPING_RIGHT		9

#define BASE_NODE_RECYCLE_TIME		0.25f

extern char MechStateByGesture[];

//-------------------------------------------------------------------------------
// class Mech3DAppearance
class Mech3DAppearanceType: public AppearanceType
{
	//--------------------------------------------
	// Stores Multi-Shapes, Animation Data, etc.
	public:
		TG_TypeMultiShapePtr		mechShape[MAX_LODS];
		TG_TypeMultiShapePtr		mechShadowShape;
		float						lodDistance[MAX_LODS];
		TG_AnimateShapePtr			mechAnim[MAX_MECH_ANIMATIONS];
		
		TG_TypeMultiShapePtr		leftArm;
		TG_TypeMultiShapePtr		rightArm;

 		TG_TypeMultiShapePtr		mechForwardDmgShape;
 		TG_TypeMultiShapePtr		mechBackwardDmgShape;

		long						rightFootprintType;					//Footprint type for this mech.
		long						leftFootprintType;				//Footprint type for this mech.
		long						shadowScalar;					//Values to scale shadow for this type of mech
		long						textureSide;					//Size of texture edge in pixels

		GestureData					gestures[MaxGestures];
		
		long						numSmokeNodes;
		long						numJumpNodes;
		long						numWeaponNodes;
		long						numFootNodes;
		NodeData					*nodeData;

		// PRESENTATION-SOCKET-PROFILE-1 (Phase-2 slice 2): OPTIONAL, data-declared
		// presentation-socket table. Populated ONLY from an optional
		// [PresentationSockets] INI block in Mech3DAppearanceType::init (stock mechs
		// have no such block -> numPresentationSockets stays 0). Read-and-validated
		// only: each declared socket name is checked (report-only, gated
		// MC2_UNIT_PROFILE_DATA) against the loaded model shape's node list. This is
		// NOT authoritative for rendering — getWeaponNode / nodeData are untouched.
		// Fixed inline storage (declared sockets are few; no heap, no layout drag).
		enum { MAX_PRESENTATION_SOCKETS = 32, PRESENTATION_SOCKET_NAME_CAP = 48 };
		long						numPresentationSockets;
		char						presentationSocketName[MAX_PRESENTATION_SOCKETS][PRESENTATION_SOCKET_NAME_CAP];


		static TG_TypeMultiShapePtr	SensorSquareShape;

		static bool animationLoadingEnabled;
		static void disableAnimationLoading(void)
		{
			animationLoadingEnabled = false;
		}
		static void enableAnimationLoading(void)
		{
			animationLoadingEnabled = true;
		}

	public:

		void init (void) 
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				mechShape[i] = NULL;
				lodDistance[i] = 0.0f;			
			}
		
			for (i=0;i<MAX_MECH_ANIMATIONS;i++)
				mechAnim[i] = NULL;

			rightFootprintType = leftFootprintType = -1;

			shadowScalar = 0;								//For stupid shadows if we need to draw them.

			textureSide = 128;								//For stupid shadows if we need to draw them.
			
			nodeData = NULL;
			numSmokeNodes = numWeaponNodes = numJumpNodes = 0;

			// PRESENTATION-SOCKET-PROFILE-1: no declared sockets by default.
			numPresentationSockets = 0;

			mechShadowShape = NULL;
    
            //sebi: init to not contain garbage
 		    mechForwardDmgShape = mechBackwardDmgShape = NULL;
            leftArm = rightArm = NULL;
            numFootNodes = 0;
            MemSet(gestures, 0);
		}

		Mech3DAppearanceType (void) 
		{
			init();
		}

		void destroy (void);
		
		~Mech3DAppearanceType (void)
		{
			destroy();
		}
		
		virtual void init (const char *fileName);
		
		long getTotalNodes (void)
		{
			return numSmokeNodes + numWeaponNodes + numJumpNodes + numFootNodes;
		}

		void setAnimation (TG_MultiShapePtr shape, DWORD animationNum);
		
		float getStartVel (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MaxGestures))
				return gestures[animationNum].startVel;

			return 0.0f;
		}
		
		float getEndVel (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MaxGestures))
				return gestures[animationNum].endVel;

			return 0.0f;
		}

		float getFrameRate (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MaxGestures) && (mechAnim[animationNum]))
				return mechAnim[animationNum]->GetFrameRate();

			return 0.0f;
		}

		long getNumFrames (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MaxGestures) && (mechAnim[animationNum]))
				return mechAnim[animationNum]->GetNumFrames();

			return 0.0f;
		}

		long getJumpLandFrame (void)
		{
			return (gestures[GestureJump].frameStart & 0x0000ffff);
		}
		
		long getJumpTakeoffFrame (void)
		{
			return (gestures[GestureJump].frameStart >> 16);
		}
};

typedef Mech3DAppearanceType *Mech3DAppearanceTypePtr;

#define MAX_DUST_POOFS			3
//-------------------------------------------------------------------------------
typedef struct _MechAppearanceData
{
	float						frameNum;
	float						mechFrameRate;

	bool						leftArmOff;
	bool						rightArmOff;
	bool						fallen;
	bool						forceStop;
	bool						atTransitionToNextGesture;
	bool						inReverse;
	bool						inJump;
	bool						jumpSetup;
	bool						jumpFXSetup;
	bool						jumpAirborne;
	bool						oncePerFrame;
	bool						lockRotation;

	bool						inDebugMoveMode;
	bool						singleStepMode;
	bool						nextStep;
	bool						prevStep;

	float						velocity;
	long						status;
	long						sensorLevel;

	long						currentStateGoal;
	long						currentGestureId;
	long						transitionState;
	long						oldStateGoal;
	long						hitGestureId;			//What gesture should I go back to when hit?

	float						currentFrame;			//One for each part.
	long						currentLOD;

	long						nodeUsed[10];				//Used to stagger the weapon nodes for firing.
	float						nodeRecycle[10];			//Used for ripple fire to find out if the node has fired recently.

	Stuff::Vector3D				jumpDestination;
	Stuff::Vector3D				jumpVelocity;			//Real velocity vector now.  Just plug into mech velocity.

	float						baseRootNodeHeight;

	long						isSmoking;
	bool						isWaking;
	bool						isDusting;
	bool						fallDust;
	bool						isHelicopter;

} MechAppearanceData;

class Mech3DAppearance: public ObjectAppearance
{
	//-------------------------------------------------------------------------------------
	// Stores instance specific data.  Frame counts, position, velocity, texture map, etc.
	protected:

		Mech3DAppearanceTypePtr		mechType;
		
		TG_MultiShapePtr			mechShape;
		TG_MultiShapePtr			mechShadowShape;
		TG_MultiShapePtr			sensorSquareShape;
		TG_MultiShapePtr			sensorTriangleShape;
		float						sensorSpin;
 
		TG_MultiShapePtr			leftArm;
		TG_MultiShapePtr			rightArm;
		
		Stuff::Vector3D				leftArmPos;
		Stuff::Vector3D				rightArmPos;

		float						leftArmHazeFactor;
		float						rightArmHazeFactor;
		bool						leftArmInView;
		bool						rightArmInView;

		float						frameNum;
		float						mechFrameRate;
		
		float						torsoRotation;
		float						leftArmRotation;
		float						rightArmRotation;

		bool						leftArmOff;
		bool						rightArmOff;
		bool						fallen;
		bool						forceStop;
		bool						atTransitionToNextGesture;
		bool						inReverse;
		bool						inJump;
		bool						jumpSetup;
		bool						jumpFXSetup;
		bool						jumpAirborne;
		bool						oncePerFrame;
		bool						lockRotation;

		bool						inDebugMoveMode;
		bool						singleStepMode;
		bool						nextStep;
		bool						prevStep;

		float						velocity;
		long						status;
		long						sensorLevel;

		long						currentStateGoal;
		long						currentGestureId;
		long						transitionState;
		long						oldStateGoal;
		long						hitGestureId;			//What gesture should I go back to when hit?

		float						currentFrame;			//One for each part.
		uint32_t					lastAnimAdvanceFrame;	//ANIM-CADENCE-FIX: g_mc2FrameCounter of last gait advance (idempotency guard)
		long						currentLOD;

		TG_LightPtr					pointLight;
		DWORD						lightId;

		// (E) T1.6: generalised SpotLight_-child illumination. PER-CHILD-
		// SPOTLIGHT-NODE (TG_LIGHT_POINT). NOT the same as `pointLight`/
		// `lightId` above — that pair is the hardcoded SLCircle_anubis
		// searchlight (TG_LIGHT_SPOT) at mech3d.cpp:3333-3383. The two
		// node-name prefixes are disjoint (SLCircle != SpotLight) so no
		// double-registration; both paths coexist until T3.x consolidation.
		// See plan R7.
		std::vector<long>			spotlightNodeIds_;   // mechShape NodeNameId
		std::vector<TG_LightPtr>	spotlightLights_;    // owned via malloc/free
		std::vector<DWORD>			spotlightSlotIds_;   // worldLights[] indices
		bool						spotlightsRegistered_;
		
		float						idleTime;				//Elapsed time since I've done something.
															//If it gets larger then X, play the idle animation.
															
		Stuff::Vector3D				jumpDestination;
		Stuff::Vector3D				jumpVelocity;			//Real velocity vector now.  Just plug into mech velocity.
		float						hazeFactor;
		
		long						*nodeUsed;				//Used to stagger the weapon nodes for firing.
		float						*nodeRecycle;			//Used for ripple fire to find out if the node has fired recently.
		
		Stuff::Vector3D				footPos[2];				//Used to store previous frame foot positions.
															//For foot poofs and footprints.
   		//NEW  GOS FX
   		gosFX::Effect				*rightDustPoofEffect[MAX_DUST_POOFS];
		gosFX::Effect				*leftDustPoofEffect[MAX_DUST_POOFS];
   		gosFX::Effect				*smokeEffect;
   		gosFX::Effect				*jumpJetEffect;
		gosFX::Effect				*rightShoulderBoom;
		gosFX::Effect				*leftShoulderBoom;
		gosFX::Effect				*criticalSmoke;
		gosFX::Effect				*waterWake;
		gosFX::Effect				*helicopterDustCloud;
		gosFX::Effect				*rightArmSmoke;
  		gosFX::Effect				*leftArmSmoke;

		bool						movedThisFrame;

		Stuff::Point3D				leftShoulderPos;
		Stuff::Point3D				rightShoulderPos;
				
		long						currentRightPoof;
		long						currentLeftPoof;
		
		long						leftFootPoofDraw[MAX_DUST_POOFS];
		long						rightFootPoofDraw[MAX_DUST_POOFS];
		
		Stuff::Point3D				lFootPosition[MAX_DUST_POOFS];
		Stuff::Point3D				rFootPosition[MAX_DUST_POOFS];
		bool						rightFootDone0;
		bool						leftFootDone0;
		bool						rightFootDone1;
		bool						leftFootDone1;
		
		bool						inCombatMode;
		
		long						isSmoking;
		bool						isWaking;
		bool						isDusting;
		bool						fallDust;
		bool						isHelicopter;
		
		float						OBBRadius;

		// C3: cached GameObjectHandle from init(obj), used by GPU-cull lifecycle gates
		// in node-position functions. -1 = no owner; set in init().
		long						actorHandle_ = -1;

		DWORD						localTextureHandle;
		DWORD						aoTextureHandle = 0;   // AO-1: imported-mech AO slot (unit 6)
		bool						aoValid = false;       // AO-1: AO handle loaded OK (gates HAS_AO)
		DWORD						normalTextureHandle = 0; // NORMALS-1: imported-mech normal slot (unit 7)
		bool						normalValid = false;     // NORMALS-1: normal handle loaded OK (gates HAS_NORMAL)
		float						baseRootNodeHeight;

		DWORD						psRed;
		DWORD						psBlue;
		DWORD						psGreen;

		char						mechName[64];

		//Arm off coolness
		Stuff::Vector3D				dVel[2];
		Stuff::Vector3D				dRot[2];
		Stuff::Vector3D				dAcc[2];
		Stuff::Vector3D				dRVel[2];
		Stuff::Vector3D				dRacc[2];
		float						dTime[2];

		float										flashDuration;
		float										duration;
		float										currentFlash;
		bool										drawFlash;
		DWORD										flashColor;

		long						rotationalNodeIndex;
		long						hitLeftNodeIndex;
		long						hitRightNodeIndex;
		long						rootNodeIndex;
		long						leftArmNodeIndex;
		long						rightArmNodeIndex;
		long						lightCircleNodeIndex;
		float						baseRootNodeDifference;

		// M2 RenderWorld handle. Set by GameAdapters::Mech::syncSpawn() via
		// setRenderWorldHandleForAdapter() after appearance->init(). Cleared by
		// GameAdapters::Mech::destroyMech() via clearRenderWorldHandleForAdapter()
		// before delete appearance. Default: invalid (never registered or retired).
		//
		// Mech3DAppearance MUST NOT call the adapter or RenderWorld directly.
		// The adapter is the bridge; this field is storage only.
		// Firewall: mclib/mech3d.h may NOT include GameAdapters/MechRenderAdapter.h.
		// The field type (RenderCore::RenderObjectHandle) is in RenderCore/Handle.h,
		// which is allowed here (RenderCore is pure; no GL, no game headers).
		RenderCore::RenderObjectHandle mechRenderHandle =
		    RenderCore::RenderObjectHandle::invalid();

		// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: last visual state pushed by the
		// game-side producer. Defaults to safe-neutral (undamaged, unflagged).
		RenderCore::MechVisualState m_visualState{};

	public:
		static PaintSchemataPtr		paintSchemata;
		static DWORD				numPaintSchemata;

		// M2 adapter accessors (public -- used ONLY by GameAdapters::Mech).
		// No other caller may use these outside the adapter TU.
		RenderCore::RenderObjectHandle getRenderWorldHandle() const {
		    return mechRenderHandle;
		}
		void setRenderWorldHandleForAdapter(RenderCore::RenderObjectHandle h) {
		    mechRenderHandle = h;
		}
		void clearRenderWorldHandleForAdapter() {
		    mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
		}

		// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: renderer-facing visual state,
		// pushed once per frame by BattleMech::render() (game side) and read
		// at submit time by Mech3DAppearance::render() (engine side). Storage
		// only -- this class never calls the renderer/adapter. The producer is
		// responsible for sanitizing values (see RenderCore::sanitizeMechVisual01).
		void setVisualState(const RenderCore::MechVisualState& s) { m_visualState = s; }
		const RenderCore::MechVisualState& getVisualState() const { return m_visualState; }

	public:

		Mech3DAppearance (void)
		{
			init();
		}

		~Mech3DAppearance (void)
		{
			destroy();
		}

		virtual void destroy (void);

		virtual void init (AppearanceTypePtr tree = NULL, GameObjectPtr obj = NULL);

		virtual void initFX (void);

		virtual AppearanceTypePtr getAppearanceType (void)
		{
			return mechType;
		}

		// C1a: expose OBBRadius via the base-class virtual so emitGpuCullRecord
		// can call app->getRadius() uniformly without reaching into protected state.
		virtual float getRadius (void)
		{
			return OBBRadius > 0.0f ? OBBRadius : 0.0f;
		}

		virtual long update (bool animate = true);
		virtual long render (long depthFixup = 0);
		virtual long renderShadows (void);

		virtual void updateFootprints (void);

		virtual void updateGeometry (void);

		virtual void setPaintScheme (void);

		virtual void setPaintScheme (DWORD red, DWORD green, DWORD blue);

		virtual void getPaintScheme (DWORD &red, DWORD &green, DWORD &blue);

		virtual void resetPaintScheme (DWORD red, DWORD green, DWORD blue);

 		virtual bool isInJump (void)
		{
			return inJump;
		}

		void flashBuilding (float dur, float fDuration, DWORD color);


		virtual bool isJumpSetup (void)
		{
			return jumpSetup;
		}

		virtual bool isJumpAirborne (void)
		{
			return jumpAirborne;
		}

		virtual bool setJumpParameters (Stuff::Vector3D &end);

		virtual bool haveFallen (void)
		{
			return fallen;
		}
		
		virtual bool isMouseOver (float px, float py);
		
		virtual bool recalcBounds (void);
		
		virtual void setBrake (bool brake)
		{
			forceStop = brake;
		}

		bool checkStop (void)
		{
			return forceStop;
		}

		virtual long getCurrentGestureId (void)
		{
			return currentGestureId;
		}

		virtual void setGesture (unsigned long gestureId)
		{
			//--------------------------------
			// We are at the goal.  Store off
			// our current gesture state.
			//
			// NOTE: This is only used to force the helicopters to be parked
			// at startup.  Any other attempt to use this will probably not work!!
			//
			// These two lines force the select box to match the helicopter's position in the sky
			currentGestureId = 2;
			inView = true;
			update();

			currentGestureId = gestureId;
			oldStateGoal = 0;				//This is ONLY valid for park!!!!
			currentStateGoal = -1;			//Not going anywhere
			transitionState = 0;			//No Transition necessary.
			currentFrame = mechType->getNumFrames (currentGestureId)-1;
		}

		virtual long setGestureGoal (long stateGoal)
		{
			//This will set which gesture we are trying to get to.
			//-----------------------------------------------------------
			// Smart(tm) Gesture code goes here.
			//
			// NO gesture queueing.  Current request is current goal.
			//
			if (oncePerFrame)
				return 0;

			if (oldStateGoal == stateGoal)
				return(0);
		
			if (currentStateGoal == stateGoal)
				return(0);

			if ((stateGoal < MECH_STATE_PARKED) || (stateGoal > MECH_STATE_LIMPING_RIGHT))
				return(0);

			if (currentStateGoal != -1)
				return 0;

			 if ((currentGestureId == GestureHitFront) ||
				 (currentGestureId == GestureHitBack) || 
				 (currentGestureId == GestureHitLeft) || 
				 (currentGestureId == GestureHitRight))
			 {
				 return 0;
			 }
 					  
			if ((stateGoal == MECH_STATE_JUMPING) && !jumpSetup)
				return 0;
				
			//---------------------------------------------------------------
			// If we are still on our way to something, check if the current
			// Gesture will allow a change IMMEDIATELY.  Set the appropriate
			// variables if this is TRUE and go on.  Otherwise, IGNORE THIS
			// COMMAND.  MECH will continually ask to change if necessary.

			transitionState = 0;				//Start at beginning gestureId of this goal.

			currentStateGoal = stateGoal;
			atTransitionToNextGesture = true;

			oncePerFrame = true;
			return -1;
		}
		
		virtual long getFrameNumber (long partNum)
		{
			return frameNum;
		}

		virtual float getNumFramesInGesture (unsigned long gestureId)
		{
			if (mechType)
				return mechType->getNumFrames(gestureId);

			return (0.0f);
		}

		virtual float getVelocityOfGesture (unsigned long gestureId)
		{
			float result = mechType->getStartVel(gestureId);
			float otherResult = mechType->getEndVel(gestureId);
			if (otherResult == result)
				return(result);
			else
			{
				float vel = otherResult - result;
				otherResult = getNumFramesInGesture(gestureId);
				if (otherResult > Stuff::SMALL)
				{
					vel *= currentFrame / otherResult;
					vel += result;
				}

				return vel;
			}
		}

		virtual Stuff::Vector3D getVelocity (void)
		{
			return jumpVelocity;
		}
		
		virtual float getVelocityMagnitude (void)
		{
			if (jumpAirborne)
				return velocity;
			else
				return getVelocityOfGesture (currentGestureId);
		}

		virtual void setCombatMode (bool combatMode)
		{
			//------------------------------------------------------------
			// However we move the arms from gun to normal mode goes here.
			inCombatMode = combatMode;
		}

		virtual void setObjectParameters (const Stuff::Vector3D &pos, float legRot, long selected, long alignment, long homeRelations);

		virtual void setMoverParameters (float turretRot, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false);

		void debugUpdate (long whichOne);

		void setJustDoIt (void);
		
		void clearJustDoIt (void);

		virtual void setObjectNameId (long objId)
		{
			objectNameId = objId;
		}

		static void LoadPaintSchemata (void);

		virtual unsigned long getAppearanceClass (void)
		{
			return MECH_APPR_TYPE;
		}

		virtual void setDebugMoveMode (void)
		{
			inDebugMoveMode = true;
		}

		//--------------------------------------------
		// Once site Objects are in place, go get 'em
		virtual void setWeaponNodeUsed (long nodeId);
		
		virtual Stuff::Vector3D getWeaponNodePosition (long nodeId);
		
		virtual Stuff::Vector3D getNodePosition (long nodeId);
		
		virtual Stuff::Vector3D getNodeNamePosition (const char *nodeName);
		
		virtual long getWeaponNode (long weapontype);

		// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): resolve a declared
		// hardpoint socket NAME to the weapon-node INDEX the runtime consumes
		// (index into mechType->nodeData[], in the [numSmokeNodes,
		// numSmokeNodes+numWeaponNodes) weapon-node range -- the SAME domain
		// getWeaponNode() returns). Returns -1 when socketName is null/empty or
		// no weapon node's nodeId matches (case-insensitive, mirroring the slice-2
		// GetNodeId scan). Pure lookup: does NOT increment nodeUsed / recycle and
		// does NOT touch nodeData -- authoritative-vs-fallback is owned by the pure
		// chooseWeaponNode() (hardpoint.h). Used only at load-time mount resolution.
		virtual long resolveWeaponNodeBySocketName (const char* socketName);

		virtual long getLowestWeaponNode (void);
		
		virtual float getWeaponNodeRecycle (long node);
		
		virtual void resetWeaponNodes (void);
		
		virtual void setWeaponNodeRecycle(long nodeId, float time);
		
 		virtual void setSingleStepMode (void)
		{
			singleStepMode ^= true;
		}
		
		virtual void setPrevFrame (void)
		{
			prevStep = true;
		}
		
		virtual void setNextFrame (void)
		{
			nextStep = true;
		}

		virtual void setSensorLevel (long lvl)
		{
			sensorLevel = lvl;
		}
		
		//------------------------------------------------------------------------------------------
		//Puts mech into hit mode IF and ONLY IF the mech is standing, walking, running or limping.
		// NO OTHER GESTURE IS VALID!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		virtual void hitFront (void);
	
		virtual void hitBack (void);
		
		virtual void hitLeft (void);

		virtual void hitRight (void);
		
		virtual void blowLeftArm (void);
		
		virtual void blowRightArm (void);
		
		virtual void setObjStatus (long oStatus);

		virtual bool PerPolySelect (long mouseX, long mouseY);
		
		virtual Stuff::Point3D getRootNodeCenter (void)
		{
			// DEGRADE-DON'T-CRASH: partial import with no base shape -> origin.
			if (!mechShape)
				return Stuff::Point3D(0.0f, 0.0f, 0.0f);
			Stuff::Point3D result = mechShape->GetRootNodeCenter();
			return result;
		}

		virtual void setAlphaValue (BYTE aVal)
		{
			// DEGRADE-DON'T-CRASH: partial import with no base shape.
			if (!mechShape)
				return;
			mechShape->SetAlphaValue(aVal);
			
			//Sensor shape fades in opposite direction from mover
			sensorSquareShape->SetAlphaValue(0xff - aVal);
			sensorTriangleShape->SetAlphaValue(0xff - aVal);
		}

		virtual void setMechName( const char* pName )
		{
			strcpy( mechName, pName );
		}
		
		virtual void startSmoking (long smokeLvl);
		virtual void startWaterWake (void);
		virtual void stopWaterWake (void);
		virtual void playEjection (void);

		void copyTo (MechAppearanceData *data);
		void copyFrom (MechAppearanceData *data); 

		bool leftArmRecalc (void);
		bool rightArmRecalc (void);

		virtual bool getRightArmOff (void)
		{
			return rightArmOff;
		}

		virtual bool getLeftArmOff (void)
		{
			return leftArmOff;
		}

		virtual Stuff::Vector3D getHitNodeLeft (void);
		virtual Stuff::Vector3D getHitNodeRight (void);

		virtual Stuff::Vector3D getNodeIdPosition (long nodeId);
};


//-------------------------------------------------------------------------------

#endif
