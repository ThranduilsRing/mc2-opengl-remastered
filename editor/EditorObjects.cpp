/***************************************************************
* FILENAME: EditorObjects.cpp
* DESCRIPTION: Implements Editor object wrappers and Editor object behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#define EDITOROBJECTS_CPP
#include "stdafx.h"
#include "EditorObjects.h"

#ifndef INIFILE_H
#include "IniFile.h"
#endif

#ifndef OBJECTAPPEARANCE_H
#include "Objectappearance.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef BUILDINGLINK_H
#include "BuildingLink.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif

#include "MissionFitBrainSource.h"   // TECHBRAIN-PATROL-VIEWER-1: declarative Brain{} patrol import

#include "resource.h"

#include "PilotProfilePreserve.h"


#include "../ARM/Microsoft.Xna.Arm.h"

#include "EditorResourceFallback.h"
#include "Gates/gate.h"
using namespace Microsoft::Xna::Arm;
extern IProviderEngine* armProvider;


extern HSTRRES gameResourceHandle;

// PILOT-PROFILE-PRESERVE-1: loud, unconditional data-loss warning (rare;
// fires only when a save would blank a [WarriorN] Profile). Same log file
// as the MC2_EDITOR_TRACE probes, but NOT gated -- data loss must be seen.
static void PilotDataLossWarn( const char* msg )
{
	OutputDebugStringA( msg );
	OutputDebugStringA( "\n" );
	FILE* f = fopen( "editor-startup.log", "a" );
	if ( f )
	{
		fputs( "[PILOT-PROFILE-PRESERVE-1] WARNING: ", f );
		fputs( msg, f );
		fputc( '\n', f );
		fclose( f );
	}
}

// EDITOR-STATIC-DEDRAW: free an editor object's appearance, invalidating its static-prop
// RENDER recipe FIRST. Appearance::destroy() is virtual -> Bldg/TreeAppearance::destroy()
// call invalidateStaticRegistration() (clears the GpuStaticPropRegistry RecipeRange). A
// bare `delete appearance` runs only the (empty) ~ObjectAppearance and NEVER calls
// destroy(), so a deleted building/tree kept rendering as a ghost once placed props began
// registering static recipes (EDITOR-STATIC-TEXTURE-PREWARM-1). destroy() nulls what it
// frees, so the following delete is a safe memory free with no double-free.
static void EditorFreeObjectAppearance( ObjectAppearance*& app )
{
	if ( app )
	{
		app->destroy();
		delete app;
		app = nullptr;
	}
}


Pilot::PilotInfo Pilot::s_BadPilots[MAX_PILOT] = { nullptr };
Pilot::PilotInfo Pilot::s_GoodPilots[MAX_PILOT] = { nullptr };
long	Pilot::goodCount = 0;
long	Pilot::badCount = 0;

void *EditorObject::operator new (size_t mySize)
{
	void *result = nullptr;
	result = systemHeap->Malloc(mySize);
	
	return(result);
}

void EditorObject::operator delete (void *us)
{
	systemHeap->Free(us);
}

EditorObject::EditorObject()
{
	appearInfo = new AppearanceInfo();
	appearInfo->appearance = nullptr;
	appearInfo->refCount = 1;
	cellColumn = cellRow = id = 0;
	forestId = -1;
	scale = 1.0;
}
EditorObject::EditorObject( const EditorObject& src )
{
	if ( &src != this )
	{
		appearInfo = src.appearInfo;
		appearInfo->refCount++;
		cellColumn = src.cellColumn;
		cellRow = src.cellRow;
		id = src.id;
		forestId = src.forestId;
		scale = src.scale;
	}

}

EditorObject& EditorObject::operator=( const EditorObject& src )
{
	if ( &src != this )
	{
		/* "destruct" the current object first */
		if (appearInfo)
		{
			gosASSERT(appearInfo->refCount != 0);

			appearInfo->refCount --;
			if ( appearInfo->refCount < 1 )
			{
				EditorFreeObjectAppearance(appearInfo->appearance);
				appearInfo->appearance = nullptr;

				delete appearInfo;
				appearInfo = nullptr;
			}
		}

		appearInfo = src.appearInfo;
		appearInfo->refCount++;
		cellColumn = src.cellColumn;
		cellRow = src.cellRow;
		id = src.id;
		forestId = src.forestId;
		scale = src.scale;
	}

	return *this;
}

EditorObject::~EditorObject()
{ 
	if (appearInfo)
	{
		gosASSERT(appearInfo->refCount != 0);

		appearInfo->refCount --;
		if ( appearInfo->refCount < 1 )
		{
			EditorFreeObjectAppearance(appearInfo->appearance);
			appearInfo->appearance = nullptr;

			delete appearInfo;
			appearInfo = nullptr;
		}
	}
}

void EditorObject::setDamage( bool bDamage )
{
	appearance()->setDamage( bDamage );
//	appearance()->setDamageLvl( 1000000 );
}

bool EditorObject::getDamage( ) const
{
	return appearance()->damage ? true : false;
}

const char* EditorObject::getDisplayName() const
{
	return EditorObjectMgr::instance()->getObjectName( id );
}

void EditorObject::setAlignment( int align )
{
	if ( appearance()->teamId != align )
	{
		if (align == 8)			//New magical force Neutral buttons
			align = -1;
		appearance()->teamId = align;
		BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( this );

		if ( pLink )
		{
			pLink->SetParentAlignment( appearance()->teamId );
		}
	}
}

int EditorObject::getIndexInGroup() const
{
	return EditorObjectMgr::getIndexInGroup( id );
}

int EditorObject::getGroup() const
{
	return EditorObjectMgr::getGroup( id );
}

