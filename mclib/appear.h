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
#include <algorithm>
#include <optional>
#include <atomic>
#ifndef DAPPEAR_H
#include"dappear.h"
#endif

#ifndef DAPRTYPE_H
#include"daprtype.h"
#endif

#ifndef FLOATHELP_H
#include"floathelp.h"
#endif

#include "mc2_czstring.h"
#include<stuff/stuff.hpp>
#include "unit_anim_state.h"
#include "contact_visibility.h"
#include "presentation_phase.h"
#include "../code/terrain_dirty_queue.h"

unsigned currentAppearanceRegistryGeneration (void); // apprtype.cpp
// Forward declaration for prewarmStaticLightBake parameter.
class Camera;
//---------------------------------------------------------------------------
// Macro definitions
#include "dstd.h"  // NO_ERR

inline constexpr float HUD_DEPTH = 0.9999f; // HUD Objects draw over everything else (reverse-Z: near=1, far=0).

inline constexpr int WIDTH = 19; // really half the width...
inline constexpr int HEIGHT = 4;

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
    std::shared_ptr<mc2::sim::UnitAnimState> simAnim_ = std::make_shared<mc2::sim::UnitAnimState>();
    mc2::sim::UnitAnimState renderAnim_;
    bool simulationOwned_ = false;
    long pendingStatus_ = 0;
    float pendingFlash_[2] = {0.0f, 0.0f};
    DWORD pendingFlashColor_ = 0;
    bool hasPendingStatus_ = false, hasPendingFlash_ = false;
    // True while renderAnim_ and appliedEffects_ already hold what presentAnimation(sim) + presentEffects() would leave.
    // Every mutable path to either animation copy clears it (sim worker and presentation may both write: relaxed atomic).
    mutable std::atomic<bool> animSettled_{false};
    // Set once a mover caches simAnim_ (pinnedSimulationAnimation): its writes bypass the clears, so it never settles.
    mutable std::atomic<bool> animPinned_{false};
