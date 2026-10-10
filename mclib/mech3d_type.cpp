// Mech3DAppearance: Mech3DAppearanceType init/destroy/setAnimation and the weapon-node lookups.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mech3d_internal.h"

static uint32_t s_lcSkipCount3d = 0u;

#define MAX_ANIMATION_FILES			25
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
	static const bool s_assimpMechImport = mc2::gate::get<"MC2_ASSIMP_MECH_IMPORT">();
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
				// META-FIX design B PART 1: REQUIRED slot — checked load
				// (required_missing marker + fail-loud placeholder on failure;
				// cold on stock, where the load always succeeds).
				if (i == 0 && importSourceBase[0]) {
					long rc = mechShape[i]->LoadFromFile(importSourceBase);   // opt-in: modern import
					tglVerifyShapeLoaded(mechShape[i], rc, "mech3d.base_lod_import",
						importSourceBase, &baseShapeLoadFailed);
				} else if (i == 0) {
					FullPathFileName mechName;
					mechName.init(tglPath,aseFileName,".ase");

					tglLoadShapeChecked(mechShape[i], mechName,               // stock ASE call, wrapped
						"mech3d.base_lod", &baseShapeLoadFailed);
				} else {
					FullPathFileName mechName;
					mechName.init(tglPath,aseFileName,".ase");

					// Declared-but-absent higher LODs are NORMAL stock-data
					// degradation (live tier1 evidence: 2storycivlivingl2) —
					// only LOD0 is the required slot.
					mechShape[i]->LoadTGMultiShapeFromASE(mechName); // TGL-LOAD-EXEMPT(declared LOD>0 absent = stock degradation; LOD0 is required)
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
		// META-FIX design B PART 1: REQUIRED slot — checked load.
		if (importSourceBase[0]) {
			long rc = mechShape[0]->LoadFromFile(importSourceBase);   // opt-in: modern import
			tglVerifyShapeLoaded(mechShape[0], rc, "mech3d.base_import",
				importSourceBase, &baseShapeLoadFailed);
		} else {
			FullPathFileName mechName;
			mechName.init(tglPath,aseFileName,".ase");

			tglLoadShapeChecked(mechShape[0], mechName,               // stock ASE call, wrapped
				"mech3d.base", &baseShapeLoadFailed);
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
	
		mechShadowShape->LoadTGMultiShapeFromASE(gvName); // TGL-LOAD-EXEMPT(optional shadow; loader-side PART-0 logging; GetNumShapes-checked delete-on-fail below)
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
	
	result = rightArm->LoadTGMultiShapeFromASE(mechRightArmName); // TGL-LOAD-EXEMPT(optional arm; delete-on-fail is normal stock degradation)
	if (result)
	{
		delete rightArm;
		rightArm = NULL;
	}
	
	leftArm = new TG_TypeMultiShape;
	gosASSERT(leftArm != NULL);
	
	result = leftArm->LoadTGMultiShapeFromASE(mechLeftArmName); // TGL-LOAD-EXEMPT(optional arm; delete-on-fail is normal stock degradation)
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
		mechForwardDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional fall-damage shape; GetNumShapes-checked)

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
		mechBackwardDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional fall-damage shape; GetNumShapes-checked)

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

		// META-FIX design B PART 1: REQUIRED static (sensor contact art).
		tglLoadShapeChecked(SensorSquareShape, sensorName,
			"mech3d.sensor_square", NULL);
	}

	if (GVAppearanceType::SensorCircleShape == NULL && !numJumpNodes)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"circularcontact",".ase");

		GVAppearanceType::SensorCircleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorCircleShape != NULL);

		tglLoadShapeChecked(GVAppearanceType::SensorCircleShape, sensorName,
			"mech3d.sensor_circle", NULL);
	}

	if (GVAppearanceType::SensorTriangleShape == NULL)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"trianglecontact",".ase");

		GVAppearanceType::SensorTriangleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorTriangleShape != NULL);

		tglLoadShapeChecked(GVAppearanceType::SensorTriangleShape, sensorName,
			"mech3d.sensor_triangle", NULL);
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
		// UNIT-DEFINITION-VALIDATOR-1 (Phase-2 slice 8): route the socket facet
		// through the ONE consolidated validateUnitDefinition() pass instead of the
		// standalone validateDeclaredSockets() entry. Only the declaredSockets list is
		// populated at this appearance-init site; profile/loadout/anim/UI inputs stay
		// empty -> zero extra findings, identical verdict to the old per-facet call.
		// The glue still names each absent socket for the modder (richer than the
		// generic message); the aggregate verdict just gates whether we emit at all.
		UnitDefinitionValidationInputs in;
		in.declaredSockets = declared;
		const std::vector<UnitProfileFinding> findings =
			validateUnitDefinition(in, theActionExecutorRegistry());
		if (!findings.empty())
		{
			for (long si = 0; si < numPresentationSockets; si++)
			{
				if (!declared[si].present)
					printf("[UnitDefinition:VALIDATE] %s: declared socket '%s' not found on model (reason=%d)\n",
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
			if (mc2::gate::get<"MC2_MECH_NODE_DIAG">() && s_diagN++ < 24)
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