void EditorObject::setAppearance( int Group, int indexInGroup )
{
	if ( Group != EditorObjectMgr::getGroup( id ) || indexInGroup != EditorObjectMgr::getIndexInGroup( id ) )
	{
		
		AppearanceInfo* appearInfo2 = new AppearanceInfo();
		appearInfo2->appearance = EditorObjectMgr::instance()->getAppearance( Group, indexInGroup );
		appearInfo2->refCount = 1;
		static long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
		gosASSERT((8 > appearance()->teamId) && (-1 <= appearance()->teamId));
		appearInfo2->appearance->setObjectParameters( appearance()->position, appearance()->rotation, appearance()->selected, appearance()->teamId, homeRelations[appearance()->teamId+1]);
		appearInfo2->appearance->setDamage(appearance()->damage);
		id = EditorObjectMgr::instance()->getID( Group, indexInGroup );

		if (appearInfo)
		{
			gosASSERT(appearInfo->refCount != 0);

			appearInfo->refCount --;
			if ( appearInfo->refCount < 1 )
			{
				EditorFreeObjectAppearance(appearInfo->appearance);
				appearInfo->appearance = nullptr;
			
				delete appearInfo;
				appearInfo = nullptr;
			

			}
		}

		appearInfo = appearInfo2;
	}
}

void EditorObject::select( bool bSelect )
{ 
	EditorObjectMgr::instance()->select(*this, bSelect);
}

unsigned long EditorObject::getColor() const
{
	switch( appearInfo->appearance->teamId )
	{
	case EDITOR_TEAMNONE:
		return 0;
	case EDITOR_TEAM1:
		return SB_GREEN;
		break;
	case EDITOR_TEAM2:
		return SB_RED;
		break;
	case EDITOR_TEAM3:
		return SB_BLUE;
	case EDITOR_TEAM4:
		return SB_ORANGE;
	case EDITOR_TEAM5:
		return SB_WHITE;
	case EDITOR_TEAM6:
		return SB_GRAY;
	case EDITOR_TEAM7:
		return SB_BLACK;
	case EDITOR_TEAM8:
		return SB_YELLOW;
		break;
	
	default:
		gosASSERT( false );

	}

	return 0xffffffff;
}

void EditorObject::getCells( long& row, long& col ) const
{
	row = cellRow;
	col = cellColumn;
}

EditorObject::AppearanceInfo& EditorObject::AppearanceInfo::operator=( const AppearanceInfo& src )
{
	if ( &src != this )
	{
		// assuming ref count stays the same, since that refers to the graphic...
		appearance->rotation = src.appearance->rotation;
		appearance->position = src.appearance->position;
		appearance->barStatus = src.appearance->barStatus;
		appearance->damage = src.appearance->damage;
		appearance->fadeTable = src.appearance->fadeTable;
		appearance->paintScheme = src.appearance->paintScheme;
		appearance->selected = src.appearance->selected;
	}

	return *this;
}

int	EditorObject::getSpecialType() const
{
	return EditorObjectMgr::instance()->getSpecialType( getID() );
}



Unit::Unit( int align )
{
	pAlternativeInstances = new CUnitList;
	unsigned long squadNum = EditorObjectMgr::instance()->getNextAvailableSquadNum();
	setSquad(squadNum);
	EditorObjectMgr::instance()->registerSquadNum(squadNum);
	setSelfRepairBehaviorEnabled(true);
	pilot.info = align == 0 ? &Pilot::s_GoodPilots[0] : &Pilot::s_BadPilots[0];
	baseColor = 0x00ffffff;
	highlightColor = 0x00c0c0c0;
	highlightColor2 = 0x00808080;
	variant = 0;
	orderType = ORDER_NONE;
	stance = 2;   // AttitudeType ATTITUDE_NORMAL
	orderAuthored = false;
	importChecked = false;
	brainFsm[0] = 0;
	brainBehavior = BRAIN_UNKNOWN;
}

Unit::~Unit()
{
	delete pAlternativeInstances;
}

Unit::Unit( const Unit& src ) : EditorObject( src )
{
	
	if ( &src != this )
	{
		brain = src.brain;
		lance = src.lance;
		lanceIndex = src.lanceIndex;
		pilot = src.pilot;
		unsigned long color1, color2, color3;
		src.getColors(color1, color2, color3);
		setColors(color1, color2, color3);
		setSelfRepairBehaviorEnabled(src.getSelfRepairBehaviorEnabled());
		setVariant(src.getVariant());
		pAlternativeInstances = new CUnitList;
		(*pAlternativeInstances) = *(src.pAlternativeInstances);
		squad = src.squad;
		orderType = src.orderType;
		stance = src.stance;
		waypoints = src.waypoints;
		orderAuthored = src.orderAuthored;
		importChecked = src.importChecked;
		strncpy( brainFsm, src.brainFsm, sizeof( brainFsm ) ); brainFsm[sizeof( brainFsm ) - 1] = 0;
		brainBehavior = src.brainBehavior;
	}

	variant = 0;
}

void Unit::CastAndCopy(const EditorObject &master)
{
	const Unit *pCastedMaster = dynamic_cast<const Unit *>(&master);
	if (nullptr != pCastedMaster)
	{
		(*this) = (*pCastedMaster);
	}
	else
	{
		gosASSERT(false);
		(*((EditorObject *)this)) = (master);
	}
}

bool Unit::save( FitIniFile* file, int WarriorNumber )
{
	
	bool bIsVehicle = dynamic_cast<GVAppearance*>(appearance()) ? true : false;
	
	if ( !bIsVehicle )
		return Unit::save( file, WarriorNumber, 1, appearance()->teamId == EDITOR_TEAM1 ?
		"PM207300" : "PM101100" );

	else
		return Unit::save( file, WarriorNumber, 2, 
		appearance()->teamId == EDITOR_TEAM1 ? "pv20600" : "pv20500" );
}

