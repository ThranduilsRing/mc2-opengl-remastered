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

#include "mc2_audit.h"   // META-FIX design B PART 2: appr_type_fail backstop

#include<cstdlib>
#include<cstdio>
#include "Gates/gate.h"

//---------------------------------------------------------------------------
// static Globals
UserHeapPtr AppearanceTypeList::appearanceHeap = nullptr;
AppearanceTypeListPtr appearanceTypeList = nullptr;

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
void* AppearanceType::operator new (size_t memSize)
{
	void* result = nullptr;
	if (AppearanceTypeList::appearanceHeap && AppearanceTypeList::appearanceHeap->heapReady())
	{
		result = AppearanceTypeList::appearanceHeap->Malloc(memSize);
	}
		
	return(result);
}

//---------------------------------------------------------------------------
void AppearanceType::operator delete (void* treePtr)
{
	long result;
	if (AppearanceTypeList::appearanceHeap && AppearanceTypeList::appearanceHeap->heapReady())
	{
		result = AppearanceTypeList::appearanceHeap->Free(treePtr);
	}
}

//---------------------------------------------------------------------------
void AppearanceType::init (const char *fileName)
{
	name = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(fileName)+1);
	strcpy(name,fileName);

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
		PAUSE(("AppearanceType: missing appearance INI %s -- using default bounds", (const char*)iniName));
		return;
	}
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
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
		PAUSE(("AppearanceType: missing appearance INI %s -- using default bounds", (const char*)iniName));
		return;
	}
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",iniName));
 		else
			typeLowerRight.z = tmpy;
    }
 
	iniFile.close();   
}	

//---------------------------------------------------------------------------
void AppearanceType::destroy (void)
{
	AppearanceTypeList::appearanceHeap->Free(name);
	name = nullptr;
}

//---------------------------------------------------------------------------
// class AppearanceTypeList
void AppearanceTypeList::init (unsigned long heapSize)
{
	appearanceHeap = new UserHeap;
	gosASSERT(appearanceHeap != nullptr);

	appearanceHeap->init(heapSize,"APPEAR");

	//No More sprite files.  Objects now have direct control over their appearances.
	
}

//---------------------------------------------------------------------------
AppearanceTypePtr AppearanceTypeList::getAppearance (unsigned long apprNum, const char *appearFile)
{
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
			std::printf("[LOADRACE] getAppearance NULL: reason=class0 apprNum=0x%08lx "
				"appearFile=%p(%s)\n", apprNum, (const void*)appearFile,
				appearFile ? appearFile : "<null>");
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
			std::printf("[LOADRACE] getAppearance NULL: reason=appearFileNull "
				"apprNum=0x%08lx class=%ld\n", apprNum, appearanceClass);
			std::fflush(stdout);
		}
		return nullptr;
	}

	//-----------------------------------------------------------
	// Scan the list of appearances and see if we have this one.
	appearanceType = head;
	while (appearanceType && S_stricmp(appearanceType->name,appearFile) != 0)
	{
		appearanceType = appearanceType->next;
	}

	if (appearanceType)
	{
		appearanceType->numUsers++;
	}
	else
	{
		//---------------------------------------------------------
		// This appearance is not on the list yet, so load/new it.	
		switch (appearanceClass)
		{
			//----------------------------------------
			case MECH_TYPE:
			{
				appearanceType = new Mech3DAppearanceType;
				gosASSERT(appearanceType != nullptr);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = nullptr;

				if (head == nullptr)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case GV_TYPE:
			{
				appearanceType = new GVAppearanceType;
				gosASSERT(appearanceType != nullptr);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = nullptr;

				if (head == nullptr)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;
			
			//----------------------------------------
			case TREED_TYPE:
			{
				appearanceType = new TreeAppearanceType;
				gosASSERT(appearanceType != nullptr);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = nullptr;

				if (head == nullptr)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case GENERIC_APPR_TYPE:
			{
				appearanceType = new GenericAppearanceType;
				gosASSERT(appearanceType != nullptr);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = nullptr;

				if (head == nullptr)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case BLDG_TYPE:
			{
				appearanceType = new BldgAppearanceType;
				gosASSERT(appearanceType != nullptr);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = nullptr;

				if (head == nullptr)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			default:
				return(nullptr);
		}
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
			key = key * 33 + (unsigned char)*p;
		if (mc2AuditOnce("APPR_TYPE_FAIL", key))
			mc2AuditLinef("[ASSET_LOAD v1] event=appr_type_fail name=%s apprNum=0x%08lx",
				appearFile, apprNum);
	}

	return appearanceType;
}
		
//---------------------------------------------------------------------------
long AppearanceTypeList::removeAppearance (AppearanceTypePtr which)
{
	AppearanceTypePtr appearanceType = head;
	AppearanceTypePtr previous = nullptr;
	
	while (appearanceType && (appearanceType != which))
	{
		previous = appearanceType;
		appearanceType = appearanceType->next;
	}

	if (appearanceType)
		appearanceType->numUsers--;
	else
		return(-1);
	
	//----------------------------------------------------------
	// NEVER remove the types anymore.  Save cache time!
//#if 0
	if (appearanceType && (appearanceType->numUsers == 0))
	{
		//------------------------------------------------------
		// Check if there is no previous appearanceType in list
		if (previous == nullptr)
		{
			//----------------------------------------------------
			// if there was no previous, head is the next in list
			head = appearanceType->next;
		}
		else
		{
			previous->next = appearanceType->next;
		}
		
		//-------------------------------------------------------------
		// Make sure that we don't gratuitously free the last pointer
		if (appearanceType == last)
			last = previous;
			
		delete appearanceType;
		appearanceType = nullptr;
	}
//#endif
	
	return NO_ERR;
}

//---------------------------------------------------------------------------
void AppearanceTypeList::destroy (void)
{
	//---------------------------------------------------------------------
	// Run through the list and force a destroy call for each list element.
	// This will free any RAM used by each individual appearance.
	// DO NOT really need to do this.  Just toss the heap!
	
	AppearanceTypePtr currentAppr = head;
	while (currentAppr)
	{
		AppearanceTypePtr nextPtr = currentAppr->next;
		currentAppr->destroy();
		currentAppr = nextPtr;
	}
	

	head = last = nullptr;
	
	delete appearanceHeap;
	appearanceHeap = nullptr;
	
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
bool AppearanceTypeList::pointerCanBeDeleted (void *ptr)
{
	if (appearanceHeap && appearanceHeap->pointerOnHeap(ptr))
		return true;

	return false;
}

//---------------------------------------------------------------------------
