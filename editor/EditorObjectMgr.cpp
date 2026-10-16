/***************************************************************
* FILENAME: EditorObjectMgr.cpp
* DESCRIPTION: Implements Editor object catalog loading, naming, and object management.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added screen-space single-click object picking while preserving drag-select behavior.
****************************************************************/

#include "EditorObjectMgr_internal.h"
#include "Gates/gate.h"

void EditorObjMgrTrace(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	// Always feed crash ring — visible in last_trace.txt on CTD.
	crashbundle_append(buf);

	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}


EditorObjectMgr*	EditorObjectMgr::s_instance = nullptr;

// by Methuselas: the legacy "mc2res.dll:N Not defined" sentinel means missing
// Editor catalog text. Treat it as absent data and generate safe UI names;
// never display the sentinel as a real object or group label.
static bool EditorObjectMgr_IsMissingEditorText(const char* text)
{
	if (!text || !text[0])
		return true;

	return (strstr(text, "mc2res.dll:") != nullptr) ||
		   (strstr(text, "Not defined") != nullptr);
}

static void EditorObjectMgr_CopySafeName(char* dst, long dstLen, const char* src, const char* fallbackPrefix, long fallbackValue)
{
	if (!dst || dstLen <= 0)
		return;

	if (src && src[0] && !EditorObjectMgr_IsMissingEditorText(src))
	{
		strncpy(dst, src, dstLen - 1);
		dst[dstLen - 1] = 0;
		return;
	}

	_snprintf(dst, dstLen, "%s_%ld", fallbackPrefix ? fallbackPrefix : "Unnamed", fallbackValue);
	dst[dstLen - 1] = 0;
}


inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}


EditorObjectMgr::EditorObjectMgr()
{ 
	gosASSERT( !s_instance ); 
	s_instance = this;   // should only be one of these
}

EditorObjectMgr::~EditorObjectMgr()
{
	clear();

	if ( appearanceTypeList )
		delete appearanceTypeList;
	appearanceTypeList = nullptr;

	for( auto groupIter = groups.begin(); groupIter != groups.end(); groupIter++ )
	{

		for( auto buildIter = (*groupIter).buildings.begin(); buildIter != (*groupIter).buildings.end(); buildIter++ )
		{
			if (  (*buildIter ).varNames )
			{
				for ( int i = 0; i < 16; i++ )
				{
					if ( (*buildIter).varNames[i] )
					{
						free ( ( *buildIter).varNames[i] );
						(*buildIter).varNames[i] = nullptr;
					}
				}

				free ( ( *buildIter ).varNames );
				( *buildIter ).varNames = nullptr;
			}
		}
	}
	s_instance = nullptr;
}

void EditorObjectMgr::clear()
{
		for ( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			delete *iter;
		}

		for ( auto mIter = units.begin();
			mIter != units.end(); mIter++ )
			{
				Unit* pMech = *mIter;
				delete pMech;
			}

			
		buildings.clear();
		units.clear();

		for ( auto liter = links.begin(); liter != links.end(); liter++ )
			delete *liter;

		links.clear();

		selectedObjects.clear();
		selectedObjectsNeedsToBeSynched = false;

		for ( auto dIter = dropZones.begin(); dIter != dropZones.end(); dIter++ )
			delete *dIter;

		dropZones.clear();
		
		resetAvailableSquadNums();
		
	for( auto groupIter = groups.begin(); groupIter != groups.end(); groupIter++ )
	{
		for( auto buildIter = (*groupIter).buildings.begin(); buildIter != (*groupIter).buildings.end(); buildIter++ )
		{
			(*buildIter).appearanceType = nullptr;
		}
	}

	for ( auto fIter = forests.begin(); fIter != forests.end(); fIter++ )
	{
		delete ( *fIter );
	}

	forests.clear();

}