bool Unit::save( FitIniFile* file, int WarriorNumber, int controlDataType, const char* objectProfile )
{
	if (mechAsset)
	{
		const char * iniFilename = EditorObjectMgr::instance()->getFileName( id );
		char buf[512] = {0};

		if (iniFilename && iniFilename[0])
		{
			strcpy(buf, "Data\\TGL\\");
			strcat(buf, iniFilename);
			strcat(buf, ".ini");

			IProviderAssetPtr objAssetPtr = armProvider->OpenAsset(buf,
								AssetType_Physical, ProviderType_Primary);

			if (getDisplayName()[0])
			{
				objAssetPtr->AddProperty("DisplayName", getDisplayName());
			}

			objAssetPtr->AddProperty("ObjectType", "Mech");
			objAssetPtr->Close();
		}

		mechAsset->AddProperty("Type", "Warrior");

		strcpy(buf, "Data\\Missions\\Profiles\\");
		strcat(buf, objectProfile);
		strcat(buf, ".fit");
		mechAsset->AddRelationship("ObjectProfile", buf);

		// iniFilename may be NULL for units whose type ID isn't in the editor
		// object database (e.g. a mech type not present in the current mod
		// config).  Guard both CSVFile and AppearanceFile relationships with
		// the same null check that already guards the TGL block above.
		if (iniFilename && iniFilename[0])
		{
			strcpy(buf, "Data\\Objects\\");
			strcat(buf, iniFilename);
			strcat(buf, ".cvs");
			mechAsset->AddRelationship("CSVFile", buf);

			strcpy(buf, "Data\\TGL\\");
			strcat(buf, iniFilename);
			strcat(buf, ".ini");
			mechAsset->AddRelationship("AppearanceFile", buf);
		}
	}

	pilot.save( file, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0 );
	brain.save( file, WarriorNumber, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0);

	char tmp[256];
	sprintf( tmp, "Part%ld", WarriorNumber );
	file->writeBlock( tmp );
	file->writeIdULong( "ObjectNumber", EditorObjectMgr::instance()->getFitID( id ) );
	file->writeIdULong( "ControlType", 2 );
	int playerNum = appearance()->teamId;
	file->writeIdBoolean( "PlayerPart", playerNum == EDITOR_TEAM1 ? true : false );
	file->writeIdChar( "MyIcon", 0 );
	int teamNum = EditorData::instance->PlayersRef().PlayerRef(playerNum).DefaultTeam();
	file->writeIdChar( "TeamID", teamNum );
	file->writeIdChar( "CommanderID",playerNum );
	file->writeIdULong( "Pilot", WarriorNumber );
	file->writeIdFloat( "PositionX", appearance()->position.x  );
	file->writeIdFloat( "PositionY", appearance()->position.y );
	file->writeIdFloat( "Rotation", appearance()->rotation );
	file->writeIdLong( "Active", 1 );
	file->writeIdLong( "Exists", 1 );
	file->writeIdFloat( "Damage", (float)((int)getDamage()) );
	file->writeIdULong( "BaseColor", baseColor );
	file->writeIdULong( "HighlightColor1", highlightColor );
	file->writeIdULong( "HighlightColor2", highlightColor2 );

	unsigned long tmpULong = 0;
	if (getSelfRepairBehaviorEnabled())
	{
		tmpULong = 1;
	}
	file->writeIdULong( "SelfRepairBehavior", tmpULong );

	file->writeIdULong( "ControlDataType", controlDataType );
	file->writeIdString( "ObjectProfile", objectProfile );
	// getFileName may return NULL for unknown object type IDs — write empty string rather than crash.
	const char* csvFileName = EditorObjectMgr::instance()->getFileName( id );
	file->writeIdString( "CSVFile", csvFileName ? csvFileName : "" );
	file->writeIdULong( "VariantNumber", variant );

	file->writeIdULong( "SquadNum", getSquad() );
	file->writeIdULong( "NumAlternatives", pAlternativeInstances->Count() );

	// Patrol/Move order authoring (additive — only written when the user has
	// AUTHORED an order in-editor, so stock missions whose patrol lives in the
	// brain .abl and was only imported for display are NOT rewritten).
	if ( orderAuthored && ( orderType != ORDER_NONE || !waypoints.empty() ) )
	{
		file->writeIdLong( "OrderType", orderType );
		file->writeIdLong( "OrderStance", stance );
		file->writeIdULong( "WaypointCount", (unsigned long)waypoints.size() );
		for ( size_t i = 0; i < waypoints.size(); ++i )
		{
			char k[64];
			sprintf( k, "Waypoint%uX", (unsigned)i ); file->writeIdFloat( k, waypoints[i].x );
			sprintf( k, "Waypoint%uY", (unsigned)i ); file->writeIdFloat( k, waypoints[i].y );
			sprintf( k, "Waypoint%uZ", (unsigned)i ); file->writeIdFloat( k, waypoints[i].z );
		}
	}

	return true;
}

