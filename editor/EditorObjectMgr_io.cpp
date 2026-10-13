// EditorObjectMgr: save/load of buildings, mechs, forests and drop zones.
// Declared in the class headers; shared names in EditorObjectMgr_internal.h.
#include "EditorObjectMgr_internal.h"
#include <algorithm>
#include "mc2fmt.h"


bool EditorObjectMgr::save( PacketFile& PakFile, int whichPacket )
{
	File file;
	
	if ( !buildings.Count() )
	{
		PakFile.writePacket( whichPacket, NULL );
		PakFile.writePacket( whichPacket + 1, NULL );
	}

	int bufferSize = buildings.Count() * (4 * sizeof(float) + 6 * sizeof(long) ) + 2 * sizeof(long);
	char* pBuffer = (char *) malloc( bufferSize );
	long* pTacMapPoints = (long *)malloc( sizeof(long) * ( buildings.Count() * 2 + 1 ) );
	long* pPoints = pTacMapPoints + 1;
	long pointCounter = 0;
	
	file.open(pBuffer, bufferSize );
	file.writeLong( buildings.Count() );

	gos_VERTEX point;
	
	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			int id = (*iter)->id;
			Building& bldg = groups[(id >> 16) & 0x000000ff].buildings[(id >> 8) & 0x000000ff];

			if ( bldg.writeOnTacMap )
			{
				TacMap::worldToTacMap( (*iter)->appearance()->position, 0, 0, 115, 115, point );
				*pPoints++ = point.x;
				*pPoints++ = point.y;
				pointCounter++;
			}
			
			file.writeLong( bldg.fitID );
			file.writeFloat( (*iter)->appearance()->position.x );
			file.writeFloat( (*iter)->appearance()->position.y );
			file.writeFloat( (*iter)->appearance()->position.z );

			file.writeFloat( (*iter)->appearance()->rotation );

			file.writeLong( (*iter)->appearance()->damage );

			
			file.writeLong( (*iter)->appearance()->teamId );
			BuildingLink* pLink = getLinkWithChild(*iter);
			if ( pLink )
			{
				Stuff::Vector3D pos = pLink->GetParentPosition();
				// convert to cells
				int cellI, cellJ;
				land->worldToCell( pos, cellJ, cellI );

				long val = (cellJ << 16) | cellI;

				//How about BEFORE we write this, we make sure there's really a building there!
				EditorObject* pObject = getObjectAtCell( cellJ, cellI );
				if (!pObject)
				{
					val = 0xffffffff;
					PAUSE(("Turret/Gate linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!  Link Deleted",cellJ,cellI,pos.x,pos.y));
				}

				file.writeLong( val );
			}
			else
				file.writeLong( 0xffffffff );
			
			file.writeLong( (*iter)->getForestID() + 1 );
			if ( (*iter)->getForestID() != -1 )
				file.writeLong( (*iter)->getScale()  );
			else
				file.writeLong( 0 );
		}

	int pos = file.getLogicalPosition();
	file.seek( 0 );

	if ( !PakFile.writePacket( whichPacket, (unsigned char*)pBuffer, pos ) )
	{
		gosASSERT( false );
	}

	*pTacMapPoints = pointCounter;

	PakFile.writePacket( whichPacket + 1, (BYTE*)pTacMapPoints, ( pointCounter * 2 + 1 ) * sizeof( long ) );

	free( pTacMapPoints );
	free( pBuffer );

	if (!justResaveAllMaps) {
		int numTurrets = 0;
		for ( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
		{
			switch ((*iter)->getSpecialType())
			{
			case EDITOR_TURRET:
				{
					numTurrets += 1;
				}
				break;
			}
		}
		if (64/*max turrets*/ < numTurrets )
		{
			CString CStr;
			CStr.Format(IDS_TOO_MANY_TURRETS, numTurrets);
			extern bool g_cliSuppressModals;
			extern void EditorSmokeLogSuppressedModal(const char*, const char*);
			if (g_cliSuppressModals)
				EditorSmokeLogSuppressedModal(CStr, "IDS_TOO_MANY_TURRETS default=IDOK");
			else
				AfxMessageBox(CStr);
		}
	}

	return true;
}