void EditorObjectMgr::init( const char* bldgListFileName, const char* objectFileName )
{
	File lstFile;

	lstFile.open( const_cast< char* >(bldgListFileName) );

	unsigned char buffer[512];

	lstFile.readLine( buffer, 512 );
	PacketFile objectFile;

	long result = objectFile.open( const_cast< char* >(objectFileName) );
	gosASSERT(result == NO_ERR);
		

	while( lstFile.getLogicalPosition() < lstFile.getLength() )
	{
		
		
		lstFile.readLine( buffer, 512 );

		if ( !strlen( (char*)buffer ) )
			break;

		Building bldg;
		memset(&bldg, 0, sizeof(Building));
		strcpy(bldg.name, "Unnamed_Object");
		strcpy(bldg.fileName, "");
		strcpy(bldg.tgaName, "NONE");
		bldg.appearanceType = nullptr;
		bldg.specialType = UNSPECIAL;
		bldg.varNames = nullptr;
		bldg.forestId = 0;
		bldg.scale = 1.0f;
		
		char tmpBuffer[64];
		int  bufferLength = 64;
		unsigned char* pCur = buffer;
		char GroupName[64];
		strcpy(GroupName, "Group_0");

		if (ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
			strcpy( bldg.fileName, tmpBuffer );

		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			int group = atoi( tmpBuffer );

			// Object menu group labels are editor-sandbox resources now.
			// Do not require mc2res.dll for the Objects menu.
			if (!EditorResourceCatalog::GetObjectGroupNameByIndex(group, GroupName, 64) || !GroupName[0])
				EditorSafeLoadString( IDS_GROUPS + group, GroupName, 64, gameResourceHandle );

			EditorObjectMgr_CopySafeName(GroupName, 64, GroupName, "Group", group);
		}
	
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			int strId = atoi( tmpBuffer );
			bldg.nameID = strId;
			EditorSafeLoadString( strId, tmpBuffer, 64 );
			EditorObjectMgr_CopySafeName(bldg.name, 64, tmpBuffer, bldg.fileName[0] ? bldg.fileName : "Object", strId);
		}

		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			if ( stricmp( tmpBuffer, "BUILDING" ) == 0 )
				bldg.type = BLDG_TYPE;
			else if ( stricmp( tmpBuffer, "TREE" ) == 0 )
				bldg.type = TREED_TYPE;
			else if ( stricmp( tmpBuffer, "VEHICLE" ) == 0 )
				bldg.type = GV_TYPE;
			else 
				bldg.type = MECH_TYPE;
		}

		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			bldg.fitID = atoi( tmpBuffer );
			if ( NO_ERR != objectFile.seekPacket(bldg.fitID) )
				gosASSERT( false );

			int fileSize = objectFile.getPacketSize();

			FitIniFile bldgFile;
			result = bldgFile.open(&objectFile, fileSize);

			unsigned long lowTemplate, highTemplate;
			bldgFile.readIdULong( "LowTemplate", lowTemplate );
			bldgFile.readIdULong( "HighTemplate", highTemplate );

			bldg.impassability = ((unsigned __int64)highTemplate << 32) | (unsigned __int64)lowTemplate;
			bldgFile.readIdBoolean( "BlocksLineOfFire", bldg.blocksLineOfFire );
			bldgFile.seekBlock("ObjectClass");				
			bldgFile.readIdLong("ObjectTypeNum", bldg.objectTypeNum);
			bldgFile.seekBlock("General");				
			bldg.isHoverCraft = false;
			bldgFile.readIdBoolean("HoverCraft", bldg.isHoverCraft);
			bldgFile.seekBlock("ObjectType");				
			char tmpBuf[2000];
			strcpy(tmpBuf, "");
			bldgFile.readIdString("Name", tmpBuf, 1000);
			if (EditorObjectMgr_IsMissingEditorText(bldg.name) && tmpBuf[0])
				EditorObjectMgr_CopySafeName(bldg.name, 64, tmpBuf, bldg.fileName[0] ? bldg.fileName : "Object", bldg.fitID);
			if (0 == strcmp(tmpBuf, "monsoon")) {
				int i = 11;
			}
			bldgFile.close();
		}

			
		if ( ExtractNextString( pCur,  tmpBuffer, bufferLength ) )
		{
			if ( strstr( tmpBuffer, "TREE" ) )
			{
				bldg.forestId = atoi( tmpBuffer + 4 );
			}
			else if ( stricmp( tmpBuffer, "GATE" ) == 0 )
				bldg.specialType = EDITOR_GATE;
			else if ( stricmp( tmpBuffer, "GATE_CONTROL" ) == 0 )
				bldg.specialType = GATE_CONTROL;
			else if ( stricmp( tmpBuffer, "POWER_GENERATOR" ) == 0 )
				bldg.specialType = POWER_STATION;
			else if ( stricmp( tmpBuffer, "SENSOR_CONTROL" ) == 0 )
				bldg.specialType = SENSOR_CONTROL;
			else if ( stricmp( tmpBuffer, "SENSOR" ) == 0 )
				bldg.specialType = SENSOR_TOWER;
			else if ( stricmp( tmpBuffer, "SPOTLIGHT_CONTROL" ) == 0 )
				bldg.specialType = SPOTLIGHT_CONTROL;
			else if ( stricmp( tmpBuffer, "SPOTLIGHT" ) == 0 )
				bldg.specialType = SPOTLIGHT;
			else if ( stricmp( tmpBuffer, "LOOKOUT" ) == 0 )
				bldg.specialType = LOOKOUT;
			else if ( stricmp( tmpBuffer, "TURRET_CONTROL" ) == 0 )
				bldg.specialType = TURRET_CONTROL;
			else if ( stricmp( tmpBuffer, "TURRET_GENERATOR" ) == 0 )
				bldg.specialType = TURRET_GENERATOR;
			else if ( stricmp( tmpBuffer, "BRIDGE_CONTROL" ) == 0 )
				bldg.specialType = BRIDGE_CONTROL;
			else if ( stricmp( tmpBuffer, "BRIDGE" ) == 0 )
				bldg.specialType = EDITOR_BRIDGE;		
			else if ( stricmp( tmpBuffer, "DROPZONE" ) == 0 )
				bldg.specialType = DROPZONE;
			else if ( stricmp( tmpBuffer, "TURRET" ) == 0 )
				bldg.specialType = EDITOR_TURRET;
			else if ( stricmp( tmpBuffer, "NAVMARKER" ) == 0 )
				bldg.specialType = NAV_MARKER;
			else if ( stricmp( tmpBuffer, "WALL" ) == 0 )
				bldg.specialType = WALL;
			else if ( stricmp( tmpBuffer, "RPBUILDING" ) == 0 )
				bldg.specialType = RESOURCE_BUILDING;
			else if ( stricmp( tmpBuffer, "HELICOPTER" ) == 0 ) 
				bldg.specialType = HELICOPTER;
		}

		bldg.capturable = ExtractNextInt( pCur ) ? true : false;
		bldg.alignable = ExtractNextInt( pCur ) ? true : false;
		bldg.writeOnTacMap = ExtractNextInt( pCur );
		
		if (ExtractNextString(pCur, tmpBuffer, bufferLength))
			strcpy(bldg.tgaName,tmpBuffer);
		else
			strcpy(bldg.tgaName,"NONE");

		ExtractNextFloat( pCur );
		bldg.scale = ExtractNextFloat( pCur );

		CSVFile csvFile;
		FullPathFileName csvFileName;
		csvFileName.init(objectPath,bldg.fileName,".csv");

		char varName[256];
		if ( NO_ERR == csvFile.open( csvFileName ) )
		{
			bldg.varNames = (char**) malloc( sizeof( char* ) * 16 );
			for ( int i = 0; i < 16; ++i )
			{
				if ( NO_ERR == csvFile.readString( 23 + 97 * i, 2, varName, 256 ) && strlen( varName ) )
				{
					bldg.varNames[i] = (char *) malloc( sizeof( char ) * ( strlen( varName ) + 1 ) );
					strcpy( bldg.varNames[i], varName );
				}
				else
					bldg.varNames[i] = nullptr;

			}
		}


		if ( bldg.specialType == DROPZONE )
		{
			strcpy( dropZoneGroup.name, GroupName );
			dropZoneGroup.buildings.push_back( bldg );
		}
		else
		{
			bool bNeedGroup = true;		
			for ( auto iter = groups.begin(); iter != groups.end(); iter++ )
			{
				if  ( strcmp( (*iter).name, GroupName ) == 0 )
				{
					bNeedGroup = false;
					(*iter).buildings.push_back( bldg );
				}
			}
			if ( bNeedGroup )
			{
				Group grp;
				strcpy( grp.name, GroupName );
				grp.buildings.push_back( bldg );
				groups.push_back( grp );
			}
		}
	}
}