bool Unit::load( FitIniFile* file, int warriorNumber )
{
	long result = 0;
	char tmp;
	file->readIdChar( "CommanderID", tmp );
	appearance()->teamId = tmp;
	file->readIdFloat( "PositionX", appearance()->position.x  );
	file->readIdFloat( "PositionY", appearance()->position.y );
	file->readIdFloat( "Rotation", appearance()->rotation );

	int tmpCellRow = 0;
	int tmpCellColumn = 0;
	land->worldToCell( appearance()->position, tmpCellRow, tmpCellColumn );
	cellRow = tmpCellRow;
	cellColumn = tmpCellColumn;

	float fDamage = 0.0;
	result = file->readIdFloat( "Damage", fDamage );
	if ((NO_ERR == result) && (0.0 != fDamage))
	{
		setDamage(true);
	}

	file->readIdULong( "BaseColor", baseColor );
	file->readIdULong( "HighlightColor1", highlightColor );
	file->readIdULong( "HighlightColor2", highlightColor2 );

	unsigned long tmpULong = 1;
	result = file->readIdULong( "SelfRepairBehavior", tmpULong );
	if ((NO_ERR == result) && (0 == tmpULong))
	{
		setSelfRepairBehaviorEnabled(false);
	}
	else
	{
		setSelfRepairBehaviorEnabled(true);
	}

	file->readIdULong( "VariantNumber", variant );

	unsigned long squadNum = 1;
	result = file->readIdULong("SquadNum", squadNum);
	if (NO_ERR != result) {
		// the unit should already have a valid default squad assigned at construction
	} else {
		setSquad(squadNum);
		EditorObjectMgr::instance()->registerSquadNum(squadNum);
	}

	tmpNumAlternativeInstances = 0;
	file->readIdULong( "NumAlternatives", tmpNumAlternativeInstances );
	tmpAlternativeStartIndex = 0;
	if (0 < tmpNumAlternativeInstances)
	{
		file->readIdULong( "AlternativeStartIndex", tmpAlternativeStartIndex );
	}

	// Patrol/Move order authoring (additive — absent in stock missions; defaults
	// to ORDER_NONE / Normal stance / no waypoints). Read from the Part block,
	// before the Warrior seekBlock below.
	orderType = ORDER_NONE;
	stance = 2;
	waypoints.clear();
	orderAuthored = false;
	importChecked = false;
	{
		long tmpOrder = ORDER_NONE;
		if ( NO_ERR == file->readIdLong( "OrderType", tmpOrder ) )
			orderType = (int)tmpOrder;
		long tmpStance = 2;
		if ( NO_ERR == file->readIdLong( "OrderStance", tmpStance ) )
			stance = (int)tmpStance;
		unsigned long wpCount = 0;
		if ( NO_ERR == file->readIdULong( "WaypointCount", wpCount ) )
		{
			// Editor-authored order present in the .fit -> it owns persistence.
			// (Leave importChecked false so brain fsm/behavior still get analyzed;
			// the patrol-import is guarded by orderAuthored/non-empty waypoints.)
			orderAuthored = true;
			for ( unsigned long i = 0; i < wpCount; ++i )
			{
				Stuff::Vector3D wp; wp.x = wp.y = wp.z = 0.0f;
				char k[64];
				sprintf( k, "Waypoint%luX", i ); file->readIdFloat( k, wp.x );
				sprintf( k, "Waypoint%luY", i ); file->readIdFloat( k, wp.y );
				sprintf( k, "Waypoint%luZ", i ); file->readIdFloat( k, wp.z );
				waypoints.push_back( wp );
			}
		}
	}

	char blockId[256];
	sprintf(blockId,"Warrior%d",warriorNumber);
	file->seekBlock(blockId);
	pilot.load( file, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0 );

	brain.load( file, warriorNumber);
	
	setColors(baseColor,highlightColor,highlightColor2);
	
	return true;
}

void Unit::getColors( unsigned long& color1, unsigned long& color2, unsigned long& color3 ) const
{
	color1 = baseColor;
	color2 = highlightColor;
	color3 = highlightColor2;
}

void Unit::setColors( unsigned long color1, unsigned long color2, unsigned long color3 ) 
{
	baseColor = color1;
	highlightColor = color2;
	highlightColor2 = color3;
	
	appearance()->resetPaintScheme(color2,color3,color1);
}

Unit& Unit::operator=( const Unit& src )
{
	if ( &src != this )
	{
		brain = src.brain;
		lance = src.lance;
		lanceIndex = src.lanceIndex;
		pilot = src.pilot;
		unsigned long color1, color2, color3;
		src.getColors(color1, color2, color3);
		setColors(color1, color2, color3);
		setSelfRepairBehaviorEnabled(src.getSelfRepairBehaviorEnabled());
		setVariant(src.getVariant());
		(*pAlternativeInstances) = *(src.pAlternativeInstances);
		squad = src.squad;
		orderType = src.orderType;
		stance = src.stance;
		waypoints = src.waypoints;
		orderAuthored = src.orderAuthored;
		importChecked = src.importChecked;
		strncpy( brainFsm, src.brainFsm, sizeof( brainFsm ) ); brainFsm[sizeof( brainFsm ) - 1] = 0;
		brainBehavior = src.brainBehavior;

		EditorObject::operator=( src );
	}

	return *this;
}