bool EditorObjectMgr::load( PacketFile& PakFile, int whichPacket )
{
	PakFile.seekPacket( whichPacket );
	int size = PakFile.getPacketSize( );
	char* pBuffer = (char *)malloc(size);
	PakFile.readPacket( whichPacket, (unsigned char*)pBuffer );
	
	File file;
	file.open( pBuffer, size );
	int count = file.readLong();

	for ( int i = 0; i < count; ++i )
	{
		int id = file.readLong();

		Stuff::Vector3D vector;
		vector.x = file.readFloat();
		vector.y = file.readFloat();
		vector.z = file.readFloat();

		float rotation = file.readFloat();
		int damage = file.readLong();
		int teamId = file.readLong();
		int cells = file.readLong();
		int forestId = file.readLong() - 1;
		long scale = file.readLong();

		unsigned long group, index;
		if ( !getBuildingFromID(id, group, index, false ) )
		{
			// Unknown object ids are valid in modded/remastered sandboxes.
			// The packet record has already been consumed above; skipping here
			// keeps the stream aligned for the remaining objects.
			continue;
		}

		// A saved marker position is authored state, including free transforms.
		// Brush placement snapping must not change it during document reload.
		const bool snapToCell = getSpecialType(getID(group, index)) != NAV_MARKER;
		EditorObject* pInfo = addBuilding(vector, group, index, EDITOR_TEAMNONE, rotation, 1.0f, snapToCell);
		if ( !pInfo || !pInfo->appearance() )
		{
			continue;
		}

		pInfo->appearance()->rotation = rotation;

		if ( damage )
			pInfo->appearance()->setDamage(1);

		pInfo->appearance()->teamId = teamId;

		if ( cells && (cells != 0xffffffff)) // this building is linked
		{
			long cellJ = cells >> 16;
			long cellI = cells & 0x0000ffff;

			Stuff::Vector3D pos;

			land->cellToWorld( cellJ, cellI, pos );
			pos.z = land->getTerrainElevation( pos );

			bool bFound = false;
			for ( LINK_LIST::EIterator iter = links.Begin();
			!iter.IsDone(); iter++ )
			{
				if ( (*iter)->GetParentPosition() == pos )
				{
					(*iter)->AddChild( pInfo );
					bFound = true;
					break;
				}
			}

			if ( !bFound )
			{
				BuildingLink* pLink = new BuildingLink( pos );
				pLink->AddChild( pInfo );
				links.Append( pLink );       
			}
		}

		pInfo->setForestID( forestId );
		if ( pInfo->getForestID() != -1 )
			pInfo->setScale( scale );

	}

	LINK_LIST linksToDelete;

	// gotta go through all the links and make sure they have id's
	for ( LINK_LIST::EIterator iter = links.Begin();
			!iter.IsDone(); iter++ )
			{
				if ( (*iter)->parent.m_ID == -1 )
				{
					// This absolutely positively could never have worked!
					// Why?  Because getObjectAtPosition depends upon the object BEING ON SCREEN!!!!!!!
					// At this point, we're still loading!!!
					// We should, instead, be checking getObjectAtCell which ALWAYS works!
					// -fs
					int cellR, cellC;
					land->worldToCell((*iter)->parent.pos,cellR,cellC);

					EditorObject* pObject = getObjectAtCell( cellR, cellC );
					gosASSERT( pObject );

					if ( pObject )
						(*iter)->parent.m_ID = pObject->getID();
					else
						linksToDelete.Append( *iter );
				}
			}

	for ( LINK_LIST::EIterator iter = linksToDelete.Begin();	!iter.IsDone(); iter++ )
	{
		deleteLink( *iter );
	}
	


	free( pBuffer );

	return true;

}