int EditorObjectMgr::ExtractNextString( unsigned char*& pFileLine, char* pBuffer, int bufferLength )
{
	int i = 0;
	for ( ; i < 512; ++i )
	{
		if ( pFileLine[i] == '\n' )
			break;
		else if ( pFileLine[i] == ',' )
			break;
		else if ( pFileLine[i] == NULL )
			break;
	}

	if ( i == 512 )
		return false;

	gosASSERT( i < bufferLength );
	memcpy( pBuffer, pFileLine, i );
	pBuffer[i] = NULL;
	bufferLength = i + 1;
	pFileLine += i + 1;

	return i;

}

long textToLong (const char *num)
{
	long result = 0;
	
	char *hexOffset = (char*)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = (long)strlen(hexOffset)-1;
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = (long)strlen(hexOffset)-1;
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}

	
	return(result);
}

int EditorObjectMgr::ExtractNextInt( unsigned char*& pFileLine )
{
	char buffer[1024];

	int count = ExtractNextString( pFileLine, buffer, 1024 );

	if ( count > 0 )
	{
		return textToLong( buffer );
	}

	return -1;
}

float EditorObjectMgr::ExtractNextFloat( unsigned char*& pFileLine )
{
	char buffer[1024];

	int count = ExtractNextString( pFileLine, buffer, 1024 );

	if ( count > 0 )
	{
		return atof( buffer );
	}

	return -1;
}