// Parse a patrol brain .abl for its startPatrolPath[i,0/1] waypoint literals.
// The unit's brain file (brainName.abl at warriorPath) is exactly the script the
// engine loads, so existing stock patrols live here as explicit XY coordinates:
//   startPatrolState[1] = <count>;
//   startPatrolPath[0, 0] = <x>;  startPatrolPath[0, 1] = <y>;  ...
// (also matches base variants like startBase1PatrolPath[i,j]). z is left 0; the
// overlay re-samples terrain elevation when drawing.
// Analyze a unit's brain .abl (the exact script the engine loads from
// <warriorPath><brainName>.abl): extract any patrol path (startPatrolPath[i,0/1]
// literals), the fsm name, and a coarse behavior tag. Returns true if the file
// opened (even with no patrol). fsmOut/behaviorOut are always written.
static bool EditorAnalyzeBrain( const char* brainName, std::vector<Stuff::Vector3D>& wpOut,
	char* fsmOut, size_t fsmCap, int& behaviorOut )
{
	wpOut.clear();
	if ( fsmOut && fsmCap ) fsmOut[0] = 0;
	behaviorOut = Unit::BRAIN_UNKNOWN;
	if ( !brainName || !brainName[0] )
		return false;

	char path[512];
	sprintf( path, "%s%s.abl", warriorPath, brainName );
	File f;
	long openRc = f.open( path );
	if ( mc2::gate::get<"MC2_EDITOR_TRACE">() )
	{
		fprintf( stderr, "[PATROL] open '%s' rc=%ld (warriorPath='%s' brain='%s')\n",
			path, openRc, warriorPath, brainName );
		fflush( stderr );
	}
	if ( NO_ERR != openRc )
		return false;

	std::vector<float> xs, ys;
	std::vector<unsigned char> hasX, hasY;
	bool sawPatrol = false, sawGuard = false, sawAttack = false;
	char line[1024];
	while ( !f.eof() )
	{
		long n = f.readLine( (MemoryPtr)line, (long)sizeof( line ) - 1 );
		if ( n <= 0 )
			break;
		if ( n >= (long)sizeof( line ) ) n = (long)sizeof( line ) - 1;
		line[n] = 0;

		// fsm name: "fsm <name>;" (first occurrence).
		if ( fsmOut && fsmCap && !fsmOut[0] )
		{
			const char* fp = strstr( line, "fsm " );
			if ( fp == line || ( fp && ( fp == line || fp[-1] == '\t' || fp[-1] == ' ' ) ) )
			{
				fp += 4;
				while ( *fp == ' ' || *fp == '\t' ) ++fp;
				size_t k = 0;
				while ( fp[k] && fp[k] != ';' && fp[k] != ' ' && fp[k] != '\t' && fp[k] != '\r' && fp[k] != '\n' && k < fsmCap - 1 )
				{ fsmOut[k] = fp[k]; ++k; }
				fsmOut[k] = 0;
			}
		}

		if ( strstr( line, "PATROL_TYPE" ) || strstr( line, "PatrolPath" ) ) sawPatrol = true;
		if ( strstr( line, "guard" ) || strstr( line, "Guard" ) )            sawGuard  = true;
		if ( strstr( line, "attack" ) || strstr( line, "Attack" ) )          sawAttack = true;

		const char* p = strstr( line, "PatrolPath[" );
		if ( !p )
			continue;
		p += 11;   // strlen("PatrolPath[")
		int idx = -1, comp = -1;
		if ( sscanf( p, "%d , %d", &idx, &comp ) != 2 &&
			 sscanf( p, "%d,%d", &idx, &comp ) != 2 )
			continue;
		const char* eq = strchr( p, '=' );
		if ( !eq )
			continue;
		float val = (float)atof( eq + 1 );
		if ( idx < 0 || idx > 4096 )
			continue;
		if ( comp == 0 )
		{
			if ( (int)xs.size() <= idx ) { xs.resize( idx + 1, 0.f ); hasX.resize( idx + 1, 0 ); }
			xs[idx] = val; hasX[idx] = 1;
		}
		else if ( comp == 1 )
		{
			if ( (int)ys.size() <= idx ) { ys.resize( idx + 1, 0.f ); hasY.resize( idx + 1, 0 ); }
			ys[idx] = val; hasY[idx] = 1;
		}
	}

	size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	for ( size_t i = 0; i < count; ++i )
		if ( i < hasX.size() && i < hasY.size() && hasX[i] && hasY[i] )
		{
			// .abl PatrolPath coords are raw world coords, same frame as unit
			// PositionX/Y (verified: mc2_01_Pat1 unit (3434,1983) sits next to its
			// startBase1PatrolPath[0]=(3008,1472); both mc2_01 and mc2_03 patrol coords
			// span the same +/- world range as their unit positions). No transform.
			Stuff::Vector3D wp; wp.x = xs[i]; wp.y = ys[i]; wp.z = 0.0f;
			wpOut.push_back( wp );
		}

	// Classify: patrol path wins; else guard/attack by keyword; else idle.
	if ( sawPatrol || !wpOut.empty() ) behaviorOut = Unit::BRAIN_PATROL;
	else if ( sawGuard )               behaviorOut = Unit::BRAIN_GUARD;
	else if ( sawAttack )              behaviorOut = Unit::BRAIN_ATTACK;
	else                               behaviorOut = Unit::BRAIN_IDLE;
	return true;
}

// TECHBRAIN-PATROL-VIEWER-1: declarative mission.fit Brain{} Primary-OPORD Patrol import.
// Matches this unit's spawn (ux,uy) to a parsed Brain record via MissionFitBrainSource --
// the SAME shared cache + position-match tolerance the "Declarative Brain" panel section
// uses (editor/UnitBrainPanel.cpp), so what the panel shows for a unit and what this seeds
// onto its waypoint list (and therefore what editor/EditorDebugOverlay.cpp's drawOnePatrol
// draws on the map) can never disagree. Returns true (and fills wpOut/loopOut) only when the
// matched brain's Primary OPORD is a waypointed Patrol; false leaves the caller to fall back
// to the legacy .abl PatrolPath[] literal scan (EditorAnalyzeBrain above), which remains the
// only source for missions with no declarative Brain{} block.
static bool EditorImportDeclarativePatrol( float ux, float uy,
	std::vector<Stuff::Vector3D>& wpOut, bool& loopOut )
{
	wpOut.clear();
	loopOut = false;
	MissionFitBrainSource::Refresh();
	const MissionFitBrain* b = MissionFitBrainSource::MatchForPos( ux, uy );
	if ( !b || 0 != strcmp( b->primary.type, "Patrol" ) || b->primary.waypointCount <= 0 )
		return false;
	for ( int i = 0; i < b->primary.waypointCount; ++i )
	{
		Stuff::Vector3D wp;
		wp.x = b->primary.waypoints[i].x;
		wp.y = b->primary.waypoints[i].y;
		wp.z = 0.0f;   // overlay re-samples terrain elevation when drawing, same as the .abl path
		wpOut.push_back( wp );
	}
	loopOut = b->primary.loop;
	return true;
}