public:
    // Non-mover appearances and previews retain ordinary visual admission.
    mc2::ContactVisibility contactView = mc2::contactVisibility(CONTACT_VISUAL);
    long terrainPresentationHandle = -1; // only terrain-pool owners bind this
    bool terrainPoseDirty = false; // consumed after the simulation joins
    void markTerrainPresentationDirty() const { mc2::terrainDirtyQueue().mark(terrainPresentationHandle); }
    // Generation of the type registry that issued this holder's appearance type (stamped by init(tree)); 0 = none.
    unsigned registryGeneration = 0;
    bool simulationOwned() const { return simulationOwned_; }
    const mc2::sim::UnitAnimState& presentedAnimation() const { return renderAnim_; }
    mc2::sim::UnitAnimState& unitAnim() { if (!simPresentationPass) markTerrainPresentationDirty(); animSettled_.store(false, std::memory_order_relaxed); return simPresentationPass ? renderAnim_ : *simAnim_; }
    const mc2::sim::UnitAnimState& unitAnim() const { return simPresentationPass ? renderAnim_ : *simAnim_; }
    auto simulationAnimation() const { markTerrainPresentationDirty(); animSettled_.store(false, std::memory_order_relaxed); return simAnim_; }
    auto pinnedSimulationAnimation() const { animPinned_.store(true, std::memory_order_relaxed); return simulationAnimation(); }
    void presentAnimation(const mc2::sim::UnitAnimState& value) { animSettled_.store(false, std::memory_order_relaxed); renderAnim_ = value; flushSimEvents(); }
    // Terrain walk: presentAnimation(sim) + presentEffects(), skipped when neither copy was touched since the last
    // settled visit (a repeat would copy the same state and compare equal effects: a no-op). Clears again if the
    // effects pass mutated renderAnim_ (blown arms) so the next visit re-presents exactly as before.
    void presentSettledAnimation() {
        if (animSettled_.load(std::memory_order_relaxed)) return;
        animSettled_.store(!animPinned_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        renderAnim_ = *simAnim_;
        flushSimEvents();
        presentEffects();
        if (renderAnim_.leftArmOff != simAnim_->leftArmOff || renderAnim_.rightArmOff != simAnim_->rightArmOff)
            animSettled_.store(false, std::memory_order_relaxed);
    }
    // Kill and flash events the tick recorded (it must not touch draw state): consumed here, once per presentation visit.
    // Last status wins; the draw implementations are idempotent.
    void recordSimStatus(long oStatus) { markTerrainPresentationDirty(); pendingStatus_ = oStatus; hasPendingStatus_ = true; }
    void recordSimFlash(float duration, float flashDuration, DWORD color) {
        markTerrainPresentationDirty();
        pendingFlash_[0] = duration; pendingFlash_[1] = flashDuration; pendingFlashColor_ = color; hasPendingFlash_ = true;
    }
    void flushSimEvents() {
        if (hasPendingStatus_) { hasPendingStatus_ = false; setObjStatus(pendingStatus_); }
        if (hasPendingFlash_) { hasPendingFlash_ = false; flashBuilding(pendingFlash_[0], pendingFlash_[1], pendingFlashColor_); }
    }
    void presentEffects() {
        const auto& value = renderAnim_;
        // Requests are tick-owned; effect creation, random debris and shape edits
        // occur only while consuming the completed presentation snapshot.
        if (appliedEffects_.smokeLevel != value.smokeLevel) startSmoking(value.smokeLevel);
        if (appliedEffects_.wakeRequested != value.wakeRequested) {
            if (value.wakeRequested) startWaterWake(); else stopWaterWake();
        }
        if (appliedEffects_.activityEffect != value.activityEffect || appliedEffects_.activityLoop != value.activityLoop) {
            if (value.activityEffect >= 0) startActivity(value.activityEffect, value.activityLoop);
            else if (value.activityEffect == -2) stopActivity();
        }
        if (appliedEffects_.ejectionSerial != value.ejectionSerial) playEjection();
        if (appliedEffects_.destructionSerial != value.destructionSerial) playDestruction();
        if (value.leftArmOff && !appliedEffects_.leftArmOff) { renderAnim_.leftArmOff = false; blowLeftArm(); }
        if (value.rightArmOff && !appliedEffects_.rightArmOff) { renderAnim_.rightArmOff = false; blowRightArm(); }
        appliedEffects_.smokeLevel = value.smokeLevel;
        appliedEffects_.wakeRequested = value.wakeRequested;
        appliedEffects_.activityEffect = value.activityEffect;
        appliedEffects_.activityLoop = value.activityLoop;
        appliedEffects_.ejectionSerial = value.ejectionSerial;
        appliedEffects_.destructionSerial = value.destructionSerial;
        appliedEffects_.leftArmOff = value.leftArmOff;
        appliedEffects_.rightArmOff = value.rightArmOff;
    }
    struct AppliedEffects {
        long smokeLevel = 0, activityEffect = -1;
        uint32_t ejectionSerial = 0, destructionSerial = 0;
        bool wakeRequested = false, activityLoop = false, leftArmOff = false, rightArmOff = false;
    } appliedEffects_;
    virtual void refreshSimPose() {}
    // Completes a simulation pose deferred into the tick's cross-mech batch (Mech3DAppearance); direct simPose readers
    // call it before trusting simPose->ready.
    virtual void settleSimPose() {}
    // What the tick feeds the sim half of an appearance (animation state machine, SimPose). Only the tick writes it; the draw
    // state (position, rotation, status, alpha, bars ...) is pushed by presentation from the owner's state.
    struct SimInput {
        glm::vec3 position{};
        float rotation = 0.0f, torsoRotation = 0.0f;
        long status = 0;
        bool hasStatus = false;   // the owner pushed a status this tick
        bool debugMove = false;
        float pitch = 0.0f, roll = 0.0f;   // vehicles: body pitch/roll request while wrecked
    };
    virtual void setSimInput(const SimInput&) {}
    std::shared_ptr<mc2::sim::SimPose> simPose = std::make_shared<mc2::sim::SimPose>();

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
		// Presentation-walk ground-height cache: valid while xy and TerrainRuntime::g_heightEpoch match.
		float                       groundCacheX = 0.0f, groundCacheY = 0.0f, groundCacheZ = 0.0f;
		uint32_t                    groundCacheEpoch = 0xFFFFFFFFu;

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

		virtual void init (AppearanceTypePtr tree = nullptr, GameObjectPtr obj = nullptr)
		{
			if (tree)
				registryGeneration = currentAppearanceRegistryGeneration();
			simulationOwned_ = obj != nullptr;
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

		// True when the drawn LOD can change with camera distance alone; such
		// objects must keep their per-camera-change presentation visit.
		virtual bool lodFollowsCamera (void) const
		{
			return false;
		}

		// static-world-s2: registry recipe whose render visit this frame settled (submitted to the static registry
		// with a baked light slot and nothing per-frame to redo), else empty. The render extract may then skip the object.
		virtual std::optional<int32_t> residentRenderRecipe (void) const
		{
			return std::nullopt;
		}

		// perf-terrain-walk: true when render() would draw and capture nothing with the current presentation state, so
		// the render extract may leave the object out of the frame's walk. Conservative default: it always draws.
		virtual bool renderIsNoOp (void) const
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
		// The registry recipe that casts this appearance's static shadow: its shadow LOD (LOD0), whatever LOD is drawn. -1 = casts none.
		virtual int32_t getShadowCasterRecipeIndex() const { return getStaticRecipeIndex(); }

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
			return nullptr;
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
			barStatus = std::min(barStatus, 1.0f);
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

		virtual bool setJumpParameters (glm::vec3 &jumpGoal)
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

		// A weapon assigned to a node on an arm that has since been shot off fires from another valid weapon node of
		// a compatible type on the remaining body. Returns node unchanged when it is still usable (read-only, no node bookkeeping).
		virtual long remapLostArmWeaponNode (long node, long weaponType)
		{
			return node;
		}

		virtual void resetWeaponNodes (void)
		{
		}

		virtual void setWeaponNodeRecycle(long nodeId, float time)
		{
		}
		
		virtual glm::vec3 getSmokeNodePosition (long nodeId)
		{
			glm::vec3 position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual glm::vec3 getDustNodePosition (long nodeId)
		{
			glm::vec3 position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
 		virtual glm::vec3 getWeaponNodePosition (long node)
		{
			glm::vec3 position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}

		virtual glm::vec3 getNodePosition (long nodeId)
		{
			glm::vec3 position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual glm::vec3 getNodeNamePosition (mc2::czstring nodeName)
		{
			glm::vec3 position;
			position.x = position.y = position.z = 0.0f;

			return position;
		}
		
		virtual glm::vec3 getNodeIdPosition (long nodeId)
		{
			glm::vec3 position;
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

		virtual void setObjectParameters (const glm::vec3 &pos, float rot, long selected, long team, long homeRelations) = 0;

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
		
		virtual long calcCellsCovered (glm::vec3& pos, short* cellList) {
			return(0);
		}

		virtual void markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
		{
		}
		
		virtual long markMoveMap (bool passable, long* lineOfSightRect, bool useheight = false, short* cellList = nullptr)
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
		
		virtual glm::vec3 getVelocity (void)
		{
			glm::vec3 result;
			result = glm::vec3(0.0f);
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
		
		virtual glm::vec3 getRootNodeCenter (void)
		{
			glm::vec3 result;
			result = glm::vec3(0.0f);
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

		virtual void setMechName( mc2::czstring pName ){}
		
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

		virtual glm::vec3 getHitNode (void)
		{
			glm::vec3 result;
			result = glm::vec3(0.0f);
			return result;
		}

		virtual glm::vec3 getHitNodeLeft (void)
		{
			glm::vec3 result;
			result = glm::vec3(0.0f);
			return result;
		}

		virtual glm::vec3 getHitNodeRight (void)
		{
			glm::vec3 result;
			result = glm::vec3(0.0f);
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
