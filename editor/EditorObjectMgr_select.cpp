// EditorObjectMgr: selection handling and forest editing.
// Declared in the class headers; shared names in EditorObjectMgr_internal.h.
#include "EditorObjectMgr_internal.h"
#include <algorithm>


void EditorObjectMgr::select( const Stuff::Vector4D& pos1, const Stuff::Vector4D& pos2 )
{
	int xMin, xMax;
	int yMin, yMax;
	
	if ( pos1.x < pos2.x )
	{
		xMin = pos1.x;
		xMax = pos2.x;
	}
	else
	{
		xMin = pos2.x;
		xMax = pos1.x;
	}

	if ( pos1.y < pos2.y )
	{
		yMin = pos1.y;
		yMax = pos2.y;
	}
	else
	{
		yMin = pos2.y;
		yMax = pos1.y; 
	}

	Stuff::Vector4D screenPos;

	for ( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( !pAppearance )
				continue;

			// Marquee bounds must match the rendered camera projection at every zoom.
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pAppearance->position, screenPos ) )
				continue;
			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				addToSelection(pObject);
			}
		}

		for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( !pAppearance )
				continue;

			// Marquee bounds must match the rendered camera projection at every zoom.
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pAppearance->position, screenPos ) )
				continue;
			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				addToSelection(pObject);
			}
		}

		for ( auto dIter = dropZones.begin();
		dIter != dropZones.end(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( !pAppearance )
				continue;

			glm::vec3 pos = pObject->getPosition();
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pos, screenPos ) )
				continue;

			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				addToSelection(pObject);
			}
			
		}
			 

}

void EditorObjectMgr::select( EditorObject &object, bool bSelect )
{
	ObjectAppearance* pAppearance = object.appearance();
	if ( !pAppearance )
	{
		// Partially loaded objects without appearances are not selectable.
		return;
	}

	// Bars match marquee selection without entering legacy text or bracket rendering.
	pAppearance->selected = bSelect ? DRAW_BARS : 0;

	if (true == bSelect)
	{
		addToSelection(&object);
	}
	else 
	{
		removeFromSelection(&object);
	}
}