void Unit::importPatrolFromBrainIfNeeded()
{
	if ( importChecked )
		return;
	importChecked = true;

	// .abl analysis always runs: it supplies brainFsm + a coarse brainBehavior tag for the
	// "AI Brain" panel section regardless of which source ends up supplying the patrol path.
	std::vector<Stuff::Vector3D> wps;
	EditorAnalyzeBrain( brain.getBrainName(), wps, brainFsm, sizeof( brainFsm ), brainBehavior );
	bool loop = true;   // legacy .abl patrols: always closed-loop (unchanged prior behavior)

	// Declarative mission.fit Brain{} Primary-OPORD Patrol is AUTHORITATIVE when this unit's
	// spawn position matches one (carver_v_enhanced content) -- overrides the .abl scan above.
	if ( appearance() )
	{
		std::vector<Stuff::Vector3D> declWps;
		bool declLoop = false;
		if ( EditorImportDeclarativePatrol( (float)appearance()->position.x,
			(float)appearance()->position.y, declWps, declLoop ) )
		{
			wps = declWps;
			loop = declLoop;
			brainBehavior = BRAIN_PATROL;
		}
	}

	// Import the patrol for display only when there is no editor-authored / existing order.
	if ( !( orderAuthored || !waypoints.empty() || orderType != ORDER_NONE ) && !wps.empty() )
	{
		waypoints = wps;
		orderType = loop ? ORDER_PATROL : ORDER_MOVE;
		// orderAuthored stays false -> display only; not rewritten on save.
	}

	// MC2_EDITOR_TRACE: dump unit position vs first imported waypoint so any
	// coordinate-frame mismatch can be read off directly.
	if ( mc2::gate::get<"MC2_EDITOR_TRACE">() && !wps.empty() && appearance() )
	{
		fprintf( stderr, "[PATROL] brain=%s unitPos=(%.0f,%.0f) wp0=(%.0f,%.0f) wpN=%u loop=%d\n",
			brain.getBrainName(), appearance()->position.x, appearance()->position.y,
			wps[0].x, wps[0].y, (unsigned)wps.size(), loop ? 1 : 0 );
		fflush( stderr );
	}
}

void Unit::setSquad(unsigned long newSquad) {
	squad = newSquad;
	CUnitList::EIterator iter = pAlternativeInstances->Begin();
	while (!iter.IsDone())
	{
		(*iter).setSquad(newSquad);
		iter++;
	}
}


bool DropZone::save( FitIniFile* file, int number )
{
	Stuff::Vector3D pos = getPosition();
	
	file->writeIdLong( "NumSlots", 4 );
	if ( bVTol )
	{
		file->writeIdBoolean( "IsVTOL", 1 );
		file->writeIdFloat( "PositionX", pos.x );
		file->writeIdFloat( "PositionY", pos.y );
	}

	else
	{

		file->writeIdFloat( "PositionX", pos.x );
		file->writeIdFloat( "PositionY", pos.y );
	
		file->writeIdFloat( "OffsetX0", 0 );
		file->writeIdFloat( "OffsetY0", 0 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -71 );
		file->writeIdFloat( "OffsetY0", 71 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -71 );
		file->writeIdFloat( "OffsetY0", -71 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -142 );
		file->writeIdFloat( "OffsetY0", 0 );
		file->writeIdFloat( "Rotation0", 45);

	}

	return true;
}

DropZone::DropZone( const Stuff::Vector3D& pos, int alignment, bool bvtol )
{
	bVTol = bvtol;
}

void DropZone::CastAndCopy(const EditorObject &master)
{
	const DropZone *pCastedMaster = dynamic_cast<const DropZone *>(&master);
	if (nullptr != pCastedMaster)
	{
		(*this) = (*pCastedMaster);
	}
	else
	{
		gosASSERT(false);
		(*((EditorObject *)this)) = (master);
	}
}

bool Brain::save( FitIniFile* file, int warriorNumber, bool bPlayer )
{
	if (brainName[0])
	{
		file->writeIdString( "Brain", brainName);
		file->writeIdLong( "NumCells", numCells );
		file->writeIdLong( "NumStaticVars", numStaticVars );
		
		char text[256];
		for (long i=0;i<numCells;i++)
		{
			sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
			file->writeBlock(text);
			file->writeIdLong("Cell",  cellNum[i]);
			file->writeIdLong("MemType", cellType[i]);
			file->writeIdFloat("Value", cellData[i]);
		}

		if (mechAsset && brainName[0])
		{
			char buf[512] = {0};
			strcpy(buf, "Data\\Missions\\Warriors\\");
			strcat(buf, brainName);
			strcat(buf, ".abl");
			mechAsset->AddRelationship("Brain", buf);
		}
	}
	else
	{
		if (!bPlayer)
		{
			file->writeIdString( "Brain", "DredAttack01");
			file->writeIdLong( "NumCells", 3 );
			file->writeIdLong( "NumStaticVars", 0 );
			
			char text[256];
			for (long i=0;i<3;i++)
			{
				sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
				file->writeBlock(text);
				
				switch (i)
				{
					case 0:
						file->writeIdLong("Cell",  2);
						file->writeIdLong("MemType", 1);
						file->writeIdFloat("Value", 150);
					break;
					
					case 1:
						file->writeIdLong("Cell",  9);
						file->writeIdLong("MemType", 0);
						file->writeIdFloat("Value", 0.0f);
					break;
					
					case 2:
						file->writeIdLong("Cell",  28);
						file->writeIdLong("MemType", 0);
						file->writeIdFloat("Value", 0.0f);
					break;
				}
			}
		}
		else
		{
			file->writeIdString( "Brain", "PBrain");
			file->writeIdLong( "NumCells", 0 );
			file->writeIdLong( "NumStaticVars", 0 );
		}
	}

	return true;
}

