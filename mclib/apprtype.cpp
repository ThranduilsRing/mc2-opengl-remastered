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

#include<cstdlib>
#include<cstdio>

//---------------------------------------------------------------------------
// static Globals
UserHeapPtr AppearanceTypeList::appearanceHeap = NULL;
AppearanceTypeListPtr appearanceTypeList = NULL;

//---------------------------------------------------------------------------
// MC2_LOAD_RACE_TRACE: gated, one-shot mission-load residency-race diagnostics.
// Default-OFF; produces zero output unless the gate is set. These globals are
// the shared counters read by the per-run verdict in gameosmain.cpp and written
// by the appearance (apprtype/mech3d) and terrain-residency (txmmgr) paths.
// See docs/tier1_env_vars.md.
bool g_loadRaceTrace                     = (getenv("MC2_LOAD_RACE_TRACE") != nullptr);
long g_loadRaceApprNull                  = 0;    // getAppearance() NULL returns
long g_loadRaceMechDegrade               = 0;    // Mech3DAppearance::init degrade-branch hits
long g_loadRaceMechDegradeReal           = 0;    // ...of which real actors (obj != NULL)
long g_loadRaceMechOkReal                = 0;    // real actors (obj != NULL) that built a shape
long g_loadRaceMechOkPreview             = 0;    // preview/logistics (obj == NULL) built a shape
long g_loadRaceTerrainFirstResidentFrame = -1;   // first frame all terrain textures GPU-resident
bool g_loadRaceTerrainResidentNow        = false;// terrain fully resident as of last flush

//---------------------------------------------------------------------------
// Class AppearanceType
void* AppearanceType::operator new (size_t memSize)
{
	void* result = NULL;
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
	name = NULL;
}

//---------------------------------------------------------------------------
// class AppearanceTypeList
void AppearanceTypeList::init (unsigned long heapSize)
{
	appearanceHeap = new UserHeap;
	gosASSERT(appearanceHeap != NULL);

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
	AppearanceTypePtr appearanceType = NULL;
	
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
		return(NULL);
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
		return NULL;
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
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
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
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
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
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
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
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
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
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
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
				return(NULL);
		}
	}
	
	return appearanceType;
}
		
//---------------------------------------------------------------------------
long AppearanceTypeList::removeAppearance (AppearanceTypePtr which)
{
	AppearanceTypePtr appearanceType = head;
	AppearanceTypePtr previous = NULL;
	
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
		if (previous == NULL)
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
		appearanceType = NULL;
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
	

	head = last = NULL;
	
	delete appearanceHeap;
	appearanceHeap = NULL;
	
 	if (GVAppearanceType::SensorCircleShape)
	{
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = NULL;
	}
	
	if (GVAppearanceType::SensorTriangleShape)
	{
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = NULL;
	}
	
	if (Mech3DAppearanceType::SensorSquareShape)
	{
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = NULL;
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
