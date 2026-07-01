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
#include"mech3d.h"
#endif

#include "gos_static_prop_killswitch.h"  // g_useGpuStaticProps
#include "../GameOS/gameos/gpu_cull_readback.h"  // C3: GPU visibility queries
#include "../code/gameobj.h"  // C3: full GameObject definition for obj->getHandle() in init()
#include "../GameOS/gameos/gos_mech_batcher.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include "anim_override_registry.h"  // ANIM-OVERRIDE-MVP: declarative gesture-clip remap
#include "mech_anim_runtime.h"  // BT2018-SKEL-ENGINE-1B-RUNTIME: imported-mech per-frame re-bake
#include "../GameOS/gameos/diagnostic_trace.h"  // ANIM_ADVANCE trace (double-step recon)

extern uint32_t g_mc2FrameCounter;  // defined mclib/tgl.cpp:3718 — ANIM_ADVANCE double-step probe
#include "spotlight_diag.h"  // T1.16 — (E)-owned slot tagging for per-slot probe
#include <cstdint>  // M2.5 (Q6 amendment 2): uint64_t for MLR mech draw counter
#include <unordered_map>  // ANIM-CADENCE-GUARD-1: per-appearance last-advance frame stamp
#include <vector>  // PRESENTATION-SOCKET-PROFILE-1: DeclaredSocketCheck list
#include "unitprofile.h"              // PRESENTATION-SOCKET-PROFILE-1: mc2UnitProfileDataEnabled()
#include "unit_profile_validator.h"  // PRESENTATION-SOCKET-PROFILE-1: validateDeclaredSockets()

// M2.5 (Q6 amendment 2): always-on MLR mech draw counter. Incremented
// at the legacy mechShape->Render(true) fallback site (~line 2623) so
// the M2.6 readiness decision rule has live data on Path-B incidence.
// NOT env-gated: M2.6 must consult this number regardless of
// MC2_OBJECT_ID_BUFFER state. Consumed per-mission by
// GpuMechBatcher::onMapUnload() via consumeAndResetMlrMechDraws().
static uint64_t s_mlrMechDrawsThisMission = 0;

// M2.5: getter for the per-mission MLR draw count, callable from
// GpuMechBatcher::onMapUnload() in a different TU. Declaration in
// gos_mech_batcher.cpp is at file scope per external-review C1.
extern "C" uint64_t consumeAndResetMlrMechDraws() {
    const uint64_t v = s_mlrMechDrawsThisMission;
    s_mlrMechDrawsThisMission = 0;
    return v;
}

// MC2_MECH_LOD_TRACE=1: per-actor LOD-swap boundary print.
static const bool s_mechLodTrace = (getenv("MC2_MECH_LOD_TRACE") != nullptr);

// C3: env-gated lifecycle routing killswitch (same env var as objmgr.cpp).
// MC2_GPU_CULL_LIFECYCLE=1 enables GPU visibility-based node-position early-outs.
static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);

// MC2_GPU_CULL_LIFECYCLE_TRACE=1: verbose per-actor lifecycle boundary prints.
// Default off (too noisy for production). Fires at: init, first GPU-cull skip per actor.
static const bool s_lcTrace = (getenv("MC2_GPU_CULL_LIFECYCLE_TRACE") != nullptr);
static uint32_t s_lcSkipCount3d = 0u;
#define LC3D_TRACE(fmt, ...) \
    do { if (s_lcTrace) { printf("[GPU_CULL_LIFECYCLE v1] mech3d " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while(0)

// T1.15 SpotLight_ illumination diagnostic — registration probe (mech class).
// First-hit always-on; per-summary every 600 updateGeometry calls when env=1.
static const bool s_spotDiagMechEnabled = (getenv("MC2_SPOT_DIAG") != nullptr);
static unsigned long s_spotDiagMechRegistered = 0;
static unsigned long s_spotDiagMechOverflows  = 0;
static unsigned long s_spotDiagMechActors     = 0;
static unsigned long s_spotDiagMechCalls      = 0;

// MC2_FX_COUNT_LOG=1: env-gated per-effect-site GOSFX draw counter for the
// Mech3DAppearance::render() effect sites. Default OFF, zero behavior change.
// Counts draw attempts (control reaching the site); emits a [FX_COUNT v1]
// summary line every 9000 ticks. Agent-checkable oracle for effect counts.
static const bool s_fxCountLog = (getenv("MC2_FX_COUNT_LOG") != nullptr);
namespace {
	enum FxSite {
		FX_RDUST=0, FX_LDUST, FX_JUMP, FX_LBOOM, FX_RBOOM,
		FX_CSMOKE, FX_SMOKE, FX_WAKE, FX_HELIDUST, FX_RARMSMOKE, FX_LARMSMOKE,
		FX_SITE_COUNT
	};
	static unsigned long long s_fxDrawCounts[FX_SITE_COUNT] = {0};
	static unsigned long long s_fxTotalDraws = 0;
	static void fxCountTick(int site) {
		++s_fxDrawCounts[site];
		if ((++s_fxTotalDraws % 9000ULL) == 0ULL) {
			std::printf(
				"[FX_COUNT v1] event=summary total=%llu rDust=%llu lDust=%llu jump=%llu "
				"lBoom=%llu rBoom=%llu critSmoke=%llu smoke=%llu wake=%llu heliDust=%llu "
				"rArmSmoke=%llu lArmSmoke=%llu\n",
				s_fxTotalDraws,
				s_fxDrawCounts[FX_RDUST], s_fxDrawCounts[FX_LDUST], s_fxDrawCounts[FX_JUMP],
				s_fxDrawCounts[FX_LBOOM], s_fxDrawCounts[FX_RBOOM], s_fxDrawCounts[FX_CSMOKE],
				s_fxDrawCounts[FX_SMOKE], s_fxDrawCounts[FX_WAKE], s_fxDrawCounts[FX_HELIDUST],
				s_fxDrawCounts[FX_RARMSMOKE], s_fxDrawCounts[FX_LARMSMOKE]);
		}
	}
	// Shutdown emit via atexit() — mirrors the proven shutdownTexResolveTable
	// pattern (tex_resolve_table.cpp:78). A static with a CONSTRUCTOR side
	// effect runs reliably at dynamic-init; the registered handler fires at
	// clean process exit and always prints final per-site totals (even total=0
	// if a short/idle mission drew no effects — proves the chain end to end).
	static void fxCountEmit() {
		if (!s_fxCountLog) return;
		std::printf(
			"[FX_COUNT v1] event=shutdown total=%llu rDust=%llu lDust=%llu jump=%llu "
			"lBoom=%llu rBoom=%llu critSmoke=%llu smoke=%llu wake=%llu heliDust=%llu "
			"rArmSmoke=%llu lArmSmoke=%llu\n",
			s_fxTotalDraws,
			s_fxDrawCounts[FX_RDUST], s_fxDrawCounts[FX_LDUST], s_fxDrawCounts[FX_JUMP],
			s_fxDrawCounts[FX_LBOOM], s_fxDrawCounts[FX_RBOOM], s_fxDrawCounts[FX_CSMOKE],
			s_fxDrawCounts[FX_SMOKE], s_fxDrawCounts[FX_WAKE], s_fxDrawCounts[FX_HELIDUST],
			s_fxDrawCounts[FX_RARMSMOKE], s_fxDrawCounts[FX_LARMSMOKE]);
		std::fflush(stdout);
	}
	// atexit registration happens once from Mech3DAppearance::render() (a code
	// path guaranteed to run), not from a discardable anon-namespace static.
}

// Returns true if the texture file or its .ktx2 BC7 sidecar exists.
// Used to gate TGL texture loading so sensor shapes work when .tga is absent
// in a slim deploy (route-2 in txmmgr.cpp will decode the .ktx2 instead).
static bool textureOrKtxSidecarExists(const char* tgaPath)
{
	if (fileExists(tgaPath))
		return true;
	const char* v = getenv("MC2_TEXMGR_KTX_PRIMARY");
	const bool ktxPrimary = (!v || !v[0]) ? true : (v[0] != '0');  // default mode 1
	if (!ktxPrimary)
		return false;
	char ktx[1024];
	strncpy(ktx, tgaPath, sizeof(ktx) - 1);
	ktx[sizeof(ktx) - 1] = 0;
	char* dot   = strrchr(ktx, '.');
	char* slash = strrchr(ktx, '/');
	if (dot && (!slash || dot > slash)) *dot = 0;
	if (strlen(ktx) + 6 < sizeof(ktx)) strcat(ktx, ".ktx2");
	return fileExists(ktx);
}

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef MSTATES_H
#include"mstates.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef CEVFX_H
#include"cevfx.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef CRATER_H
#include"crater.h"
#endif

#ifndef GVACTOR_H
#include"gvactor.h"
#endif

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

//-------------------------------------------------------------------------------
// Static Globals
extern float worldUnitsPerMeter;
extern bool useFog;
extern float metersPerWorldUnit;
extern MidLevelRenderer::MLRClipper * theClipper;
bool useNonWeaponEffects = true;
bool useHighObjectDetail = true;
bool InEditor = false;

extern bool MLRVertexLimitReached;
extern bool useShadows;

#define	BODYSTATE_NORMAL			0
#define	BODYSTATE_STANDING			1
#define	BODYSTATE_PARKED			2
#define	BODYSTATE_FALLEN_BACKWARD	3
#define	BODYSTATE_FALLEN_FORWARD	4

#define EXPAND_FACTOR 		(1.25f)

#define HELICOPTER_FACTOR	(25.0f)

char MechStateByGesture[MAX_MECH_ANIMATIONS] = {
	BODYSTATE_PARKED,			// GesturePark					 0 -- Park 
	BODYSTATE_STANDING,			// GestureParkToStand			 1 -- ParkToStand 
	BODYSTATE_STANDING,		// GestureStand					 2 -- Stand 
	BODYSTATE_NORMAL,			// GestureStandToWalk			 3 -- StandToWalk 
	BODYSTATE_NORMAL,			// GestureWalk					 4 -- Walk 
	BODYSTATE_PARKED,		// GestureStandToPark			 5 -- StandToPark
	BODYSTATE_NORMAL,			// GestureWalkToRun				 6 -- WalkToRun
	BODYSTATE_NORMAL,			// GestureRun					 7 -- Run
	BODYSTATE_NORMAL,			// GestureRunToWalk				 8 -- RunToWalk 
	BODYSTATE_NORMAL,			// GestureReverse				 9 -- Reverse
	BODYSTATE_NORMAL,			// GestureStandToReverse		10 -- StandToReverse
	BODYSTATE_NORMAL,	// GestureLimpLeft				11 -- LimpLeft
	BODYSTATE_NORMAL,	// GestureLimpRight				12 -- LimpRight
	BODYSTATE_STANDING,		// GestureIdle					13 -- Idle
	BODYSTATE_FALLEN_BACKWARD,	// GestureFallBackward			14 -- FallBackward 
	BODYSTATE_FALLEN_FORWARD,	// GestureFallForward			15 -- FallForward 
	BODYSTATE_NORMAL,			// GestureHitFront				16 -- HitFront
	BODYSTATE_NORMAL,			// GestureHitBack				17 -- HitBack
	BODYSTATE_NORMAL,			// GestureHitLeft				18 -- HitLeft
	BODYSTATE_NORMAL,			// GestureHitRight				19 -- HitRight
	BODYSTATE_NORMAL,			// GestureJump					20 -- Jump
	BODYSTATE_FALLEN_FORWARD,	// GestureRollover				21 -- Rollover from FallBack to FallForward 
	BODYSTATE_FALLEN_FORWARD,	// GestureGetup					22 -- Get up from FallForward 
	BODYSTATE_FALLEN_FORWARD,	// GestureFallenForward			23 -- Fallen Forward (Single Frame, mech on ground)
	BODYSTATE_FALLEN_FORWARD,	// GestureFallenBackward		24 -- Fallen Backward (Single Frame, mech on ground)
};																

#define MAX_ANIMATION_FILES			25
#define idleMAX						(20.0f)

#define FX_POOF_ID					32
#define FX_JUMP_ID					43
#define FX_SMOKE_ID					48

#define SPIN_RATE		90.0f
#define JUMP_PITCH		(90.0f)

extern bool reloadBounds;
const char* MechAnimationNames[MaxGestures+2] = 
{
	"StandToPark",		//GesturePark							 0 -- Park 
	"ParkToStand",		//GestureParkToStand					 1 -- ParkToStand 
	"",					//GestureStand							 2 -- Stand 
	"STtoWK",			//GestureStandToWalk					 3 -- StandToWalk 
	"Walk",				//GestureWalk							 4 -- Walk 
	"StandToPark",		//GestureStandToPark					 5 -- StandToPark
	"WKtoRN",			//GestureWalkToRun						 6 -- WalkToRun
	"Run",				//GestureRun							 7 -- Run
	"RNToWK",			//GestureRunToWalk						 8 -- RunToWalk 
	"Walk",				//GestureReverse						 9 -- Reverse
	"WKtoST",			//GestureStandToReverse					10 -- StandToReverse
	"LimpLeft",			//GestureLimpLeft						11 -- LimpLeft
	"LimpRight",		//GestureLimpRight						12 -- LimpRight
	"Idle",				//GestureIdle							13 -- Idle
	"FallBackward",		//GestureFallBackward					14 -- FallBackward 
	"FallForward",		//GestureFallForward					15 -- FallForward 
	"HitFront",			//GestureHitFront						16 -- HitFront
	"HitBack",			//GestureHitBack						17 -- HitBack
	"HitLeft",			//GestureHitLeft						18 -- HitLeft
	"HitRight",			//GestureHitRight						19 -- HitRight
	"Jump",				//GestureJump							20 -- Jump
	"GetupBack",		//GestureRollover						21 -- Getup from FallBack 
	"GetupFront",		//GestureGetup							22 -- Getup from FallForward 
	"FallForward",		//GestureFallenForward					23 -- Fallen Forward (Single Frame, mech on ground)
	"FallBackward",		//GestureFallenBackward					24 -- Fallen Backward (Single Frame, mech on ground)
	"FallBackwardDam",	//Destroyed States
	"FallForwardDam"	//Destroyed States
};																

PaintSchemataPtr	Mech3DAppearance::paintSchemata = NULL;
DWORD				Mech3DAppearance::numPaintSchemata = 0;

TG_TypeMultiShapePtr Mech3DAppearanceType::SensorSquareShape = NULL;

extern int ObjectTextureSize;

#define FOOTPRINT_SLOP			2

bool Mech3DAppearanceType::animationLoadingEnabled = true;

//-------------------------------------------------------------------------------
// class Mech3DAppearanceType
void Mech3DAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

	//---------------------------------------------------
	// Load the Gesture Data.  Should be in filename.ini
	FullPathFileName mechIniName;
	mechIniName.init(tglPath,fileName,".ini");

	gosASSERT(fileExists(mechIniName));

	FitIniFile mechFile;

	long result = mechFile.open(mechIniName);
	gosASSERT(result == NO_ERR);
	
	for (long i=0;i<MaxGestures;i++)
	{
		char blockId[256];
		sprintf(blockId,"Gestures%d",i);

		result = mechFile.seekBlock(blockId);
		// sebi
		//gosASSERT(result == NO_ERR);

		result = mechFile.readIdFloat("StartVel",gestures[i].startVel);
		//sebi
		//gosASSERT(result == NO_ERR);

		result = mechFile.readIdFloat("EndVel",gestures[i].endVel);
		//sebi
		//gosASSERT(result == NO_ERR);

		result = mechFile.readIdLong("StartFrame",gestures[i].frameStart);
		//sebi
		//gosASSERT(result == NO_ERR);

		result = mechFile.readIdBoolean("Reverse",gestures[i].reverse);
		//sebi
		//gosASSERT(result == NO_ERR);
		
		result = mechFile.readIdLong("RightFootDown0",gestures[i].rightFootDownFrame0);
		if (result != NO_ERR)
			gestures[i].rightFootDownFrame0 = 99999;
			
		result = mechFile.readIdLong("RightFootDown1",gestures[i].rightFootDownFrame1);
		if (result != NO_ERR)
			gestures[i].rightFootDownFrame1 = 99999;
			
		result = mechFile.readIdLong("LeftFootDown0",gestures[i].leftFootDownFrame0);
		if (result != NO_ERR)
			gestures[i].leftFootDownFrame0 = 99999;
			
		result = mechFile.readIdLong("LeftFootDown1",gestures[i].leftFootDownFrame1);
		if (result != NO_ERR)
			gestures[i].leftFootDownFrame1 = 99999;
 	}

	// ASSIMP-MECH-IMPORT-1 — STRICT OPT-IN. An optional [Import] section with
	// Source=<base> opts a mech into the modern-format (.glb/.fbx) importer for
	// its LOD0 shape. Stock mechs (no [Import] block) leave importSourceBase
	// empty and take the EXACT original LoadTGMultiShapeFromASE path below —
	// byte-identical, no probe. Only [Import] Source= mechs call LoadFromFile.
	char importSourceBase[256] = "";
	// ASSIMP-MECH-IMPORT default-OFF guard. The mech GLB import path is experimental
	// and the only shipped opt-in (the Flea, via flea.ini [Import] Source="Flea")
	// points at a broken Z-up GLB that renders the Fire Ant tipped 90deg. Ignore
	// mech [Import] blocks by default so a stray/redeployed [Import] flea.ini can
	// NEVER put the Flea on the broken path again — it falls back to the stock
	// ASE/FST shape. Set MC2_ASSIMP_MECH_IMPORT=1 to opt back in once a mech's GLB
	// is verified good. (Building/prop GLB import is unaffected — separate path.)
	static const bool s_assimpMechImport = (getenv("MC2_ASSIMP_MECH_IMPORT") != nullptr);
	if (s_assimpMechImport &&
	    mechFile.seekBlock("Import") == NO_ERR &&
	    mechFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
	    importSourceBase[0])
	{
		// Strip extension so LoadFromFile gets a bare base name and probes
		// for the right format itself. "madcat.glb" -> "madcat".
		char* dot = strrchr(importSourceBase, '.');
		if (dot) *dot = '\0';
	}

	result = mechFile.seekBlock("TGLData");
	if (result != NO_ERR)
		STOP(("Mech %s has no TGL Data",fileName));

	char aseFileName[512];
	result = mechFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames
		char aseFileName[512];
		for (long i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			sprintf(baseName,"FileName%d",i);
			sprintf(baseLODDist,"Distance%d",i);
			
			result = mechFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = mechFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				mechShape[i] = new TG_TypeMultiShape;
				gosASSERT(mechShape[i] != NULL);
			
				// ASSIMP-MECH-IMPORT-1 — strict opt-in: only LOD0 of a mech with
				// an [Import] Source= override uses the modern importer; every
				// other slot (incl. all stock mechs) takes the original path.
				if (i == 0 && importSourceBase[0]) {
					mechShape[i]->LoadFromFile(importSourceBase);   // opt-in: modern import
				} else {
					FullPathFileName mechName;
					mechName.init(tglPath,aseFileName,".ase");

					mechShape[i]->LoadTGMultiShapeFromASE(mechName); // stock: EXACT original call, unchanged
				}
			}
			else if (!i)
			{
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		mechShape[0] = new TG_TypeMultiShape;
		gosASSERT(mechShape[0] != NULL);

		// ASSIMP-MECH-IMPORT-1 — strict opt-in single-LOD case.
		if (importSourceBase[0]) {
			mechShape[0]->LoadFromFile(importSourceBase);   // opt-in: modern import
		} else {
			FullPathFileName mechName;
			mechName.init(tglPath,aseFileName,".ase");

			mechShape[0]->LoadTGMultiShapeFromASE(mechName); // stock: EXACT original call, unchanged
		}
	}

	// Register all loaded LODs with the GPU mech batcher (idempotent).
	// Pre-finalize at this point — finalizeGeometry() runs at end of map load.
	for (int lod = 0; lod < MAX_LODS; ++lod) {
		if (mechShape[lod]) {
			GpuMechBatcher::instance().registerTypeLod(this, lod);
		}
	}

	result = mechFile.readIdString("ShadowName",aseFileName,511);
	if (result == NO_ERR)
	{
		//----------------------------------------------
		// Base Shadow shape.
		mechShadowShape = new TG_TypeMultiShape;
		gosASSERT(mechShadowShape != NULL);
	
		FullPathFileName gvName;
		gvName.init(tglPath,aseFileName,".ase");
	
		mechShadowShape->LoadTGMultiShapeFromASE(gvName);
	}

	//------------------------------------
	// Load Arms to blow off Next.
	char leftArmName[1024];
	sprintf(leftArmName,"%sLeftArm",fileName);
	
	FullPathFileName mechLeftArmName;
	mechLeftArmName.init(tglPath,leftArmName,".ase");

	char rightArmName[1024];
	sprintf(rightArmName,"%sRightArm",fileName);
	
	FullPathFileName mechRightArmName;
	mechRightArmName.init(tglPath,rightArmName,".ase");

	rightArm = new TG_TypeMultiShape;
	gosASSERT(rightArm != NULL);
	
	result = rightArm->LoadTGMultiShapeFromASE(mechRightArmName);
	if (result)
	{
		delete rightArm;
		rightArm = NULL;
	}
	
	leftArm = new TG_TypeMultiShape;
	gosASSERT(leftArm != NULL);
	
	result = leftArm->LoadTGMultiShapeFromASE(mechLeftArmName);
	if (result)
	{
		delete leftArm;
		leftArm = NULL;
	}
 	//----------------------------------------------
	
	//-----------------------------------------------
	// Load Damaged States
	// They are named mechnamefallForward and mechnameFallBackward
	//
	char forwardName[1024];
	sprintf(forwardName,"%sFallForwardDam",fileName);

	char backwardName[1024];
	sprintf(backwardName,"%sFallForwardDam",fileName);

	{
		FullPathFileName dmgName;
		dmgName.init(tglPath,forwardName,".ase");

		mechForwardDmgShape = new TG_TypeMultiShape;
		gosASSERT(mechForwardDmgShape != NULL);
		mechForwardDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!mechForwardDmgShape->GetNumShapes())
		{
			delete mechForwardDmgShape;
			mechForwardDmgShape = NULL;
		}
	}

	{
		FullPathFileName dmgName;
		dmgName.init(tglPath,backwardName,".ase");

		mechBackwardDmgShape = new TG_TypeMultiShape;
		gosASSERT(mechBackwardDmgShape != NULL);
		mechBackwardDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!mechBackwardDmgShape->GetNumShapes())
		{
			delete mechBackwardDmgShape;
			mechBackwardDmgShape = NULL;
		}
	}

 	//-----------------------
	// Load Animations Next.
	if (animationLoadingEnabled)
	{
		for (int i=0;i<MAX_ANIMATION_FILES;i++)
		{
			char animName[512];
			sprintf(animName,"%s%s",fileName,MechAnimationNames[i]);

			FullPathFileName animPath;
			FullPathFileName otherPath;

			// ANIM-OVERRIDE-MVP: declarative gesture remap. If the active mod's
			// data/anim_overrides/anims.json remaps this (mech,gesture), load the
			// override clip from the mod dir; otherwise use the stock
			// <mech><GestureSuffix> convention path. Default-off: with no manifest
			// resolve() returns null -> stock path -> zero stock behaviour change.
			const AnimOverrideRecord* animOv =
				AnimOverrideRegistry::instance().resolve(fileName, MechAnimationNames[i]);
			if (animOv)
			{
				animPath.init(animOv->manifestDir.c_str(), animOv->sourceBase.c_str(), ".ase");
				otherPath.init(animOv->manifestDir.c_str(), animOv->sourceBase.c_str(), ".agl");
			}
			else
			{
				animPath.init(tglPath,animName,".ase");
				otherPath.init(tglPath,animName,".agl");
			}

			if (fileExists(animPath) || fileExists(otherPath))
			{
				mechAnim[i] = new TG_AnimateShape;
				gosASSERT(mechAnim[i] != NULL);

				//---------------------------------------------------------------------------------------------
				// If this animation does not exist, it is not a problem!
				// Mech will simply freeze until animation is "over" and then move to next animation in chain.
				mechAnim[i]->LoadTGMultiShapeAnimationFromASE(animPath,mechShape[0]);

				if (gestures[i].reverse)
					mechAnim[i]->ReverseFrameRate();
			}
			else
				mechAnim[i] = NULL;
		}
	}

	//------------------------------
	// Load up the foot print type.
	result = mechFile.seekBlock("FootPrint");
	if (result != NO_ERR)
	{
		leftFootprintType = 0;
		rightFootprintType = 0;
	}
		
	result = mechFile.readIdLong("FootprintType",rightFootprintType);
	if (result != NO_ERR)
	{
		result = mechFile.readIdLong("RightFootprintType",rightFootprintType);
		if (result != NO_ERR)
		{
			leftFootprintType = 0;
			rightFootprintType = 0;
		}
		
		result = mechFile.readIdLong("LeftFootprintType",leftFootprintType);
		if (result != NO_ERR)
			leftFootprintType = rightFootprintType;
	}
	else
	{
		leftFootprintType = rightFootprintType;
	}
	
	//-----------------------------------------------
	// Load up the Node Data.
	result = mechFile.seekBlock("Nodes");
	if (result != NO_ERR)
		numSmokeNodes = numWeaponNodes = numJumpNodes = 0;
		
	result = mechFile.readIdLong("NumSmoke",numSmokeNodes);
	if (result != NO_ERR)
		numSmokeNodes = 0;
		
	result = mechFile.readIdLong("NumWeapon",numWeaponNodes);
	if (result != NO_ERR)
		numWeaponNodes = 0;

	result = mechFile.readIdLong("NumJumpjet",numJumpNodes);
	if (result != NO_ERR)
		numJumpNodes = 0;

	result = mechFile.readIdLong("NumFeet",numFootNodes);
	if (result != NO_ERR)
		numFootNodes = 0;

	if (numJumpNodes+numWeaponNodes+numSmokeNodes+numFootNodes)
	{
		nodeData = (NodeData *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(NodeData)*(numJumpNodes+numWeaponNodes+numSmokeNodes+numFootNodes));
		gosASSERT(nodeData != NULL);
		
		for (int i=0;i<numSmokeNodes;i++)
		{
			char blockId[512];
			sprintf(blockId,"SmokeNode%d",i);
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char smokeName[512];
			result = mechFile.readIdString("SmokeNodeName",smokeName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(smokeName)+1); 
			gosASSERT(nodeData[i].nodeId != NULL);
			
			strcpy(nodeData[i].nodeId,smokeName);
			nodeData[i].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
		
		for (int i=0;i<numWeaponNodes;i++)
		{
			char blockId[512];
			sprintf(blockId,"WeaponNode%d",i);
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char weaponName[512];
			result = mechFile.readIdString("WeaponNodeName",weaponName,511);
			gosASSERT(result == NO_ERR);
			
			long weaponType;
			result = mechFile.readIdLong("WeaponType",weaponType);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(weaponName)+1); 
			gosASSERT(nodeData[i+numSmokeNodes].nodeId != NULL);
			
			strcpy(nodeData[i+numSmokeNodes].nodeId,weaponName);
			nodeData[i+numSmokeNodes].weaponType = weaponType;
			
 		}
		
		for (int i=0;i<numJumpNodes;i++)
		{
			char blockId[512];
			sprintf(blockId,"JumpJetNode%d",i);
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char jumpName[512];
			result = mechFile.readIdString("JumpNodeName",jumpName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes+numWeaponNodes].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(jumpName)+1); 
			gosASSERT(nodeData[i+numSmokeNodes+numWeaponNodes].nodeId != NULL);
			
			strcpy(nodeData[i+numSmokeNodes+numWeaponNodes].nodeId,jumpName);
			nodeData[i+numSmokeNodes+numWeaponNodes].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
		
		for (int i=0;i<numFootNodes;i++)
		{
			char blockId[512];
			sprintf(blockId,"FootNode%d",i);
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char footName[512];
			result = mechFile.readIdString("FootNodeName",footName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(footName)+1); 
			gosASSERT(nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].nodeId != NULL);
			
			strcpy(nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].nodeId,footName);
			nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
	}
	
  	//----------------------------------------------
	// Load up sensor textures if not yet defined.
	// For helicopters, load up the vehicle one!!
	if (SensorSquareShape == NULL && numJumpNodes)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"squarecontact",".ase");
		
		SensorSquareShape = new TG_TypeMultiShape;
		gosASSERT(SensorSquareShape != NULL);
		
		SensorSquareShape->LoadTGMultiShapeFromASE(sensorName);
	}

	if (GVAppearanceType::SensorCircleShape == NULL && !numJumpNodes)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"circularcontact",".ase");
	
		GVAppearanceType::SensorCircleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorCircleShape != NULL);
	
		GVAppearanceType::SensorCircleShape->LoadTGMultiShapeFromASE(sensorName);
	}

	if (GVAppearanceType::SensorTriangleShape == NULL)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"trianglecontact",".ase");
	
		GVAppearanceType::SensorTriangleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorTriangleShape != NULL);

		GVAppearanceType::SensorTriangleShape->LoadTGMultiShapeFromASE(sensorName);
	}

	// PRESENTATION-SOCKET-PROFILE-1 (Phase-2 slice 2): OPTIONAL data-declared
	// presentation-socket table + report-only validation. Additive & fallback:
	// stock mechs have no [PresentationSockets] block -> the seekBlock fails
	// side-effect-free (BLOCK_NOT_FOUND, cursor untouched) -> numPresentationSockets
	// stays 0 -> zero findings -> full stock parity. This reads and VALIDATES the
	// table only; it does NOT feed getWeaponNode / nodeData / any render path (that
	// is a later slice). Format mirrors the [Nodes] idiom above:
	//   [PresentationSockets]
	//   NumSockets = N
	//   [Socket0] SocketName = <model node name>
	numPresentationSockets = 0;
	if (mechFile.seekBlock("PresentationSockets") == NO_ERR)
	{
		long numSockets = 0;
		if (mechFile.readIdLong("NumSockets", numSockets) != NO_ERR)
			numSockets = 0;
		if (numSockets > MAX_PRESENTATION_SOCKETS)
			numSockets = MAX_PRESENTATION_SOCKETS;
		for (long si = 0; si < numSockets; si++)
		{
			char blockId[64];
			sprintf(blockId, "Socket%ld", si);
			if (mechFile.seekBlock(blockId) != NO_ERR)
				continue;
			char socketName[PRESENTATION_SOCKET_NAME_CAP] = "";
			if (mechFile.readIdString("SocketName", socketName,
			                          PRESENTATION_SOCKET_NAME_CAP - 1) != NO_ERR)
				continue;
			if (!socketName[0])
				continue;
			strncpy(presentationSocketName[numPresentationSockets], socketName,
			        PRESENTATION_SOCKET_NAME_CAP - 1);
			presentationSocketName[numPresentationSockets][PRESENTATION_SOCKET_NAME_CAP - 1] = '\0';
			numPresentationSockets++;
		}
	}

	// Report-only socket validation, GATED behind MC2_UNIT_PROFILE_DATA. Gate-OFF
	// (and gate-ON with no declared sockets, i.e. every stock unit) -> no output,
	// behavioral parity. For each declared socket, ask the loaded LOD0 shape "does
	// a node of this exact name exist?" -- the impure half of the pure
	// validateDeclaredSockets() check. GetNodeId()-scan mirrors the same node-name
	// lookup GetTransformedNodePosition() uses at runtime.
	if (mc2UnitProfileDataEnabled() && numPresentationSockets > 0)
	{
		std::vector<DeclaredSocketCheck> declared;
		declared.reserve(numPresentationSockets);
		for (long si = 0; si < numPresentationSockets; si++)
		{
			DeclaredSocketCheck d;
			d.socket  = HardpointSocketName(presentationSocketName[si]);
			d.present = false;
			// Node-name present iff LOD0 shape has a TypeShape with this nodeId.
			if (mechShape[0])
			{
				const long numNodes = mechShape[0]->GetNumShapes();
				for (long ni = 0; ni < numNodes; ni++)
				{
					const char* nid = mechShape[0]->GetNodeId(ni);
					if (nid && S_stricmp(nid, presentationSocketName[si]) == 0)
					{
						d.present = true;
						break;
					}
				}
			}
			declared.push_back(d);
		}
		// The pure check owns the verdict (one finding per absent socket); the glue
		// only names each missing socket for the modder. `declared` preserves
		// declaration order, so a non-empty findings list means >=1 absent entry.
		const std::vector<UnitProfileFinding> findings = validateDeclaredSockets(declared);
		if (!findings.empty())
		{
			for (long si = 0; si < numPresentationSockets; si++)
			{
				if (!declared[si].present)
					printf("[UnitProfile:VALIDATE] %s: declared socket '%s' not found on model (reason=%d)\n",
					       fileName ? fileName : "<unknown>",
					       presentationSocketName[si],
					       static_cast<int>(UPV_HARDPOINT_SOCKET_MISSING));
			}
		}
	}

}