bool EditorObjectMgr::canAddBuilding( const Stuff::Vector3D& position,
								float rotation,
								unsigned long group,
								unsigned long indexWithinGroup )
{
	
	if ( !land )
		return false;  // no terrain -> worldToCell derefs NULL

	int realCellI, realCellJ;
	land->worldToCell( position, realCellJ, realCellI );

	EditorObject * cellObject = getObjectAtCell( realCellJ, realCellI );

	if ( cellObject )
	{
		// appearance() can be NULL; guard before ->rotation.
		if (cellObject->appearance() && cellObject->appearance()->rotation == rotation )
			return false;
	}
	
	return land->IsEditorSelectTerrainPosition(position);
}

bool EditorObjectMgr::canAddDropZone( const Stuff::Vector3D& position, int alignment, bool bVTol )
{

	int alignmentCount = 0;
	for ( auto iter = dropZones.begin(); iter != dropZones.end(); iter++ )
	{
		if  (bVTol)
		{
			if ( (*iter)->isVTol() )
				return false;
		}
		else
		{
			if ( (*iter)->getAlignment() == alignment  && !(*iter)->isVTol() )
				alignmentCount++;
		}
	}

	if ( alignmentCount > 3 )
		return false;

	return true;
}

bool EditorObjectMgr::addBuilding( EditorObject* pObj )
{
	return addBuilding( pObj->appearance()->position, getGroup( pObj->id ), 
		getIndexInGroup( pObj->id ), pObj->appearance()->teamId, pObj->appearance()->rotation ) ?
		true : false;
}

EditorObject* EditorObjectMgr::addDropZone( const Stuff::Vector3D& position, int alignment, bool bVTol )
{
	gosASSERT( (canAddDropZone( position, alignment, bVTol )  ) );
	
	DropZone* pLast = new DropZone( position, alignment, bVTol );
	
	dropZones.push_back( pLast ) ;

	ObjectAppearance* pAppear = getAppearance( &dropZoneGroup.buildings.front() );

	pLast->appearInfo->appearance = pAppear;

	
	int cellRow = 0;
	int cellColumn = 0;
	land->worldToCell( position, cellRow, cellColumn );
	pLast->cellRow = cellRow;
	pLast->cellColumn = cellColumn;
	pLast->id = DROPZONE << 24 | -1<< 16 | -1 << 8;

	Stuff::Vector3D realPos;
	land->getCellPos( pLast->cellRow, pLast->cellColumn, realPos );
	long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	gosASSERT((8 > alignment) && (-1 <= alignment));
	pLast->appearance()->setObjectParameters( realPos, 0, false, alignment,homeRelations[alignment+1]);

	eye->update();
	pLast->appearance()->update();
	pLast->appearance()->setInView( true );
	pLast->appearance()->rotation = 0;
	pLast->appearance()->teamId = alignment;

	return pLast;
}


