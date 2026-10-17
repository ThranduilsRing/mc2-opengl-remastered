//---------------------------------------------------------------------------
//
// Appear.h -- File contains the Basic Game Appearance Declaration
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef APPEAR_H
#define APPEAR_H
//---------------------------------------------------------------------------
// Include Files
#ifndef DAPPEAR_H
#include"dappear.h"
#endif

#ifndef DAPRTYPE_H
#include"daprtype.h"
#endif

#ifndef FLOATHELP_H
#include"floathelp.h"
#endif

#include<stuff/stuff.hpp>
// Forward declaration for prewarmStaticLightBake parameter.
class Camera;
//---------------------------------------------------------------------------
// Macro definitions
#ifndef MAX_ULONG
#define MAX_ULONG		0x70000000		//Must be able to square this and still get big number!!
#endif

#ifndef NO_ERR
#define NO_ERR			0
#endif

#define HUD_DEPTH		0.9999f			//HUD Objects draw over everything else (reverse-Z: near=1, far=0).

#define WIDTH			19				// really half the width...
#define HEIGHT			4

inline constexpr float BARTEST = 0.001f;

extern FloatHelpPtr globalFloatHelp;
extern unsigned long currentFloatHelp;


// would have loved to make object flags, but it looks like we're out...
inline constexpr int DRAW_NORMAL = 0x00;
inline constexpr int DRAW_BARS = 0x01;
inline constexpr int DRAW_TEXT = 0x02;
inline constexpr int DRAW_BRACKETS = 0x04;
inline constexpr int DRAW_COLORED = 0x08;


struct _ScenarioMapCellInfo;
struct _GameObjectFootPrint;
//---------------------------------------------------------------------------
// Class definitions
class Appearance
{
	//Data Members
	//-------------
	protected:
		Stuff::Vector4D				screenPos;		//Where am I on Screen?  INCLUDES correct Z now!

		bool						visible;		//Current FOW status to help draw
		bool						seen;			//Current FOW status to help draw
		
	public:

		bool						inView;			//Can I be Seen?
		// alpha-Stage 1 v3 §4: producer-side decomposed gates. Set by
		// recalcBounds() via setVisibilityGatesFromLegacy(). Stage 1 ships
		// them with byte-identical semantics (all four = inView per the
		// helper); later stages migrate per-concern consumers to read
		// the concern-specific bool. lifecycleAlive is EVENT-DRIVEN, NOT
		// camera-driven: set TRUE in init(), cleared only by destroy
		// events (NOT by recalcBounds, NOT by setInView). Spec:
		// docs/superpowers/specs/2026-05-20-appearance-inview-unconflation-design.md
		bool						renderVisible;
		bool						simActive;
		bool						lifecycleAlive;
		bool						aiPresentable;

		// FRAME-JOBS-1: per-object frame stamp. Set by recalcBoundsAndStamp() AFTER
		// recalcBounds() completes. Serial path reads this to skip recompute when
		// MC2_FRAME_JOBS=1. Plain uint32_t; no race (parallelForRange barrier ensures
		// workers finish before serial reads).
		uint32_t                    boundsFrame;

		Stuff::Vector4D				upperLeft;		//used to draw select boxes.  Can be 3D Now!
		Stuff::Vector4D				lowerRight;		//used to draw select boxes.
		
		float						barStatus;		//Status Bar Length.
		DWORD						barColor;		//Status Bar Color.
	
	//Member Functions
	//-----------------
	public:

			
		Appearance (void)
		{
			inView = FALSE;
			// alpha-Stage 1 v3 §5 Stage 1 ctor zero-init contract:
			// renderVisible/simActive/aiPresentable default FALSE (will
			// be overwritten by first recalcBounds via helper).
			// lifecycleAlive defaults TRUE — actor exists at construction
			// time; cleared only by destroy events.
			renderVisible   = FALSE;
			simActive       = FALSE;
			lifecycleAlive  = TRUE;
			aiPresentable   = FALSE;
			boundsFrame     = 0;
			screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
			upperLeft.x = upperLeft.y = upperLeft.z = upperLeft.w = -999.0f;
			lowerRight.x = lowerRight.y = lowerRight.z = lowerRight.w = -999.0f;
			barStatus = 1.0;
			barColor = 0x0;

			visible = seen = false;
		}