//----------------------------------------------------------------------------
void Mech3DAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	long i=0;
	for (i=0;i<MAX_LODS;i++)
	{
		if (mechShape[i])
		{
			delete mechShape[i];
			mechShape[i] = NULL;
		}
	}

	if (mechShadowShape)
	{
		delete mechShadowShape;
		mechShadowShape = NULL;
	}

	if (rightArm)
	{
		delete rightArm;
		rightArm = NULL;
	}

	if (leftArm)
	{
		delete leftArm;
		leftArm = NULL;
	}

	if (mechForwardDmgShape)
	{
		delete mechForwardDmgShape;
		mechForwardDmgShape = NULL;
	}

	if (mechBackwardDmgShape)
	{
		delete mechBackwardDmgShape;
		mechBackwardDmgShape = NULL;
	}

	for (i=0;i<MAX_ANIMATION_FILES;i++)
	{
		delete mechAnim[i];
		mechAnim[i] = NULL;
	}
	
	for (i=0;i<getTotalNodes();i++)
	{
		AppearanceTypeList::appearanceHeap->Free(nodeData[i].nodeId);
		nodeData[i].nodeId = NULL;
	}

	AppearanceTypeList::appearanceHeap->Free(nodeData);
	
	nodeData = NULL;
	numSmokeNodes = numWeaponNodes = numJumpNodes = 0;
}