EditorObject* EditorObjectMgr::addBuilding( const Stuff::Vector3D& position, unsigned long group,
									 unsigned long indexWithinGroup, int alignment, float rotation,
									 float scale, bool bSnapToCell )
{
	if ( group == 255 )
		return addDropZone( position, alignment, 0 );
	
	int type = getType( group, indexWithinGroup );
	int specialType = getSpecialType( getID( group, indexWithinGroup ) );
	
	EditorObject* info = nullptr;
	
	if ( specialType == NAV_MARKER )
	{
		info = new NavMarker;	
		buildings.push_back( info );
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );
	}
	else if ( (type == BLDG_TYPE) || (type == TREED_TYPE) )
	{

		info = new EditorObject;	
		buildings.push_back( info );
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );
	}
	else if ( type == GV_TYPE  || type == MECH_TYPE )
	{
		Unit* tmp = new Unit( alignment );
		units.push_back( tmp );
		info = tmp;
		info->appearInfo->appearance = getAppearance( group, indexWithinGroup );

		unsigned long c1, c2, c3;
		tmp->getColors(c1, c2, c3);
		tmp->appearance()->resetPaintScheme(c1, c2, c3);
	}
	
	int cellRow = 0;
	int cellColumn = 0;
	land->worldToCell( position, cellRow, cellColumn );
	info->cellRow = cellRow;
	info->cellColumn = cellColumn;
	info->id = type << 24 | group << 16 | indexWithinGroup << 8;

	Stuff::Vector3D realPos;
	land->getCellPos( info->cellRow, info->cellColumn, realPos );
	long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	gosASSERT((8 > alignment) && (-1 <= alignment));
    Stuff::Vector3D pos = bSnapToCell ? realPos : position;
	info->appearance()->setObjectParameters( pos, 0, false, alignment,homeRelations[alignment+1]);
	if (auto* unit = dynamic_cast<Unit*>(info)) unit->refreshPartFromCatalog();

	eye->update();
	info->appearance()->update();
	info->appearance()->setInView( true );
	info->appearance()->rotation = rotation;
	info->appearance()->teamId = isAlignable( info->id ) ? alignment : EDITOR_TEAMNONE;
	if ( scale != 1.0 )
		info->appearance()->scale( scale );

	// META-FIX (editor UI hardening): addBuilding is the single creation
	// primitive every place/paste/load path routes through, yet it historically
	// never registered the new prop. A freshly-placed building/tree was left
	// unregistered -> never entered the GPU static-prop recipe -> drew neither
	// body nor static shadow (the draw-time recovery in BldgAppearance::render
	// does NOT self-heal placed props in the editor: [BLDG_DIAG] notreg pool
	// never drains, while markVis==staticNow for the load-registered props that
	// DO draw). The 8 sibling edit paths (paste EditorInterface.cpp:6310, undo
	// Action.cpp:622, settings BuildingSettingsDlg.cpp:203, move
	// SelectionBrush.cpp:271, ...) each call registerStatic() to compensate;
	// fold it into the primitive so all paths are correct-by-construction.
	// Scope matches registerStaticPropsForMissionLoad() (the proven load walk):
	// everything in the `buildings` list (buildings / trees / nav markers).
	// Units (GV_TYPE/MECH_TYPE, separate `units` list) are EXCLUDED --
	// registerStatic() corrupts a Unit (see EditorInterface.cpp:6462).
	// registerStatic() is idempotent (guards on staticReg.registered), so the
	// sibling calls become harmless no-ops. Must run AFTER final position /
	// rotation / scale above, since registerStatic bakes shapeToWorld from the
	// current transform.
	if ( info && info->appearance() && (type != GV_TYPE) && (type != MECH_TYPE) )
	{
		ObjectAppearance* a = info->appearance();
		// META-FIX part 1 (registration): addBuilding is the single creation
		// primitive every place/paste/load path routes through, yet historically
		// never registered the new prop. registerStatic arms the GPU static-prop
		// recipe (idempotent; sibling place/edit paths become no-ops). Scope ==
		// registerStaticPropsForMissionLoad (buildings/trees/nav markers); units
		// (separate list) excluded -- registerStatic corrupts a Unit (see
		// EditorInterface.cpp:6462). Must run AFTER final position/rotation/scale.
		a->registerStatic();

		// registerStatic arms the light-bake latch. The load-time drain is one-shot,
		// so objects placed after loading need an explicit bake to become static
		// and visible to the GPU batcher.
		if ( eye && eye->getNumLights() > 0 )
			a->prewarmStaticLightBake( eye );
	}

	return info;
}