		virtual void init (AppearanceTypePtr tree = NULL, GameObjectPtr obj = NULL)
		{
			inView = FALSE;
			// alpha-Stage 1 v3 §5 Stage 1 init zero-init contract
			// (mirror of ctor — see Appearance() above for rationale).
			renderVisible   = FALSE;
			simActive       = FALSE;
			lifecycleAlive  = TRUE;
			aiPresentable   = FALSE;
			boundsFrame     = 0;
			screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
			upperLeft.x = upperLeft.y = upperLeft.z = upperLeft.w = -999.0f;
			lowerRight.x = lowerRight.y = lowerRight.z = lowerRight.w = -999.0f;
			barStatus = 1.0;
			barColor = 0x0;

			visible = seen = false;
		}
		
		virtual void initFX (void)
		{
		}

		virtual void destroy (void)
		{
			init();
		}
		
		virtual ~Appearance (void)
		{
			destroy();
		}
		
		virtual long update (bool animate = true)
		{
			//Perform any frame by frame tasks.  Animations, etc.
			return NO_ERR;
		}

		// Slice 3 (static-update bypass): return true ONLY when this appearance
		// has no per-frame work this frame. Default false = always run update().
		// Per-class overrides supply the actual logic.
		//
		// CONTRACT (load-bearing for Stage 3.D): this predicate must NOT inspect
		// the owning GameObject/TerrainObject — the appearance has no back-pointer.
		// Owner-side transient state (OBJECT_FLAG_FALLING, damage, power-supply,
		// active GOSFX) must be composed at the call site in code/terrobj.cpp.
		// Violating this will silently skip the impact frame for falling trees.
		virtual bool IsStaticNow (void) const
		{
			return false;
		}

		// Stage 3.C: stamp-advance path for static registry. Default no-op.
		// Override in TreeAppearance calls TG_MultiShape::Touch() without
		// running the full vertex transform.
		virtual void touch() {}

		// Stage 3.C: clear static registration when the owner transitions to
		// dynamic (falling, damage, override). Default no-op. Override in
		// TreeAppearance calls GpuStaticPropRegistry::invalidate() internally.
		virtual void invalidateStaticRegistration() {}

		// Task 5 (Track B): mission-load bulk registration. Default no-ops; each
		// concrete appearance class that participates in the static-prop registry
		// overrides these and writes directly to its own typed staticReg member
		// (HC-1: no void* cast, no shared interface).
		virtual void registerStatic()               {}
		virtual bool isStaticRegistered()  const    { return false; }
		// m5 fix (RenderWorld Slice M1): late-spawn path readback. Base
		// returns -1 (no recipe); BldgAppearance / TreeAppearance override
		// to return staticReg.recipeIndex when registered, else -1.
		virtual int32_t getStaticRecipeIndex() const { return -1; }

		// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
		// Called after finalizeGeometry() when world lights are valid.
		// Drains the needsFullBakeNextFrame H4 latch using the permanent SSBO
		// path (mc2WriteStaticLightSlot) so off-screen props are baked at
		// mission load rather than waiting for first on-screen update().
		// Default no-op; BldgAppearance and TreeAppearance override.
		// Returns true if this object was baked in this call.
		virtual bool prewarmStaticLightBake(Camera* cam) { return false; }
		// Returns true if this object still needs a full bake after prewarm.
		virtual bool needsPrewarmBake() const { return false; }

		virtual long render (long depthFixup = 0)
		{
			//Decide whether or not I can be seen and add me to render list.
			return NO_ERR;
		}