//----------------------------------------------------------------------------
void Mech3DAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != NULL);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MaxGestures);

	if (mechAnim[animationNum])
		mechAnim[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

//-----------------------------------------------------------------------------

#define MAX_MECHS					1
Stuff::Vector3D debugMechActorPosition[MAX_MECHS];
float mechDebugAngle[MAX_MECHS];
float torsoDebugAngle[MAX_MECHS];
//-----------------------------------------------------------------------------
// class Mech3DAppearance
void Mech3DAppearance::resetWeaponNodes (void)
{
	//THis should never be called after the game inits!!
	for (long i=0;i<mechType->numWeaponNodes;i++)
	{
		nodeUsed[i] = 0;
		nodeRecycle[i] = BASE_NODE_RECYCLE_TIME;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::setWeaponNodeUsed (long weaponNode)
{
	//THis should never be called after the game inits!!
	weaponNode -= mechType->numSmokeNodes;
   	if ((weaponNode >= 0) && (weaponNode < mechType->numWeaponNodes))
	{
		nodeUsed[weaponNode]++;
		nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::setWeaponNodeRecycle(long nodeId, float time)
{
	nodeId -= mechType->numSmokeNodes;
   	if ((nodeId >= 0) && (nodeId < mechType->numWeaponNodes))
		nodeRecycle[nodeId] = time;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D Mech3DAppearance::getWeaponNodePosition (long nodeId)
{
	Stuff::Vector3D result = position;
	if ((nodeId < mechType->numSmokeNodes) || (nodeId >= (mechType->numSmokeNodes+mechType->numWeaponNodes)))
		return result;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	// 1-frame weapon-spawn-root artifact on visibility transition: accepted by design.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))) {
			++s_lcSkipCount3d;
			if (s_lcSkipCount3d == 1u || (s_lcSkipCount3d % 600u) == 0u)
				LC3D_TRACE("event=node_skip actorHandle=%ld total=%u", actorHandle_, s_lcSkipCount3d);
			return result;
		}
	} else {
		if (!inView)
			return result;
	}

	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!

	// 1A (BT2018-MECH-NODE-MANIFEST-1A): an imported mech's merged single shape has
	// no named TG nodes, so the legacy lookup below returns origin. Resolve the
	// firepoint from the package node manifest + live clip globals instead.
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld((const void*)mechShape, tKey,
				mechType->nodeData[nodeId].nodeId,
				(const float*)mechShape->GetShapeRec(0)->shapeToWorld.entries, wp)) {
			static int s_diagN = 0;
			if (getenv("MC2_MECH_NODE_DIAG") && s_diagN++ < 24)
				fprintf(stderr, "[NODE-DIAG-CALLER] mech pos=(%.1f,%.1f,%.1f) node='%s' world=(%.1f,%.1f,%.1f) delta=(%.1f,%.1f,%.1f)\n",
				        position.x, position.y, position.z,
				        mechType->nodeData[nodeId].nodeId,
				        wp[0], wp[1], wp[2],
				        wp[0]-position.x, wp[1]-position.y, wp[2]-position.z);
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2];
			return result;
		}
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);

   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;

   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,mechType->nodeData[nodeId].nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D Mech3DAppearance::getNodeNamePosition (const char *nodeName)
{
	Stuff::Vector3D result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D Mech3DAppearance::getNodeIdPosition (long nodeId)
{
	Stuff::Vector3D result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D Mech3DAppearance::getNodePosition (long nodeId)
{
	Stuff::Vector3D result = position;
	if ((nodeId < 0) || (nodeId >= mechType->getTotalNodes()))
		return result;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,mechType->nodeData[nodeId].nodeId);

	return result;
}

//-----------------------------------------------------------------------------
long Mech3DAppearance::getLowestWeaponNode (void)
{
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	long bestNode = -1;
	float lowestPosZ;
	long numSmokeNodes = mechType->numSmokeNodes;
	lowestPosZ = 9999999999999.0f;
	for (long i=0;i<mechType->numWeaponNodes;i++)
	{
		Stuff::Vector3D nodePosition = getNodePosition(i+numSmokeNodes);
		if (nodePosition.z < lowestPosZ)
		{
			lowestPosZ = nodePosition.z;
			bestNode = i+numSmokeNodes;
		}
	}
		
   	if ((lowestPosZ == 0.0f) || (bestNode < 0) || (bestNode >= mechType->getTotalNodes()))
   		return -1;

 	return bestNode;
}

//-----------------------------------------------------------------------------
long Mech3DAppearance::getWeaponNode (long weaponType)
{
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	// BIG change here.  This is ONLY called at load time.
	// NEVER during actual game execution.  In this way,
	// Weapons always fire from the same nodes!!
	long leastUsed = 999999999;
	long bestNode = -1;
	long numSmokeNodes = mechType->numSmokeNodes;
	for (long i=0;i<mechType->numWeaponNodes;i++)
	{
		switch (weaponType)
		{
			case MECH3D_WEAPONTYPE_MISSILE:
				if ((mechType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_NONENERGY))
				{
					//This node is blown off.
					if (rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					//This node is blown off.
					if (leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
					if (nodeUsed[i] < leastUsed)
					{
						leastUsed = nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_BALLISTIC:
				if ((mechType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_DIRECT) ||
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_NONENERGY))
				{
					//This node is blown off.
					if (rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					//This node is blown off.
					if (leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
 					if (nodeUsed[i] < leastUsed)
					{
						leastUsed = nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ENERGY:
				if ((mechType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_DIRECT) ||
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY))
				{
					//This node is blown off.
					if (rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					//This node is blown off.
					if (leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
 					if (nodeUsed[i] < leastUsed)
					{
						leastUsed = nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ANY:
				//This node is blown off.
				if (rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
					continue;
					
				//This node is blown off.
				if (leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
					continue;
					
 				if (nodeUsed[i] < leastUsed)
				{
					leastUsed = nodeUsed[i];
					bestNode = i + numSmokeNodes;
				}
			break;

			default:
				STOP(("Sent down a bad weapon type %d",weaponType));
		}
	}
		
   	if ((bestNode < 0) || (bestNode >= mechType->getTotalNodes()))
   		return -1;

	//This should never be called AFTER the game inits!!!
	setWeaponNodeUsed(bestNode);

 	return bestNode;
}

//-----------------------------------------------------------------------------
// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): socket NAME -> weapon-node
// INDEX. Scans ONLY the weapon-node range of nodeData[] (the same range
// getWeaponNode() picks from) and returns the matching index, or -1. Read-only:
// unlike getWeaponNode() this does NOT call setWeaponNodeUsed() -- the caller is
// choosing a specific declared socket, not consuming a round-robin slot.
long Mech3DAppearance::resolveWeaponNodeBySocketName (const char* socketName)
{
	if (!socketName || !socketName[0])
		return -1;
	const long numSmokeNodes = mechType->numSmokeNodes;
	for (long i = 0; i < mechType->numWeaponNodes; i++)
	{
		const long idx = numSmokeNodes + i;
		const char* nid = mechType->nodeData[idx].nodeId;
		if (nid && S_stricmp(nid, socketName) == 0)
			return idx;
	}
	return -1;
}

//-----------------------------------------------------------------------------
float Mech3DAppearance::getWeaponNodeRecycle (long node)
{
	node -= mechType->numSmokeNodes;
	
	if ((node >=0) && (node < mechType->numWeaponNodes))
		return nodeRecycle[node];
		
	return 9999.0f;		//NOT a weapon node, never recycled!!
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	// M2: defensive reset. Guards against in-place re-init without a prior
	// delete/new (which would carry a stale handle into the new instance).
	// The adapter's syncSpawn assert fires on a valid handle, making
	// accidental double-registration visible in debug builds.
	mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
	mechType = (Mech3DAppearanceType *)tree;

	// C3: cache owner handle for GPU-cull node-position early-outs.
	actorHandle_ = (obj != nullptr) ? obj->getHandle() : -1;
	LC3D_TRACE("event=init actorHandle=%ld lifecycle=%d", actorHandle_, (int)s_gpuCullLifecycle);

	mechName[0] = 0;

	pilotNameID = -1;

	paintScheme = -1;
	objectNameId = 30862;

	baseRootNodeHeight = -99999.9f;
	baseRootNodeDifference = 0.0f;

	rotationalNodeIndex = -1;
	hitLeftNodeIndex = hitRightNodeIndex = -1;
	rootNodeIndex = lightCircleNodeIndex = -1;
	leftArmNodeIndex = rightArmNodeIndex = -1;

	// (E) T1.6: lazy-init key for SpotLight_ children. Vectors default-init
	// to empty. lightCircleNodeIndex above is for the legacy anubis
	// SLCircle_anubis path; SpotLight_ is a different node-name prefix.
	spotlightsRegistered_ = false;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	
	frameNum = 0.0f;
	
	rightShoulderPos.x = rightShoulderPos.y = rightShoulderPos.z = -99999.9f;
	leftShoulderPos.x = leftShoulderPos.y = leftShoulderPos.z = -99999.9f;

	idleTime = RandomNumber(idleMAX);
	
	hazeFactor = 0.0f;

	rotation = torsoRotation = leftArmRotation = rightArmRotation = 0;

	leftArmOff = rightArmOff = fallen = false;

	position.x = position.y = position.z = 0.0f;

	velocity = 0.0f;

	teamId = paintScheme = -1;
	homeTeamRelationship = 0;

	selected = 0;

	status = 0;

	lockRotation = oncePerFrame = false;

	sensorLevel = 0;
	sensorSpin = 0.0f;

	OBBRadius = 0.0f;
	currentLOD = 0;
	
	jumpVelocity.Zero();
	jumpFXSetup = false;
	
	nodeUsed = NULL;
	nodeRecycle = NULL;

	currentRightPoof = currentLeftPoof = 0;
	
	localTextureHandle = 0xffffffff;
	
	isSmoking = -1;
	isWaking = false;
	isDusting = false;
	movedThisFrame = false;
	fallDust = false;
	criticalSmoke = NULL;
	smokeEffect = NULL;
	waterWake = NULL;
	helicopterDustCloud = NULL;
	isHelicopter = false;
	
	leftArm = rightArm = NULL;
	rightArmSmoke = leftArmSmoke = NULL;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;


	//Default to Bright RGB.
	psRed = psGreen = psBlue = 0xffffffff;

	// DEGRADE-DON'T-CRASH: ensure the instance shape is NULL before the build
	// block so a partially-imported mech (missing appearance .ini / missing
	// TGLData / missing base-LOD .ase) leaves a well-defined NULL, not garbage.
	// Mech3DAppearanceType::init() falls through its STOP()/gosASSERT() (both
	// no-ops in RelWithDebInfo) when those assets are absent, leaving
	// mechType->mechShape[0] == NULL. Without this guard mechShape =
	// mechType->mechShape[0]->CreateFrom() below derefs NULL and crashes the
	// encyclopedia / mech-bay preview. The per-frame render()/update()/
	// recalcBounds() paths early-out on the resulting NULL mechShape.
	mechShape = NULL;

	if (mechType && mechType->mechShape[0])
	{
		// Re-register all LODs with the GPU mech batcher. GpuMechBatcher::onMapLoad()
		// clears s_typeLodIndex between sessions, but AppearanceTypeList caches the
		// Mech3DAppearanceType so Mech3DAppearanceType::init() (which calls
		// registerTypeLod) is never called again for cached types. Without this,
		// submitActor() finds the type absent from s_typeLodIndex and returns false,
		// causing invisible mechs on any map after the first session. registerTypeLod
		// is idempotent (no-op if already registered in this session).
		for (int _lod = 0; _lod < MAX_LODS; ++_lod) {
			if (mechType->mechShape[_lod])
				GpuMechBatcher::instance().registerTypeLod(mechType, _lod);
		}

		mechShape = mechType->mechShape[0]->CreateFrom();
		
		sensorTriangleShape = GVAppearanceType::SensorTriangleShape->CreateFrom();
		
		if (mechType->leftArm)
			leftArm = mechType->leftArm->CreateFrom();
		
		if (mechType->rightArm)
			rightArm = mechType->rightArm->CreateFrom();
		
		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<sensorTriangleShape->GetNumTextures();i++)
		{
			char txmName[1024];
			sensorTriangleShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				sensorTriangleShape->SetTextureHandle(i,0xffffffff);
			}
		}
 
		if (mechType->numJumpNodes)
		{
			sensorSquareShape = Mech3DAppearanceType::SensorSquareShape->CreateFrom();
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (int i=0;i<sensorSquareShape->GetNumTextures();i++)
			{
				char txmName[1024];
				sensorSquareShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					sensorSquareShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		else
		{
			sensorSquareShape = GVAppearanceType::SensorCircleShape->CreateFrom();
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (int i=0;i<sensorSquareShape->GetNumTextures();i++)
			{
				char txmName[1024];
				sensorSquareShape->GetTextureName(i,txmName,256);

				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");

				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					sensorSquareShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		
 		if (mechType->numWeaponNodes)
		{
			nodeUsed = (long *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(long) * mechType->numWeaponNodes);
			gosASSERT(nodeUsed != NULL);
			memset(nodeUsed,0,sizeof(long) * mechType->numWeaponNodes);
			
			nodeRecycle = (float *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(float) * mechType->numWeaponNodes);
			gosASSERT(nodeRecycle != NULL);
			
			for (long i=0;i<mechType->numWeaponNodes;i++)
				nodeRecycle[i] = 0.0f;
		}
		
		for (int i=0;i<mechType->numWeaponNodes;i++)
		{
			if (mechShape->isChildOf(mechType->nodeData->nodeId,"joint_ruarm"))
				mechType->nodeData[i+mechType->numSmokeNodes].isRArmNode = true;
			else
				mechType->nodeData[i+mechType->numSmokeNodes].isRArmNode = false;
			
			if (mechShape->isChildOf(mechType->nodeData->nodeId,"joint_luarm"))
				mechType->nodeData[i+mechType->numSmokeNodes].isLArmNode = true;
			else
				mechType->nodeData[i+mechType->numSmokeNodes].isLArmNode = false;
		}

		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D nodeCenter = mechShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = boxCoords[i].GetLength();
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;
		}
		
		mechType->boundsUpperLeftX = (-OBBRadius);
		mechType->boundsUpperLeftY = (-OBBRadius * 2.0);
		  		     
		mechType->boundsLowerRightX = (OBBRadius);
		mechType->boundsLowerRightY = (OBBRadius);
		
		if (!mechType->getDesignerTypeBounds())
		{
			mechType->typeUpperLeft.x = mechShape->GetMaxBox().x + nodeCenter.x;
			mechType->typeUpperLeft.y = mechShape->GetMaxBox().z + nodeCenter.z;
			mechType->typeUpperLeft.z = mechShape->GetMaxBox().y + nodeCenter.y;

			mechType->typeLowerRight.x = mechShape->GetMinBox().x + nodeCenter.x;
			mechType->typeLowerRight.y = mechShape->GetMinBox().z + nodeCenter.z;
			mechType->typeLowerRight.z = mechShape->GetMinBox().y + nodeCenter.y;

			//Now expand box by some percentage to make selection easier.
			mechType->typeUpperLeft.x *= EXPAND_FACTOR;
			mechType->typeUpperLeft.y *= EXPAND_FACTOR;
			mechType->typeUpperLeft.z *= EXPAND_FACTOR;

			mechType->typeLowerRight.x *= EXPAND_FACTOR;
			mechType->typeLowerRight.y *= EXPAND_FACTOR;
			mechType->typeLowerRight.z *= EXPAND_FACTOR;
		}
	}
	else
	{
		// DEGRADE-DON'T-CRASH: partially-imported mech — appearance type exists
		// but has no usable base-LOD shape (missing .ini / TGLData / .ase). Skip
		// shape build; mechShape stays NULL and the render path renders nothing.
		// One-time warn so the missing asset is visible in logs (not silent).
		static bool s_warnedMissingMechShape = false;
		if (!s_warnedMissingMechShape)
		{
			s_warnedMissingMechShape = true;
			std::printf("[MECH_LOAD] WARN: mech appearance has no base shape "
				"(missing/partial import) -- rendering nothing. mechType=%p\n",
				(void*)mechType);
			std::fflush(stdout);
		}
	}

	mechFrameRate = 30.0f;
		
 	forceStop = atTransitionToNextGesture = inReverse =	inJump = false;

	inDebugMoveMode = singleStepMode = nextStep = prevStep = false;

	currentStateGoal = -1;		//Always start ready to change gestures
	currentGestureId = 2;		//Always start in Stand Mode
    inCombatMode = false;       // sebi: init, so will not contain garbage

	transitionState = 0;
	oldStateGoal = 1;			//Always start in Stand Mode

	currentFrame = 0.0f;
	lastAnimAdvanceFrame = 0xFFFFFFFFu;	//ANIM-CADENCE-FIX: no advance yet

	pointLight = NULL;
	lightId = 0xffffffff;
	
	hitGestureId = -1;
	
	jumpSetup = false;
	jumpAirborne = inJump = false;
	
	footPos[0].x = footPos[0].y = footPos[0].z = 1000.0f;
	footPos[1].x = footPos[1].y = footPos[1].z = 1000.0f;
	
	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		rightDustPoofEffect[i] = NULL;
		leftDustPoofEffect[i] = NULL;
	}
	
	smokeEffect = NULL;
	jumpJetEffect = NULL;
	rightShoulderBoom = leftShoulderBoom = NULL;
	
	leftFootDone0 = rightFootDone0 = leftFootDone1 = rightFootDone1 = false;
	
	leftFootPoofDraw[0] = leftFootPoofDraw[1] = leftFootPoofDraw[2] = 
	rightFootPoofDraw[0] = rightFootPoofDraw[1] = rightFootPoofDraw[2] = false;
	
	if (mechType && mechType->mechShadowShape)
	{
		mechShadowShape = mechType->mechShadowShape->CreateFrom();

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<mechShadowShape->GetNumTextures();i++)
		{
			char txmName[1024];
			mechShadowShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");

			if (fileExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					mechShadowShape->SetTextureHandle(i,gosTextureHandle);
					mechShadowShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					mechShadowShape->SetTextureHandle(i,gosTextureHandle);
					mechShadowShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				mechShadowShape->SetTextureHandle(i,0xffffffff);
			}
		}
	}
	else
	{
		mechShadowShape = NULL;
	}
}

void Mech3DAppearance::initFX (void)
{
	//-----------------------------------------------
	//Create FX here so they are always ready to go!
	if (!InEditor)
	{
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_POOF_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_POOF_ID));
			
			if (gosEffectSpec)
			{
				for (long i=0;i<MAX_DUST_POOFS;i++)
				{
					rightDustPoofEffect[i] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(rightDustPoofEffect[i] != NULL);
				}
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_POOF_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_POOF_ID));
			
			if (gosEffectSpec)
			{
				for (long i=0;i<MAX_DUST_POOFS;i++)
				{
					leftDustPoofEffect[i] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(leftDustPoofEffect[i] != NULL);
				}
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_SMOKE_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				smokeEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(smokeEffect != NULL);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}

   		if (strcmp(weaponEffects->GetEffectName(FX_JUMP_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_JUMP_ID));
			
			if (gosEffectSpec)
			{
				jumpJetEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(jumpJetEffect != NULL);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
}

//------------------------------------------------------------------------------------------
//Puts mech into hit mode IF and ONLY IF the mech is standing, walking, reversing, running or limping.
// NO OTHER GESTURE IS VALID!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
void Mech3DAppearance::hitFront (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitFront;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
		
		inReverse = false;
							  
		currentFrame = firstFrame;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::hitBack (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitBack;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;

		inReverse = false;
			
		currentFrame = firstFrame;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::hitLeft (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitLeft;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
			
		inReverse = false;
			
		currentFrame = firstFrame;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::hitRight (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitRight;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
		
		inReverse = false;
		
			
		currentFrame = firstFrame;
	}
}

//-----------------------------------------------------------------------------
bool Mech3DAppearance::PerPolySelect (long mouseX, long mouseY)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> not selectable.
	if (!mechShape)
		return false;
	return mechShape->PerPolySelect(mouseX,mouseY);
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::LoadPaintSchemata (void)
{
	FullPathFileName paintName;
	paintName.init(tglPath,"paintSchemata",".fit");

	FitIniFile paintFile;
	long result = paintFile.open(paintName);
	gosASSERT(result == NO_ERR);

	result = paintFile.seekBlock("Main");
	gosASSERT(result == NO_ERR);

	result = paintFile.readIdULong("NumPaintSchemes",numPaintSchemata);
	gosASSERT(result == NO_ERR);

	paintSchemata = (PaintSchemataPtr)AppearanceTypeList::appearanceHeap->Malloc(numPaintSchemata * sizeof(PaintSchemata));
	gosASSERT(paintSchemata != NULL);

	memset(paintSchemata,0xff,numPaintSchemata * sizeof(PaintSchemata));

	for (long i=0;i<numPaintSchemata;i++)
	{
		char blockId[512];
		sprintf(blockId,"Scheme%d",i);
		result = paintFile.seekBlock(blockId);
		gosASSERT(result == NO_ERR);

		result = paintFile.readIdULong("RedColor",paintSchemata[i].redColor);
		gosASSERT(result == NO_ERR);

		result = paintFile.readIdULong("GreenColor",paintSchemata[i].greenColor);
		gosASSERT(result == NO_ERR);

		result = paintFile.readIdULong("BlueColor",paintSchemata[i].blueColor);
		gosASSERT(result == NO_ERR);
	}

	paintFile.close();
}	

//-----------------------------------------------------------------------------
void Mech3DAppearance::setPaintScheme (void)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> nothing to paint.
	if (!mechShape)
		return;

	//----------------------------------------------------------------------------
	// Simple really.  Get the texture memory, apply the paint scheme, let it go!
	DWORD gosHandle = mcTextureManager->get_gosTextureHandle(mechShape->GetTextureHandle(0));

	if (gosHandle && gosHandle != 0xffffffff)
	{
		//-------------------
		// Lock the texture.
		TEXTUREPTR textureData;
		gos_LockTexture(gosHandle, 0, 0, &textureData);

		//-------------------------------------------------------
		// Dominant-channel paint classifier. A pixel belongs to a paint slot
		// iff its R/G/B dominates the other two by at least kRatio. Boundary
		// (antialiased) pixels blend tint vs original proportional to how
		// cleanly they dominate -- the original code required strict purity,
		// which missed every antialiased mask edge and every upscaled
		// gradient pixel.
		DWORD *textureMemory = textureData.pTexture;
		for (long i=0;i<textureData.Height;i++)
		{
			for (long j=0;j<textureData.Width;j++)
			{
				DWORD srcColor = *textureMemory;
				DWORD srcA = (srcColor & 0xff000000);
				int   srcR = (int)((srcColor >> 16) & 0xff);
				int   srcG = (int)((srcColor >>  8) & 0xff);
				int   srcB = (int)( srcColor        & 0xff);

				// `shade`     = tint brightness from dominant-channel magnitude
				//                (preserves the original log-shade curve).
				// `mix`       = confidence this pixel is a mask pixel
				//                (0=not a mask, 1=comfortably past threshold).
				// Separating these two means a slightly-impure interior pixel
				// (e.g. (240,20,10) from an upscaled texture) gets mix~=1
				// and still receives full tint, while a boundary pixel whose
				// dominance barely clears threshold gets a soft blend.
				int   slot       = -1;
				DWORD slotColor  = 0;
				float shade      = 0.0f;
				float mix        = 0.0f;
				const int   kMinDom      = 16;
				const float kRatio       = 3.0f;
				const float kRatioMargin = 1.5f; // mix saturates at ratio = kRatio * kRatioMargin

				int domChan = 0, maxOther = 0;
				if (srcR >= kMinDom && (float)srcR >= kRatio * (float)srcG && (float)srcR >= kRatio * (float)srcB)
				{
					slot = 0; slotColor = psRed;
					domChan = srcR;
					maxOther = srcG > srcB ? srcG : srcB;
				}
				else if (srcG >= kMinDom && (float)srcG >= kRatio * (float)srcR && (float)srcG >= kRatio * (float)srcB)
				{
					slot = 1; slotColor = psGreen;
					domChan = srcG;
					maxOther = srcR > srcB ? srcR : srcB;
				}
				else if (srcB >= kMinDom && (float)srcB >= kRatio * (float)srcR && (float)srcB >= kRatio * (float)srcG)
				{
					slot = 2; slotColor = psBlue;
					domChan = srcB;
					maxOther = srcR > srcG ? srcR : srcG;
				}

				if (slot != -1)
				{
					// Shade from dominant-channel magnitude (original curve).
					float d = (float)domChan / 255.0f;
					shade = 1.0f - (1.0f - d) * (1.0f - d);

					// Confidence saturates once the dominance ratio is
					// comfortably past threshold, so upscaled-texture
					// interior pixels (ratio ~10-15) get full tint while
					// near-threshold boundary pixels get a soft blend.
					float ratio = (float)domChan / (float)(maxOther + 1);
					float margin = (ratio - kRatio) / (kRatio * (kRatioMargin - 1.0f));
					if (margin > 1.0f) margin = 1.0f;
					if (margin < 0.0f) margin = 0.0f;
					mix = margin;
				}

				DWORD newColor = srcColor;
				if (slot != -1 && mix > 0.0f)
				{
					float tintR = (float)((slotColor >> 16) & 0xff) * shade;
					float tintG = (float)((slotColor >>  8) & 0xff) * shade;
					float tintB = (float)( slotColor        & 0xff) * shade;

					float outR = tintR * mix + (float)srcR * (1.0f - mix);
					float outG = tintG * mix + (float)srcG * (1.0f - mix);
					float outB = tintB * mix + (float)srcB * (1.0f - mix);

					unsigned char r = (unsigned char)(outR < 0.0f ? 0.0f : outR > 255.0f ? 255.0f : outR);
					unsigned char g = (unsigned char)(outG < 0.0f ? 0.0f : outG > 255.0f ? 255.0f : outG);
					unsigned char b = (unsigned char)(outB < 0.0f ? 0.0f : outB > 255.0f ? 255.0f : outB);

					newColor = srcA | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
				}

				*textureMemory = newColor;
				textureMemory++;
			}
		}

		//------------------------
		// Unlock the texture
		gos_UnLockTexture(gosHandle);
	}
}	

//---------------------------------------------------------------------------
DWORD bgrTorgb (DWORD frontRGB)
{
	DWORD tmp;
	tmp = (((0x00ff0000) & frontRGB)>>16) + ((0x0000ff00) & frontRGB) + (((0x000000ff) & frontRGB)<<16);
	
	return(tmp);
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::setPaintScheme (DWORD mcRed, DWORD mcGreen, DWORD mcBlue)
{
#if defined(BGR)
	// These come into here bgr instead of RGB.  CONVERT!
	psRed = bgrTorgb(mcRed);
	psBlue = bgrTorgb(mcBlue);
	psGreen = bgrTorgb(mcGreen);
#else /*BGR*/
	psRed = mcRed;
	psBlue = mcBlue;
	psGreen = mcGreen;
#endif /*BGR*/

	setPaintScheme();	
}	

//-----------------------------------------------------------------------------
void Mech3DAppearance::getPaintScheme( DWORD& red, DWORD& green, DWORD& blue )
{
#if defined(BGR)
	red = bgrTorgb(psRed);
	blue = bgrTorgb(psBlue);
	green = bgrTorgb(psGreen);
#else /*BGR*/
	red = psRed;
	blue = psBlue;
	green = psGreen;
#endif /*BGR*/
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::resetPaintScheme (DWORD red, DWORD green, DWORD blue)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> no texture to
	// repaint. Called directly from the encyclopedia / mech-bay preview setup
	// (SimpleCamera::setMech) before any frame; mechShape->GetTextureName below
	// would deref NULL.
	if (!mechShape)
		return;

	// AO-1: load the imported-mech AO map once (independent of paint scheme; AO is
	// linear grayscale, not recolored). Loaded here (before resetPaintScheme's early
	// returns) so it runs on every path. HAS_AO is asserted at submit ONLY if aoValid,
	// so a missing/failed AO never darkens the mech (white/no-op fallback).
	if (!aoValid && mechShape->GetNumShapes() > 0) {
		const void* aoTypeKey = mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr;
		const char* aoName = mc2mechanim::ImportedMechAoTexName(aoTypeKey);
		if (aoName && aoName[0]) {
			char aoPath[1024];
			sprintf(aoPath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
			FullPathFileName aoFull;
			aoFull.init(aoPath, (char*)aoName, "");
			if (textureOrKtxSidecarExists(aoFull)) {
				aoTextureHandle = mcTextureManager->loadTexture(aoFull, gos_Texture_Solid,
					gosHint_DontShrink);
				aoValid = (aoTextureHandle != 0 && aoTextureHandle != 0xFFFFFFFFu);
			}
			if (!aoValid)
				fprintf(stderr, "[MECH_IMPORT] AO '%s' not loaded -> no-op (white)\n", aoName);
		}
	}

	// NORMALS-1: load the imported-mech normal map once (same contract as AO: linear
	// BC7, base-chassis, skin-independent). HAS_NORMAL is asserted at submit ONLY if
	// normalValid, so a missing/failed normal never corrupts shading (v_normal fallback).
	if (!normalValid && mechShape->GetNumShapes() > 0) {
		const void* nrmTypeKey = mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr;
		const char* nrmName = mc2mechanim::ImportedMechNormalTexName(nrmTypeKey);
		if (nrmName && nrmName[0]) {
			char nrmPath[1024];
			sprintf(nrmPath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
			FullPathFileName nrmFull;
			nrmFull.init(nrmPath, (char*)nrmName, "");
			if (textureOrKtxSidecarExists(nrmFull)) {
				normalTextureHandle = mcTextureManager->loadTexture(nrmFull, gos_Texture_Solid,
					gosHint_DontShrink);
				normalValid = (normalTextureHandle != 0 && normalTextureHandle != 0xFFFFFFFFu);
			}
			if (!normalValid)
				fprintf(stderr, "[MECH_IMPORT] normal '%s' not loaded -> no-op (v_normal)\n", nrmName);
		}
	}

	//---------------------------------------------------------------------------------
	// Simple really.  Toss the current texture, reload the RGB and reapply the colors

	DWORD gosHandle = mcTextureManager->get_gosTextureHandle(localTextureHandle);
	mcTextureManager->removeTexture(gosHandle);
	
	//-------------------------------------------------
	// Load the texture and store its handle.
	char txmName[1024];
	mechShape->GetTextureName(0,txmName,256);

   	char texturePath[1024];
   	sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

   	FullPathFileName textureName;
   	textureName.init(texturePath,txmName,"");

	//DWORD paintInstance = (red << 16) + (green << 8) + (blue);
	/* The texture manager asks for a unique 32bit identifier for every texture instance.
	However, it requires 72 bits to fully describe a mech texture (the base color (stored in
	the variable "red"), highlight color1 (blue), and highlight color2 (green), each of which
	is a 24bit number made up of 8bit r, g, and b components). Instead of creating a
	mapping between the mech textures used (probably less than 2^32 of them) and
	32bit identifiers, for the sake of expediency I'm just taking the 3 most significant bits of
	the 9 rgb components to make a 27 bit identifier. This means that two mech textures
	that are close in color (i.e. all of the 3 most significant bits of the 9 rgb components are
	the same) will be treated as the same texture, which is not necessarily a bad thing in
	our case.  LOD is never needed because if the texture is different, its NAME will be different!!*/
	DWORD ccbase = ((red >> 5) & 7) + (((red >> 13) & 7) << 3) + (((red >> 21) & 7) << 6);
	DWORD cchighlight1 = ((green >> 5) & 7) + (((green >> 13) & 7) << 3) + (((green >> 21) & 7) << 6);
	DWORD cchighlight2 = ((blue >> 5) & 7) + (((blue >> 13) & 7) << 3) + (((blue >> 21) & 7) << 6);
	DWORD paintInstance = (ccbase << 18) + (cchighlight1 << 9) + (cchighlight2);

	// KTX2-INFRA-1: accept a BC7 .ktx2 sidecar when the .tga is absent (homogeneous
	// imported-mech textures ship as .ktx2 only; txmmgr KTX_PRIMARY decodes it). Was
	// fileExists(textureName) — that skipped -> black mech for .ktx2-only deploys.
	if (textureOrKtxSidecarExists(textureName))
	{
		// gosHint_MipmapFilter0: opt this texture into GL mipmap+trilinear
		// via gosTexture::createHardwareTexture. Full mip chain is
		// regenerated after the paint-scheme edit in setPaintScheme() so
		// minified mechs at zoom-out no longer alias through the unpainted
		// mask-boundary pixels that fix A already resolved at mip 0.
		const DWORD mechHints = gosHint_MipmapFilter0 | gosHint_DontShrink;
		DWORD textureInstanceAlreadyExists = mcTextureManager->textureInstanceExists(textureName,gos_Texture_Solid,mechHints,paintInstance);
		if (!textureInstanceAlreadyExists)
			localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,mechHints,paintInstance);
		else
			localTextureHandle = textureInstanceAlreadyExists;
			
		mechShape->SetTextureHandle(0,localTextureHandle);
		mechShape->SetTextureAlpha(0,false);
		
		if (leftArm)
		{
			leftArm->SetTextureHandle(0,localTextureHandle);
			leftArm->SetTextureAlpha(0,false);
		}
		
		if (rightArm)
		{
			rightArm->SetTextureHandle(0,localTextureHandle);
			rightArm->SetTextureAlpha(0,false);
		}
		
  		if (textureInstanceAlreadyExists)
		{
			/* In this case, the texture returned should already have the paint scheme
			applied. */

			//Still need to store psRed/psGreen/psBlue!!!!
			psRed = red;
			psGreen = green;
			psBlue = blue;
			return;
		}
	}
	else
	{
		//PAUSE(("Warning: %s texture name not found",textureName));
		mechShape->SetTextureHandle(0,0xffffffff);
		if (leftArm)
			leftArm->SetTextureHandle(0,0xffffffff);
			
		if (rightArm)
			rightArm->SetTextureHandle(0,0xffffffff);
	}
	
	setPaintScheme(red,green,blue);
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::setObjectParameters (const Stuff::Vector3D &pos, float Rot, long sel, long team, long homeRelations)
{
	movedThisFrame = false;
	if ((rotation != Rot) || (pos != position))
		movedThisFrame = true;
 
	if ((currentGestureId == 23) || (currentGestureId == 24) || (currentGestureId == 21) || (currentGestureId == 22) ||
		(currentGestureId == 14) || (currentGestureId == 15))
	{
	}
	else
	{
		rotation = Rot;
		if (rotation > 180)
			rotation -= 360;

		if (rotation < -180)
			rotation += 360;
	}

	position = pos;

	selected = sel;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	isHelicopter = isAirborne;

	if ((currentGestureId == 23) || (currentGestureId == 24) || (currentGestureId == 21) || (currentGestureId == 22) ||
		(currentGestureId == 14) || (currentGestureId == 15))
	{
	}
	else
	{
		torsoRotation = turretRot;
		leftArmRotation = lArmRot;
		rightArmRotation = rArmRot;

		if (torsoRotation > 180)
			torsoRotation -= 360;

		if (torsoRotation < -180)
			torsoRotation += 360;
	}
}	

//-----------------------------------------------------------------------------
void Mech3DAppearance::updateFootprints (void)
{

}

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

//-----------------------------------------------------------------------------
bool Mech3DAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool Mech3DAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);

	// DEGRADE-DON'T-CRASH: no base shape (partial import) -> treat as not in
	// view; the mechShape->GetNodeNameId() / LOD-swap derefs below are unsafe.
	if (!mechShape)
	{
		setVisibilityGatesFromLegacy(false);
		return inView;
	}

	Stuff::Vector4D tempPos;
	bool wasInView = inView;
	setVisibilityGatesFromLegacy(false);
	float eyeDistance = 0.0f;

	if (eye)
	{
		//ALWAYS need to do this or select is YAYA
		// [PROJECTZ:ScreenXYOracle id=mech3d_screen_pos]
		eye->projectForScreenXY(position,screenPos);
		
 		//--------------------------------------------------
		// First, if we are using perspective, figure out
		// if object too far from camera.  Far Clip Plane.
		if (eye->usePerspective)
		{
			Stuff::Point3D Distance;
			Stuff::Point3D objPosition;
			Stuff::Point3D eyePosition(eye->getCameraOrigin());
			objPosition.x = -position.x;
			objPosition.y = position.z;
			objPosition.z = position.y;
	
			Distance.Subtract(objPosition,eyePosition);
			eyeDistance = Distance.GetApproximateLength();
			if (eyeDistance > Camera::MaxClipDistance)
			{
				hazeFactor = 1.0f;
				setVisibilityGatesFromLegacy(false);
			}
			else if (eyeDistance > Camera::MinHazeDistance)
			{
				Camera::HazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
				setVisibilityGatesFromLegacy(true);
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				setVisibilityGatesFromLegacy(true);
			}
			
			//-----------------------------------------------------------------
			// If inside farClip plane, check if behind camera.
			// Find angle between lookVector of Camera and vector from camPos
			// to Target.  If angle is less then halfFOV, object is visible.
			if (inView)
			{
				Stuff::Vector3D Distance;
				Stuff::Point3D objPosition;
				Stuff::Point3D eyePosition(eye->getCameraOrigin());
				objPosition.x = -position.x;
				objPosition.y = position.z;
				objPosition.z = position.y;
		
				Distance.Subtract(objPosition,eyePosition);
				Distance.Normalize(Distance);
				
				float cosine = Distance * eye->getLookVector();
 				if (cosine > eye->cosHalfFOV)
					setVisibilityGatesFromLegacy(true);
				else
					setVisibilityGatesFromLegacy(false);
			}
		}
		else
		{
			Camera::HazeFactor = 0.0f;
			setVisibilityGatesFromLegacy(true);
		}
		
		if (inView)
		{
			if (reloadBounds)
				mechType->reinit();

			//mechType->boundsLowerRightY = (OBBRadius * eye->getTiltFactor() * 2.0f);
			
			//-------------------------------------------------------------------------
			// do a rough check if on screen.  If no where near, do NOT do the below.
			// Mighty mighty slow!!!!
			// Use the original check done before all this 3D madness.  Dig out sourceSafe tomorrow!
			tempPos = screenPos;
			upperLeft.x = tempPos.x;
			upperLeft.y = tempPos.y;
			
			lowerRight.x = tempPos.x;
			lowerRight.y = tempPos.y;
			
			upperLeft.x += (mechType->boundsUpperLeftX * eye->getScaleFactor());
			upperLeft.y += (mechType->boundsUpperLeftY * eye->getScaleFactor());
	
			lowerRight.x += (mechType->boundsLowerRightX * eye->getScaleFactor());
			lowerRight.y += (mechType->boundsLowerRightY * eye->getScaleFactor());

			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				//We are on screen.  Figure out selection box.
				Stuff::Vector3D boxCoords[8];
				Stuff::Vector4D bcsp[8];
	
				if (rootNodeIndex == -1)
					rootNodeIndex = mechShape->GetNodeNameId("joint_root");

				//Gotta let the NodeIdPosition know that its matrix is valid or invalid so this actually does clip mech to screen!!
				// Leave the old one in place until we are inView again!
				// Should fix flickering on screen edge?  It does.  Must let jump flicker or he never comes down.
				setVisibilityGatesFromLegacy(wasInView);
				if (inView || (currentGestureId == 20))
					baseRootNodeDifference = (getNodeIdPosition(rootNodeIndex).z - position.z) - baseRootNodeHeight;

				if (inView && isHelicopter)
					baseRootNodeDifference -= HELICOPTER_FACTOR;

				setVisibilityGatesFromLegacy(true);

				if (InEditor)
				{
					if (isHelicopter)
						baseRootNodeDifference = 0.0f - HELICOPTER_FACTOR;
					else
						baseRootNodeDifference = 0.0f;
				}

				boxCoords[0].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[0].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[0].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[1].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[1].y = position.y + mechType->typeLowerRight.y;
				boxCoords[1].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[2].x = position.x + mechType->typeLowerRight.x;
				boxCoords[2].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[2].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[3].x = position.x + mechType->typeLowerRight.x;
				boxCoords[3].y = position.y + mechType->typeLowerRight.y;
				boxCoords[3].z = position.z + mechType->typeUpperLeft.z + baseRootNodeDifference;
	
				boxCoords[4].x = position.x + mechType->typeLowerRight.x;
				boxCoords[4].y = position.y + mechType->typeLowerRight.y;
				boxCoords[4].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[5].x = position.x + mechType->typeLowerRight.x;
				boxCoords[5].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[5].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[6].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[6].y = position.y + mechType->typeLowerRight.y;
				boxCoords[6].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				boxCoords[7].x = position.x + mechType->typeUpperLeft.x;
				boxCoords[7].y = position.y + mechType->typeUpperLeft.y;
				boxCoords[7].z = position.z + mechType->typeLowerRight.z + baseRootNodeDifference;
	
				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=mech3d_box_rect]
					eye->projectForScreenXY(boxCoords[i],bcsp[i]);
					if (!i)
					{
						maxX = minX = bcsp[i].x;
						maxY = minY = bcsp[i].y;
					}
					
					if (i)
					{
						if (bcsp[i].x > maxX)
							maxX = bcsp[i].x;
						
						if (bcsp[i].x < minX)
							minX = bcsp[i].x;
							
						if (bcsp[i].y > maxY)
							maxY = bcsp[i].y;
						
						if (bcsp[i].y < minY)
							minY = bcsp[i].y;
					}
				}
		
				upperLeft.x = minX;
				upperLeft.y = minY;
				lowerRight.x = maxX;
				lowerRight.y = maxY;
				
				// C3: when GPU cull is active, a GPU-visible actor must run LOD-swap
				// even if the CPU screen-rect test disagrees (N-1 lag can diverge).
				const bool cpuScreenRect = (lowerRight.x >= 0) && (lowerRight.y >= 0) &&
					(upperLeft.x <= eye->getScreenResX()) &&
					(upperLeft.y <= eye->getScreenResY());
				// Only consult GPU visibility when readback is actually enabled.
				// LIFECYCLE=1 without READBACK=1 makes readback_isActorVisibleLagged fail-open
				// (returns true for every actor), which would unconditionally widen the LOD-swap
				// block and cause a perf regression instead of a win.
				const bool rbVisible = (s_gpuCullLifecycle && gpu_cull::readback_isEnabled())
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
					: false;
				if (cpuScreenRect || rbVisible)
				{
					setVisibilityGatesFromLegacy(cpuScreenRect || rbVisible);
					
					if (status != OBJECT_STATUS_DESTROYED)
					{
						bool baseLOD = true;
						DWORD selectLOD = 0;
						if (useHighObjectDetail)
						{
							//-------------------------------------------------------------------------------
							//Set LOD of Model here because we have the distance and we KNOW we can see it!
							for (long i=1;i<MAX_LODS;i++)
							{
								if (mechType->mechShape[i] && mechType->mechShape[i]->GetNumShapes() && (eyeDistance > mechType->lodDistance[i]))
								{
									baseLOD = false;
									selectLOD = i;
								}
							}
						}
						else
						{
							if (mechType->mechShape[1] && mechType->mechShape[1]->GetNumShapes())
							{
								baseLOD = false;
								selectLOD = 1;
							}
						}

						// we are at this LOD level.
						if (selectLOD != currentLOD)
						{
							if (s_mechLodTrace) {
								std::fprintf(stderr,
									"[MECHLOD v1] event=lod_swap actor=%p old_lod=%lu new_lod=%lu\n",
									(void*)this, (unsigned long)currentLOD, (unsigned long)selectLOD);
							}
							currentLOD = selectLOD;

							BYTE alphaValue = mechShape->GetAlphaValue();
							//mechShape->ClearAnimation();	//DO NOT do this with animating things!!
							mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
							delete mechShape;
							mechShape = NULL;

							mechShape = mechType->mechShape[currentLOD]->CreateFrom();
							mechShape->SetAlphaValue(alphaValue);

							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);

							if (rightArmOff)
								mechShape->StopUsing("joint_ruarm");

							if (leftArmOff)
								mechShape->StopUsing("joint_luarm");
						}

						if (currentLOD && baseLOD)
						{
						// we are at the Base LOD level.
							currentLOD = 0;

							BYTE alphaValue = mechShape->GetAlphaValue();
							//treeShape->ClearAnimation();
							mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
							delete mechShape;
							mechShape = NULL;

							mechShape = mechType->mechShape[currentLOD]->CreateFrom();
							mechShape->SetAlphaValue(alphaValue);

							//Automatically reloads texture and stores it.
							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);

							if (rightArmOff)
								mechShape->StopUsing("joint_ruarm");

							if (leftArmOff)
								mechShape->StopUsing("joint_luarm");
						}

						mechType->setAnimation(mechShape,currentGestureId);
					}
				}
				else
				{
					setVisibilityGatesFromLegacy(false);		//Did alot of extra work checking this, but WHY draw and insult to injury?
				}
			}
			else
			{
				setVisibilityGatesFromLegacy(false);
			}
		}
	}

	return(inView);
}

//-----------------------------------------------------------------------------
long Mech3DAppearance::render (long depthFixup)
{
	// FX_COUNT one-time init: register the shutdown emit from a code path that
	// is guaranteed to run (a discardable anon-namespace static can be elided
	// by /OPT:REF). Unconditional init print proves render() runs + flag state.
	{
		static bool s_fxInitDone = false;
		if (!s_fxInitDone) {
			s_fxInitDone = true;
			std::printf("[FX_COUNT v1] event=init flag=%d\n", s_fxCountLog ? 1 : 0);
			std::fflush(stdout);
			if (s_fxCountLog) std::atexit(&fxCountEmit);
		}
	}

	// DEGRADE-DON'T-CRASH: a partially-imported mech (no base shape) leaves
	// mechShape NULL (see init()). Render nothing rather than deref NULL.
	if (!mechShape)
	{
		if (g_mechPreviewRenderDepth > 0 && getenv("MC2_LOG_PREVIEW"))
		{
			FILE* f = fopen("preview_debug.log","a");
			if (f) { fprintf(f,"[PREVIEW] Mech3D::render EARLY-OUT mechShape=NULL (partial-import guard)\n"); fflush(f); fclose(f); }
		}
		// DIAG (GPU-MECH-EQUIV recon): capture the null-shape invisible cohort
		// (Cohort B — broken appearance type with mechShape==NULL early-outs here,
		// before both GPU submit and CPU fallback). Gated by MC2_MECH_RESTORE_TRACE.
		{
			static const bool s_mrtNull = (getenv("MC2_MECH_RESTORE_TRACE") != nullptr);
			static int s_nullEmits = 0;
			if (s_mrtNull && g_mechPreviewRenderDepth == 0 && s_nullEmits < 20000) {
				++s_nullEmits;
				std::fprintf(stderr,
					"[MECHRESTORE v1] event=nullshape frame=%u actor=%lu type=%p status=%d\n",
					(unsigned)g_mc2FrameCounter, (unsigned long)actorHandle_,
					(void*)mechType, (int)status);
			}
		}
		return NO_ERR;
	}

	// Force textures to reload due to unique instance.
	mechShape->SetTextureHandle(0,localTextureHandle);

	if (rightArm)
		rightArm->SetTextureHandle(0,localTextureHandle);

	if (leftArm)
		leftArm->SetTextureHandle(0,localTextureHandle);

	// C3: render gate — GPU-lagged visibility when killswitch is enabled.
	// Preserve the g_useGpuStaticProps fallback for static-prop path.
	// PREVIEW-FIX: SimpleCamera UI preview (Mech Bay / Purchase) always renders
	// the mech regardless of world inView/GPU-cull — the preview is the whole
	// point of the panel and its visibility is owned by the UI, not world cull.
	const bool mechShouldRender = (g_mechPreviewRenderDepth > 0)
		? true
		: s_gpuCullLifecycle
			? (gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)) || g_useGpuStaticProps)
			: (inView || g_useGpuStaticProps);
	if (mechShouldRender)
	{
		if (g_mechPreviewRenderDepth > 0 && getenv("MC2_LOG_PREVIEW"))
		{
			FILE* f = fopen("preview_debug.log","a");
			if (f) { fprintf(f,"[PREVIEW] Mech3D::render preview shouldRender=1 visible=%d mechShape=%p currentLOD=%d\n",(int)visible,(void*)mechShape,(int)currentLOD); fflush(f); fclose(f); }
		}
		if (visible)
		{
			uint32_t color = SD_BLUE;
			uint32_t highLight = 0x007f7f7f;
			if ((teamId > -1) && (teamId < 8)) {
				static uint32_t highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
				static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
				color = colorTable[homeTeamRelationship];
				highLight = highLightTable[homeTeamRelationship];
			}
			if (selected & DRAW_COLORED && duration <= 0 )
			{
				mechShape->SetARGBHighLight(highLight);
			}
			else
			{
				mechShape->SetARGBHighLight(highlightColor);
			}

			Camera::HazeFactor = hazeFactor;

			if (drawFlash)
			{
				mechShape->SetARGBHighLight(flashColor);
			}
			//---------------------------------------------
			// Call Multi-shape render stuff here.
			//
			// GPU mech batcher Slice A: try GPU submit first; fall back to CPU
			// path if registration / capacity / shader-init says so. Counter
			// recorded BEFORE registration check so eligible-actor totals are
			// accurate even when the actor opts out for unrelated reasons.
			GpuMechBatcher::instance().recordEligibleActor();

			// Slice C1: render-only mech GPU cull. If MC2_GPU_MECH_CULL is on
			// AND the GPU lagged-readback says this actor was invisible last
			// frame, skip the GPU mech submit. We DO NOT bypass mechShape->Render
			// (CPU fallback) because the actor still ran updateGeometry/AI/etc;
			// we just skip writing its geometry into the GPU mech bucket. update()
			// has already run by the time render() is called, so AI/lifecycle/
			// damage all proceed normally for offscreen actors. Independent of
			// MC2_GPU_CULL_LIFECYCLE which gates a separate concern.
			//
			// Stale readback semantics: returns true (visible) for actors with
			// no readback record yet (newly spawned), so this is fail-open in
			// uncertainty. One frame of stale "invisible" → one frame of
			// missing render, recovered next frame at 60Hz.
			bool mechGpuCullSkip = false;
			if (g_useGpuMechs && g_useGpuMechCull) {
				// CONSERVATIVE-OR (mirrors terrobj.cpp / static-prop fix):
				// the lagged GPU readback may CONFIRM invisibility but must
				// NOT DENY visibility. Only skip the GPU submit when BOTH the
				// lagged readback AND the CPU inView test agree the mech is
				// offscreen. The lagged readback is unstable frame-to-frame
				// (d65552ab made readback default-ON; slot rotation + camera
				// motion flip it even for a dead-center actor). When it flipped
				// to "invisible" we skipped the GPU submit and fell back to the
				// CPU mechShape->Render() path, which does not actually draw
				// while GPU mech batching owns rendering -> the mech vanished
				// for that frame -> center-screen flicker. Requiring CPU
				// inView agreement means an in-view mech is never skipped, so
				// it is always GPU-drawn; only genuinely-offscreen mechs (both
				// signals agree) still skip, preserving the cull perf win.
				const bool lagVis = gpu_cull::readback_isActorVisibleLagged(
				    static_cast<uint32_t>(actorHandle_));
				if (!lagVis && !inView) {
					mechGpuCullSkip = true;
				}
			}

			bool gpuMechSubmitted = false;
			// PREVIEW-FIX: in SimpleCamera UI preview context, skip the GPU mech
			// submit entirely so gpuMechSubmitted stays false and the CPU MLR
			// draw below runs. The GPU batcher flushes with the world snapshot /
			// terrain MVP and would draw the preview mech off the UI viewport.
			const bool previewContext = (g_mechPreviewRenderDepth > 0);
			if (g_useGpuMechs && !mechGpuCullSkip && g_drawMechs && !previewContext) {
				// Replicate the highlight selection from the CPU SetARGBHighLight
				// branches above so the GPU path sees the same color choice.
				uint32_t gpuHighlightARGB = highLight;
				if (selected & DRAW_COLORED && duration <= 0)
					gpuHighlightARGB = highLight;
				else
					gpuHighlightARGB = (uint32_t)highlightColor;
				if (drawFlash)
					gpuHighlightARGB = (uint32_t)flashColor;

				GpuMechSubmitDesc desc{};
				desc.mechShape      = mechShape;
				desc.mechType       = mechType;
				desc.currentLOD     = (int)currentLOD;
				desc.slot0TexHandle = (uint32_t)localTextureHandle;
				// AO-1: carry the AO slot (0 when no/failed AO). HAS_AO bit (4) added
				// below ONLY if aoValid, so the shader samples unit 6 iff a real AO loaded.
				desc.slot6TexHandle = aoValid ? (uint32_t)aoTextureHandle : 0u;
				// NORMALS-1: carry the normal-map slot. HAS_NORMAL bit (5) added below ONLY
				// if normalValid, so the shader samples unit 7 iff a real normal map loaded.
				desc.slot7TexHandle = normalValid ? (uint32_t)normalTextureHandle : 0u;
				// Slice B1: use the dedup-cache slot populated by
				// CacheGpuLightData() in update(). 0xFFFFFFFFu sentinel
				// means "not yet cached" (e.g. spawn-frame before the
				// first update() runs) — fall back to slot 0. Visual
				// effect for that one frame: either inherits another
				// actor's lighting (slot 0 contents at that moment) or,
				// if slot 0 has numLights==0, calc_light returns the
				// ambient floor (kAmbientFloor=0.35 grey) thanks to
				// MC2_STATIC_PROP_LIGHTING. Either case is imperceptible
				// at 60 Hz.
				const uint32_t cachedLI = mechShape->getCachedGpuLightIndex();
				desc.lightDataIndex = (cachedLI == 0xFFFFFFFFu) ? 0u : cachedLI;
				// Slice B+ (2026-05-09): per-actor lightsOut from object
				// status. CPU path at mech3d.cpp:3154 sets
				// mechShape->SetLightsOut(true) for these states; GPU path
				// reads bit 1 of inst.renderFlags in mech.vert and skips
				// per-light contributions, leaving only the ambient floor.
				const bool lightsOut =
					(status == OBJECT_STATUS_DESTROYED) ||
					(status == OBJECT_STATUS_DISABLED) ||
					(status == OBJECT_STATUS_SHUTDOWN);
				desc.renderFlags    = lightsOut ? 0x2u : 0x0u;  // bit 1
				if (aoValid) desc.renderFlags |= 0x10u;          // AO-1: bit 4 = HAS_AO (handle loaded)
				if (normalValid) desc.renderFlags |= 0x20u;      // NORMALS-1: bit 5 = HAS_NORMAL (handle loaded)
				desc.highlightARGB  = gpuHighlightARGB;
				// Slice B2: pack actor hazeFactor [0,1] into the alpha
				// byte of fogARGB. mech.vert combines it with
				// g_scene.fogColor.rgb to drive per-actor fog mix in
				// mech.frag. RGB of fogARGB is unused (the engine fog
				// color is global; per-actor color tinting deferred).
				const float hazeClamped = (hazeFactor < 0.0f) ? 0.0f : (hazeFactor > 1.0f ? 1.0f : hazeFactor);
				const uint8_t hazeByte  = (uint8_t)(hazeClamped * 255.0f + 0.5f);
				desc.fogARGB        = (uint32_t)hazeByte << 24;

				// M2.5 (Q3 unconditional): forward the M2-stored RenderWorld handle
				// to the GPU. M2 stored the handle on Mech3DAppearance::mechRenderHandle
				// via GameAdapters::Mech::registerMech. M2.5 emits the bits to
				// attachment-2 via mech.frag under MC2_OBJECT_ID_BUFFER.
				//
				// CPU fill is UNCONDITIONAL per spec Q3: env-OFF still pays the
				// load+store (< 10 ns per instance) so instance data shape stays
				// stable; the env gate lives at the GLSL macro level. Realistic
				// cost at mc2_24 (46 mechs): < 1 us per frame.
				//
				// Handle::invalid().raw() == 0 by definition, so any pre-register
				// frame or actor that missed registration writes 0 -- correctly
				// classified as "background" by RenderWorld::lookupAtPixel.
				desc.objectIdRaw    = getRenderWorldHandle().raw();

				// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: forward the sanitized
				// per-mech visual state pushed by BattleMech::render(). Slice 1
				// only carries it to the SSBO/debug dump; no shader consumes it
				// yet (byte-identical). damage01/flags land in GpuMechInstance;
				// heat01 rides along for the debug dump (always 0, USEHEAT off).
				{
					const RenderCore::MechVisualState& vs = getVisualState();
					desc.heat01      = vs.heat01;
					desc.damage01    = vs.damage01;
					desc.visualFlags = vs.flags;
				}

				gpuMechSubmitted = GpuMechBatcher::instance().submitActor(desc);
				// Only count as a fallback if the GPU path was nominally
				// enabled at this point. submitActor returns false on
				// (killswitch off || !finalized || shader fail || late
				// registration). Killswitch-off and not-yet-finalized are
				// not "fallbacks" — they're "GPU path not active." Without
				// this gate, every actor in MC2_GPU_MECHS=0 mode is logged
				// as ShaderInitFailure, making the [MECHBATCHER v1]
				// event=summary fallback counters useless.
				if (!gpuMechSubmitted && g_useGpuMechs) {
					GpuMechBatcher::instance().recordCpuFallback(
						GpuMechBatcher::instance().wasLastFailureLateRegistration()
							? GpuMechFallbackReason::UnregisteredType
							: GpuMechFallbackReason::ShaderInitFailure);
				}
			}

			// MECH-CULL-CPU-FIX: mechGpuCullSkip gates GPU submit (correct), but must
			// NOT gate the CPU fallback. readback_isActorVisibleLagged() uses lagged
			// GPU cull results; on camera-move frames the lagged readback shows some
			// center-screen mechs as invisible (frustum shifted between readback frame
			// and now), causing them to skip BOTH GPU and CPU render → visual pop.
			// CPU fallback is always safe — it renders via the established CPU path and
			// the mech is properly depth-tested / frustum-clipped by the rasterizer.
			// The GPU cull DOES still gate GpuMechBatcher submission (line above) so
			// off-screen mechs are still excluded from the GPU batcher workload.
			if (!gpuMechSubmitted && g_drawMechs) {
				// M2.5 (Q6 amendment 2): count MLR/CPU-fallback draws so
				// the always-on per-mission mlr_mech_summary line reflects
				// Path-B incidence. M2.6 readiness decision consults this
				// value (spec §12 Q6 amendment 3).
				++s_mlrMechDrawsThisMission;
				mechShape->Render(true);  // CPU path — unchanged

				// PREVIEW-FIX diagnostic: confirm the UI preview path takes the
				// CPU draw and skips GPU submit. MC2_MECH_PREVIEW_TRACE=1, once
				// per process (preview renders ~60Hz; one line is enough).
				if (previewContext) {
					static bool s_prevTraceInit = false;
					static bool s_prevTrace = false;
					if (!s_prevTraceInit) {
						s_prevTraceInit = true;
						s_prevTrace = (getenv("MC2_MECH_PREVIEW_TRACE") != nullptr);
					}
					static bool s_prevTraceEmitted = false;
					if (s_prevTrace && !s_prevTraceEmitted) {
						s_prevTraceEmitted = true;
						std::printf("[MECH_PREVIEW v1] event=preview_render gpu_submit=0 cpu_draw=1 depth=%d\n",
							g_mechPreviewRenderDepth);
						std::fflush(stdout);
					}
				}
			}

			// [MECHRESTORE v1] per-actor submit discriminator. Gated +
			// globally throttled (one load needs only the first frames;
			// per-actor-per-frame in this hot path would flood). The
			// invisible-from-savegame signature is gpuMechSubmitted=0
			// cullSkip=0 finalized=0 for every actor of one type.
			{
				static const bool s_mechRestoreTrace =
					(getenv("MC2_MECH_RESTORE_TRACE") != nullptr);
				static int s_mechRestoreEmits = 0;
				// DIAG (GPU-MECH-EQUIV recon): emit EVERY submitted=0 actor on a
				// separate generous cap (the mech-bay preview consumes the 2000
				// success cap before the mission roster renders). frame + inView
				// let us tell persistent invisibility (same actor failing across
				// many frames, in-view) from a 1-frame spawn transient, and
				// cullSkip vs lateReg tells offscreen-cull from registration-fail.
				static int s_mechRestoreFailEmits = 0;
				const bool emitFail = (!gpuMechSubmitted && s_mechRestoreFailEmits < 20000);
				if (s_mechRestoreTrace && (emitFail || s_mechRestoreEmits < 2000)) {
					if (emitFail) ++s_mechRestoreFailEmits; else ++s_mechRestoreEmits;
					std::fprintf(stderr,
						"[MECHRESTORE v1] event=submit frame=%u actor=%lu type=%p lod=%d "
						"submitted=%d cullSkip=%d lateReg=%d finalized=%d status=%d inView=%d\n",
						(unsigned)g_mc2FrameCounter,
						(unsigned long)actorHandle_, (void*)mechType, (int)currentLOD,
						gpuMechSubmitted ? 1 : 0, mechGpuCullSkip ? 1 : 0,
						GpuMechBatcher::instance().wasLastFailureLateRegistration() ? 1 : 0,
						GpuMechBatcher::instance().isFinalized() ? 1 : 0, (int)status,
						(int)inView);
				}
			}

			if (selected & DRAW_BARS)
			{
				drawBars();
			}
			if ( selected & DRAW_BRACKETS )
			{
				drawSelectBrackets(color);
			}
			if ((objectNameId != -1) && (selected & DRAW_TEXT) && !sensorLevel )
			{
				drawBars();

				if ( strlen( mechName ) )
				{
					drawTextHelp( mechName, color );
				}
				else
				{
					char tmpString[255];
					cLoadString(objectNameId, tmpString, 254);
					drawTextHelp(tmpString, color);
				}
				if ( strlen( pilotName ) )
				{
					drawPilotName( pilotName, color );
				}
				else if ( pilotNameID != -1 ) // only draw your own
				{
					char tmpPilotName[255];
					cLoadString( pilotNameID, tmpPilotName, 254 );
					drawPilotName( tmpPilotName, color );

				}

			}

			//------------------------------------------------
			// Render GOSFX
			if (!InEditor)
			{
				gosFX::Effect::DrawInfo drawInfo;
				drawInfo.m_clipper = theClipper;
				
				MidLevelRenderer::MLRState mlrState;
				mlrState.SetDitherOn();
				mlrState.SetTextureCorrectionOn();
				mlrState.SetZBufferCompareOn();
				mlrState.SetZBufferWriteOn();
		
				drawInfo.m_state = mlrState;
				drawInfo.m_clippingFlags = 0x0;
				
				if (!isWaking && useNonWeaponEffects)
				{
					for (long i=0;i<MAX_DUST_POOFS;i++)
					{
						if (rightFootPoofDraw[i] && rightDustPoofEffect[i] && rightDustPoofEffect[i]->IsExecuted())
						{
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(rFootPosition[i]);
							
							drawInfo.m_parentToWorld = &shapeOrigin;
					 
							if (!MLRVertexLimitReached)
								rightDustPoofEffect[i]->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RDUST);
						}
						
						if (leftFootPoofDraw[i] && leftDustPoofEffect[i] && leftDustPoofEffect[i]->IsExecuted())
						{
							Stuff::LinearMatrix4D 	shapeOrigin;
							Stuff::LinearMatrix4D	localToWorld;
							
							shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
							shapeOrigin.BuildTranslation(lFootPosition[i]);
							
							drawInfo.m_parentToWorld = &shapeOrigin;
					 
							if (!MLRVertexLimitReached)
								leftDustPoofEffect[i]->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LDUST);
						}
					}
				}
				
				if ((currentGestureId == GestureJump) && inJump && jumpJetEffect && jumpJetEffect->IsExecuted())
				{
					long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
					Stuff::Vector3D jumpNodePos = getNodePosition(jumpNodeId);
					
					Stuff::Point3D			actualPosition;
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
					
					actualPosition.x = -jumpNodePos.x;
					actualPosition.y = jumpNodePos.z;
					actualPosition.z = jumpNodePos.y;
					
 					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(actualPosition);
					
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						jumpJetEffect->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_JUMP);
				}
						
				//----------------------------------------------------------------
				// Arm Blown FX
				if (leftShoulderBoom)
				{
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					if (leftArmNodeIndex == -1)
						leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

					Stuff::Vector3D leftNodePos = getNodeIdPosition(leftArmNodeIndex);
					leftShoulderPos.x = -leftNodePos.x;
					leftShoulderPos.y = leftNodePos.z;
					leftShoulderPos.z = leftNodePos.y;
		
 					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(leftShoulderPos);
							
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						leftShoulderBoom->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LBOOM);
				}
				
				if (rightShoulderBoom)
				{
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					if (rightArmNodeIndex == -1)
						rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

					Stuff::Vector3D rightNodePos = getNodeIdPosition(rightArmNodeIndex);
					rightShoulderPos.x = -rightNodePos.x;
					rightShoulderPos.y = rightNodePos.z;
					rightShoulderPos.z = rightNodePos.y;
					
 					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(rightShoulderPos);
							
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
					
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						rightShoulderBoom->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RBOOM);
				}
				
				//------------------------------------------------
				// All other FXs
				if (isSmoking != -1)
				{
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
							
					Stuff::Vector3D smokeNodePos = getNodePosition(0);
					Stuff::Point3D smokePos;
					smokePos.x = -smokeNodePos.x;
					smokePos.y = smokeNodePos.z;
					smokePos.z = smokeNodePos.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(smokePos);
							
					drawInfo.m_parentToWorld = &shapeOrigin;
					if (isSmoking > 0)
					{
						//Loops until told to stop.
						if (!MLRVertexLimitReached)
							criticalSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_CSMOKE);
					}
					else
					{
						if (!MLRVertexLimitReached)
							smokeEffect->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_SMOKE);
					}
				}
				
				if (waterWake && isWaking && (currentGestureId != 20))	//Do NOT draw wake if we are jumping.  keep calcing it, though!
				{
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					Stuff::Point3D wakePos;
					wakePos.x = -position.x;
					wakePos.y = Terrain::waterElevation;
					wakePos.z = position.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(wakePos);
							
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);

					if (!inReverse)
						localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					else
						localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);

					localResult.Multiply(localToWorld,shapeOrigin);
		
 					drawInfo.m_parentToWorld = &localResult;
					if (!MLRVertexLimitReached)
						waterWake->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_WAKE);
				}
				
				if (isDusting && helicopterDustCloud)
				{
					Stuff::LinearMatrix4D 	shapeOrigin;
					Stuff::LinearMatrix4D	localToWorld;
					Stuff::LinearMatrix4D	localResult;
							
					Stuff::Point3D wakePos;
					wakePos.x = -position.x;
					wakePos.y = position.z;
					wakePos.z = position.y;
					
					shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
					shapeOrigin.BuildTranslation(wakePos);
							
					/*
					Stuff::UnitQuaternion effectRot;
					effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
					localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
					localResult.Multiply(localToWorld,shapeOrigin);
					*/
		
 					drawInfo.m_parentToWorld = &shapeOrigin;
					if (!MLRVertexLimitReached)
						helicopterDustCloud->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_HELIDUST);
				}
			}
				   
#ifdef DRAW_BOX
			//---------------------------------------------------------
			// Render the Bounding Box to see if it is OK.
			Stuff::Vector3D nodeCenter = mechShape->GetRootNodeCenter();

			Stuff::Vector3D boxCoords[8];
			boxCoords[0].x = position.x + mechType->typeUpperLeft.x;
			boxCoords[0].y = position.y + mechType->typeUpperLeft.y;
			boxCoords[0].z = position.z + mechType->typeUpperLeft.z;

			boxCoords[1].x = position.x + mechType->typeUpperLeft.x;
			boxCoords[1].y = position.y + mechType->typeLowerRight.y;
			boxCoords[1].z = position.z + mechType->typeUpperLeft.z;

			boxCoords[2].x = position.x + mechType->typeLowerRight.x;
			boxCoords[2].y = position.y + mechType->typeUpperLeft.y;
			boxCoords[2].z = position.z + mechType->typeUpperLeft.z;

			boxCoords[3].x = position.x + mechType->typeLowerRight.x;
			boxCoords[3].y = position.y + mechType->typeLowerRight.y;
			boxCoords[3].z = position.z + mechType->typeUpperLeft.z;

			boxCoords[4].x = position.x + mechType->typeLowerRight.x;
			boxCoords[4].y = position.y + mechType->typeLowerRight.y;
			boxCoords[4].z = position.z + mechType->typeLowerRight.z;

			boxCoords[5].x = position.x + mechType->typeLowerRight.x;
			boxCoords[5].y = position.y + mechType->typeUpperLeft.y;
			boxCoords[5].z = position.z + mechType->typeLowerRight.z;

			boxCoords[6].x = position.x + mechType->typeUpperLeft.x;
			boxCoords[6].y = position.y + mechType->typeLowerRight.y;
			boxCoords[6].z = position.z + mechType->typeLowerRight.z;

			boxCoords[7].x = position.x + mechType->typeUpperLeft.x;
			boxCoords[7].y = position.y + mechType->typeUpperLeft.y;
			boxCoords[7].z = position.z + mechType->typeLowerRight.z;
			
 			Stuff::Vector4D screenPos[8];
			for (long i=0;i<8;i++)
			{
				// [PROJECTZ:ScreenXYOracle id=mech3d_box_wire]
				eye->projectForScreenXY(boxCoords[i],screenPos[i]);
			}

			{
				LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,NULL,-1);
				newElement.draw();
			}
			
			{
				LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,NULL,-1);
				newElement.draw();
			}