bool	EditorObjectMgr::deleteBuilding( const EditorObject* pInfo )
{
	BuildingLink* pLink = getLinkWithParent( pInfo );
	if ( pLink )
	{
		deleteLink( pLink );
	}
	{
		pLink = getLinkWithBuilding( pInfo );
		if ( pLink )
		{
			pLink->RemoveObject( pInfo );
		}
	}
	if (!buildings.empty() && pInfo)
	{
		for (size_t index = buildings.size(); index > 0; )
		{
			--index;
			auto iter = buildings.begin() + index;
			if( pInfo->cellColumn == (*iter)->cellColumn
				&& pInfo->cellRow == (*iter)->cellRow
				&& pInfo->appearance() == (*iter)->appearance() )
			{
				// Undo snapshots share appearances, so removal must retire the recipe immediately.
				if ( (*iter)->appearance() )
					(*iter)->appearance()->invalidateStaticRegistration();
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*iter);
				delete (*iter);
				removeFromSelection(*iter);
				EditorData::instance->handleObjectInvalidation(*iter);
				buildings.erase(iter);
				return true;
			}
		}
	}

	if (!units.empty() && pInfo)
	{
		for (size_t index = units.size(); index > 0; )
		{
			--index;
			auto mIter = units.begin() + index;
			if( pInfo->cellColumn == (*mIter)->cellColumn
				&& pInfo->cellRow == (*mIter)->cellRow )
			{
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*mIter);
				delete( *mIter );
				removeFromSelection(*mIter);
				EditorData::instance->handleObjectInvalidation(*mIter);
				units.erase(mIter);
				return true;
			}		
		}
	}

	for ( auto dIter = dropZones.begin(); dIter != dropZones.end(); dIter++ )
	{
			if( pInfo->cellColumn == (*dIter)->cellColumn
				&& pInfo->cellRow == (*dIter)->cellRow )
			{
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*dIter);
				delete( *dIter );
				removeFromSelection(*dIter);
				EditorData::instance->handleObjectInvalidation(*dIter);
				dropZones.erase( dIter );
				return true;
			}		
	
	}

	return false;
}


int EditorObjectMgr::getBuildingGroupCount() const
{
	return static_cast<int>(groups.size());
}

int EditorObjectMgr::getNumberBuildingsInGroup( int group ) const
{
	if ( group < 0 || group >= static_cast<int>(groups.size()) )
		return 0;

	return static_cast<int>(groups[group].buildings.size());
}

void EditorObjectMgr::getBuildingGroupNames( const char** names, int& NumberOfNames ) const
{
	if ( NumberOfNames < static_cast<int>(groups.size()) )
		NumberOfNames = static_cast<int>(groups.size());

	int i = 0;
	for ( auto iter = groups.begin(); iter != groups.end(); iter++, i++ )
		{
			if ( i >= NumberOfNames )
				return;
			
			names[i] = (*iter).name;
		}
}

void EditorObjectMgr::getBuildingNamesInGroup( int group, const char** names, int& NumberOfNames ) const
{
	if ( group < 0 || group >= static_cast<int>(groups.size()) )
	{
		NumberOfNames = 0;
		return;
	}

	const Group* pGroup = &groups[group];
	
	if ( NumberOfNames < static_cast<int>(pGroup->buildings.size()) )
		NumberOfNames = static_cast<int>(pGroup->buildings.size());

	int i = 0;
	for ( auto iter = pGroup->buildings.begin(); iter != pGroup->buildings.end(); iter++, i++ )
		{
			if ( i >= NumberOfNames )
				return;
			
			names[i] = (*iter).name;
		}

	NumberOfNames = i;
}

//*************************************************************************************************
// this will NOT RETURN MECHS AND VEHICLES
bool EditorObjectMgr::getBuildingFromID( int fitID, unsigned long& group, unsigned long& index, bool canBeMech )
{
	group = 0;
	index = 0;
	
	for( auto groupIter = groups.begin(); groupIter != groups.end(); groupIter++ )
			{
				index = 0;

				for( auto buildIter = (*groupIter).buildings.begin(); buildIter != (*groupIter).buildings.end(); buildIter++ )
					{
						if ( (*buildIter).fitID == fitID &&
								( canBeMech || 
									(*buildIter).type != GV_TYPE
								 && (*buildIter).type != MECH_TYPE )  )
						{
							return true;
						}

						index++;
					}

					group++;
			}

	return false;
}

int	EditorObjectMgr::getFitID( int id ) const
{
	// Drop zones and unresolved IDs can decode outside the catalog.
	if ( getGroup(id) >= static_cast<int>(groups.size())
		|| getIndexInGroup(id) >= static_cast<int>(groups[getGroup(id)].buildings.size()) )
		return 0;
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].fitID;
}


bool EditorObjectMgr::getBlocksLineOfFire( int id ) const
{
	// Drop zones and unresolved IDs can decode outside the catalog.
	if ( getGroup(id) >= static_cast<int>(groups.size())
		|| getIndexInGroup(id) >= static_cast<int>(groups[getGroup(id)].buildings.size()) )
		return false;
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].blocksLineOfFire;

}
bool EditorObjectMgr::getIsHoverCraft( int id ) const
{
	// Drop zones and unresolved IDs can decode outside the catalog.
	if ( getGroup(id) >= static_cast<int>(groups.size())
		|| getIndexInGroup(id) >= static_cast<int>(groups[getGroup(id)].buildings.size()) )
		return false;
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].isHoverCraft;

}
__int64	EditorObjectMgr::getImpassability( int id )
{
	// Drop zones and unresolved IDs can decode outside the catalog.
	if ( getGroup(id) >= static_cast<int>(groups.size())
		|| getIndexInGroup(id) >= static_cast<int>(groups[getGroup(id)].buildings.size()) )
		return 0;
	return groups[getGroup(id)].buildings[getIndexInGroup(id)].impassability;
}