		virtual long renderShadows (void)
		{
			return NO_ERR;
		}

		virtual AppearanceTypePtr getAppearanceType (void)
		{
			return NULL;
		}

		bool canBeSeen (void)
		{
			return(inView);
		}

		// alpha-Stage 1 v3 §4.4: producer-side helper. recalcBounds()
		// overrides call this with the computed visibility bit; it sets
		// inView + the 3 new camera-driven gates (renderVisible /
		// simActive / aiPresentable) in lockstep. lifecycleAlive is
		// EXCLUDED — it is event-driven (init/destroy), never
		// camera-cleared. Stage 1 ships with byte-identical semantics:
		// the 3 new bools all equal inView. Stages 2-5 migrate
		// consumers to read their concern-specific bool; Stage 3
		// widens simActive with hysteresis; Stage 6 widens
		// renderVisible with sticky-bit admit.
		void setVisibilityGatesFromLegacy (bool v)
		{
			inView         = v;
			renderVisible  = v;
			simActive      = v;
			aiPresentable  = v;
			// lifecycleAlive intentionally NOT touched — event-driven only.
		}

		void setInView (bool viewStatus)
		{
			// alpha-Stage 1 v3 §5 Stage 1: the 8 force-true callsites
			// (gamecam compass/sky, gate, turret, missiongui HUD-VTOL,
			// mover LOS save/restore) genuinely want the actor to behave
			// as visible across all concerns. Route through the helper
			// so all 4 camera-driven gates stay coherent. lifecycleAlive
			// remains untouched (helper excludes it).
			setVisibilityGatesFromLegacy(viewStatus);
		}
				
		Stuff::Vector4D getScreenPos (void)
		{
			return screenPos;
		}

		virtual bool isMouseOver (float px, float py)
		{
			return FALSE;		//Never over a base appearance
		}
				
		virtual void drawSelectBox (DWORD color);

		virtual void drawSelectBrackets (DWORD color);

		virtual void drawBars (void);

		void drawTextHelp (char *text);
		void drawTextHelp (char *text, unsigned long color);

		void drawPilotName(char *text, unsigned long color ); // next line below drawTextHelp

		
		virtual bool recalcBounds (void)
		{
			//-------------------------------------------------------
			// returns TRUE is this appearance is Visible this frame
			// alpha-Stage 1 v3: route through helper for gate coherence
			// (sets renderVisible/simActive/aiPresentable = FALSE in
			// lockstep with inView; lifecycleAlive untouched).
			setVisibilityGatesFromLegacy(FALSE);
			return inView;
		}

		// Returns true only for types audited as worker-safe for parallel recalcBounds.
		// Default false = serial handles this type.
		virtual bool isRecalcBoundsWorkerSafe() const { return false; }

		// FRAME-JOBS-2B: opt-in for parallel touch() prepass. Default false — only
		// audited appearance families override this. Worker path checks this BEFORE
		// calling touch(). Serial path is unchanged.
		virtual bool isTouchWorkerSafe() const { return false; }

		// FRAME-JOBS-2D: split touch() into two phases to eliminate mutex contention.
		// isTouchSplitSafe() — true if this type implements the split. Default false.
		// touchWorkerPrepass() — lock-free per-instance prep; called on worker threads.
		//   MUST NOT acquire any mutex, call txmmgr light-data paths, emit GL, or
		//   touch shared state.
		// touchSerialCommit() — light-data resubmit; called serially on main thread
		//   after worker join. Default delegates to touch() for non-split types.
		virtual bool isTouchSplitSafe() const { return false; }
		virtual void touchWorkerPrepass() {}
		virtual void touchSerialCommit() { touch(); }