void EditorObjectMgr::unselectAll()
{
	for ( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( pAppearance )
				pAppearance->selected = false;
		}
		for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		for ( auto dIter = dropZones.begin();
		dIter != dropZones.end(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		selectedObjects.clear();
}


bool EditorObjectMgr::hasSelection()
{
#if 1
	syncSelectedObjectPointerList();
	if (!selectedObjects.empty())
	{
		return true;
	}
#else
	for ( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				return true;
		}
		for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
		{
			if ((*mIter)->appearance()->selected )				
				return true;
		}
#endif

	return false;
}

int EditorObjectMgr::getSelectionCount()
{
#if 1
	syncSelectedObjectPointerList();
	return static_cast<int>(selectedObjects.size());
#else
	int counter = 0;
	for ( auto iter = buildings.begin();
		iter != buildings.end(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				counter++;
		}
		for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
		{
			if ((*mIter)->appearance()->selected )				
				counter++;
		}

	return counter;
#endif
}

EditorObjectMgr::EDITOR_OBJECT_LIST EditorObjectMgr::getSelectedObjectList()
{
	syncSelectedObjectPointerList();
	return selectedObjects;
}

void EditorObjectMgr::syncSelectedObjectPointerList()
{
	if (!selectedObjectsNeedsToBeSynched)
	{
		return;
	}
	selectedObjects.clear();
	for ( auto iter = buildings.begin()
		;iter != buildings.end(); iter++ )
	{
		const EditorObject* pObject = (*iter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.push_back(*iter);
	}
	for ( auto mIter = units.begin();
	mIter != units.end(); mIter++ )
	{
		const EditorObject* pObject = (*mIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.push_back(*mIter);
	}
	for ( auto dIter = dropZones.begin();
	dIter != dropZones.end(); dIter++ )
	{
		const EditorObject* pObject = (*dIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : nullptr;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.push_back(*dIter);
	}

	selectedObjectsNeedsToBeSynched = false;
}

void EditorObjectMgr::deleteSelectedObjects()
{
	for (size_t index = buildings.size(); index > 0; )
	{
		--index;
		auto iter = buildings.begin() + index;
		if ((*iter)->appearance() && (*iter)->appearance()->selected)
		{
			// Undo-held appearances outlive removal; their static recipes must stop drawing.
			(*iter)->appearance()->invalidateStaticRegistration();
			if (EditorInterface::instance())
				EditorInterface::instance()->notifyObjectDeleted(*iter);
			buildings.erase(iter);
		}
	}

	for (size_t index = units.size(); index > 0; )
	{
		--index;
		auto uIter = units.begin() + index;
		if ((*uIter)->appearance() && (*uIter)->appearance()->selected)
		{
			if (EditorInterface::instance())
				EditorInterface::instance()->notifyObjectDeleted(*uIter);
			units.erase(uIter);
		}
	}

	for (size_t index = dropZones.size(); index > 0; )
	{
		--index;
		auto dIter = dropZones.begin() + index;
		if ((*dIter)->appearance() && (*dIter)->appearance()->selected)
		{
			(*dIter)->appearance()->invalidateStaticRegistration();
			if (EditorInterface::instance())
				EditorInterface::instance()->notifyObjectDeleted(*dIter);
			dropZones.erase(dIter);
		}
	}

	selectedObjects.clear();
}

void EditorObjectMgr::adjustObjectsToNewTerrainHeights()
{
	if (!buildings.empty())
	{
		auto iter = std::prev(buildings.end());
		float Z = land->getTerrainElevation( (*iter)->appearance()->position );
		(*iter)->appearance()->position.z = Z;
		(*iter)->appearance()->update();
	}

	for ( auto lIter = links.begin(); lIter != links.end(); lIter++ )
	{
		(*lIter)->FixHeights();	
	}

	for ( auto uIter = units.begin(); uIter != units.end(); uIter++ )
	{
		float Z = land->getTerrainElevation( (*uIter)->appearance()->position );
		(*uIter)->appearance()->position.z = Z;
		(*uIter)->appearance()->update();
	}

}

int EditorObjectMgr::getNextAvailableForestID()
{
	int max = -1;

	for ( auto iter = forests.begin(); iter != forests.end(); iter++)
	{
		if ( (*iter)->getID() > max )
		{
			max = (*iter)->getID();
		}
	}

	int ID = max + 1;
	return ID;
}

long EditorObjectMgr::createForest( const Forest& forest )
{
	int ID = getNextAvailableForestID();
	Forest* pForest = new Forest( ID );
	CString oldForestName = pForest->getFileName();
	*pForest = forest;
	pForest->ID =  ID;
	if ( !strstr( pForest->getFileName(), ".fit" ) )
		pForest->setFileName( oldForestName );

	forests.push_back( pForest );
	
	doForest( *pForest );

	return ID;

}

void EditorObjectMgr::editForest( long& oldID, const Forest& forest )
{
	land->unselectAll();

	for (size_t index = buildings.size(); index > 0; )
	{
		--index;
		auto bIter = buildings.begin() + index;
		if ( (*bIter)->getForestID() == oldID )
		{
			long cellI, cellJ;
			(*bIter)->getCells( cellJ, cellI );
			long tileI = cellI/3;
			long tileJ = cellJ/3;

			land->selectVertex( tileJ, tileI, true );

			delete *bIter;
			buildings.erase(bIter);
		}
	}

	for ( auto iter = forests.begin(); iter != forests.end(); iter++ )
	{
		if ( (*iter)->getID() == oldID )
		{
			*(*iter) = forest;
			doForest( *(*iter) );
			break;
		}
	}

	land->unselectAll();
	
}

void EditorObjectMgr::removeForest( const Forest& forest )
{
	unselectAll();
	for (size_t index = buildings.size(); index > 0; )
	{
		--index;
		auto bIter = buildings.begin() + index;
		if ( (*bIter)->getForestID() == forest.getID() )
		{
			delete *bIter;
			buildings.erase(bIter);
		}
	}

	for ( auto iter = forests.begin(); iter != forests.end(); iter++ )
	{
		if ( (*iter)->getID() == forest.getID() )
		{
			delete (*iter);
			forests.erase( iter );
			return;
		}
	}
}

void EditorObjectMgr::selectForest( long ID )
{
	unselectAll();

	for ( auto bIter = buildings.begin(); bIter != buildings.end(); bIter++  )
	{
		if ( (*bIter)->getForestID() == ID )
		{
			select( *(*bIter), true );
		}
	}
}

void EditorObjectMgr::doForest( const Forest& forest )
{
	float centerX = forest.centerX;
	float centerY = forest.centerY;

	float maxDensity = forest.maxDensity/9;
	float minDensity = forest.minDensity/9;

	long probabilities[FOREST_TYPES];

	long probabilityTotal = 0;
	int i = 0;
	for ( i= 0; i < FOREST_TYPES; i++ )
	{
		probabilityTotal += forest.percentages[i];
	}

	for ( i = 0; i < FOREST_TYPES; i++ )
	{
		probabilities[i] = forest.percentages[i]/probabilityTotal * 100;
		if ( i > 0 )
		{
			probabilities[i] = probabilities[i-1] + probabilities[i];
		}
	}



	float r2 = forest.radius; // temporary... calc real radius at some point
	r2 *= r2; // square it

	for ( int j = 0; j < land->realVerticesMapSide; j++ )
	{
		for ( int i = 0; i < land->realVerticesMapSide; i++ )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				long cellI = i * 3;
				long cellJ = j * 3;

				for ( int l = -1; l < 2; l++ )
				{
					for ( int m = -1; m < 2; m++ )
					{
						glm::vec3 pos;

						if ( cellJ + l < 0 || cellJ + l > land->realVerticesMapSide * 3 )
							continue;
						if ( cellI + m < 0 || cellI + m > land->realVerticesMapSide * 3 )
							continue;

						EditorObject* pObject = getObjectAtCell( cellJ + l, cellI + m );
						if ( pObject&& pObject->getForestID() == -1 ) // only forests allow multiples in a cell
						{
							continue;
						}
						
						land->cellToWorld( cellJ + l, cellI + m, pos );
						pos.z = land->getTerrainElevation( pos );

						float deltaX = centerX - pos.x;
						float deltaY = centerY - pos.y;
						float tmpR  = deltaX * deltaX + deltaY * deltaY;
						tmpR = sqrt( tmpR );

						float fm = (minDensity - maxDensity)/forest.radius;
						float density = maxDensity + fm * ( tmpR );
						long lDensity = (long)density;

						float remainder = density - (float)lDensity;

						long count = (int)density + (RandomNumber( 100 ) < remainder * 100 ? 1 : 0);

						
						for ( int t = 0; t < count; t++ )
						{
							long random = RandomNumber( 100 );
							int group = -1;
							int index = -1;
							for ( int k = 0; k < FOREST_TYPES; k++ )
							{
								if ( random < probabilities[k] )
								{
									getRandomTreeFromGroup( k+1, group, index );
									break;
								}
							}

							if ( group == -1 || index == -1 )
							{
								continue;
							}

							glm::vec3 treePos = pos;

							treePos.x += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;
							treePos.y += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;

							int rotation = RandomNumber( 32 );

							float scale = RandomNumber( 100 * (forest.maxHeight - forest.minHeight) );
							scale = forest.minHeight + (scale)/100.f;

							EditorObject* pObject = (EditorObject*)addBuilding( treePos, group, 
								index, -1, rotation * 11.25, scale, !forest.bRandom );

							pObject->setForestID( forest.getID() );
						}
					}

				}
			}
		}
	}
}

const Forest* EditorObjectMgr::getForest( long ID )
{
	for ( auto iter = forests.begin(); iter != forests.end(); iter++ )
		{
			if ( (*iter)->getID() == ID )
				return (*iter);
		}

	gosASSERT( 0 ); // you shouldn't get here

	return nullptr;
}

long EditorObjectMgr::getForests( Forest** pForests, long& count )
{

	if ( static_cast<int>(forests.size()) > count )
	{
		count = static_cast<int>(forests.size());
		return -1;
	}

	count = 0;

	for ( auto iter = forests.begin(); iter != forests.end(); iter++ )
	{
		pForests[count++] = (*iter);
	}

	return count;
}

void EditorObjectMgr::getRandomTreeFromGroup( int treeGroup, int& group, int& index )
{
	group = TREE_GROUP;
	index  = 0;

	int treeCount = 0;
	Group* pGroup = &groups[TREE_GROUP];
	if ( pGroup )
	{
		for( auto buildIter = groups[group].buildings.begin();
			buildIter != groups[group].buildings.end(); buildIter++ )
			{
				if ( (*buildIter).forestId == treeGroup )
					treeCount++;
			}

		int whichOne = RandomNumber( treeCount );
		treeCount = 0;

		int counter = 0;
		for( auto buildIter = groups[group].buildings.begin();
			buildIter != groups[group].buildings.end(); buildIter++, counter++ )
			{
				if ( (*buildIter).forestId == treeGroup  )
				{
					if ( treeCount == whichOne )
					{
						index = counter;
					}
					treeCount++;
				}
			}

	}


}

void EditorObjectMgr::addToSelection(EditorObject* object)
{
    if (std::find(selectedObjects.begin(), selectedObjects.end(), object) == selectedObjects.end())
        selectedObjects.push_back(object);
}

void EditorObjectMgr::removeFromSelection(EditorObject* object)
{
    const auto found = std::find(selectedObjects.begin(), selectedObjects.end(), object);
    if (found != selectedObjects.end())
        selectedObjects.erase(found);
}