int EditorObjectMgr::getType( unsigned long group, unsigned long indexWithinGroup )
{
	// gosASSERT is a no-op in this build, so it provides no real bounds
	// protection; an unresolved mission object ID (caller passed past-the-end
	// group/index) would OOB-deref here. Guard explicitly.
	if ( group >= static_cast<int>(groups.size()) )
		return -1;
	Group* pGroup = &groups[group];

	gosASSERT( static_cast<int>(pGroup->buildings.size()) > indexWithinGroup );
	if ( indexWithinGroup >= static_cast<int>(pGroup->buildings.size()) )
		return -1;
	Building* pBuilding = &pGroup->buildings[indexWithinGroup];

	return pBuilding->type;

}

EditorObjectMgr::Building::~Building()
{

}

bool EditorObjectMgr::moveBuilding( EditorObject* pInfo, long cellJ, long cellI )
{
	
	if (getObjectAtCell( cellJ, cellI ) && (getObjectAtCell( cellJ, cellI ) != pInfo))
	{
		return false;
	}

	BuildingLink* pLink = getLinkWithParent( pInfo );
	if ( pLink )
	{
		Stuff::Vector3D worldPos;
		land->cellToWorld( cellJ, cellI, worldPos );
		worldPos.z = land->getTerrainElevation( worldPos );
		pLink->SetParentPosition( worldPos );
	}

	pLink = getLinkWithChild( pInfo );
	if ( pLink )
	{
		pLink->RemoveObject( pInfo );
	}
	
	Stuff::Vector3D position;
	land->cellToWorld( cellJ, cellI, position );

	position.z = land->getTerrainElevation( position );

	pInfo->appearance()->position = position;
	pInfo->cellRow = cellJ; 
	pInfo->cellColumn = cellI;

	if ( pLink )
		pLink->AddChild( pInfo );

	return true;
}

EditorObject* EditorObjectMgr::getObjectAtCell( long realCellJ, long realCellI )
{
	for( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			if (  labs( realCellI - ((*iter)->cellColumn - 4)) <= 8
				&& labs( realCellJ - ((*iter)->cellRow - 4)) <= 8 )
			{
				__int64 FootPrint = getImpassability( (*iter)->id );
				
				int i = realCellI - ((*iter)->cellColumn - 4);
				int j = realCellJ - ((*iter)->cellRow - 4);
				if ( (FootPrint >> ( j * 8 + i )) & 0x00000001 || (i == 4 && j == 4))
					return (*iter);
					
			}

		}

		for( auto uIter = units.begin();
		uIter != units.end(); uIter++ )
		{
			if (  labs( realCellI - ((*uIter)->cellColumn - 4)) <= 8
				&& labs( realCellJ - ((*uIter)->cellRow - 4)) <= 8 )
			{
				__int64 FootPrint = getImpassability( (*uIter)->id );
				
				int i = realCellI - ((*uIter)->cellColumn - 4);
				int j = realCellJ - ((*uIter)->cellRow - 4);
				if ( (FootPrint >> ( j * 8 + i )) & 0x00000001 || (i == 4 && j == 4))
					return (*uIter);
					
			}

		}

	return nullptr;

}

EditorObject* EditorObjectMgr::getBuilding( const EditorObject &building )
{
	for( auto iter = buildings.begin(); iter != buildings.end(); iter++ )
	{
		if( building.cellColumn == (*iter)->cellColumn
			&& building.cellRow == (*iter)->cellRow )
		{
			return (*iter);
		}
	}
	return nullptr;
}

BuildingLink* EditorObjectMgr::getLinkWithBuilding( const EditorObject* pObj )
{
	for ( auto iter = links.begin(); iter != links.end(); iter++ )
	{
		if ( (*iter)->HasParent( pObj ) || (*iter)->HasChild( pObj ) )
			return *iter;
	}

	return nullptr;
}

BuildingLink* EditorObjectMgr::getLinkWithParent( const EditorObject* pObj )
{
	for ( auto iter = links.begin(); iter != links.end(); iter++ )
	{
		if ( (*iter)->HasParent( pObj ) )
			return *iter;
	}

	return nullptr;
}