#endif					
		}
		
		if ((sensorLevel > 0) && (sensorLevel < 5))
		{
			//---------------------------------------
			// Draw Sensor Contact here.
			if (sensorLevel > 1)
			{
				sensorSquareShape->Render();
			}
			else
			{
				sensorTriangleShape->Render();
			}
		}
	}

	if (rightArmOff && rightArm && rightArmInView)
	{
		Camera::HazeFactor = rightArmHazeFactor;

		rightArm->Render(true);

		if (rightArmSmoke)
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;
 		
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			rightShoulderPos.x = -rightArmPos.x;
			rightShoulderPos.y = rightArmPos.z;
			rightShoulderPos.z = rightArmPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(rightShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			drawInfo.m_parentToWorld = &localResult;
			if (!MLRVertexLimitReached)
				rightArmSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_RARMSMOKE);
		}
	}
 	
 	if (leftArmOff && leftArm && leftArmInView)
	{
		Camera::HazeFactor = leftArmHazeFactor;

		leftArm->Render(true);
		
		if (leftArmSmoke)
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;
				
 			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			leftShoulderPos.x = -leftArmPos.x;
			leftShoulderPos.y = leftArmPos.z;
			leftShoulderPos.z = leftArmPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(leftShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			drawInfo.m_parentToWorld = &localResult;
			if (!MLRVertexLimitReached)
				leftArmSmoke->Draw(&drawInfo); if (s_fxCountLog) fxCountTick(FX_LARMSMOKE);
		}
	}
	
 	return NO_ERR;
}