bool  EditorObjectMgr::loadMechs( FitIniFile& file )
{
	// disable animation loading to decrease loading time
	Mech3DAppearanceType::disableAnimationLoading();

	long count = 0;
	if (file.seekBlock("Parts") != NO_ERR || !EditorFit::longValue(file, "NumParts", count, true) || count > 100000)
		return EditorFit::reject("Part count");

	int alternativeInstancesCounter = count + 1;
	Stuff::Vector3D position;
	position.x = position.y = position.z = 0;
	char buffer[256];
	for ( int i = 1; i < count + 1; ++i )
	{
		sprintf( buffer, "Part%ld", i );
		long fitID = 0;
		if (file.seekBlock(buffer) != NO_ERR || !EditorFit::longValue(file, "ObjectNumber", fitID, true))
			return EditorFit::reject("Part object number");
		
		unsigned long group, index;
		if ( !getBuildingFromID( fitID, group, index, true ) )
		{
			// An unresolved unit cannot be edited losslessly. Reject the document
			// instead of permitting a save that silently removes the unit.
			return EditorFit::reject("Part object absent from catalog");
		}

		EditorObject* pObject = (EditorObject*)(addBuilding( position, group, index, 0, 0 ));
		if ( !pObject )
			continue;

		gosASSERT( dynamic_cast<Unit*>(pObject)  );

		if (!pObject->load(&file, i))
		{
			deleteBuilding(pObject);
			return false;
		}

		if (Mech3DAppearance* m3d = dynamic_cast<Mech3DAppearance*>(pObject->appearance()))
			GameAdapters::Mech::syncSpawn(*m3d, 0u);

		alternativeInstancesCounter += 1;
		int tmpI = ((Unit *)(pObject))->tmpAlternativeStartIndex;
		if (0 < tmpI)
		{
			alternativeInstancesCounter = tmpI;
		}

		int j;
		for (j = 0; j < ((Unit *)(pObject))->tmpNumAlternativeInstances; j += 1)
		{
			sprintf( buffer, "Part%ld", alternativeInstancesCounter );
			long alternativeFitID = 0;
			if (file.seekBlock(buffer) != NO_ERR || !EditorFit::longValue(file, "ObjectNumber", alternativeFitID, true))
				return EditorFit::reject("alternative Part object number");
			
			unsigned long group, index;
			if (!getBuildingFromID(alternativeFitID, group, index, true))
				return EditorFit::reject("alternative Part object absent from catalog");

			EditorObject* pAltObject = (EditorObject*)(addBuilding( position, group, index, 0, 0 ));
			gosASSERT( dynamic_cast<Unit*>(pAltObject)  );
			if (!pAltObject || !pAltObject->load(&file, alternativeInstancesCounter))
			{
				if (pAltObject) deleteBuilding(pAltObject);
				return false;
			}
			((Unit *)(pObject))->pAlternativeInstances->Append(*((Unit *)(pAltObject)));
			deleteBuilding(pAltObject);

			alternativeInstancesCounter += 1;
		}
	}

	return true;
	
}