BuildingLink* EditorObjectMgr::getLinkWithChild( const EditorObject* pObj )
{
	for ( auto iter = links.begin(); iter != links.end(); iter++ )
	{
		if ( (*iter)->HasChild( pObj ) )
			return *iter;
	}

	return nullptr;
}

void EditorObjectMgr::addLink( BuildingLink* pLink )
{
	for ( auto iter = links.begin(); iter != links.end(); iter++ )
	{
		if ( (*iter) == pLink )
			return;
	}

	links.push_back( pLink );
}

void EditorObjectMgr::deleteLink( BuildingLink* pLink )
{
	for ( auto iter = links.begin(); iter != links.end(); iter++ )
	{
		if ( (*iter) == pLink )
		{
			delete (*iter);

			links.erase( iter );
			return;
		}
	}
}

const char*	EditorObjectMgr::getObjectName( int ID ) const
{
	unsigned long group = getGroup(ID);
	unsigned long index = getIndexInGroup(ID);

	if ( group >= static_cast<int>(groups.size()) )
		return "Unknown Object";

	if ( index >= static_cast<int>(groups[group].buildings.size()) )
		return "Unknown Object";

	const char* name = groups[group].buildings[index].name;
	return (name && name[0]) ? name : "Unknown Object";
}

const char* EditorObjectMgr::getGroupName( int ID ) const
{
	if ( ID < 0 || ID >= static_cast<int>(groups.size()) )
		return "Unknown Group";

	const char* name = groups[ID].name;
	return (name && name[0]) ? name : "Unknown Group";
}

void EditorObjectMgr::getUnitGroupNames( const char** names, int* ids, int& numberOfEm ) const
{
	int counter = 0;
	int index = 0;
	
	for ( auto iter = groups.begin(); iter != groups.end(); index++, iter++ )
	{
		if ( static_cast<int>((*iter).buildings.size()) && ((*iter).buildings.front().type == MECH_TYPE || (*iter).buildings.front().type == GV_TYPE ) )
		{
			if ( counter < numberOfEm )
			{
				names[counter] = (*iter).name;
				ids[counter] = index;
			}

			counter++;
		}
	}

	numberOfEm = counter;
}

int EditorObjectMgr::getUnitGroupCount( ) const
{
	int counter = 0;
	
	for ( auto iter = groups.begin(); iter != groups.end(); iter++ )
	{
		if ( static_cast<int>((*iter).buildings.size()) && ( (*iter).buildings.front().type == MECH_TYPE ||(*iter).buildings.front().type == GV_TYPE ) )
		{
			counter++;
		}

	}

	return counter;

}

void EditorObjectMgr::getSelectedUnits( SelectedUnits& selUnits )
{
	for ( auto iter = units.begin(); iter != units.end(); iter++ )
	{
		if ( (*iter)->isSelected() )
			selUnits.push_back( (*iter) );
	}
}

int			EditorObjectMgr::getNumberOfVariants( int group, int indexInGroup ) const
{
		Building bldg = groups[group].buildings[indexInGroup];

		if ( !bldg.varNames  )
			return 0;

        int i = 0;
        for ( i = 0; i < 16; ++i )
        {
            if ( !bldg.varNames[i] )
                break;
        }

        return i + 1;			
}

void		EditorObjectMgr::getVariantNames( int group, int indexInGroup, const char** names, int& numberOfNames ) const
{
	Building bldg = groups[group].buildings[indexInGroup];

    int i = 0;
    for ( i = 0; i < 16 && i < numberOfNames; ++i )
    {
        if ( !bldg.varNames[i] )
            break;

        names[i] = bldg.varNames[i];
    }

    numberOfNames =  i;			

}


void EditorObjectMgr::registerSquadNum(unsigned long squadNum)
{
	if (squadNum >= nextAvailableSquadNum)
	{
		nextAvailableSquadNum = squadNum + 1;
	}
}

Unit* EditorObjectMgr::findUnitByMechHandle(RenderCore::RenderObjectHandle h)
{
	for (auto iter = buildings.begin(); iter != buildings.end(); iter++)
	{
		EditorObject* obj = *iter;
		if (!obj || !obj->appearance())
			continue;
		Unit* unit = dynamic_cast<Unit*>(obj);
		if (!unit)
			continue;
		if (Mech3DAppearance* m3d = dynamic_cast<Mech3DAppearance*>(unit->appearance()))
		{
			if (m3d->getRenderWorldHandle().bits == h.bits)
				return unit;
		}
	}
	return nullptr;
}