//-----------------------------------------------------------------------------
long Mech3DAppearance::renderShadows (void)
{
	// Skip legacy blob shadows when shadow maps are active
	if (gos_IsTerrainTessellationActive())
		return NO_ERR;

	// DEGRADE-DON'T-CRASH: partial import with no base shape -> no shadow.
	if (!mechShape)
		return NO_ERR;

	mechShape->SetTextureHandle(0,localTextureHandle);

	// C3: route renderShadows gate to GPU-lagged visibility when killswitch is enabled.
	const bool mechShadowVisible = s_gpuCullLifecycle
		? (gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)) && visible)
		: (inView && visible);
	if (mechShadowVisible)
	{
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		// DONT RENDER UNTIL FINAL MECH DATA FROM MR CHOI
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		if (mechShadowShape)
			mechShadowShape->RenderShadows(true);
		else
			mechShape->RenderShadows(true);
	}

	return(NO_ERR);
}

bool oneMechPlease = false;

//-----------------------------------------------------------------------------
void Mech3DAppearance::setObjStatus (long oStatus)
{
	if ( (status != oStatus) && (!InEditor))
	{
		if (oStatus == OBJECT_STATUS_DESTROYED)
		{
			if (currentGestureId == 23)
			{
				if (mechType->mechForwardDmgShape)
				{
					mechShape->ClearAnimation();
					mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
					delete mechShape;
					mechShape = NULL;

					mechShape = mechType->mechForwardDmgShape->CreateFrom();
				}
			}
			else if (currentGestureId == 24)
			{
				if (mechType->mechBackwardDmgShape)
				{
					mechShape->ClearAnimation();
					mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
					delete mechShape;
					mechShape = NULL;

					mechShape = mechType->mechBackwardDmgShape->CreateFrom();
				}
			}

			currentLOD = 0;

			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<mechShape->GetNumTextures();i++)
			{
				char txmName[1024];
				mechShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = 0;
						
						if (!i)
						{
							localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
							mechShape->SetTextureHandle(i,localTextureHandle);
							mechShape->SetTextureAlpha(i,true);
						}
						else
						{
							gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
							mechShape->SetTextureHandle(i,gosTextureHandle);
							mechShape->SetTextureAlpha(i,true);
						}
						
					}
					else
					{
						DWORD gosTextureHandle = 0;
						
						if (!i)
						{
							localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
							mechShape->SetTextureHandle(i,localTextureHandle);
							mechShape->SetTextureAlpha(i,false);
						}
						else
						{
							gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
							mechShape->SetTextureHandle(i,gosTextureHandle);
							mechShape->SetTextureAlpha(i,false);
						}
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					mechShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

		if (status == OBJECT_STATUS_DESTROYED && oStatus == OBJECT_STATUS_NORMAL)
		{
			STOP(("Destroyed Mech just came back to life!!"));
		}
	}
	
	status = oStatus;
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::updateGeometry (void)
{
	ZoneScopedN("GameLogic.Mech3D.UpdateGeometry");
	// F3 CPU projection cost-baseline: sidecar bucket (NEVER in
	// projection_total). One scope per visible mech per frame.
	::mc2_cpu_proj_cost::SidecarScope _f3_skin_scope(
	    ::mc2_cpu_proj_cost::SIDECAR_SKINNING_CHAIN);

	// BT2018-SKEL-ENGINE-1B-RUNTIME / 1C: advance + re-pose imported skinned mechs
	// once per frame BEFORE TransformMultiShape re-reads the shared type geometry
	// below. Idempotent on g_mc2FrameCounter (the second combat update() is a no-op
	// here); near-free when no imported animated mech is registered. 1C selects the
	// clip from this mech's movement (gesture + position/leg-heading deltas).
	{
		mc2mechanim::MechMotion _mm;
		_mm.gestureId = (int)currentGestureId;
		_mm.px = position.x; _mm.py = position.y; _mm.pz = position.z;
		_mm.legHeadingDeg = rotation;
		// PER-ACTOR: actorKey = this instance shape; typeKey = the chassis type shape
		// (== the batcher's rec.importedGpuType / desc.mechShape keying).
		const void* _typeKey = (mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr);
		// ASSIMP-MECH-PAUSE-GATE-1: stock mechs freeze their gait while paused (the
		// if(animate) guard below at ~5000). Imported mechs advance through updateGeometry
		// regardless, so they kept striding in place during pause. Freeze the imported clip
		// clock when gamePaused (timing.h global). Killswitch MC2_ASSIMP_PAUSE_GATE=0 restores
		// the old always-advance behavior for A/B.
		extern bool gamePaused;  // mclib/timing.h
		static const bool s_assimpPauseGate = []{
			const char* v = std::getenv("MC2_ASSIMP_PAUSE_GATE");
			return !(v && v[0] == '0');   // default-ON; only "0" disables
		}();
		const bool _advanceClock = s_assimpPauseGate ? !gamePaused : true;
		mc2mechanim::TickImportedMechs(frameLength, (unsigned)g_mc2FrameCounter, _mm,
		                               (const void*)mechShape, _typeKey, _advanceClock);
		// ANIM-FROZEN-WARNING-FIX-1: imported-mech GPU animation IS implemented and
		// working at shipped defaults (per-actor model-delta palette; proven by
		// IMPORTED-GPU-ANIM-READPATH-RECON-1). It is armed by MC2_MECH_IMPORT_GPU.
		// The pose is only FROZEN when the GPU mech path is on but that import-GPU
		// gate is OFF (the stock per-node branch then draws the immutable rest VBO).
		// So warn ONLY in that genuinely-frozen case (was firing unconditionally and
		// LYING whenever the import-GPU path was armed). Both workarounds given.
		if (g_useGpuMechs && mc2mechanim::AnyImportedAnim() &&
		    !mc2mechanim::ImportedGpuEnabled()) {
			static bool s_warnedImportGpu = false;
			if (!s_warnedImportGpu) {
				s_warnedImportGpu = true;
				fprintf(stderr, "[MECH_IMPORT] animated imported mech with the import-GPU "
				                "palette OFF shows a FROZEN pose; set MC2_MECH_IMPORT_GPU=1 "
				                "(or run MC2_GPU_MECHS=0)\n");
			}
		}
	}

	//Always override with our local instance.
	mechShape->SetTextureHandle(0,localTextureHandle);
	
	if (rightArm)
		rightArm->SetTextureHandle(0,localTextureHandle);

	if (leftArm)
		leftArm->SetTextureHandle(0,localTextureHandle);

	// Slice D-shadow-state-strip: when GPU mech path + state-strip
	// killswitch + tessellation are all active, skip ALL per-frame state
	// setters on mechShadowShape. Recon (D-shadow-skip §Q1-Q4 + D-shadow-
	// state-strip §Q1-Q4) proved no consumer of these setters' effects
	// exists in this configuration: instance-state setters feed only
	// TransformMultiShape (already retired by D-shadow-skip), and the
	// global-static side effect (SetLightList writing s_listOfLights) is
	// overwritten by mechShape's identical call at mech3d.cpp:3407 before
	// any consumer reads it.
	// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: former MC2_GPU_MECH_SHADOW_STATE_STRIP
	// (default-ON, strict no-op on tessellated GPU-mech path) retired to constant.
	const bool stripShadowState =
		g_useGpuMechs &&
		gos_IsTerrainTessellationActive();

	if ((status == OBJECT_STATUS_DESTROYED) ||
		(status == OBJECT_STATUS_DISABLED) || 
		(status == OBJECT_STATUS_SHUTDOWN))
	{
		mechShape->SetLightsOut(true);
	}
	else
	{
		mechShape->SetLightsOut(false);
	}
	
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;

	//Update flashing regardless of view!!!
	if (duration > 0.0f)
	{
		duration -= frameLength;
		currentFlash -= frameLength;
		if (currentFlash < 0.0f)
		{
			drawFlash ^= true;
			currentFlash = flashDuration;
		}
	}
	else
	{
		drawFlash = false;
	}

	//if (visible)
	{
		//-------------------------------------------
		// Does math necessary to draw mech.
		Stuff::UnitQuaternion qRotation;
		float yaw = rotation * DEGREES_TO_RADS;
		qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		
		if ( land )
			lightIntensity = land->getTerrainLight(position);
							
		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);
	
		DWORD lightRGB = (lightr<<16) + (lightg<<8) + lightb;
		
		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);
	
		DWORD fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;
	
		if (useFog && (xlatPosition.y < fogStart))
		{
			float fogFactor = fogStart - xlatPosition.y;
			if (fogFactor < 0.0)
				fogRGB = 0xff<<24;
			else
			{
				fogFactor /= (fogStart - fogFull);
				if (fogFactor <= 1.0)
				{
					fogFactor *= fogFactor;
					fogFactor = 1.0 - fogFactor;
					fogFactor *= 256.0;
				}
				else
				{
					fogFactor = 256.0;
				}
	
				unsigned char fogResult = fogFactor;
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}
	
		if (useFog)
			mechShape->SetFogRGB(fogRGB);
		else
			mechShape->SetFogRGB(0xffffffff);

		//-----------------------------------
		//Test of Mech as Point Light Source
		if (!pointLight && eye->isNight)
		{
			if (lightCircleNodeIndex == -1)
				lightCircleNodeIndex = mechShape->GetNodeNameId("SLCircle_anubis");

			Stuff::Vector3D lightPos = getNodeIdPosition(lightCircleNodeIndex);
			if (lightPos != position)
			{
				pointLight = (TG_LightPtr)malloc(sizeof(TG_Light));
				pointLight->init(TG_LIGHT_SPOT);
				lightId = eye->addWorldLight(pointLight);

				pointLight->SetaRGB(0xffffff00);
				pointLight->SetIntensity(0.15f);
				pointLight->SetFalloffDistances(50.0f, 250.0f);
			}
		}
		
		if (pointLight)	
		{
			if (visible && (sensorLevel > 4) && !InEditor)
			{
				if (lightCircleNodeIndex == -1)
					lightCircleNodeIndex = mechShape->GetNodeNameId("SLCircle_anubis");

				Stuff::Vector3D lightPos = getNodeIdPosition(lightCircleNodeIndex);

				Stuff::Point3D ourPosition;
				ourPosition.x = -lightPos.x;
				ourPosition.y = lightPos.z;
				ourPosition.z = lightPos.y;
		
				pointLight->direction = ourPosition;
		
				pointLight->spotDir.x = -position.x;
				pointLight->spotDir.y = position.z;
				pointLight->spotDir.z = position.y;

				pointLight->maxSpotLength = 50.0f;

				Stuff::LinearMatrix4D lightToWorldMatrix;
				lightToWorldMatrix.BuildTranslation(ourPosition);
				lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				pointLight->SetLightToWorld(&lightToWorldMatrix);
				pointLight->SetPosition(&lightPos);
				pointLight->active = true;
			}
			else
			{
				pointLight->active = false;
			}
		}

		// (E) T1.6 / T1.7: generalised SpotLight_-child illumination. Parallel
		// path to the SLCircle_anubis pointLight block above (R7: distinct
		// node-name prefixes, no double-registration). Lazy first-night
		// register; per-frame in-place update afterwards (T1.7: no per-frame
		// pool churn). Public accessors (msl.h:431 GetNumShapes, msl.h:438
		// GetShapeRec, tgl.h:951 GetIsSpotlight) because Mech3DAppearance is
		// NOT in the TG_MultiShape friend list at msl.h:251-256.
		// (E) T3.1: gate retired; behavior is now unconditional. Coexists with
		// the SLCircle_anubis pointLight block above (R7: distinct node-name
		// prefix, no double-registration).
		if (mechShape)
		{
			if (!spotlightsRegistered_ && eye->isNight)
			{
				// [SPOT_DIAG v1] T1.15 per-actor registration counters.
				int diagChildrenWalked = 0;
				int diagSpotlightsFound = 0;
				int diagRegistered = 0;
				int diagOverflow = 0;
				for (int i = 0; i < mechShape->GetNumShapes(); ++i)
				{
					const TG_ShapeRec* recp = mechShape->GetShapeRec(i);
					if (!recp) continue;
					TG_Shape* c = recp->node;
					if (!c || !recp->processMe) continue;
					++diagChildrenWalked;
					if (!c->GetIsSpotlight()) continue;
					++diagSpotlightsFound;

					const char* nodeName = c->getNodeName();
					if (!nodeName) continue;
					long nodeId = mechShape->GetNodeNameId(nodeName);
					if (nodeId == -1) continue;

					TG_LightPtr light = (TG_LightPtr)malloc(sizeof(TG_Light));
					light->init(TG_LIGHT_POINT);              // v1 (OQ2/M6) — POINT
					light->SetaRGB(0xffffff00);                // anubis-equiv default
					light->SetIntensity(0.15f);                // anubis default
					light->SetFalloffDistances(50.0f, 250.0f); // anubis default

					long slotId = eye->addWorldLight(light);
					if (slotId < 0) { free(light); continue; }
					spotlightNodeIds_.push_back(nodeId);
					spotlightLights_.push_back(light);
					spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
					// T1.16 — tag this slot as (E)-owned, source=Mech.
					mc2_spotlight_diag::tag_slot(slotId, mc2_spotlight_diag::Mech);
					++diagRegistered;
				}
				diagOverflow = diagSpotlightsFound - diagRegistered;
				if (diagOverflow < 0) diagOverflow = 0;
				s_spotDiagMechRegistered += (unsigned long)diagRegistered;
				s_spotDiagMechOverflows  += (unsigned long)diagOverflow;
				++s_spotDiagMechActors;
				if (s_spotDiagMechActors <= 8) {
					std::fprintf(stderr,
						"[SPOT_DIAG v1] event=first_register class=mech actor_id=%ld "
						"children_walked=%d spotlights_found=%d registered=%d overflow=%d\n",
						actorHandle_, diagChildrenWalked, diagSpotlightsFound,
						diagRegistered, diagOverflow);
					std::fflush(stderr);
				}
				spotlightsRegistered_ = true;  // register-once flag (M3)
			}
			// [SPOT_DIAG v1] T1.15 per-summary registration aggregate (mech).
			if (s_spotDiagMechEnabled) {
				++s_spotDiagMechCalls;
				if ((s_spotDiagMechCalls % 600) == 0) {
					std::fprintf(stderr,
						"[SPOT_DIAG v1] event=registration_summary class=mech "
						"calls=%lu actors_first_hit=%lu lights_registered=%lu overflows=%lu\n",
						s_spotDiagMechCalls, s_spotDiagMechActors,
						s_spotDiagMechRegistered, s_spotDiagMechOverflows);
					std::fflush(stderr);
				}
			}

			// T1.7: per-frame in-place update. UNCONDITIONAL once registered
			// (C-r2 C2: lights stay allocated across day/night; toggle active).
			// active gate matches anubis verbatim (C-r1 C5): visible && (sensorLevel
			// > 4) && !InEditor, plus isNight (bare field, no parens, per C-r3 C2).
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
			{
				Stuff::Vector3D childPos =
					getNodeIdPosition(spotlightNodeIds_[k]);
				spotlightLights_[k]->SetPosition(&childPos);
				// Rule-2 correctness fix: lightToWorld is consumed at
				// msl.cpp:1659. Without it, the precomputed s_lightToShape
				// collapses to worldToShape alone and the light's effective
				// world position is lost. Matches the existing anubis
				// pattern at mech3d.cpp:3373-3377 (translation-only).
				Stuff::LinearMatrix4D lightToWorldMatrix;
				Stuff::Point3D childPosP;
				childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
				lightToWorldMatrix.BuildTranslation(childPosP);
				lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f, 0.0f, 0.0f));
				spotlightLights_[k]->SetLightToWorld(&lightToWorldMatrix);
				// T1.13: dropped (sensorLevel > 4) gate. The original gate
				// was inherited verbatim from the anubis-specific pointLight
				// branch above (~:3358) where "show enemy searchlight only when
				// player has sensor lock" is the correct semantic. For the
				// generalized SpotLight_ vector path this is overly
				// restrictive: player-owned mechs (sensorLevel=5) work, but
				// any enemy mech at sensorLevel<5 is stuck active=false. The
				// generalized path should illuminate for any visible mech at
				// night regardless of sensor state.
				spotlightLights_[k]->active =
					(eye->isNight && visible && !InEditor);
			}
		}

		// D-gpu-pose-instrument: AnimPose stage — animation pose state setters
		// (setAnimation, SetFrameNum, SetNodeRotation × N) on body + shadow
		// shapes, plus the shadow-shape transform conditional. Per-stage
		// attribution for the trimodal Mech3D.UpdateGeometry histogram.
		{ ZoneScopedN("Mech3D.UpdateGeometry.AnimPose");
		//----------------------------------------------------
		// Set Animation State Here.
		// ONLY ONE case now:
		//		-We are in an art generated state,  Just playback animation
		// All cases covered now.
		if (status != OBJECT_STATUS_DESTROYED)
		{
			mechType->setAnimation(mechShape,currentGestureId);
			mechShape->SetFrameNum(currentFrame);

			if (mechShadowShape && !stripShadowState)
			{
				mechType->setAnimation(mechShadowShape,currentGestureId);
				mechShadowShape->SetFrameNum(currentFrame);
			}
		}
	
		Stuff::UnitQuaternion torsoRot;
		torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
		if (rotationalNodeIndex == -1)
	   		rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

		mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
 	
		if (mechShadowShape)
			mechShape->SetUseShadow(false);
			
		if (mechShadowShape && useShadows && !stripShadowState)
		{
			if (rotationalNodeIndex == -1)
	   			rotationalNodeIndex = mechShadowShape->SetNodeRotation("joint_torso",&torsoRot);

			mechShadowShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

 			mechShadowShape->SetNodeRotation("joint_torso",&torsoRot);
			mechShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			// Slice D-shadow-skip: when GPU mech path is on AND skip
			// killswitch is on AND tessellation is active (modern shadow
			// path engaged), omit the call entirely. Mech3DAppearance::
			// renderShadows early-returns on tessellation (mech3d.cpp:3054),
			// so no consumer of TransformMultiShape's outputs exists in
			// this configuration. Modern dynamic shadows use g_shadowShapes[]
			// (txmmgr.cpp:130, 1589-1620), a separate data path that does
			// NOT consume mechShadowShape state. See D-shadow-skip spec
			// §Recon for full grep-verified consumer enumeration including
			// opposite-direction grep on addShadowShape.
			//
			// Tessellation gate is belt-and-suspenders: if a user disables
			// tessellation, the legacy RenderShadows path becomes reachable
			// and would need TransformMultiShape outputs.
			//
			// Slice C3-shadow (FAST_TRANSFORM): when SKIP is off but
			// FAST_TRANSFORM is on, use _PositionsOnly to skip the per-
			// vertex CPU lighting kernel only. RenderShadows (tgl.cpp:3577)
			// hardcodes gVertex.argb to 0x3f000000 and reads
			// listOfShadowTVertices populated by MultiTransformShadows
			// (which still dispatches at msl.cpp:1765 in that branch).
			// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: former MC2_GPU_MECH_SHADOW_SKIP retired.
			if (g_useGpuMechs && gos_IsTerrainTessellationActive()) {
				// Skip — modern engine has no consumer.
			} else if (g_useGpuMechs && g_useGpuMechShadowFastTransform) {
				mechShadowShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
			} else {
				mechShadowShape->TransformMultiShape(&xlatPosition, &qRotation);
			}
		}
		} // end AnimPose zone

		// D-gpu-pose-instrument: BodyXform stage — body's SetLightList +
		// TransformMultiShape* dispatch. The dominant per-mech work; will be
		// further broken down by zones inside TransformMultiShape itself.
		{ ZoneScopedN("Mech3D.UpdateGeometry.BodyXform");
		mechShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice C3-revised: when GPU mech path is on AND fast-transform
		// killswitch is on, use _PositionsOnly to skip the per-vertex
		// CPU lighting kernel. Output of that kernel (listOfVertices[j].argb)
		// is only consumed by mechShape->Render(true) which Slice A
		// bypasses; GPU shader does its own lighting via calc_light().
		// BODY ONLY — arms (4459, 4543) and shadow (3377) explicitly
		// stay full TransformMultiShape; their Render(true) callers
		// still depend on the lighting bake.
		// Slice D-leaf-skip-v2: when GPU mech path is on AND leaf-skip
		// killswitch is on AND tessellation is active, use HierarchyOnly
		// to additionally skip per-leaf dispatch + MultiTransformShadows.
		// Recon proved zero practical consumer in this configuration
		// (mechShape->Render(true) bypassed by Slice A; RenderShadows
		// unreachable on tessellation; findMoverByMouse rect-only — see
		// killswitch comment in gos_mech_killswitch.h).
		// PREVIEW-FIX: the SimpleCamera UI preview (Mech Bay / Purchase) draws
		// via the CPU MLR mechShape->Render(true) path, which consumes the full
		// TransformMultiShape output (listOfVertices positions + argb lighting).
		// The GPU fast-transform variants (_PositionsOnly / _HierarchyOnly) leave
		// listOfVertices stale, so TG_Shape::Render early-returns and the preview
		// goes blank. Force the full transform whenever a preview render is in
		// flight. World/tactical mechs (depth == 0) keep the GPU fast paths.
		if (g_mechPreviewRenderDepth > 0) {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		} else if (g_useGpuMechs && g_useGpuMechLeafSkip && gos_IsTerrainTessellationActive()) {
			mechShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &qRotation);
		} else if (g_useGpuMechs && g_useGpuMechFastTransform) {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape(&xlatPosition, &qRotation);
		}
		} // end BodyXform zone
	}

	// D-gpu-pose-instrument: Effects stage — foot poofs, jump fx, weapon
	// node positions. getNodePosition callers live here; potential cost
	// contributor.
	if (visible && (sensorLevel > 4) && !InEditor && useNonWeaponEffects)
	{
		ZoneScopedN("Mech3D.UpdateGeometry.Effects");
		//--------------------------------------------------------------
		// Having already transformed the mech, the foot poofs go here.
		Stuff::Vector3D rFootPos, lFootPos;
		long footId1 = mechType->getTotalNodes() - 2;
		long footId0 = mechType->getTotalNodes() - 1;
		
		rFootPos = getNodePosition(footId1);
		lFootPos = getNodePosition(footId0);
	
		if (!rightFootDone0 &&
			(currentFrame >= (mechType->gestures[currentGestureId].rightFootDownFrame0-FOOTPRINT_SLOP)) &&
			(currentFrame <= (mechType->gestures[currentGestureId].rightFootDownFrame0+FOOTPRINT_SLOP)))
		{
			//Foot is on ground.  Poof and footprint.
			rightFootDone0 = true;
	
			if (rightDustPoofEffect[currentRightPoof] && (currentGestureId != 13))
			{
				Stuff::Point3D			actualPosition;
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				
				rFootPosition[currentRightPoof].x = -rFootPos.x;
				rFootPosition[currentRightPoof].y = rFootPos.z;
				rFootPosition[currentRightPoof].z = rFootPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(rFootPosition[currentRightPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
				rightDustPoofEffect[currentRightPoof]->Start(&info);
				rightFootPoofDraw[currentRightPoof] = true;
				
				currentRightPoof++;
				if (currentRightPoof >= MAX_DUST_POOFS)
					currentRightPoof = 0;
			}
			
			//Draw footprint here.
			if (craterManager && !isWaking)
				craterManager->addCrater(mechType->rightFootprintType,rFootPos,rotation);
		}
		
		if (!rightFootDone1 && 
			(currentFrame >= (mechType->gestures[currentGestureId].rightFootDownFrame1-FOOTPRINT_SLOP)) &&
			(currentFrame <= (mechType->gestures[currentGestureId].rightFootDownFrame1+FOOTPRINT_SLOP)))
		{
			//Foot is on ground.  Poof and footprint.
			rightFootDone1 = true;
	
			if (rightDustPoofEffect[currentRightPoof] && (currentGestureId != 13))
			{
				Stuff::Point3D			actualPosition;
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				
				rFootPosition[currentRightPoof].x = -rFootPos.x;
				rFootPosition[currentRightPoof].y = rFootPos.z;
				rFootPosition[currentRightPoof].z = rFootPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(rFootPosition[currentRightPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
				rightDustPoofEffect[currentRightPoof]->Start(&info);
				rightFootPoofDraw[currentRightPoof] = true;
				
				currentRightPoof++;
				if (currentRightPoof >= MAX_DUST_POOFS)
					currentRightPoof = 0;
			}
			
			//Draw footprint here.
			if (craterManager && !isWaking)
				craterManager->addCrater(mechType->rightFootprintType,rFootPos,rotation);
		}
		
		if (rightFootDone0 &&
			(currentFrame < (mechType->gestures[currentGestureId].rightFootDownFrame0-FOOTPRINT_SLOP)) ||
		INT32	(currentFrame > (mechType->gestures[currentGestureId].rightFootDownFrame0+FOOTPRINT_SLOP)))
		{
			rightFootDone0 = false;
		}
		
		if (rightFootDone1 &&
			(currentFrame < (mechType->gestures[currentGestureId].rightFootDownFrame1-FOOTPRINT_SLOP)) ||
			(currentFrame > (mechType->gestures[currentGestureId].rightFootDownFrame1+FOOTPRINT_SLOP)))
		{
			rightFootDone1 = false;
		}
		
		if (!leftFootDone0 && 
			(currentFrame >= (mechType->gestures[currentGestureId].leftFootDownFrame0-FOOTPRINT_SLOP)) &&
			(currentFrame <= (mechType->gestures[currentGestureId].leftFootDownFrame0+FOOTPRINT_SLOP)))
		{
			//Foot is on ground.  Poof and footprint.
			leftFootDone0 = true;
	
			if (leftDustPoofEffect[currentLeftPoof] && (currentGestureId != 13))
			{
				Stuff::Point3D			actualPosition;
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				
				lFootPosition[currentLeftPoof].x = -lFootPos.x;
				lFootPosition[currentLeftPoof].y = lFootPos.z;
				lFootPosition[currentLeftPoof].z = lFootPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(lFootPosition[currentLeftPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
				leftDustPoofEffect[currentLeftPoof]->Start(&info);
				leftFootPoofDraw[currentLeftPoof] = true;
				
				currentLeftPoof++;
				if (currentLeftPoof >= MAX_DUST_POOFS)
					currentLeftPoof = 0;
			}
			
			//Draw footprint here.
			if (craterManager && !isWaking)
				craterManager->addCrater(mechType->leftFootprintType,lFootPos,rotation);
		}
		
		if (!leftFootDone1 && 
			(currentFrame >= (mechType->gestures[currentGestureId].leftFootDownFrame1-FOOTPRINT_SLOP)) &&
			(currentFrame <= (mechType->gestures[currentGestureId].leftFootDownFrame1+FOOTPRINT_SLOP)))
		{
			//Foot is on ground.  Poof and footprint.
			leftFootDone1 = true;
	
			if (leftDustPoofEffect[currentLeftPoof] && (currentGestureId != 13))
			{
				Stuff::Point3D			actualPosition;
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				
				lFootPosition[currentLeftPoof].x = -lFootPos.x;
				lFootPosition[currentLeftPoof].y = lFootPos.z;
				lFootPosition[currentLeftPoof].z = lFootPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(lFootPosition[currentLeftPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
				leftDustPoofEffect[currentLeftPoof]->Start(&info);
				leftFootPoofDraw[currentLeftPoof] = true;
				
				currentLeftPoof++;
				if (currentLeftPoof >= MAX_DUST_POOFS)
					currentLeftPoof = 0;
			}
			
			//Draw footprint here.
			if (craterManager && !isWaking)
				craterManager->addCrater(mechType->leftFootprintType,lFootPos,rotation);
		}
		
		if (leftFootDone0 &&
			(currentFrame < (mechType->gestures[currentGestureId].leftFootDownFrame0-FOOTPRINT_SLOP)) ||
			(currentFrame > (mechType->gestures[currentGestureId].leftFootDownFrame0+FOOTPRINT_SLOP)))
		{
			leftFootDone0 = false;
		}
		
		if (leftFootDone1 &&
			(currentFrame < (mechType->gestures[currentGestureId].leftFootDownFrame1-FOOTPRINT_SLOP)) ||
			(currentFrame > (mechType->gestures[currentGestureId].leftFootDownFrame1+FOOTPRINT_SLOP)))
		{
			leftFootDone1 = false;
		}
	}
	
	// D-gpu-pose-instrument: Sensors stage — sensorTriangleShape +
	// sensorSquareShape transforms. Per user: ALL mechs have sensors;
	// need attribution.
	//
	// MECH-KILLSWITCH-SENSORSKIP-RETIRE-1: on the GPU mech path, skip the entire
	// sensor block when sensorLevel ∈ {0, 5}. Sensor SHAPES only render when
	// sensorLevel ∈ [1,4] (mech3d.cpp:2948-2960); for player mechs (sensorLevel=5)
	// and undetected enemies (sensorLevel=0) the transform work has no consumer.
	// Skip gate is the exact INVERSE of the Render gate — strict no-op. sensorSpin
	// drift while skipped is imperceptible (a continuously-rotating marker that pops
	// in at any angle is indistinguishable). Formerly gated by the default-ON
	// MC2_GPU_MECH_SENSOR_SKIP killswitch (now retired to this constant); still
	// requires g_useGpuMechs (the CPU path transforms sensors regardless).
	{ ZoneScopedN("Mech3D.UpdateGeometry.Sensors");
	const bool skipSensors = g_useGpuMechs &&
		(sensorLevel == 0 || sensorLevel >= 5);
	if (!skipSensors) {
	Stuff::UnitQuaternion totalRotation;
	sensorSpin += SPIN_RATE * frameLength;
	if (sensorSpin > 180)
		sensorSpin -= 360;

	if (sensorSpin < -180)
		sensorSpin += 360;

	totalRotation = Stuff::EulerAngles(0.0f,sensorSpin * DEGREES_TO_RADS,0.0f);

	float baseRootNodeDifference = baseRootNodeHeight;
	if (isHelicopter)
		baseRootNodeDifference -= HELICOPTER_FACTOR;

	xlatPosition.y += baseRootNodeDifference;
	//----------------------------------------------
	// Do geometry here to draw sensor contact
	sensorTriangleShape->SetFogRGB(0xffffffff);
	sensorTriangleShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorTriangleShape->TransformMultiShape(&xlatPosition,&totalRotation);

	//----------------------------------------------
	// Do geometry here to draw sensor contact
	sensorSquareShape->SetFogRGB(0xffffffff);
	sensorSquareShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorSquareShape->TransformMultiShape(&xlatPosition,&totalRotation);
	} // end !skipSensors
	} // end Sensors zone
	
	//-----------------------------------------
	// Create Jump FX Here.
	if (!jumpFXSetup && (currentGestureId == GestureJump) && inJump && jumpJetEffect)
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		Stuff::Vector3D jumpNodePos = getNodePosition(jumpNodeId);
		
		Stuff::Point3D			actualPosition;
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		actualPosition.x = -jumpNodePos.x;
		actualPosition.y = jumpNodePos.z;
		actualPosition.z = jumpNodePos.y;
		
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(actualPosition);
		
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
					
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		jumpJetEffect->Start(&info);
		
		jumpFXSetup = true;
	}
	
	//------------------------------------------------
	// Update GOSFX
	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		if (rightFootPoofDraw[i] && rightDustPoofEffect[i] && rightDustPoofEffect[i]->IsExecuted())
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
				
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(rFootPosition[i]);
	
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = rightDustPoofEffect[i]->Execute(&info);
			if (!result)
			{
				rightFootPoofDraw[i] = false;
			}
		}
		
		if (leftFootPoofDraw[i] && leftDustPoofEffect[i] && leftDustPoofEffect[i]->IsExecuted())
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
				
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(lFootPosition[i]);
	
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = leftDustPoofEffect[i]->Execute(&info);
			if (!result)
			{
				leftFootPoofDraw[i] = false;
			}
		}
	}
	
	if ((currentGestureId == GestureJump) && inJump && jumpJetEffect && jumpJetEffect->IsExecuted())
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		Stuff::Vector3D jumpNodePos = getNodePosition(jumpNodeId);
		
		Stuff::Point3D			actualPosition;
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		actualPosition.x = -jumpNodePos.x;
		actualPosition.y = jumpNodePos.z;
		actualPosition.z = jumpNodePos.y;
		
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(actualPosition);
		
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
					
 		Stuff::OBB boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		jumpJetEffect->Execute(&info);
	}
	
	if (!inJump && jumpFXSetup)
	{
		jumpJetEffect->Stop();
		jumpFXSetup = false;
	}
	
	//----------------------------------------------------------------
	// Arm Blown FX
	if (leftShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		if (leftArmNodeIndex == -1)
			leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

		Stuff::Vector3D leftNodePos = getNodeIdPosition(leftArmNodeIndex);
		leftShoulderPos.x = -leftNodePos.x;
		leftShoulderPos.y = leftNodePos.z;
		leftShoulderPos.z = leftNodePos.y;
		
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(leftShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		bool result = leftShoulderBoom->Execute(&info);
		if (!result)
		{
			leftShoulderBoom->Kill();
			delete leftShoulderBoom;
			leftShoulderBoom = NULL;
		}
	}
	
	if (rightShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		if (rightArmNodeIndex == -1)
			rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

		Stuff::Vector3D rightNodePos = getNodeIdPosition(rightArmNodeIndex);
		rightShoulderPos.x = -rightNodePos.x;
		rightShoulderPos.y = rightNodePos.z;
		rightShoulderPos.z = rightNodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(rightShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		bool result = rightShoulderBoom->Execute(&info);
		if (!result)
		{
			rightShoulderBoom->Kill();
			delete rightShoulderBoom;
			rightShoulderBoom = NULL;
		}
	}
	
	//-------------------------------------------------
	// All other effects
	if ((criticalSmoke && (isSmoking > 0)) || (smokeEffect && (isSmoking == 0)))
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
				
		Stuff::Vector3D smokeNodePos = getNodePosition(0);
		Stuff::Point3D smokePos;
		smokePos.x = -smokeNodePos.x;
		smokePos.y = smokeNodePos.z;
		smokePos.z = smokeNodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(smokePos);
				
		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
		if (isSmoking > 0)
		{
			//Loops until told to stop.
			criticalSmoke->Execute(&info);
		}
		else
		{
			smokeEffect->Execute(&info);
		}
	}
	
	if (waterWake && isWaking)
	{
		if (movedThisFrame)
		{
			waterWake->SetLoopOn();
			waterWake->SetExecuteOn();
		}
		else
		{
			waterWake->SetLoopOff();
			waterWake->SetExecuteOn();
		}
		
 		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		if (!inReverse)
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		else
			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		
		waterWake->Execute(&info);
	}
	
	if (helicopterDustCloud && isDusting)
	{
 		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = position.z;
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		/*
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		*/
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
		bool result = helicopterDustCloud->Execute(&info);
		if (!result)
		{
			helicopterDustCloud->Kill();
			isDusting = false;
		}
	}
	 
}	

#ifdef _DEBUG
FilePtr logFile = NULL;
#endif

//-----------------------------------------------------------------------------
bool Mech3DAppearance::setJumpParameters (Stuff::Vector3D &end)
{
	if (!inJump)
	{
		jumpSetup = true;
		jumpDestination = end;

		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
bool Mech3DAppearance::leftArmRecalc (void)
{
	if (!leftArm)
		return false;

	leftArmInView = false;
	float eyeDistance = 0.0f;
	//--------------------------------------------------
	// First, if we are using perspective, figure out
	// if object too far from camera.  Far Clip Plane.
	if (eye->usePerspective)
	{
		Stuff::Point3D Distance;
		Stuff::Point3D objPosition;
		Stuff::Point3D eyePosition(eye->getCameraOrigin());
		objPosition.x = -leftArmPos.x;
		objPosition.y = leftArmPos.z;
		objPosition.z = leftArmPos.y;

		Distance.Subtract(objPosition,eyePosition);
		eyeDistance = Distance.GetApproximateLength();
		if (eyeDistance > Camera::MaxClipDistance)
		{
			leftArmHazeFactor = 1.0f;
			leftArmInView = false;
		}
		else if (eyeDistance > Camera::MinHazeDistance)
		{
			leftArmHazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
			leftArmInView = true;
		}
		else
		{
			leftArmHazeFactor = 0.0f;
			leftArmInView = true;
		}

		//-----------------------------------------------------------------
		// If inside farClip plane, check if behind camera.
		// Find angle between lookVector of Camera and vector from camPos
		// to Target.  If angle is less then halfFOV, object is visible.
		const bool armParentVisL = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if (armParentVisL)
		{
			Distance.Normalize(Distance);

			float cosine = Distance * eye->getLookVector();
			if (cosine > eye->cosHalfFOV)
				leftArmInView = true;
			else
				leftArmInView = false;
		}
	}

	return leftArmInView;
}

//-----------------------------------------------------------------------------
bool Mech3DAppearance::rightArmRecalc (void)
{
	if (!rightArm)
		return false;

	rightArmInView = false;
	float eyeDistance = 0.0f;
	//--------------------------------------------------
	// First, if we are using perspective, figure out
	// if object too far from camera.  Far Clip Plane.
	if (eye->usePerspective)
	{
		Stuff::Point3D Distance;
		Stuff::Point3D objPosition;
		Stuff::Point3D eyePosition(eye->getCameraOrigin());
		objPosition.x = -rightArmPos.x;
		objPosition.y = rightArmPos.z;
		objPosition.z = rightArmPos.y;

		Distance.Subtract(objPosition,eyePosition);
		eyeDistance = Distance.GetApproximateLength();
		if (eyeDistance > Camera::MaxClipDistance)
		{
			rightArmHazeFactor = 1.0f;
			rightArmInView = false;
		}
		else if (eyeDistance > Camera::MinHazeDistance)
		{
			rightArmHazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
			rightArmInView = true;
		}
		else
		{
			rightArmHazeFactor = 0.0f;
			rightArmInView = true;
		}

		//-----------------------------------------------------------------
		// If inside farClip plane, check if behind camera.
		// Find angle between lookVector of Camera and vector from camPos
		// to Target.  If angle is less then halfFOV, object is visible.
		const bool armParentVisR = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if (armParentVisR)
		{
			Distance.Normalize(Distance);

			float cosine = Distance * eye->getLookVector();
			if (cosine > eye->cosHalfFOV)
				rightArmInView = true;
			else
				rightArmInView = false;
		}
	}

	return rightArmInView;
}

//-----------------------------------------------------------------------------
long Mech3DAppearance::update (bool animate) 
{

#ifdef _DEBUG
	if (!logFile)
	{
		logFile = new File;
		long result = logFile->create("gesture.log");
		if (result != NO_ERR)
		{
			delete logFile;
			logFile = NULL;
		}
	}
#endif

	// DEGRADE-DON'T-CRASH: partial import with no base shape -> nothing to
	// animate; the many mechShape-> derefs below are unsafe. Return as if the
	// frame advanced successfully so callers (encyclopedia preview, mech bay)
	// keep running.
	if (!mechShape)
		return TRUE;

	//----------------------------------------
	// Recycle the weapon Nodes
	if (nodeRecycle)
	{
		for (long i=0;i<mechType->numWeaponNodes;i++)
		{
			if (nodeRecycle[i] > 0.0f)
			{
				nodeRecycle[i] -= frameLength;
				if (nodeRecycle[i] < 0.0f)
					nodeRecycle[i] = 0.0f;
			}
		}
	}

	oncePerFrame = false;
	bool setFirstFrame = false;

	if ((currentGestureId == GestureHitFront) ||
		(currentGestureId == GestureHitBack) || 
		(currentGestureId == GestureHitLeft) || 
		(currentGestureId == GestureHitRight))
	{
		if (atTransitionToNextGesture)
		{
			currentGestureId = hitGestureId;
			hitGestureId = -1;
			setFirstFrame = true;
			long firstFrame = mechType->gestures[currentGestureId].frameStart;

			if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
				firstFrame = mechType->getNumFrames(currentGestureId)-1;

			inReverse = false;
			if (mechType->getFrameRate(currentGestureId) < 0.0)
				inReverse = true;

			atTransitionToNextGesture = false;
			
			currentFrame = firstFrame;
		}
	}
	
	//----------------------------------------------
	//If mech is in 2 and does NOT have a target.
	//Fix please
	if (!inCombatMode && (currentGestureId == 2))
	{
		idleTime += frameLength;
		if (idleTime > idleMAX)
		{
			currentGestureId = 13;
		}
	}
	else
	{
		idleTime = 0.0f;
	}
	
	if ((currentGestureId == 13) && atTransitionToNextGesture)
	{
		currentGestureId = 2;
		
		currentFrame = 0;
	}
	
	//--------------------------------------------------------------
	// Check to see if we need to stop NOW!!!!!!!!!
	if (checkStop() && !inDebugMoveMode && !jumpSetup && (getVelocityMagnitude() != 0.0f) && (currentGestureId != GestureJump))
	{
		//--------------------------------
		// We are at the goal.  Store off
		// our current gesture state.
		currentGestureId = 2;		//Force us to the stand gesture
		oldStateGoal = 1;			//We always came FROM STAND
		currentStateGoal = -1;		//Not going anywhere
		transitionState = 0;		//No Transition necessary.  So be ready at beginning if we need one!
		currentFrame = 0;			//In the "how did this ever work file"  Duh, need to reset the frame!
	}
	
	long newGestureId = -1;			//Assume we do not need to transition.
	
	//--------------------------------------------------------------
	// Check if currentStateGoal is -1.
	// If it is not, we need to move to a new gesture to get to our goal.
	// Check if we can change gestures.  If yes, go through change over.
	// Combines three separate old chunks o code.
	if (currentStateGoal != -1)
	{
		//--------------------------------------------
		// Find out how we get to Goal.  If result is
		// -1, we are already at goal.
		long arrayIdx = (oldStateGoal * GESTURE_OFFSET_SIZE) + (currentStateGoal *  MAX_TRANSITION_GESTURES) + transitionState;
		newGestureId = transitionArray[arrayIdx];

		if (atTransitionToNextGesture)	//This is only set when we are ready for the next gesture if these is one!
		{
			transitionState++;		//Ready to go to next state.  Increment the transition counter for next frame!

			long firstFrame = currentFrame;
			if (newGestureId == -1)		//We have reached our goal.
			{
				//--------------------------------
				// We are at the goal.  Store off
				// our current gesture state.
				oldStateGoal = currentStateGoal;
				if (oldStateGoal == MECH_STATE_JUMPING)
					oldStateGoal = 1;		//We are always standing AFTER a jump or a fall!
				
				currentStateGoal = -1;
				transitionState = 0;
				atTransitionToNextGesture = false;
				
				if (mechType->getFrameRate(currentGestureId) < 0.0)
					inReverse = true;
			}
			else
			//----------------------------------------------------
			// reset all frames to zero for this gesture.
			// NOTE:  This may be a two way gesture.  Most will be
			// set to zero, but some may need to be set to last.
			//
			{
				//----------------------------------------------
				// Now actually reset everything and setup flags
				// to facilitate the playing of the new gesture
				currentGestureId = newGestureId;
				setFirstFrame = true;
				firstFrame = mechType->gestures[currentGestureId].frameStart;
				if (currentGestureId == GestureJump)
					firstFrame = 0;			//We store information in the firstFrame of the jump.  They always start at 0!

				if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
					firstFrame = mechType->getNumFrames(currentGestureId)-1;

				inReverse = false;
				if (mechType->getFrameRate(currentGestureId) < 0.0)
					inReverse = true;

				atTransitionToNextGesture = false;
			}

			currentFrame = firstFrame;
		}
	}
		
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
	{
		//----------------------------------------------------
		// This will bypass animation if we go the old way.
		// Just set stateGoal to 7 or 8 if its not.
		if (currentStateGoal < 7)
		{
			if (RollDice(50))
			{
				setGestureGoal(7);
			}
			else
			{
				setGestureGoal(8);
			}
		}
	}

	//------------------------------------------------------------
	mechFrameRate = mechType->getFrameRate(currentGestureId);

	if (mechFrameRate < 0.0)
		mechFrameRate = -mechFrameRate;

	//---------------------
	// Do jump magic here.
	if ((currentGestureId == GestureJump) && !inJump)
	{
		inJump = true;
		jumpSetup = false;
		
		//-----------------------------------------------------
		// Distance is straight line distance.  This is only
		// the case if the mech is FACING the correct way.  If
		// not, we must increase distance based on facing.
		float takeoffFrame = mechType->getJumpTakeoffFrame();
		float landingFrame = mechType->getJumpLandFrame();

		//-----------------------------------------
		// Get mech flying toward targetPosition.
		Stuff::Vector3D ownerPosition = position;
		Stuff::Vector3D targetPosition = jumpDestination;
		jumpVelocity.Subtract(targetPosition,ownerPosition);
					
		float velMag = jumpVelocity.GetLength() / ((landingFrame - takeoffFrame) / 30.0f);
		jumpVelocity.Normalize(jumpVelocity);
		jumpVelocity *= velMag;
		
		//---------------------------------------------
		// Jump Velocity is now a velocity vector which 
		// will move the mech WITHOUT turning to the landing point.
	}
	
	if (currentGestureId == GestureJump && inJump)
	{
		if (currentFrame >= mechType->getJumpTakeoffFrame())
		{
			jumpAirborne = true;
		}
		
		if ((currentFrame >= mechType->getJumpLandFrame()) && jumpAirborne)
		{
			jumpAirborne = false;
			jumpVelocity.Zero();
			
			if (jumpJetEffect)
			{
				jumpJetEffect->Stop();	//Done drawing it.
			}
		}
	}
 	
	//--------------------------------------------------------
	// Force frames to last frame of FALL if fallen
	fallen = FALSE;
	if ((currentGestureId == GestureFallenForward) || (currentGestureId == GestureFallenBackward))
	{
		fallen = TRUE;
		if (currentStateGoal != -1)
			atTransitionToNextGesture = TRUE;
			
		//Try playing the helicopter dust cloud for grins to see how it looks!
		// Only play once!!!  Until they get back up.
		if (!fallDust)
		{
			playEjection();
			fallDust = true;
		}
	}
	else if (currentGestureId == GesturePark)
	{
		fallDust = false;
		if (currentStateGoal != -1)
			atTransitionToNextGesture = TRUE;
	}
	else	//Normal Frame Increment
	{
		fallDust = false;
		if (animate)
		{
			float frameInc = 0.0f;
			if (singleStepMode && nextStep)
			{
				frameInc = 1.0f;
			}
			else if (singleStepMode && prevStep)
			{
				frameInc = -1.0f;
			}
			else if (!singleStepMode)
			{
				//--------------------------------------------------------
				// Make sure animation runs no faster than mechFrameRate fps.
				frameInc = mechFrameRate * frameLength;
			}
			
			//------------------------------------------
			// Always ASSUME we cannot transition yet.
			// Saves myriad compares below.
			atTransitionToNextGesture = FALSE;
					
			//---------------------------------------
			// Increment Frames -- Everything else!
			if ((frameInc != 0.0f) || (singleStepMode))
			{
				if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
				{
					// ANIM-CADENCE-FIX (gate MC2_ANIM_CADENCE_FIX): advance the gait at MOST
					// once per render frame. Mover::getLOSPosition() (mover.cpp:3528) calls a
					// second appearance->update() in combat to refresh weapon-node geometry,
					// which otherwise ticks the animation a 2nd time the same frame => visible
					// double-step (proven: 19390 advanced_twice events, gestures 2/4/7 == the
					// getLOSPosition gesture filter). If we already advanced this
					// g_mc2FrameCounter, skip the increment; downstream geometry still
					// recomputes at the current frame, so the LOS refresh gets valid nodes
					// without an extra gait step.
					// DEFAULT-ON killswitch (user-confirmed fix, mc2_17 Catapult/Bushwacker):
					// disable with MC2_ANIM_CADENCE_FIX=0 to A/B the double-step.
					static const bool s_animCadenceFix = []{
						const char* _v = std::getenv("MC2_ANIM_CADENCE_FIX");
						return !(_v && _v[0] == '0' && _v[1] == '\0');   // default-ON
					}();
					const bool _alreadyAdvanced = (s_animCadenceFix &&
						lastAnimAdvanceFrame == (uint32_t)g_mc2FrameCounter);
					if (!_alreadyAdvanced)
					{
						if (inReverse)
							currentFrame -= frameInc;
						else
							currentFrame += frameInc;
						lastAnimAdvanceFrame = (uint32_t)g_mc2FrameCounter;
					}
				}

				// ANIM-MECH-DOUBLE-STEP-RECON-1: emit one event per gait advance.
				// Two events with the SAME ptr AND SAME frame == gait double-advanced
				// this frame (the "double step"). Normal == exactly one per mech/frame;
				// if single but cadence looks wrong, compare mechFrameRate vs ground
				// speed (run-in-place = mover velocity, not this advance). Gated
				// MC2_ANIM_ADVANCE_TRACE + MC2_DIAG_TAGS=ANIM_ADVANCE; read via
				// get_diagnostic_events("ANIM_ADVANCE"). No behavior change.
				if (frameInc != 0.0f)
				{
					static const bool s_animAdvTrace = (std::getenv("MC2_ANIM_ADVANCE_TRACE") != nullptr);
					if (s_animAdvTrace && mc2_diag::tagEnabled("ANIM_ADVANCE"))
					{
						char _aa_buf[256];
						snprintf(_aa_buf, sizeof(_aa_buf),
						         "{\"ptr\":\"%p\",\"gesture\":%d,\"mechFrameRate\":%.3f,\"frameLength\":%.5f,\"frameInc\":%.4f,\"currentFrame\":%.3f}",
						         (void*)this, (int)currentGestureId, mechFrameRate, frameLength, frameInc, currentFrame);
						mc2_diag::writeEvent("ANIM_ADVANCE", 1, (uint64_t)g_mc2FrameCounter, _aa_buf);
					}

					// ANIM-CADENCE-GUARD-1 (non-fatal, recon — FRAME-CURRENTNESS-GUARDS-1).
					// Make the "exactly one gait advance per mech appearance per frame"
					// invariant observable. Recon (ANIM-MECH-DOUBLE-STEP) proved a single
					// advance site today, but guard it so a future second advance path
					// (worker prepass / touch-split) can't silently reintroduce the
					// double-step. Per-appearance last-advance frame stamp via static map
					// (no class-layout change → no full relink); a repeat advance in the
					// same g_mc2FrameCounter == double advance. Map touched only when
					// MC2_ANIM_CADENCE_GUARD set; events under MC2_DIAG_TAGS=ANIM_CADENCE.
					{
						static const bool s_animCadGuard = (std::getenv("MC2_ANIM_CADENCE_GUARD") != nullptr);
						if (s_animCadGuard)
						{
							static std::unordered_map<void*, uint32_t> s_lastAdvanceFrame;
							static uint64_t s_ac_advances = 0, s_ac_doubles = 0;
							++s_ac_advances;
							const uint32_t _ac_frame = (uint32_t)g_mc2FrameCounter;
							auto _ac_it = s_lastAdvanceFrame.find((void*)this);
							const bool _ac_double = (_ac_it != s_lastAdvanceFrame.end() && _ac_it->second == _ac_frame);
							s_lastAdvanceFrame[(void*)this] = _ac_frame;
							if (_ac_double)
							{
								++s_ac_doubles;
								if (mc2_diag::tagEnabled("ANIM_CADENCE"))
								{
									char _ac_buf[256];
									snprintf(_ac_buf, sizeof(_ac_buf),
									         "{\"event\":\"advanced_twice_same_frame\",\"ptr\":\"%p\",\"gesture\":%d,\"frame\":%u,\"frameInc\":%.4f,\"doubles\":%llu,\"advances\":%llu}",
									         (void*)this, (int)currentGestureId, _ac_frame, frameInc,
									         (unsigned long long)s_ac_doubles, (unsigned long long)s_ac_advances);
									mc2_diag::writeEvent("ANIM_CADENCE", 1, (uint64_t)_ac_frame, _ac_buf);
								}
							}
						}
					}
				}

				//--------------------------------------
				//Check Positive overflow of gesture
				if (currentFrame >= mechType->getNumFrames(currentGestureId))
				{
					if ((currentStateGoal == -1) && (hitGestureId == -1) && (currentGestureId != GestureIdle))		//Are we just Looping or transitioning?
					{
						currentFrame -= mechType->getNumFrames(currentGestureId);
					}
					else
					{
						//-------------------------------------------
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						atTransitionToNextGesture = TRUE;
						currentFrame = mechType->getNumFrames(currentGestureId) - 1.0f;
						
						if (currentGestureId == GestureJump)
						{
							inJump = false;
							jumpSetup = false;
							oldStateGoal = 1;		//We always started a Jump standing!
						}
					}
				}
	
				//--------------------------------------
				//Check negative overflow of gesture
				if (currentFrame < 0)
				{
					if ((currentStateGoal == -1) && (hitGestureId == -1))
					{
						currentFrame += mechType->getNumFrames(currentGestureId);
					}
					else
					{
						//-------------------------------------------
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						atTransitionToNextGesture = TRUE;
						currentFrame = 0.0f;
					}
				}
			}
			else		//This is a  single frame gesture.  We are ALWAYS ready to transition in this case!
			{
				if (currentStateGoal != -1)
					atTransitionToNextGesture = TRUE;
			}
		}
	}

	if (currentFrame < 0.0f)
		currentFrame = 0.0f;
//		STOP(("CurrentFrame of animation can never be less then zero.  Frame %f,  NumFrames %f, GestureId %d",currentFrame,mechType->getNumFrames(currentGestureId),currentGestureId));
		
	if ((currentFrame > 0.0f) && (currentFrame >= mechType->getNumFrames(currentGestureId)))
		currentFrame = 0.0f;
//		STOP(("CurrentFrame is greater then numFrames.  Frame %f,  NumFrames %f, GestureId %d",currentFrame,mechType->getNumFrames(currentGestureId),currentGestureId)); 
		
#ifdef _DEBUG
	if (logFile && (inDebugMoveMode))
	{
		char msg[1024];
		sprintf(msg,"Gesture: %d, Frame: %f, SGoal: %d, OGoal: %d, Rot: %f",currentGestureId,currentFrame,currentStateGoal,oldStateGoal,rotation);
		logFile->writeLine(msg);
	}
#endif

	// Under the GPU static-prop killswitch always run updateGeometry so
	// mechShape->TransformMultiShape runs every frame, even when the
	// broken cull (inView) thinks the mech is off-screen. Without this,
	// TG_Shape::Render silently returns on stale listOfVertices and the
	// mech geometry disappears (health bar still drawn via screenPos).
	// C3: route inView input to GPU-lagged visibility when killswitch is enabled.
	// PRESERVE cascade gate structure: turn<3 and GestureJump conditions are unchanged.
	{
		const bool gpuVis = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if ((turn < 3) || gpuVis || (currentGestureId == GestureJump) || g_useGpuStaticProps)
			updateGeometry();

		// Slice B1: refresh per-actor LightsData UBO slot index for the
		// GPU mech batcher path. Mirrors bdactor.cpp:2314 pattern. No-op
		// when killswitch is off; cheap (one hash lookup + dedup-cache
		// slot write) when on. Source: msl.cpp:1828.
		if (g_useGpuMechs && mechShape) {
			mechShape->CacheGpuLightData();
		}
	}

	//----------------------------------------------------------------------
	// If currentGestureId is 2 and baseRootNodeHeight is not set, set it!!
	if (baseRootNodeHeight == -99999.9f)
	{
		if (rootNodeIndex == -1)
			rootNodeIndex = mechShape->GetNodeNameId("joint_root");

		bool oldInView = inView;
		setVisibilityGatesFromLegacy(true);
	 	baseRootNodeHeight = (getNodeIdPosition(rootNodeIndex).z - position.z);
		setVisibilityGatesFromLegacy(oldInView);
	}

	// D-gpu-pose-instrument: Arms stage — leftArm/rightArm dynamics +
	// transform when blown off. Per user: arms attached by default;
	// these `*ArmOff && *Arm && *ArmRecalc()` paths only fire for
	// blown-arm mechs. Tracy zone fires unconditionally so we can
	// confirm typical mechs have ~0 mass here.
	{ ZoneScopedN("Mech3D.UpdateGeometry.Arms");
	//------------------------------------------------
	// If arms are off, process their geometry here!
	// MUST do every frame.  We don't know where the arms are!!!
	Stuff::Point3D xlatPosition;
	Stuff::UnitQuaternion qRotation;
 	if (leftArmOff && leftArm && leftArmRecalc())
	{
		//--------------------------------------------------------
		// Update the dynamics and position here for leftArm
		float speed = dVel[1].GetLength();
		if (speed)
		{
			Stuff::Vector3D velDiff = dAcc[1];
			velDiff *= frameLength;
			dVel[1].Add(dVel[1],velDiff);
			speed = dVel[1].GetLength(); 
			if (speed < Stuff::SMALL)
			{
				dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
				
			Stuff::Vector3D posDiff = dVel[1];
			posDiff *= frameLength;
			leftArmPos.Add(leftArmPos,posDiff);
			float elev = land->getTerrainElevation(leftArmPos); 
			if (leftArmPos.z < elev)
			{
				leftArmPos.z = elev;
				dRacc[1].Zero();
				dRVel[1].Zero();
				dTime[1] -= frameLength;
				if (dTime[1] < 0.0)
					dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
			
			Stuff::Vector3D rvDiff = dRacc[1];
			rvDiff *= frameLength;
			dRVel[1].Add(dRVel[1],rvDiff);
				
			Stuff::Vector3D rotDiff = dRVel[1];
			rotDiff *= frameLength;
			dRot[1].Add(dRot[1],rotDiff);
		}

		xlatPosition.x = -leftArmPos.x;
		xlatPosition.y = leftArmPos.z;
		xlatPosition.z = leftArmPos.y;
		
		qRotation = Stuff::EulerAngles(dRot[1].x * DEGREES_TO_RADS, dRot[1].y * DEGREES_TO_RADS, dRot[1].z * DEGREES_TO_RADS);

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see txmmgr.h)
		leftArm->SetTextureHandle(0,localTextureHandle);

		leftArm->SetFogRGB(0xffffffff);
		leftArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		leftArm->TransformMultiShape(&xlatPosition,&qRotation);
			
		if (leftArmSmoke)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			leftShoulderPos.x = -leftArmPos.x;
			leftShoulderPos.y = leftArmPos.z;
			leftShoulderPos.z = leftArmPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(leftShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = leftArmSmoke->Execute(&info);
			if (!result)
			{
				leftArmSmoke->Kill();
				delete leftArmSmoke;
				leftArmSmoke = NULL;
			}
		}
	}
	
	if (rightArmOff && rightArm && rightArmRecalc())
	{
		//--------------------------------------------------------
		// Update the dynamics and position here for leftArm
		float speed = dVel[0].GetLength();
		if (speed)
		{
			Stuff::Vector3D velDiff = dAcc[0];
			velDiff *= frameLength;
			dVel[0].Add(dVel[0],velDiff);
			speed = dVel[0].GetLength(); 
			if (speed < Stuff::SMALL)
			{
				dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
				
			Stuff::Vector3D posDiff = dVel[0];
			posDiff *= frameLength;
			rightArmPos.Add(rightArmPos,posDiff);
			float elev = land->getTerrainElevation(rightArmPos); 
			if (rightArmPos.z < elev)
			{
				rightArmPos.z = elev;
				dRacc[0].Zero();
				dRVel[0].Zero();
				dTime[0] -= frameLength;
				if (dTime[0] < 0.0)
					dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
			
			Stuff::Vector3D rvDiff = dRacc[0];
			rvDiff *= frameLength;
			dRVel[0].Add(dRVel[0],rvDiff);
				
			Stuff::Vector3D rotDiff = dRVel[0];
			rotDiff *= frameLength;
			dRot[0].Add(dRot[0],rotDiff);
		}

		xlatPosition.x = -rightArmPos.x;
		xlatPosition.y = rightArmPos.z;
		xlatPosition.z = rightArmPos.y;
		
		qRotation = Stuff::EulerAngles(dRot[0].x * DEGREES_TO_RADS, dRot[0].y * DEGREES_TO_RADS, dRot[0].z * DEGREES_TO_RADS);

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see txmmgr.h)
		rightArm->SetTextureHandle(0,localTextureHandle);

		rightArm->SetFogRGB(0xffffffff);
		rightArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		rightArm->TransformMultiShape(&xlatPosition,&qRotation);
		
		if (rightArmSmoke)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			rightShoulderPos.x = -rightArmPos.x;
			rightShoulderPos.y = rightArmPos.z;
			rightShoulderPos.z = rightArmPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(rightShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = rightArmSmoke->Execute(&info);
			if (!result)
			{
				rightArmSmoke->Kill();
				delete rightArmSmoke;
				rightArmSmoke = NULL;
			}
		}
	}
	} // end Arms zone

 	nextStep = prevStep = false;
	
	return TRUE;
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::blowLeftArm (void)
{
	if (leftArmOff)
		return;
		
	// For now, the shoulder just gets a location which does not change.
	// Can easily move to a node if a node becomes available.
	//
	if (strcmp(weaponEffects->GetEffectName(SHOULDER_POP_ID),"NONE") != 0)
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(SHOULDER_POP_ID));
		
		if (gosEffectSpec)
		{
			leftShoulderBoom = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(leftShoulderBoom != NULL);
			
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
		
	if (strcmp(weaponEffects->GetEffectName(ARM_FLYING_ID),"NONE") != 0)
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(ARM_FLYING_ID));
		
		if (gosEffectSpec)
		{
			leftArmSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(leftArmSmoke != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (leftArmNodeIndex == -1)
		leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

	Stuff::Vector3D leftNodePos = getNodeIdPosition(leftArmNodeIndex);
	leftArmPos = leftNodePos;

 	if (leftShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		leftShoulderPos.x = -leftNodePos.x;
		leftShoulderPos.y = leftNodePos.z;
		leftShoulderPos.z = leftNodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(leftShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		leftShoulderBoom->Start(&info);
	}

 	if (leftArmSmoke)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		leftShoulderPos.x = -leftArmPos.x;
		leftShoulderPos.y = leftArmPos.z;
		leftShoulderPos.z = leftArmPos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(leftShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		leftArmSmoke->Start(&info);
	}
	
	leftArmOff = true;
	
	//Calc initial Velocity, rotation and set Acceleration to down in World.
	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[1].x = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].y = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].z = RandomNumber(upBase) + upBase;

	dRVel[1].x = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].y = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].z = RandomNumber(rotBase * 2.0) - rotBase;

	dRot[1].Zero();

	dAcc[1].x = dVel[1].x * 0.1f;
	dAcc[1].y = dVel[1].y * 0.1f;
	dAcc[1].z = dVel[1].z * 0.5f;
	dAcc[1].Negate(dAcc[1]);

	dRacc[1].x = dRVel[1].x * 0.1f;
	dRacc[1].y = dRVel[1].y * 0.1f;
	dRacc[1].z = dRVel[1].z * 0.1f;
	dRacc[1].Negate(dRacc[1]);

	dTime[1] = 4.0f;

	mechShape->StopUsing("joint_luarm");
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::blowRightArm (void)
{
	if (rightArmOff)
		return;
		
	// For now, the shoulder just gets a location which does not change.
	// Can easily move to a node if a node becomes available.
	//
	if (strcmp(weaponEffects->GetEffectName(SHOULDER_POP_ID),"NONE") != 0)
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(SHOULDER_POP_ID));
		
		if (gosEffectSpec)
		{
			rightShoulderBoom = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(rightShoulderBoom != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
		
	if (strcmp(weaponEffects->GetEffectName(ARM_FLYING_ID),"NONE") != 0)
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(ARM_FLYING_ID));
		
		if (gosEffectSpec)
		{
			rightArmSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(rightArmSmoke != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (rightArmNodeIndex == -1)
		rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

	Stuff::Vector3D rightNodePos = getNodeIdPosition(rightArmNodeIndex);
	rightArmPos = rightNodePos;

 	if (rightShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		rightShoulderPos.x = -rightNodePos.x;
		rightShoulderPos.y = rightNodePos.z;
		rightShoulderPos.z = rightNodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(rightShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		rightShoulderBoom->Start(&info);
	}

 	if (rightArmSmoke)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		rightShoulderPos.x = -rightArmPos.x;
		rightShoulderPos.y = rightArmPos.z;
		rightShoulderPos.z = rightArmPos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(rightShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		rightArmSmoke->Start(&info);
	}

	rightArmOff = true;

	//Calc initial Velocity, rotation and set Acceleration to down in World.
	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[0].x = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].y = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].z = RandomNumber(upBase) + upBase;

	dRVel[0].x = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].y = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].z = RandomNumber(rotBase * 2.0) - rotBase;

	dRot[0].Zero();

	dAcc[0].x = dVel[0].x * 0.1f;
	dAcc[0].y = dVel[0].y * 0.1f;
	dAcc[0].z = dVel[0].z * 0.5f;
	dAcc[0].Negate(dAcc[0]);

	dRacc[0].x = dRVel[0].x * 0.1f;
	dRacc[0].y = dRVel[0].y * 0.1f;
	dRacc[0].z = dRVel[0].z * 0.1f;
	dRacc[0].Negate(dRacc[0]);

	dTime[0] = 4.0f;
	
	mechShape->StopUsing("joint_ruarm");
}
		
//-----------------------------------------------------------------------------
void Mech3DAppearance::startSmoking (long smokeLvl)
{
	//Check if we are already playing one.  If not, smoke away
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  Memory?
	
	if (!useNonWeaponEffects)
		return;

	if ((smokeLvl > 0) && !criticalSmoke)
	{
   		if (strcmp(weaponEffects->GetEffectName(CRITICAL_SMOKE_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(CRITICAL_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				criticalSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(criticalSmoke != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if ((smokeLvl == 0) && !smokeEffect)
	{
   		if (strcmp(weaponEffects->GetEffectName(MECH_SMOKE_ID),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(MECH_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				smokeEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(smokeEffect != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (smokeLvl != -1)
	{
		if ((isSmoking == -1) || ((isSmoking != smokeLvl) && (smokeLvl != -1)))
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;

			Stuff::Vector3D smokeNodePos = getNodePosition(0);	//Always SMOKE if it exists!!
			Stuff::Point3D smokePos;
			smokePos.x = -smokeNodePos.x;
			smokePos.y = smokeNodePos.z;
			smokePos.z = smokeNodePos.y;

			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(smokePos);

			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);

			if (smokeLvl > 0)
				criticalSmoke->Start(&info);
			else
				smokeEffect->Start(&info);
		}
	}
	else
	{
		if (smokeEffect)
			smokeEffect->Kill();
			
		if (criticalSmoke)
			criticalSmoke->Kill();
	}
	
	isSmoking = smokeLvl;
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::startWaterWake (void)
{
	//Check if we are already playing one.  If not, wake city.
	
	//Check if we are a helicopter OR we are jumping.  No WAKE if either is true.
	if (isHelicopter || currentGestureId == 20)
		return;

	if (!useNonWeaponEffects)
		return;

	//First, check if its even loaded.
	// can easily preload this.  Should we?  Memory?
	if (useNonWeaponEffects && !waterWake)
	{
   		if (strcmp(weaponEffects->GetEffectName(MECH_WATER_WAKE),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(MECH_WATER_WAKE));
			
			if (gosEffectSpec)
			{
				waterWake = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(waterWake != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (waterWake && !isWaking)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;	//Wake is at Water Level!
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		if (!inReverse)
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		else
			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);

		waterWake->Start(&info);
		isWaking = true;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::stopWaterWake (void)
{
	if (waterWake && isWaking)		//Stop the effect if we are running it!!
		waterWake->Kill();
	
	isWaking = false;
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::playEjection (void)
{
	if (InEditor)
		return;

	//Check if we are already playing one.  If not, dustCloud.  Use for falls, too?
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  Memory?
	if (useNonWeaponEffects && !helicopterDustCloud)
	{
   		if (strcmp(weaponEffects->GetEffectName(HELICOPTER_DUST_CLOUD),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(HELICOPTER_DUST_CLOUD));
			
			if (gosEffectSpec)
			{
				helicopterDustCloud = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(helicopterDustCloud != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isDusting && helicopterDustCloud)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = position.z;	//dustCloud is just centered on position.
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		/*
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		*/
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);

		helicopterDustCloud->Start(&info);
		isDusting = true;
	}
}

//-----------------------------------------------------------------------------
void Mech3DAppearance::destroy (void)
{
	AppearanceTypeList::appearanceHeap->Free(paintSchemata);
	paintSchemata = NULL;

	// (E) T1.8: paired cleanup for SpotLight_-child illumination from T1.6.
	// Walks CACHED state only (spotlightLights_/spotlightSlotIds_); does NOT
	// call getNodeIdPosition or any mechShape method — same destroy-ordering
	// discipline as bdactor.cpp T1.5 (C-r2 M5). NOTE: the pre-existing
	// `pointLight` (anubis searchlight) is intentionally NOT cleaned up here
	// — that's a documented R1 leak in the existing code, out-of-scope for
	// (E) per plan "Vedette/LRMC anubis-leak audit" follow-up.
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
		// T1.16 — pair untag with removeWorldLight.
		mc2_spotlight_diag::untag_slot(static_cast<long>(spotlightSlotIds_[k]));
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;

	if ( mechShape )
	{
		mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1: final teardown
		delete mechShape;
	}
	mechShape = NULL;

	if (mechShadowShape)
	{
		delete mechShadowShape;
		mechShadowShape = NULL;
	}

	if ( leftArm )
		delete leftArm;
	leftArm = NULL;

	if ( rightArm )
		delete rightArm;
	rightArm = NULL;

#ifdef _DEBUG
	if (logFile )
	{
		delete logFile;
		logFile = NULL;
	}

#endif

	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		if (rightDustPoofEffect[i])
		{
			rightDustPoofEffect[i]->Kill();
			delete rightDustPoofEffect[i];
			rightDustPoofEffect[i] = NULL;
		}
			
		if (leftDustPoofEffect[i])
		{
			leftDustPoofEffect[i]->Kill();
			delete leftDustPoofEffect[i];
			leftDustPoofEffect[i] = NULL;
		}
	}

	if (smokeEffect)
	{
		smokeEffect->Kill();
		delete smokeEffect;
		smokeEffect = NULL;
	}
	
	if (jumpJetEffect)
	{
		jumpJetEffect->Kill();
		delete jumpJetEffect;
		jumpJetEffect = NULL;
	}
	
	if (rightShoulderBoom)
	{
		rightShoulderBoom->Kill();
		delete rightShoulderBoom;
		rightShoulderBoom = NULL;
	}
	
	if (leftShoulderBoom)
	{
		leftShoulderBoom->Kill();
		delete leftShoulderBoom;
		leftShoulderBoom = NULL;
	}

	if (sensorSquareShape)
	{
		delete sensorSquareShape;
		sensorSquareShape = NULL;	
	}
		
	if (sensorTriangleShape)
	{
		delete sensorTriangleShape;
		sensorTriangleShape = NULL;	
	}
	
	appearanceTypeList->removeAppearance(mechType);

	if (InEditor)
	{
		AppearanceTypeList::appearanceHeap->Free(nodeUsed); 
		nodeUsed = NULL;

		AppearanceTypeList::appearanceHeap->Free(nodeRecycle);
		nodeRecycle = NULL;
	}
}

//*****************************************************************************
void Mech3DAppearance::copyTo (MechAppearanceData *data)
{
	data->frameNum = frameNum;
	data->mechFrameRate = mechFrameRate;

	data->leftArmOff = leftArmOff;
	data->rightArmOff = rightArmOff;
	data->fallen = fallen;
	data->forceStop = forceStop;
	data->atTransitionToNextGesture = atTransitionToNextGesture;
	data->inReverse = inReverse;
	data->inJump = inJump;
	data->jumpSetup = jumpSetup;
	data->jumpFXSetup = jumpFXSetup;
	data->jumpAirborne = jumpAirborne;
	data->oncePerFrame = oncePerFrame;
	data->lockRotation = lockRotation;

	data->velocity = velocity;
	data->status = status;

	data->currentStateGoal = currentStateGoal;
	data->currentGestureId = currentGestureId;
	data->transitionState = transitionState;
	data->oldStateGoal = oldStateGoal;
	data->hitGestureId = hitGestureId;

	data->currentFrame = currentFrame;
	data->currentLOD = currentLOD;

	if (mechType->numWeaponNodes > 10)
		STOP(("Mech Has too many weapon nodes to save %d",mechType->numWeaponNodes));

	memcpy(data->nodeUsed,nodeUsed,10);		 
	memcpy(data->nodeRecycle,nodeRecycle,10);

	data->isSmoking = isSmoking;
	data->isWaking = isWaking;
	data->isDusting = isDusting;
	data->fallDust = fallDust;
	data->isHelicopter = isHelicopter;

	data->baseRootNodeHeight = baseRootNodeHeight;
	data->jumpDestination = jumpDestination;
	data->jumpVelocity = jumpVelocity;
}

//*****************************************************************************
void Mech3DAppearance::copyFrom (MechAppearanceData *data)
{
	frameNum = data->frameNum;
	mechFrameRate = data->mechFrameRate;

	leftArmOff = data->leftArmOff;
	rightArmOff = data->rightArmOff;
	fallen = data->fallen;
	forceStop = data->forceStop;
	atTransitionToNextGesture = data->atTransitionToNextGesture;
	inReverse = data->inReverse;
	inJump = data->inJump;
	jumpSetup = data->jumpSetup;
	jumpFXSetup = data->jumpFXSetup;
	jumpAirborne = data->jumpAirborne;
	oncePerFrame = data->oncePerFrame;
	lockRotation = data->lockRotation;

	velocity = data->velocity;

	//Let mech reset status when it reloads so that the destroyed shapes come up.
//	status = data->status;

	currentStateGoal = data->currentStateGoal;
	currentGestureId = data->currentGestureId;
	transitionState = data->transitionState;
	oldStateGoal = data->oldStateGoal;
	hitGestureId = data->hitGestureId;

	currentFrame = data->currentFrame;
	currentLOD = data->currentLOD;

	memcpy(nodeUsed,data->nodeUsed,10);		 
	memcpy(nodeRecycle,data->nodeRecycle,10);

	isSmoking = -1;
	if (data->isSmoking >= 0)
		startSmoking(data->isSmoking);

	isWaking = false;
	if (data->isWaking)
		startWaterWake();

	isDusting = false;
	if (data->isDusting)
		playEjection();

	fallDust = false;
	if (data->fallDust)
		playEjection();

	if (data->inJump)
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		Stuff::Vector3D jumpNodePos = getNodePosition(jumpNodeId);
		
		Stuff::Point3D			actualPosition;
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		actualPosition.x = -jumpNodePos.x;
		actualPosition.y = jumpNodePos.z;
		actualPosition.z = jumpNodePos.y;
		
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(actualPosition);
		
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
					
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,NULL);
		jumpJetEffect->Start(&info);
	}

	isHelicopter = data->isHelicopter;
	baseRootNodeHeight = data->baseRootNodeHeight;
	jumpDestination = 	data->jumpDestination;
	jumpVelocity = 		data->jumpVelocity;

	// [MECHRESTORE v1] restore boundary. status is deliberately NOT
	// copied (see :5327) — emit the post-restore value + type/LOD so a
	// single savegame load correlates each mech's restored type against
	// its later submit/register events.
	{
		static const bool s_mechRestoreTrace =
			(getenv("MC2_MECH_RESTORE_TRACE") != nullptr);
		if (s_mechRestoreTrace)
			std::fprintf(stderr,
				"[MECHRESTORE v1] event=copyfrom type=%p restored_lod=%d "
				"status_restored=0 status_now=%d\n",
				(void*)mechType, (int)currentLOD, (int)status);
	}
}

void Mech3DAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}

Stuff::Vector3D Mech3DAppearance::getHitNodeLeft (void)
{
	if (hitLeftNodeIndex == -1)
		hitLeftNodeIndex = mechShape->GetNodeNameId("hit_left");

	Stuff::Vector3D result = position;
	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);

   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;

   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	// 1A: imported mech -> manifest-resolved hit node ("hit_left").
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld((const void*)mechShape, tKey, "hit_left",
				(const float*)mechShape->GetShapeRec(0)->shapeToWorld.entries, wp)) {
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2];
			return result;
		}
	}

	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,hitLeftNodeIndex);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) &&
		(result.z == 0.0f))
		result = position;

	return result;
}

Stuff::Vector3D Mech3DAppearance::getHitNodeRight (void)
{
	if (hitRightNodeIndex == -1)
		hitRightNodeIndex = mechShape->GetNodeNameId("hit_right");

	Stuff::Vector3D result = position;
	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	// 1A: imported mech -> manifest-resolved hit node ("hit_right").
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? (const void*)mechType->mechShape[currentLOD] : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld((const void*)mechShape, tKey, "hit_right",
				(const float*)mechShape->GetShapeRec(0)->shapeToWorld.entries, wp)) {
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2];
			return result;
		}
	}

	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,hitRightNodeIndex);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//*****************************************************************************