bool		EditorObjectMgr::saveMechs( FitIniFile& file )
{
	EditorObjMgrTrace("EditorObjectMgr::saveMechs: enter units.Count=%d", units.Count());
	file.writeBlock( "Warriors" );

	UNIT_LIST unitsByPlayer[8/*max players*/];
	{
		for ( UNIT_LIST::EIterator iter = units.Begin(); !iter.IsDone(); iter++ )
		{
			int alignment = (*iter)->getAlignment();
			if (8/*max players*/ > alignment)
			{
				unitsByPlayer[alignment].Append((*iter));
			}
			else 
			{
				assert(false);
			}
		}
	}

	UNIT_LIST reorderedUnits;
	UNIT_LIST reorderedUnitsByPlayer[8/*max players*/];
	for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
	{
		/*O(n-squared) sort. Who cares.*/
		int numUnitsOfThisPlayer = unitsByPlayer[playerNum].Count();
		for (int i = 0; i < numUnitsOfThisPlayer; i+= 1)
		{
			int lowestSquadNum = INT_MAX;
			UNIT_LIST::EIterator iter;
			for ( iter = unitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				if ((*iter)->getSquad() < lowestSquadNum)
				{
					lowestSquadNum = (*iter)->getSquad();
				}
			}
			for ( iter = unitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				if (lowestSquadNum == (*iter)->getSquad())
				{
					break;
				}
			}
			if (!iter.IsDone())
			{
				reorderedUnitsByPlayer[playerNum].Append(*iter);
				/*we delete the unit from unitsByPlayer[playerNum] because more than one unit
				may have the same squad number*/
				unitsByPlayer[playerNum].Delete(iter);
			}
			else
			{
				assert(false);
			}
		}
		UNIT_LIST::EIterator iter;
		for ( iter = reorderedUnitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
		{
			reorderedUnits.Append(*iter);
		}
	}
	assert(reorderedUnits.Count() == units.Count());

	{
		/*When saving, each player's units need to be grouped into "lances" of 12 units
		for historical reasons.*/
		for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
		{
			int lanceGroup = 0;
			int mateIndex = 0;
			UNIT_LIST::EIterator iter;
			for ( iter = reorderedUnitsByPlayer[playerNum].Begin(); !iter.IsDone(); iter++ )
			{
				(*iter)->setLanceInfo(lanceGroup, mateIndex);
				mateIndex += 1;
				if (12/*just twelve*/ >= mateIndex)
				{
					mateIndex = 0;
					lanceGroup += 1;
				}
			}
		}
	}

	/*
	if ( 1 > reorderedUnitsByPlayer[EDITOR_TEAM1].Count() )
	{
		EMessageBox( IDS_MUST_HAVE_PLAYER, IDS_CANT_SAVE, MB_OK );
		return false;
	}
	*/

	if (!justResaveAllMaps)
	{
		if (EditorData::instance->IsSinglePlayer())
		{
			bool enemyUnitFound = false;
			for (int playerNum = 1/*first non-user player*/; 8/*max players*/ > playerNum; playerNum += 1)
			{
				if (0/*player1's (user's) team*/ != EditorData::instance->PlayersRef().PlayerRef(playerNum).DefaultTeam())
				{
					if (0 < reorderedUnitsByPlayer[playerNum].Count())
					{
						enemyUnitFound = true;
						break;
					}
				}
			}
			if (!enemyUnitFound)
			{
				EMessageBox( IDS_NO_ENEMY_UNITS, IDS_WARNING, MB_OK );
			}

			if (0 >= reorderedUnitsByPlayer[0/*user player*/].Count())
			{
				EMessageBox( IDS_NO_PLAYER_UNITS, IDS_WARNING, MB_OK );
			}
		}
		else
		{
			for (int playerNum = 0; EditorData::instance->MaxPlayers() > playerNum; playerNum += 1)
			{
				if (1 > reorderedUnitsByPlayer[playerNum].Count())
				{
					EMessageBox( IDS_PLAYERS_WITH_NO_UNITS, IDS_WARNING, MB_OK );
					break;
				}
			}
		}
	}


	file.writeIdULong( "NumWarriors", units.Count() );

	if (!justResaveAllMaps && (units.Count() > 104))
	{
		EMessageBox( IDS_TOO_MANY_PILOTS, IDS_WARNING, MB_OK);
	}

	file.writeBlock( "Parts" );
	file.writeIdULong( "NumParts", units.Count() );

	char buffer[256];
	int counter = 1; // for some unknown reason we index from 1
	int alternativeInstancesCounter = reorderedUnits.Count() + 1;

	UNIT_LIST::EIterator iter;
	for ( iter = reorderedUnits.Begin(); !iter.IsDone(); counter++, iter++ )
	{
		sprintf( buffer, "Warrior%ld", counter );
		file.writeBlock( buffer );

		// Make a new name of the form "level23_warrior8"
		char armName[512] = {0};
		strcpy(armName, file.getFilename());
		_strlwr(armName);
		char * fitExt = strstr(armName, ".fit");
		// Guard: if getFilename() returns a path without .fit extension, skip ARM relationship.
		if (!fitExt) {
			EditorObjMgrTrace("EditorObjectMgr::saveMechs: armName has no .fit: '%s' (skipping ARM warrior relationship)", armName);
		} else {
			*fitExt = '_';
			// the bound is what is left of armName[512] after the '_' that replaced ".fit"
			mc2::fmt::to_buf_n(fitExt + 1, sizeof(armName) - static_cast<size_t>(fitExt + 1 - armName), "warrior{:02d}", mc2::fmt::as_i32(counter));
			mapAsset->AddRelationship("warrior", armName);
		}

		IProviderAssetPtr mechAssetPtr = armProvider->OpenAsset(
			fitExt ? armName : "", (AssetType)1, ProviderType_Primary);
		mechAsset = (IProviderAsset*)mechAssetPtr;

		(*iter)->save( &file, counter );

		mechAssetPtr->Close();
		mechAssetPtr = nullptr;
		mechAsset = nullptr;
		
		file.writeIdULong( "AlternativeStartIndex", alternativeInstancesCounter + 1 );

		long IndicesOfAlternatives[15/*max alternatives*/] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
		int numAlternatives = (*iter)->pAlternativeInstances->Count();
		if (15/*max alternatives*/ < numAlternatives)
		{
			gosASSERT(false);
			numAlternatives = 15/*max alternatives*/;
		}
		int j;
		for (j = 0; j <numAlternatives ; j++)
		{
			IndicesOfAlternatives[j] = (alternativeInstancesCounter + 1) + j;
		}
		file.writeIdLongArray("IndicesOfAlternatives", IndicesOfAlternatives, 15);


		alternativeInstancesCounter += 1;
		int i;
		for (i = 0; i < (*iter)->pAlternativeInstances->Count(); i++)
		{
			sprintf( buffer, "Warrior%ld", alternativeInstancesCounter );
			file.writeBlock( buffer );
			(*((*iter)->pAlternativeInstances))[i].save( &file, alternativeInstancesCounter );
			alternativeInstancesCounter += 1;
		}
	}


	file.writeBlock( "Teams" );


	{
		/*When saving, each player's units need to be grouped into "lances" of 12 units
		for historical reasons.*/
		int reorderedUnitIndex = 1; /*1-based indexing by convention*/
		for (int playerNum = 0; 8/*max players*/ > playerNum; playerNum += 1)
		{
			int reorderedUnitByPlayerIndex = 0;
			int lanceGroup = 0;
			for ( lanceGroup = 0; lanceGroup * (12/*just twelve*/) < reorderedUnitsByPlayer[playerNum].Count(); lanceGroup += 1 )
			{
				long mates[12/*just twelve*/];
				int mateIndex = 0;
				for (mateIndex = 0; 12/*just twelve*/ > mateIndex; mateIndex += 1)
				{
					if (reorderedUnitByPlayerIndex < reorderedUnitsByPlayer[playerNum].Count())
					{
						mates[mateIndex] = reorderedUnitIndex;
						reorderedUnitIndex += 1;
						reorderedUnitByPlayerIndex += 1;
					}
					else
					{
						mates[mateIndex] = 0;
					}
				}
				EString commanderBlock;
				commanderBlock.Format("Commander%ldGroup:%ld", playerNum, lanceGroup);

				file.writeBlock( commanderBlock.Data() );

				file.writeIdLongArray( "Mates", &(mates[0]), 12 );
			}
		}
	}

	return true;

}

bool EditorObjectMgr::saveForests( FitIniFile& file )
{
	long counter = 0;
	char header[256];
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++, counter++)
	{
		sprintf( header, "Forest%ld", counter );
		file.writeBlock( header );
		(*iter)->save( file );
	}

	return true;
}

