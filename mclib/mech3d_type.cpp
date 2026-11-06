// Mech3DAppearance: Mech3DAppearanceType init/destroy/setAnimation and the weapon-node lookups.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mc2_czstring.h"
#include "mech3d_internal.h"
#include "render_snap.h"
#include "../RenderCore/axis_convert.h"
#include "content_error.h"
#include <format>
#include <vector>
#include "arm_node_remap.h"

constexpr int MAX_ANIMATION_FILES = 25;
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

void Mech3DAppearanceType::init (mc2::czstring fileName)
{
	AppearanceType::init(fileName);

	FullPathFileName mechIniName;
	mechIniName.init(tglPath,fileName,".ini");

	FitIniFile mechFile;

	long result = fileExists(mechIniName) ? mechFile.open(mechIniName) : -1;
	if (result != NO_ERR)
	{
		// gosASSERT is a NO-OP in RelWithDebInfo, so a missing mech INI used to fall through into
		// readIdFloat() on an UNOPENED FitIniFile: _lseek on a bad handle -> CRT invalid-parameter
		// fast-fail (0xC0000409, no crash bundle). Flag the type so BattleMech::init substitutes the
		// fallback chassis, and name the missing file in the content log.
		baseShapeLoadFailed = true;
		mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(mechIniName), "missing mech appearance INI; fallback chassis used");
		return;
	}
	
	for (long i=0;i<MaxGestures;i++)
	{
		char blockId[256];
		auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "Gestures{}", i).out;
		*blockIdEnd = '\0';

		result = mechFile.seekBlock(blockId);

		result = mechFile.readIdFloat("StartVel",gestures[i].startVel);

		result = mechFile.readIdFloat("EndVel",gestures[i].endVel);

		result = mechFile.readIdLong("StartFrame",gestures[i].frameStart);

		result = mechFile.readIdBoolean("Reverse",gestures[i].reverse);
		
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
	// Validated imported-mech packages ship by default. Loose legacy overrides
	// require explicit opt-in: base data includes a broken Z-up Flea GLB.
	static const bool s_assimpMechImport = mc2::gate::get<"MC2_ASSIMP_MECH_IMPORT">();
	const bool importAllowed = s_assimpMechImport &&
		(mc2::gate::state<"MC2_ASSIMP_MECH_IMPORT">() == mc2::gate::Tri::On ||
		 ModOwnerHasCapability(static_cast<const char*>(mechIniName), "imported_mechs"));
	// Gesture clips (.agl/.ase) come from the INI's own name unless [Import] AnimHost= names a
	// host whose gesture timing the imported model borrows until it owns clip timing (P9).
	char animBase[256] = "";
	if (importAllowed &&
	    mechFile.seekBlock("Import") == NO_ERR &&
	    mechFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
	    importSourceBase[0])
	{
		// Strip extension so LoadFromFile gets a bare base name and probes
		// for the right format itself. "madcat.glb" -> "madcat".
		char* dot = strrchr(importSourceBase, '.');
		if (dot) *dot = '\0';
		if (mechFile.readIdString("AnimHost", animBase, 255) != NO_ERR)
			animBase[0] = '\0';
	}
	const char* gestureClipBase = animBase[0] ? animBase : fileName;

	result = mechFile.seekBlock("TGLData");
	if (result != NO_ERR)
		STOP(("Mech %s has no TGL Data",fileName));

	char aseFileName[512];
	result = mechFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		char aseFileName[512];
		for (long i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			auto baseNameEnd = std::format_to_n(baseName, sizeof(baseName) - 1, "FileName{}", i).out;
			*baseNameEnd = '\0';
			auto baseLODDistEnd = std::format_to_n(baseLODDist, sizeof(baseLODDist) - 1, "Distance{}", i).out;
			*baseLODDistEnd = '\0';
			
			result = mechFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = mechFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				mechShape[i] = std::make_unique<TG_TypeMultiShape>();
				gosASSERT(mechShape[i] != nullptr);
			
				// ASSIMP-MECH-IMPORT-1 — strict opt-in: only LOD0 of a mech with
				// an [Import] Source= override uses the modern importer; every
				// other slot (incl. all stock mechs) takes the original path.
				// META-FIX design B PART 1: REQUIRED slot — checked load
				// (required_missing marker + fail-loud placeholder on failure;
				// cold on stock, where the load always succeeds).
				if (i == 0 && importSourceBase[0]) {
					long rc = mechShape[i]->LoadFromFile(importSourceBase);   // opt-in: modern import
					tglVerifyShapeLoaded(mechShape[i].get(), rc, "mech3d.base_lod_import",
						importSourceBase, &baseShapeLoadFailed);
				} else if (i == 0) {
					FullPathFileName mechName;
					mechName.init(tglPath,aseFileName,".ase");

					tglLoadShapeChecked(mechShape[i].get(), mechName,               // stock ASE call, wrapped
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
		mechShape[0] = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(mechShape[0] != nullptr);

		// ASSIMP-MECH-IMPORT-1 — strict opt-in single-LOD case.
		// META-FIX design B PART 1: REQUIRED slot — checked load.
		if (importSourceBase[0]) {
			long rc = mechShape[0]->LoadFromFile(importSourceBase);   // opt-in: modern import
			tglVerifyShapeLoaded(mechShape[0].get(), rc, "mech3d.base_import",
				importSourceBase, &baseShapeLoadFailed);
		} else {
			FullPathFileName mechName;
			mechName.init(tglPath,aseFileName,".ase");

			tglLoadShapeChecked(mechShape[0].get(), mechName,               // stock ASE call, wrapped
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
		mechShadowShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(mechShadowShape != nullptr);
	
		FullPathFileName gvName;
		gvName.init(tglPath,aseFileName,".ase");
	
		mechShadowShape->LoadTGMultiShapeFromASE(gvName); // TGL-LOAD-EXEMPT(optional shadow; loader-side PART-0 logging; GetNumShapes-checked delete-on-fail below)
	}

	char leftArmName[1024];
	auto leftArmNameEnd = std::format_to_n(leftArmName, sizeof(leftArmName) - 1, "{}LeftArm", fileName).out;
	*leftArmNameEnd = '\0';
	
	FullPathFileName mechLeftArmName;
	mechLeftArmName.init(tglPath,leftArmName,".ase");

	char rightArmName[1024];
	auto rightArmNameEnd = std::format_to_n(rightArmName, sizeof(rightArmName) - 1, "{}RightArm", fileName).out;
	*rightArmNameEnd = '\0';
	
	FullPathFileName mechRightArmName;
	mechRightArmName.init(tglPath,rightArmName,".ase");

	rightArm = std::make_unique<TG_TypeMultiShape>();
	gosASSERT(rightArm != nullptr);
	
	result = rightArm->LoadTGMultiShapeFromASE(mechRightArmName); // TGL-LOAD-EXEMPT(optional arm; delete-on-fail is normal stock degradation)
	if (result)
	{
		rightArm.reset();
	}
	
	leftArm = std::make_unique<TG_TypeMultiShape>();
	gosASSERT(leftArm != nullptr);
	
	result = leftArm->LoadTGMultiShapeFromASE(mechLeftArmName); // TGL-LOAD-EXEMPT(optional arm; delete-on-fail is normal stock degradation)
	if (result)
	{
		leftArm.reset();
	}
	
	// Load Damaged States
	// They are named mechnamefallForward and mechnameFallBackward
	char forwardName[1024];
	auto forwardNameEnd = std::format_to_n(forwardName, sizeof(forwardName) - 1, "{}FallForwardDam", fileName).out;
	*forwardNameEnd = '\0';

	char backwardName[1024];
	auto backwardNameEnd = std::format_to_n(backwardName, sizeof(backwardName) - 1, "{}FallForwardDam", fileName).out;
	*backwardNameEnd = '\0';

	{
		FullPathFileName dmgName;
		dmgName.init(tglPath,forwardName,".ase");

		mechForwardDmgShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(mechForwardDmgShape != nullptr);
		mechForwardDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional fall-damage shape; GetNumShapes-checked)

		if (!mechForwardDmgShape->GetNumShapes())
		{
			mechForwardDmgShape.reset();
		}
	}

	{
		FullPathFileName dmgName;
		dmgName.init(tglPath,backwardName,".ase");

		mechBackwardDmgShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(mechBackwardDmgShape != nullptr);
		mechBackwardDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional fall-damage shape; GetNumShapes-checked)

		if (!mechBackwardDmgShape->GetNumShapes())
		{
			mechBackwardDmgShape.reset();
		}
	}

	if (animationLoadingEnabled)
	{
		for (int i=0;i<MAX_ANIMATION_FILES;i++)
		{
			char animName[512];
			auto animNameEnd = std::format_to_n(animName, sizeof(animName) - 1, "{}{}", gestureClipBase, MechAnimationNames[i]).out;
			*animNameEnd = '\0';

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
				mechAnim[i] = std::make_unique<TG_AnimateShape>();
				gosASSERT(mechAnim[i] != nullptr);

				// If this animation does not exist, it is not a problem!
				// Mech will simply freeze until animation is "over" and then move to next animation in chain.
				mechAnim[i]->LoadTGMultiShapeAnimationFromASE(animPath,mechShape[0].get());

				if (gestures[i].reverse)
					mechAnim[i]->ReverseFrameRate();
			}
			else
				mechAnim[i] = nullptr;
		}
	}

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
		nodeDataStore = std::make_unique<NodeData[]>(numJumpNodes+numWeaponNodes+numSmokeNodes+numFootNodes);
		nodeData = nodeDataStore.get();
		
		for (int i=0;i<numSmokeNodes;i++)
		{
			char blockId[512];
			auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "SmokeNode{}", i).out;
			*blockIdEnd = '\0';
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char smokeName[512];
			result = mechFile.readIdString("SmokeNodeName",smokeName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i].setNodeId(smokeName);
			nodeData[i].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
		
		for (int i=0;i<numWeaponNodes;i++)
		{
			char blockId[512];
			auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "WeaponNode{}", i).out;
			*blockIdEnd = '\0';
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char weaponName[512];
			result = mechFile.readIdString("WeaponNodeName",weaponName,511);
			gosASSERT(result == NO_ERR);
			
			long weaponType;
			result = mechFile.readIdLong("WeaponType",weaponType);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes].setNodeId(weaponName);
			nodeData[i+numSmokeNodes].weaponType = weaponType;
			
 		}
		
		for (int i=0;i<numJumpNodes;i++)
		{
			char blockId[512];
			auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "JumpJetNode{}", i).out;
			*blockIdEnd = '\0';
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char jumpName[512];
			result = mechFile.readIdString("JumpNodeName",jumpName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes+numWeaponNodes].setNodeId(jumpName);
			nodeData[i+numSmokeNodes+numWeaponNodes].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
		
		for (int i=0;i<numFootNodes;i++)
		{
			char blockId[512];
			auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "FootNode{}", i).out;
			*blockIdEnd = '\0';
			result = mechFile.seekBlock(blockId);
			gosASSERT(result == NO_ERR);
			
			char footName[512];
			result = mechFile.readIdString("FootNodeName",footName,511);
			gosASSERT(result == NO_ERR);
			
			nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].setNodeId(footName);
			nodeData[i+numSmokeNodes+numWeaponNodes+numJumpNodes].weaponType = MECH3D_WEAPONTYPE_NONE;
		}
	}
	
	// Load up sensor textures if not yet defined.
	// For helicopters, load up the vehicle one!!
	if (SensorSquareShape == nullptr && numJumpNodes)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"squarecontact",".ase");

		SensorSquareShape = new TG_TypeMultiShape;
		gosASSERT(SensorSquareShape != nullptr);

		// META-FIX design B PART 1: REQUIRED static (sensor contact art).
		tglLoadShapeChecked(SensorSquareShape, sensorName,
			"mech3d.sensor_square", nullptr);
	}

	if (GVAppearanceType::SensorCircleShape == nullptr && !numJumpNodes)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"circularcontact",".ase");

		GVAppearanceType::SensorCircleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorCircleShape != nullptr);

		tglLoadShapeChecked(GVAppearanceType::SensorCircleShape, sensorName,
			"mech3d.sensor_circle", nullptr);
	}

	if (GVAppearanceType::SensorTriangleShape == nullptr)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"trianglecontact",".ase");

		GVAppearanceType::SensorTriangleShape = new TG_TypeMultiShape;
		gosASSERT(GVAppearanceType::SensorTriangleShape != nullptr);

		tglLoadShapeChecked(GVAppearanceType::SensorTriangleShape, sensorName,
			"mech3d.sensor_triangle", nullptr);
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
		numSockets = std::min(numSockets, MAX_PRESENTATION_SOCKETS);
		for (long si = 0; si < numSockets; si++)
		{
			char blockId[64];
			auto blockIdEnd = std::format_to_n(blockId, sizeof(blockId) - 1, "Socket{}", si).out;
			*blockIdEnd = '\0';
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

void Mech3DAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	long i=0;
	for (i=0;i<MAX_LODS;i++)
	{
		mechShape[i].reset();
	}

	mechShadowShape.reset();

	rightArm.reset();

	leftArm.reset();

	mechForwardDmgShape.reset();

	mechBackwardDmgShape.reset();

	for (i=0;i<MAX_ANIMATION_FILES;i++)
	{
		mechAnim[i].reset();
	}
	
	nodeDataStore.reset();
	nodeData = nullptr;
	numSmokeNodes = numWeaponNodes = numJumpNodes = 0;
}

