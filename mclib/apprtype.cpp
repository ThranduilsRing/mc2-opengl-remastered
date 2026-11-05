//---------------------------------------------------------------------------
//
// ApprType.cpp -- File contains the Basic Game Appearance Type code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef APPRTYPE_H
#include"apprtype.h"
#endif

#ifndef GVACTOR_H
#include"gvactor.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef BDACTOR_H
#include"bdactor.h"
#endif

#ifndef GENACTOR_H
#include"genactor.h"
#endif

#include"cident.h"
#include"inifile.h"
#include"paths.h"

#include<gameos.hpp>

#include "sim_overlap.h"
#include "mc2_audit.h"   // META-FIX design B PART 2: appr_type_fail backstop

#include<algorithm>
#include<cstdlib>
#include<cstdio>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include <tracy/Tracy.hpp>

//---------------------------------------------------------------------------
// static Globals
AppearanceTypeListPtr appearanceTypeList = nullptr;

unsigned currentAppearanceRegistryGeneration (void)
{
	return appearanceTypeList ? appearanceTypeList->generation() : 0u;
}

// Release one use of a type, but only on the registry the holder was created from. A holder that outlives its
// mission must not touch the next registry (stale or reused address).
long releaseAppearanceType (unsigned gen, AppearanceTypePtr which)
{
	if (!appearanceTypeList || gen == 0u || gen != appearanceTypeList->generation())
		return(-1);
	return appearanceTypeList->removeAppearance(which);
}

//---------------------------------------------------------------------------
// MC2_LOAD_RACE_TRACE: gated, one-shot mission-load residency-race diagnostics.
// Default-OFF; produces zero output unless the gate is set. These globals are
// the shared counters read by the per-run verdict in gameosmain.cpp and written
// by the appearance (apprtype/mech3d) and terrain-residency (txmmgr) paths.
// See docs/tier1_env_vars.md.
bool g_loadRaceTrace                     = mc2::gate::get<"MC2_LOAD_RACE_TRACE">();
long g_loadRaceApprNull                  = 0;    // getAppearance() NULL returns
long g_loadRaceMechDegrade               = 0;    // Mech3DAppearance::init degrade-branch hits
long g_loadRaceMechDegradeReal           = 0;    // ...of which real actors (obj != NULL)
long g_loadRaceMechOkReal                = 0;    // real actors (obj != NULL) that built a shape
long g_loadRaceMechOkPreview             = 0;    // preview/logistics (obj == NULL) built a shape
long g_loadRaceTerrainFirstResidentFrame = -1;   // first frame all terrain textures GPU-resident
bool g_loadRaceTerrainResidentNow        = false;// terrain fully resident as of last flush
// S2b corrected-transient probes: latch the frames at which the indirect
// terrain colormap atlas becomes GPU-resident and the GPU terrain path first
// arms, plus the path actually taken on the most recent preflight. These
// separate "atlas/arming came late" from "atlas + arming early, shading cool".
long g_loadRaceAtlasFirstFrame           = -1;   // first frame g_atlasGLTex != 0
long g_loadRaceArmFirstFrame             = -1;   // first frame ShouldArmGpuTerrain true (gpu path)
long g_loadRaceLastTerrainPath           = 0;    // last preflight: 0=none 1=gpu-indirect 2=cpu-thin

//---------------------------------------------------------------------------
// Class AppearanceType
#include "content_error.h"

void AppearanceType::init (const char *fileName)
{
	nameStore = fileName;
	name = nameStore.data();

	//Dig out the Type Bounds here for selections
	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
	{
		// Missing appearance INI (e.g. a mod mission referencing an asset not in the
		// active install). STOP() is a NO-OP in RelWithDebInfo, so the old code fell
		// through into seekBlock()/readIdLong() on an UNOPENED FitIniFile and CTD'd.
		// Bail with default type bounds instead of crashing -- the editor stays alive
		// and the appearance just gets default selection bounds. (Game ships all
		// assets, so it never reaches this; game behavior unchanged.)
		designerTypeBounds = false;
		mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing appearance INI; default bounds");
		return;
	}
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftX; default bounds");
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftY; default bounds");
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftZ; default bounds");
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightX; default bounds");
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightY; default bounds");
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightZ; default bounds");
 		else
			typeLowerRight.z = tmpy;
    }
 
	iniFile.close();   
}	

//---------------------------------------------------------------------------
void AppearanceType::reinit (void)
{
	//Dig out the Type Bounds here for selections
	FullPathFileName iniName;
	iniName.init(tglPath,name,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
	{
		// Missing appearance INI (e.g. a mod mission referencing an asset not in the
		// active install). STOP() is a NO-OP in RelWithDebInfo, so the old code fell
		// through into seekBlock()/readIdLong() on an UNOPENED FitIniFile and CTD'd.
		// Bail with default type bounds instead of crashing -- the editor stays alive
		// and the appearance just gets default selection bounds. (Game ships all
		// assets, so it never reaches this; game behavior unchanged.)
		designerTypeBounds = false;
		mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing appearance INI; default bounds");
		return;
	}
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftX; default bounds");
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftY; default bounds");
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.UpperLeftZ; default bounds");
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightX; default bounds");
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightY; default bounds");
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing 3DBounds.LowerRightZ; default bounds");
 		else
			typeLowerRight.z = tmpy;
    }
 
	iniFile.close();   
}	

//---------------------------------------------------------------------------
void AppearanceType::destroy (void)
{
	std::string().swap(nameStore);
	name = nullptr;
}