bool EditorObjectMgr::loadForests( FitIniFile& file ) 
{
	long counter = 0;
	char header[256];
	long tmp;
	
	while( true )
	{
		sprintf( header, "Forest%ld", counter );
		if ( NO_ERR != file.seekBlock( header ) )
			break;

		file.readIdString( "FileName", header, 255 );
		file.readIdLong( "ID", tmp );

		Forest* pForest = new Forest( tmp );
		pForest->setFileName( header );

		pForest->init( file );
		pForest->ID = counter;

		forests.Append( pForest );
		counter++;
	}

	return true;
}


bool EditorObjectMgr::saveDropZones( FitIniFile& file )
{
	int counter = 0;
	char Header[256];

	for ( DROP_LIST::EIterator iter = dropZones.Begin(); !iter.IsDone(); iter++, counter++ )
	{
		sprintf( Header, "DropZone%ld", counter );
		file.writeBlock( Header );
		(*iter)->save( &file, counter );
	}

	// going to do nav markers here
	// need to sort this list first.  ick.
	EditorObject** pObjects = (EditorObject**)_alloca( sizeof( EditorObject* )  * buildings.Count() );
	int i = 0;
	for ( BUILDING_LIST::EIterator bIter = buildings.Begin(); !bIter.IsDone(); bIter++ )
	{
		pObjects[i++] = *bIter;
	}

	long count = i;

	for ( i = 1; i < count; ++i )
	{
		EditorObject* cur = pObjects[i];
		for ( int j = 0; j < i; ++j )
		{
			if ( (getFitID(cur->getID()) < getFitID(pObjects[j]->getID())) && j != i )
			{
				pObjects[i] = pObjects[j];
				pObjects[j] = cur;
				break;
			}
		}
	}

	counter = 0;
	for ( i = 0; i < count; i++ )
	{
		if ( pObjects[i]->save(& file, counter ) )
			counter++;
	}

	// so  it writes out afterward....
	file.writeBlock( "NavMarkers" );
	file.writeIdULong( "NumNavMarkers", counter );

	return true;
}