void Mech3DAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != nullptr);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MaxGestures);

	if (mechAnim[animationNum])
		mechAnim[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}
void Mech3DAppearance::resetWeaponNodes (void)
{
	//THis should never be called after the game inits!!
	for (long i=0;i<mechType->numWeaponNodes;i++)
	{
		unitAnim().nodeUsed[i] = 0;
		unitAnim().nodeRecycle[i] = BASE_NODE_RECYCLE_TIME;
	}
}

void Mech3DAppearance::setWeaponNodeUsed (long weaponNode)
{
	//THis should never be called after the game inits!!
	weaponNode -= mechType->numSmokeNodes;
   	if ((weaponNode >= 0) && (weaponNode < mechType->numWeaponNodes))
	{
		unitAnim().nodeUsed[weaponNode]++;
		unitAnim().nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

void Mech3DAppearance::setWeaponNodeRecycle(long nodeId, float time)
{
	nodeId -= mechType->numSmokeNodes;
   	if ((nodeId >= 0) && (nodeId < mechType->numWeaponNodes))
		unitAnim().nodeRecycle[nodeId] = time;
}

glm::vec3 Mech3DAppearance::getWeaponNodePosition (long nodeId)
{
    if (!simPresentationPass && simPose->ready) return nodeId >= 0 && static_cast<size_t>(nodeId) < simPose->nodes.size() ? simPose->nodes[nodeId] : simPose->root;

	POSE_VIEW_LOCALS
	glm::vec3 result = position;
	if ((nodeId < mechType->numSmokeNodes) || (nodeId >= (mechType->numSmokeNodes+mechType->numWeaponNodes)))
		return result;

	// No visibility test: update() poses every mech each sim step, so shot origins never depend on the camera.

	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!

	// 1A (BT2018-MECH-NODE-MANIFEST-1A): an imported mech's merged single shape has
	// no named TG nodes, so the legacy lookup below returns origin. Resolve the
	// firepoint from the package node manifest + live clip globals instead.
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD].get()) : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld(static_cast<const void*>(mechShape), tKey,
				mechType->nodeData[nodeId].nodeId,
				static_cast<const float*>(glm::value_ptr(mechShape->GetShapeRec(0)->shapeToWorld)), wp)) {
			static int s_diagN = 0;
			if (false && s_diagN++ < 24)
				fprintf(stderr, "[NODE-DIAG-CALLER] mech pos=(%.1f,%.1f,%.1f) node='%s' world=(%.1f,%.1f,%.1f) delta=(%.1f,%.1f,%.1f)\n",
				        position.x, position.y, position.z,
				        mechType->nodeData[nodeId].nodeId,
				        wp[0], wp[1], wp[2],
				        wp[0]-position.x, wp[1]-position.y, wp[2]-position.z);
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2] - pv_.lift;
			return result;
		}
	}

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));

   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);

   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	result = mc2snap::unliftNodeZ(mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,mechType->nodeData[nodeId].nodeId), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 Mech3DAppearance::getNodeNamePosition (mc2::czstring nodeName)
{
    if (!simPresentationPass && simPose->ready) {
        if (S_stricmp(nodeName, "cockpit") == 0) return simPose->cockpit;
        if (S_stricmp(nodeName, "hit_left") == 0) return simPose->hitLeft;
        if (S_stricmp(nodeName, "hit_right") == 0) return simPose->hitRight;
        for (long i = 0; i < mechType->getTotalNodes(); ++i)
            if (S_stricmp(nodeName, mechType->nodeData[i].nodeId) == 0)
                return simPose->nodes[i];
        const auto node = simulationShape_ ? simulationShape_->GetNodeNameId(nodeName) : -1;
        return node >= 0 && static_cast<size_t>(node) < simPose->shapeNodes.size()
            ? simPose->shapeNodes[node] : simPose->root;
    }
	POSE_VIEW_LOCALS
	glm::vec3 result = position;
    if (mechShape && mechShape->GetNumShapes() > 0) {
        const void* type = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD].get()) : nullptr;
        float world[3];
        if (mc2mechanim::GetImportedNodeWorld(mechShape, type, nodeName,
                glm::value_ptr(mechShape->GetShapeRec(0)->shapeToWorld), world))
            return glm::vec3(world[0], world[1], world[2] - pv_.lift);
    }

	// No visibility test: update() poses every mech each sim step, so the node is current on and off screen.

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mc2snap::unliftNodeZ(mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 Mech3DAppearance::getNodeIdPosition (long nodeId)
{
    if (!simPresentationPass && simPose->ready) return nodeId >= 0 && static_cast<size_t>(nodeId) < simPose->shapeNodes.size() ? simPose->shapeNodes[nodeId] : simPose->root;

	POSE_VIEW_LOCALS
	glm::vec3 result = position;

	// No visibility test: update() poses every mech each sim step, so the node is current on and off screen.

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mc2snap::unliftNodeZ(mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 Mech3DAppearance::getNodePosition (long nodeId)
{
    if (!simPresentationPass && simPose->ready) return nodeId >= 0 && static_cast<size_t>(nodeId) < simPose->nodes.size() ? simPose->nodes[nodeId] : simPose->root;

	POSE_VIEW_LOCALS
	glm::vec3 result = position;
	if ((nodeId < 0) || (nodeId >= mechType->getTotalNodes()))
		return result;

	// No visibility test: update() poses every mech each sim step, so the node is current on and off screen.

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
    
	result = mc2snap::unliftNodeZ(mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,mechType->nodeData[nodeId].nodeId), pv_.lift);

	return result;
}

long Mech3DAppearance::getLowestWeaponNode (void)
{
	long bestNode = -1;
	float lowestPosZ;
	long numSmokeNodes = mechType->numSmokeNodes;
	lowestPosZ = 9999999999999.0f;
	for (long i=0;i<mechType->numWeaponNodes;i++)
	{
		glm::vec3 nodePosition = getNodePosition(i+numSmokeNodes);
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

long Mech3DAppearance::getWeaponNode (long weaponType)
{
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
					if (unitAnim().rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					if (unitAnim().leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
					if (unitAnim().nodeUsed[i] < leastUsed)
					{
						leastUsed = unitAnim().nodeUsed[i];
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
					if (unitAnim().rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					if (unitAnim().leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
 					if (unitAnim().nodeUsed[i] < leastUsed)
					{
						leastUsed = unitAnim().nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ENERGY:
				if ((mechType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_DIRECT) ||
					(mechType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY))
				{
					if (unitAnim().rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
						continue;
						
					if (unitAnim().leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
						continue;
						
 					if (unitAnim().nodeUsed[i] < leastUsed)
					{
						leastUsed = unitAnim().nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ANY:
				if (unitAnim().rightArmOff && mechType->nodeData[i+numSmokeNodes].isRArmNode)
					continue;
					
				if (unitAnim().leftArmOff && mechType->nodeData[i+numSmokeNodes].isLArmNode)
					continue;
					
 				if (unitAnim().nodeUsed[i] < leastUsed)
				{
					leastUsed = unitAnim().nodeUsed[i];
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

// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase-2 slice 4): socket NAME -> weapon-node
// INDEX. Scans ONLY the weapon-node range of nodeData[] (the same range
// getWeaponNode() picks from) and returns the matching index, or -1. Read-only:
// unlike getWeaponNode() this does NOT call setWeaponNodeUsed() -- the caller is
// choosing a specific declared socket, not consuming a round-robin slot.
long Mech3DAppearance::resolveWeaponNodeBySocketName (mc2::czstring socketName)
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

// Read-only counterpart of getWeaponNode() for a weapon whose own node is on a lost arm (rules in mc2_arm_remap). The old
// caller remapped by constants (1 -> 3, 2 -> 5) that are body-location numbers, not node indices.
long Mech3DAppearance::remapLostArmWeaponNode (long node, long weaponType)
{
	const long numSmokeNodes = mechType->numSmokeNodes;
	const long count = mechType->numWeaponNodes;
	const long own = node - numSmokeNodes;
	if ((own < 0) || (own >= count))
		return node;

	std::vector<mc2_arm_remap::Node> nodes(static_cast<size_t>(count));
	for (long i = 0; i < count; i++)
	{
		const auto& nd = mechType->nodeData[numSmokeNodes + i];
		nodes[i] = { nd.weaponType, nd.isRArmNode, nd.isLArmNode, unitAnim().nodeUsed[i] };
	}
	return mc2_arm_remap::remapLostArmNode(nodes, own, weaponType, unitAnim().rightArmOff, unitAnim().leftArmOff) + numSmokeNodes;
}

float Mech3DAppearance::getWeaponNodeRecycle (long node)
{
	node -= mechType->numSmokeNodes;
	
	if ((node >=0) && (node < mechType->numWeaponNodes))
		return unitAnim().nodeRecycle[node];
		
	return 9999.0f;		//NOT a weapon node, never recycled!!
}