//---------------------------------------------------------------------------
// class AppearanceTypeList
AppearanceTypePtr AppearanceTypeList::getAppearance (unsigned long apprNum, const char *appearFile)
{
	ZoneScopedN("Appearance.GetType");
	mc2::overlap::assertOwner(mc2::overlap::Owner::Sim, "AppearanceTypeList::getAppearance");   // numUsers++ / registry push: Sim side or quiescent Main only
	if (appearFile) ZoneText(appearFile, strlen(appearFile));
	//----------------------------------------------------------------
	// The type of appearance is stored in the upper 8 bits of the
	// apprNum.  To get the correct packet we mask off the top 8 bits
	// and store the number.  To get the appearance type, we right shift
	// by 24.
	long appearanceClass = apprNum >> 24;
	AppearanceTypePtr appearanceType = nullptr;
	
	//----------------------------------------------------
	// If these top bits are wrong, return NULL
	if (appearanceClass == 0)
	{
		if (g_loadRaceTrace && g_loadRaceApprNull < 50)
		{
			g_loadRaceApprNull++;
			mc2::fmt::print_out("[LOADRACE] getAppearance NULL: reason=class0 apprNum=0x{:08x} appearFile={:016X}({})\n", mc2::fmt::as_u32(apprNum), mc2::fmt::as_ptr(static_cast<const void*>(appearFile)), mc2::fmt::safe(appearFile ? appearFile : "<null>"));
			std::fflush(stdout);
		}
		return(nullptr);
	}

	//----------------------------------------------------
	// If string passed in is NULL, return NULL
	if (!appearFile)
	{
		if (g_loadRaceTrace && g_loadRaceApprNull < 50)
		{
			g_loadRaceApprNull++;
			mc2::fmt::print_out("[LOADRACE] getAppearance NULL: reason=appearFileNull apprNum=0x{:08x} class={:d}\n", mc2::fmt::as_u32(apprNum), mc2::fmt::as_i32(appearanceClass));
			std::fflush(stdout);
		}
		return nullptr;
	}

	//-----------------------------------------------------------
	// Scan the registry (load order) for this appearance.
	for (auto& known : types)
	{
		if (known && S_stricmp(known->name,appearFile) == 0)
		{
			appearanceType = known.get();
			break;
		}
	}

	if (appearanceType)
	{
		appearanceType->numUsers++;
	}
	else
	{
		//---------------------------------------------------------
		// This appearance is not registered yet, so load/new it.
		std::unique_ptr<AppearanceType> fresh;
		switch (appearanceClass)
		{
			case MECH_TYPE:			fresh = std::make_unique<Mech3DAppearanceType>();	break;
			case GV_TYPE:			fresh = std::make_unique<GVAppearanceType>();		break;
			case TREED_TYPE:		fresh = std::make_unique<TreeAppearanceType>();		break;
			case GENERIC_APPR_TYPE:	fresh = std::make_unique<GenericAppearanceType>();	break;
			case BLDG_TYPE:			fresh = std::make_unique<BldgAppearanceType>();		break;
			default:
				return(nullptr);
		}

		appearanceType = fresh.get();
		appearanceType->appearanceNum = apprNum;
		appearanceType->init(appearFile);

		//----------------------------------------
		// Registered after init, as the list append always was.
		appearanceType->numUsers = 1;
		types.push_back(std::move(fresh));
	}

	// META-FIX design B PART 2 — chokepoint backstop: this is the sole
	// appearance-type factory, so ANY loader (current or future subclass)
	// whose REQUIRED base shape ended up empty funnels through here.
	// numUsers==1 exactly on the fresh-init path (cache hits increment past
	// 1), making this inherently once-per-type; mc2AuditOnce adds a belt
	// against numUsers round-trips through removeAppearance.
	if (appearanceType && appearanceType->numUsers == 1 &&
		appearanceType->baseShapeLoadFailed)
	{
		unsigned long long key = 5381;
		for (const char* p = appearFile; p && *p; ++p)
			key = key * 33 + static_cast<unsigned char>(*p);
		if (mc2AuditOnce("APPR_TYPE_FAIL", key))
			mc2AuditLinef("[ASSET_LOAD v1] event=appr_type_fail name={} apprNum=0x{:08x}",
				mc2::fmt::safe(appearFile), mc2::fmt::as_u32(apprNum));
	}

	return appearanceType;
}
		
//---------------------------------------------------------------------------
long AppearanceTypeList::removeAppearance (AppearanceTypePtr which)
{
	// A null type (failed load) has nothing to release; a null registry slot must never match.
	if (!which)
		return(-1);
	mc2::overlap::assertOwner(mc2::overlap::Owner::Sim, "AppearanceTypeList::removeAppearance");   // numUsers-- / registry erase
	auto found = std::find_if(types.begin(), types.end(),
		[which](const std::unique_ptr<AppearanceType>& known) { return known && known.get() == which; });
	if (found == types.end())
		return(-1);

	(*found)->numUsers--;

	//----------------------------------------------------------
	// The last user releases the type: the registry's erase deletes it, which runs the destructor chain (each subclass destroy()).
	if ((*found)->numUsers == 0)
		types.erase(found);

	return NO_ERR;
}

//---------------------------------------------------------------------------
void AppearanceTypeList::destroy (void)
{
	//---------------------------------------------------------------------
	// Delete every type still registered, once each and in load order: the destructor chain runs the type's destroy(), the same path
	// removeAppearance takes.
	for (auto& known : types)
		known.reset();
	types.clear();

	destroySensorShapes();
}

//---------------------------------------------------------------------------
void AppearanceTypeList::destroySensorShapes (void)
{
	if (GVAppearanceType::SensorCircleShape)
	{
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = nullptr;
	}

	if (GVAppearanceType::SensorTriangleShape)
	{
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = nullptr;
	}

	if (Mech3DAppearanceType::SensorSquareShape)
	{
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = nullptr;
	}
}

//---------------------------------------------------------------------------