		// FRAME-JOBS-1: worker-path entry point. Calls recalcBounds() then stamps boundsFrame.
		// Base no-op exists ONLY for safe virtual dispatch through Appearance*.
		// Worker path checks isRecalcBoundsWorkerSafe() BEFORE calling — the no-op
		// is never reached in practice. Do NOT remove the whitelist check on the
		// grounds that "the base no-op makes it safe": the base no-op skips the
		// stamp, so the serial path would re-compute — correctness is not the
		// concern, silent no-op behaviour for unaudited types is.
		virtual void recalcBoundsAndStamp() {}

		virtual void setGesture (unsigned long gestureId)
		{
		}

		virtual long setGestureGoal (long gestureId)
		{
			return 0;
		}
		
		virtual void setVelocityPercentage (float percent)
		{
		}
		
		virtual long getFrameNumber (void)
		{
			return 0;
		}

		virtual unsigned long getAppearanceClass (void)
		{
			return BASE_APPEARANCE;
		}
		
		virtual void setBarStatus (float stat)
		{
			barStatus = stat;
			if (barStatus > 1.0f)
				barStatus = 1.0f;
		}

		virtual void setBarColor (DWORD clr)
		{
			barColor = clr;
		}

		virtual bool getInTransition (void)
		{
			return false;
		}

		virtual long getNumFramesInGesture (long gestureId)
		{
			return 0;
		}

		virtual long getOldGestureGoal (void)
		{
			return -1;
		}
		
		virtual long getCurrentGestureGoal (void)
		{
			return -1;
		}

		virtual long getCurrentGestureId (void)
		{
			return 0;
		}

		virtual bool isInJump (void)
		{
			return false;
		}

		virtual bool isJumpSetup (void)
		{
			return false;
		}

		virtual bool isJumpAirborne (void)
		{
			return false;
		}

		virtual bool haveFallen (void)
		{
			return false;
		}

		virtual void setObjectNameId (long objId)
		{
		}

		virtual bool setJumpParameters (Stuff::Vector3D &jumpGoal)
		{
			return false;
		}

		virtual void setWeaponNodeUsed (long nodeId)
		{
		}

		virtual long getLowestWeaponNode (void)
		{
			return 0;
		}
		
		virtual long getWeaponNode (long weapontype)
		{
			return 0;
		}
		
		virtual float getWeaponNodeRecycle (long node)
		{
			return 0.0f;
		}

		virtual void resetWeaponNodes (void)
		{
		}

		virtual void setWeaponNodeRecycle(long nodeId, float time)
		{
		}
		