bool Brain::load( FitIniFile* file, int warriorNumber )
{
	file->readIdLong( "NumCells", numCells );
	file->readIdLong( "NumStaticVars", numStaticVars );
	file->readIdString( "Brain", brainName, 255);

	// if not explicitly set, then redo
	if ( strcmp( brainName, "PBrain" ) == 0 )
		brainName[0] = 0;
	else if ( stricmp( brainName, "DredAttack01" ) == 0 )
		brainName[0] = 0;

	if (0 < numCells)
	{
		cellNum = (long *)malloc(sizeof(long) * numCells);
		cellType = (long *)malloc(sizeof(long) * numCells); 
		cellData = (float *)malloc(sizeof(float) * numCells);
	}
	else
	{
		cellNum = (long *)nullptr;
		cellType = (long *)nullptr; 
		cellData = (float *)nullptr;
	}

	char text[256];
	for (long i=0;i<numCells;i++)
	{
		sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
		
		file->seekBlock(text);
		file->readIdLong("Cell",  cellNum[i]);
		file->readIdLong("MemType", cellType[i]);
		file->readIdFloat("Value", cellData[i]);
	}
	
	return true;
}

Brain::Brain( const Brain& src )
{
	numCells = src.numCells;
	numStaticVars = src.numStaticVars;
	strcpy( brainName, src.brainName );

	cellNum = (long *)malloc(sizeof(long) * numCells);
	cellType = (long *)malloc(sizeof(long) * numCells); 
	cellData = (float *)malloc(sizeof(float) * numCells);


	for (long i=0;i<numCells;i++)
	{
		cellNum[i] = src.cellNum[i];
		cellType[i] = src.cellType[i];
		cellData[i] = src.cellData[i];
	}
	
}
Brain& Brain::operator=( const Brain& src )
{
	if ( &src != this )
	{
		numCells = src.numCells;
		numStaticVars = src.numStaticVars;
		strcpy( brainName, src.brainName );

		if (0 < numCells)
		{
			cellNum = (long *)malloc(sizeof(long) * numCells);
			cellType = (long *)malloc(sizeof(long) * numCells); 
			cellData = (float *)malloc(sizeof(float) * numCells);

			for (long i=0;i<numCells;i++)
			{
				cellNum[i] = src.cellNum[i];
				cellType[i] = src.cellType[i];
				cellData[i] = src.cellData[i];
			}
		}
		else
		{
			cellNum = (long *)nullptr;
			cellType = (long *)nullptr; 
			cellData = (float *)nullptr;
		}
	}

	return *this;
	
}

void Pilot::save( FitIniFile* file, int bGoodGuy )
{
	// PILOT-PROFILE-PRESERVE-1 (QUICKSTART-PILOT-DROP-RECON-1): never silently
	// blank a Profile that existed on disk. When initPilots() loads zero
	// pilots (pilots.csv zlib-compressed inside the FST / warriors/*.fit
	// missing), every Pilot::load falls back to s_GoodPilots[0] whose
	// fileName is NULL and the old code wrote "" for EVERY warrior (the
	// Bubba x3 quickstart data loss). rawProfile carries the string read
	// from the mission .fit and wins; the catalog match covers freshly
	// placed units; "" remains the crash-safe last resort (42d4aa9e), but
	// now with a loud warning.
	bool dataLoss = false;
	const char* profileName =
		PilotProfileForSave( rawProfile, info ? info->fileName : nullptr, &dataLoss );
	if ( dataLoss )
		PilotDataLossWarn(
			"Pilot::save: writing empty Profile (no raw string from load, no "
			"pilots.csv catalog match) -- possible editor data loss" );
	file->writeIdString( "Profile", profileName );

	if (mechAsset && info && info->fileName && info->fileName[0])
	{
		char buf[512] = {0};
		strcpy(buf, "Data\\Missions\\Warriors\\");
		strcat(buf, info->fileName);
		strcat(buf, ".fit");
		mechAsset->AddRelationship("Profile", buf);
	}
}

void Pilot::load( FitIniFile* file, int bGoodGuy )
{
	long result = 0;
	char buffer[256];
	result = file->readIdString( "Profile", buffer, 256 );

	// PILOT-PROFILE-PRESERVE-1: stash the raw on-disk string so save can
	// round-trip it even when the catalog match below fails (see
	// Pilot::save). Cleared on read failure so a fresh unit stays empty.
	if ( NO_ERR == result )
	{
		strncpy( rawProfile, buffer, sizeof( rawProfile ) - 1 );
		rawProfile[sizeof( rawProfile ) - 1] = 0;
	}
	else
		rawProfile[0] = 0;

	bool bFound = 0;
	if (NO_ERR == result)
	{
		for ( int i = 0; i < goodCount; i++ )
		{
			if ( stricmp( buffer, s_GoodPilots[i].fileName ) == 0 )
			{
				info = &s_GoodPilots[i];
				bFound = 1;
				break;
			}
		}

		if ( !bFound )
		{
			for ( int i = 0; i < badCount; i++ )
			{
				if ( stricmp( buffer, s_BadPilots[i].fileName ) == 0 )
				{
					info = &s_BadPilots[i];
					bFound = 1;
					break;
				}
			}
		}
	}

	if ( !bFound )
	{
		info = &s_GoodPilots[0];
	}

	if ( !info )
	{
		Assert( 0, 0, "reassigning invalid pilot" );
		info = bGoodGuy ? &s_GoodPilots[0] : &s_BadPilots[0];
	}
	
 	
}