bool EditorObjectMgr::loadDropZones( FitIniFile& file )
{
	int counter = 0;
	char Header[256];

	while( true )
	{
		sprintf( Header, "DropZone%ld", counter );
		if ( NO_ERR != file.seekBlock( Header ) )
			return counter > 0 ? true : false;

		bool bVTol;
		file.readIdBoolean( "IsVTOL", bVTol );

		Stuff::Vector3D pos;
		file.readIdFloat( "PositionX", pos.x );
		file.readIdFloat( "PositionY", pos.y );

		pos.z = land->getTerrainElevation( pos );

		// assuming single player for now
		addDropZone( pos, 0, bVTol );

		counter++;

	}

	// no need to read in nav markers, they are saved in the building file as well

	return true;
	
	
}

bool EditorObjectMgr::loadNavMarkers(FitIniFile& file)
{
	if (file.seekBlock("NavMarkers") != NO_ERR) return true;
	long count = 0;
	if (!EditorFit::longValue(file, "NumNavMarkers", count, true) || count > buildings.Count())
		return EditorFit::reject("NavMarker count");
	std::vector<NavMarker*> markers;
	for (auto it = buildings.Begin(); !it.IsDone(); it++)
		if (auto* marker = dynamic_cast<NavMarker*>(*it)) markers.push_back(marker);
	if (static_cast<size_t>(count) != markers.size()) return EditorFit::reject("NavMarker count mismatch");
	for (long i = 0; i < count; ++i)
	{
		char block[32];
		sprintf(block, "NavMarker%ld", i);
		if (file.seekBlock(block) != NO_ERR) return EditorFit::reject("NavMarker block missing");
		float x = 0, y = 0;
		if (!EditorFit::floatValue(file, "xPos", x) || !EditorFit::floatValue(file, "yPos", y))
			return EditorFit::reject("NavMarker coordinates");
		auto found = std::find_if(markers.begin(), markers.end(), [x, y](NavMarker* marker) {
			return std::abs(marker->getPosition().x - x) < 1.0f && std::abs(marker->getPosition().y - y) < 1.0f;
		});
		if (found == markers.end()) return EditorFit::reject("NavMarker reference");
		if (!(*found)->readDefinition(file)) return false;
		markers.erase(found);
	}
	return true;
}