		virtual Stuff::Vector3D getSmokeNodePosition (long nodeId)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual Stuff::Vector3D getDustNodePosition (long nodeId)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
 		virtual Stuff::Vector3D getWeaponNodePosition (long node)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}

		virtual Stuff::Vector3D getNodePosition (long nodeId)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual Stuff::Vector3D getNodeNamePosition (const char *nodeName)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual Stuff::Vector3D getNodeIdPosition (long nodeId)
		{
			Stuff::Vector3D position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}

		virtual void setCombatMode (bool combatMode)
		{
		}

		virtual float getVelocityMagnitude (void)
		{
			return 0.0f;
		}

		virtual float getVelocityOfGesture (long gestureId)
		{
			return 0.0f;
		}


		virtual void setBrake (bool brake)
		{
		}

		virtual void setObjectParameters (const Stuff::Vector3D &pos, float rot, long selected, long team, long homeRelations) = 0;

		virtual void setMoverParameters (float turretRot, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false)
		{
		}

		virtual void updateFootprints (void)
		{
		}

		virtual void setPaintScheme (void)
		{
		}

		virtual void setPaintScheme (DWORD red, DWORD green, DWORD blue)
		{
		}

		virtual void getPaintScheme (DWORD &red, DWORD &green, DWORD &blue)
		{
		}

		virtual void resetPaintScheme (DWORD red, DWORD green, DWORD blue)
		{
		}
		
		virtual void setDebugMoveMode (void)
		{
		}

		virtual void setSingleStepMode (void)
		{
		}
		
		virtual void setPrevFrame (void)
		{
		}
		
		virtual void setNextFrame (void)
		{
		}

		virtual void setVisibility (bool vis, bool sen)
		{
			visible = vis;
			seen = sen;
		}

		virtual void setSensorLevel (long lvl)
		{
		}
		
		virtual void hitFront (void)
		{
		}
	
		virtual void hitBack (void)
		{
		}
		
		virtual void hitLeft (void)
		{
		}

		virtual void hitRight (void)
		{
		}
		
		virtual void setObjStatus (long oStatus)
		{
		}
		
		virtual long calcCellsCovered (Stuff::Vector3D& pos, short* cellList) {
			return(0);
		}

		virtual void markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
		{
		}
		
		virtual long markMoveMap (bool passable, long* lineOfSightRect, bool useheight = false, short* cellList = NULL)
		{
			return(0);
		}

		virtual void markLOS (bool clearIt = false)
		{
		
		}
		
		virtual void scale (float scaleFactor)
		{
		}
		
		virtual bool playDestruction (void)
		{
			return false;
		}
		
		virtual float getRadius (void)
		{
			return 0.0f;
		}
		
		virtual void flashBuilding (float duration, float flashDuration, DWORD color)
		{
		}
		
		virtual void setHighlightColor( long argb )
		{
		}
	
		virtual float getTopZ (void)
		{
			return 0.0f;
		}
		
		virtual void blowLeftArm (void)
		{
		}
		
		virtual void blowRightArm (void)
		{
		}

		virtual void setFilterState (bool state)
		{
		}
		
		virtual Stuff::Vector3D getVelocity (void)
		{
			Stuff::Vector3D result;
			result.Zero();
			return result;
		}

		virtual bool isSelectable()
		{
			return true;
		}
		
		virtual void setIsHudElement (void)
		{
		}

		virtual long getObjectNameId ()
		{
			return -1;
		}

		virtual bool getIsLit (void)
		{
			return false;
		}
		
		virtual void setLightsOut (bool lightFlag)
		{
		
		}

		virtual bool PerPolySelect (long mouseX, long mouseY)
		{
			return true;
		}

		virtual bool isForestClump (void)
		{	
			return false;
		}
		
		virtual Stuff::Point3D getRootNodeCenter (void)
		{
			Stuff::Point3D result;
			result.Zero();
			return result;
		}
		
		virtual void setAlphaValue (BYTE aVal)
		{
		}

		void drawIcon(  unsigned long bmpHandle, unsigned long bmpWidth,
						  unsigned long bmpHeight, unsigned long color, 
						  unsigned long where = 0 );
						  
		virtual void setSkyNumber (long skyNum)
		{
		}

		virtual void setMechName( const char* pName ){}
		
		virtual void startSmoking (long smokeLvl)
		{
		}
		
		virtual void startWaterWake (void)
		{
		}
		
		virtual void stopWaterWake (void)
		{
		}
		
		virtual void playEjection (void)
		{
		}
		
		virtual void startActivity (long effectId, bool loop)
		{
		}
		
		virtual void stopActivity (void)
		{
		}

		virtual Stuff::Vector3D getHitNode (void)
		{
			Stuff::Point3D result;
			result.Zero();
			return result;
		}

		virtual Stuff::Vector3D getHitNodeLeft (void)
		{
			Stuff::Point3D result;
			result.Zero();
			return result;
		}

		virtual Stuff::Vector3D getHitNodeRight (void)
		{
			Stuff::Point3D result;
			result.Zero();
			return result;
		}

		virtual bool getRightArmOff (void)
		{
			return false;
		}

		virtual bool getLeftArmOff (void)
		{
			return false;
		}

		virtual bool hasAnimationData (long gestureId)
		{
			return false;
		}
};		

//---------------------------------------------------------------------------
#endif