void Pilot::initPilots()
{
	CSVFile file;

	char path[256];
	strcpy( path, objectPath );
	strcat( path, "pilots.csv" );

	if ( NO_ERR != file.open( path ) )
	{
		STOP(( "couldn't find pilots.csv file" ));
		return;
	}

	char pilotFileName[256];
	strcpy(pilotFileName, "");

	PilotInfo* infos = s_GoodPilots;
	long* counter = &goodCount;

	for ( int i = 0; i < 2; i++ )
	{
		while( true )
		{
			int bytesRead = file.readLine( (BYTE*)pilotFileName, 256 );

			if ( bytesRead < 2 )
				break;

			// Guard: if the FST reader ever leaks compressed bytes (historic
			// 78 9c deflate leak), a non-printable byte in the name means the
			// data is corrupt/binary — stop loading.
			// EDITOR-PILOT-CATALOG-FST-1: scan only up to the NUL terminator.
			// File::readLine returns a count that INCLUDES the '\0' it appends,
			// so the old `ci < bytesRead` loop always hit the terminator and
			// rejected EVERY line (goodCount=0 -> empty pilot catalog).
			{
				bool validName = true;
				for ( int ci = 0; ci < bytesRead && pilotFileName[ci] != 0; ++ci )
				{
					unsigned char c = (unsigned char)pilotFileName[ci];
					if ( c < 0x20 || c > 0x7e ) { validName = false; break; }
				}
				if ( !validName )
				{
					// EDITOR-PILOT-CATALOG-FST-1: loud witness — this used to fire
					// on zlib bytes leaking through the FST reader (goodCount=0
					// -> empty pilot picker -> PILOT-PROFILE-PRESERVE-1 fallbacks).
					FILE* f = fopen( "editor-startup.log", "a" );
					if ( f )
					{
						fprintf( f, "[PILOT-CATALOG] corrupt line (pass %d), first bytes %02x %02x %02x %02x -- aborting csv read\n",
							i, (unsigned char)pilotFileName[0],
							bytesRead > 1 ? (unsigned char)pilotFileName[1] : 0,
							bytesRead > 2 ? (unsigned char)pilotFileName[2] : 0,
							bytesRead > 3 ? (unsigned char)pilotFileName[3] : 0 );
						fclose( f );
					}
					break;
				}
			}

			CString postFix;
			if (0 == i)
			{
				if ((strlen(pilotFileName) > strlen("pmw"))
					&& (0 == strnicmp(pilotFileName, "pmw", strlen("pmw"))))
				{
					/*Good pilots that start with "pmw" are single-player pilots.*/
					CString tmpStr;
					tmpStr.LoadString(IDS_PILOT_SINGLE_PLAYER_VERSION);
					postFix = tmpStr.GetBuffer(0);
				}
				else if ((strlen(pilotFileName) > strlen("pmp_"))
					&& (0 == strnicmp(pilotFileName, "pmp_", strlen("pmp_"))))
				{
					/*Good pilots that start with "pmp_" are multi-player pilots.*/
					CString tmpStr;
					tmpStr.LoadString(IDS_PILOT_MULTIPLAYER_VERSION);
					postFix = tmpStr.GetBuffer(0);
				}
			}

			FitIniFile pilotFile;
			FullPathFileName tmpPath;
			tmpPath.init(warriorPath,pilotFileName,".fit");

			if ( NO_ERR != pilotFile.open( tmpPath ) )
			{
				// Pilot .fit missing — skip rather than crash.  The editor's pilot
				// assignment dialog will just have fewer options.
				continue;
			}

			infos[*counter].fileName = new char[strlen( pilotFileName ) + 1];
			strcpy( infos[*counter].fileName, pilotFileName );

			int result = pilotFile.seekBlock( "General" );
			gosASSERT( result == 0 );

			long tmp;
			result = pilotFile.readIdLong( "descIndex", tmp );
			gosASSERT( result == NO_ERR );

			EditorSafeLoadString( tmp, pilotFileName, 64);
			strcat(pilotFileName, "  ");
			strncat(pilotFileName, postFix.GetBuffer(0), 64);

			infos[*counter].name = new char[strlen( pilotFileName  ) +1];
			strcpy( infos[*counter].name, pilotFileName );
			
			(*counter)++;

			if ( goodCount > MAX_PILOT )
				return;
		}

		file.close();
		FullPathFileName path;
		path.init(objectPath,"BadPilots",".csv");

		if ( NO_ERR != file.open( path ) )
		{
			STOP(( "couldn't find BadPilots.csv file" ));
			return;
		}

		infos = s_BadPilots;
		counter = &badCount;

	}

	// EDITOR-PILOT-CATALOG-FST-1: startup witness. good must be > 0 for the
	// pilot picker / Pilot::load catalog matching to work at all.
	{
		FILE* f = fopen( "editor-startup.log", "a" );
		if ( f )
		{
			fprintf( f, "[PILOT-CATALOG] good=%ld bad=%ld\n", goodCount, badCount );
			fclose( f );
		}
	}
}

void Pilot::setName( const char* newName )
{
	// PILOT-PROFILE-PRESERVE-1: a user reassignment must beat the stashed
	// on-disk string in Pilot::save, so sync rawProfile with the new match.
	int i = 0;
	for ( i = 0; i < goodCount; i++ )
	{
		if ( stricmp( newName, s_GoodPilots[i].name ) == 0 )
		{
			info = &s_GoodPilots[i];
			syncRawProfile();
			return;
		}
	}

	for ( i = 0; i < badCount; i++ )
	{
		if ( stricmp( newName, s_BadPilots[i].name ) == 0 )
		{
			info = &s_BadPilots[i];
			syncRawProfile();
			return;
		}
	}

	gosASSERT( 0 );

}

bool NavMarker::save( FitIniFile* file, int warriorNumber )
{ 
	char text[32];
	sprintf( text, "NavMarker%ld", warriorNumber );
	file->writeBlock( text );

	file->writeIdFloat( "xPos", appearance()->position.x );
	file->writeIdFloat( "yPos", appearance()->position.y );
	file->writeIdFloat( "radius", 256.f );
	return true; 
}

NavMarker::NavMarker()
{
}


