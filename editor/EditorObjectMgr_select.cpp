// EditorObjectMgr: selection handling and forest editing.
// Declared in the class headers; shared names in EditorObjectMgr_internal.h.
#include "EditorObjectMgr_internal.h"


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

	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			// BUG1 (zoomed-in marquee): use the modern GL forward projection (the same
			// one getObjectAtScreenPosition / the drag jacobian use) instead of legacy
			// projectZ. projectZ diverges from the rendered camera when zoomed in, so
			// object centers landed outside the screen rect and nothing got selected.
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pAppearance->position, screenPos ) )
				continue;
			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
		}

		for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			// BUG1 (zoomed-in marquee): use the modern GL forward projection (the same
			// one getObjectAtScreenPosition / the drag jacobian use) instead of legacy
			// projectZ. projectZ diverges from the rendered camera when zoomed in, so
			// object centers landed outside the screen rect and nothing got selected.
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pAppearance->position, screenPos ) )
				continue;
			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
		}

		for ( DROP_LIST::EIterator dIter = dropZones.Begin();
		!dIter.IsDone(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( !pAppearance )
				continue;

			Stuff::Vector3D pos = pObject->getPosition();
			if ( !EditorObjectMgr_ProjectScreenXY_GL( pos, screenPos ) )   // BUG1: modern projection
				continue;

			if ( screenPos.x >= xMin && screenPos.x <= xMax
				 && screenPos.y >= yMin && screenPos.y <= yMax )
			{
				pAppearance->selected = true;
				selectedObjects.AddUnique(pObject);
			}
			
		}
			 

}

void EditorObjectMgr::select( EditorObject &object, bool bSelect )
{
	ObjectAppearance* pAppearance = object.appearance();
	if ( !pAppearance )
	{
		// by Methuselas: left-click selection is live again under the embedded
		// renderer.  Treat appearance-less legacy objects as non-selectable
		// instead of dereferencing a partially loaded object.
		return;
	}

	// by Methuselas: keep single-click selection on the same render flag path
	// as the now-working drag-select code.  Drag-select writes `true`, which is
	// equivalent to DRAW_BARS.  Avoid DRAW_TEXT because it can enter legacy
	// cLoadString() name rendering, and avoid DRAW_BRACKETS here until the
	// remastered Editor bracket path is separately proven safe for single-click.
	pAppearance->selected = bSelect ? DRAW_BARS : 0;

	if (true == bSelect)
	{
		selectedObjects.AddUnique(&object);
	}
	else 
	{
		selectedObjects.RemoveIfThere(&object);
	}
}

void EditorObjectMgr::unselectAll()
{
	// mh: at somepoint we can change the code so that it iterates over the
	// selectedObjects list instead of the lists of all the buildings and units
	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			EditorObject* pObject = (*iter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}
		for ( UNIT_LIST::EIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
		{
			EditorObject* pObject = (*mIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		for ( DROP_LIST::EIterator dIter = dropZones.Begin();
		!dIter.IsDone(); dIter++ )
		{
			EditorObject* pObject = (*dIter);
			ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
			if ( pAppearance )
				pAppearance->selected = false;
		}

		selectedObjects.Clear();
}


bool EditorObjectMgr::hasSelection()
{
#if 1
	syncSelectedObjectPointerList();
	if (0 < selectedObjects.Count())
	{
		return true;
	}
#else
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				return true;
		}
		for ( UNIT_LIST::EConstIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
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
	return selectedObjects.Count();
#else
	int counter = 0;
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if ((*iter)->appearance()->selected )				
				counter++;
		}
		for ( UNIT_LIST::EConstIterator mIter = units.Begin();
		!mIter.IsDone(); mIter++ )
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
	selectedObjects.Clear();
	for ( BUILDING_LIST::EConstIterator iter = buildings.Begin()
		;!iter.IsDone(); iter++ )
	{
		const EditorObject* pObject = (*iter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*iter);
	}
	for ( UNIT_LIST::EConstIterator mIter = units.Begin();
	!mIter.IsDone(); mIter++ )
	{
		const EditorObject* pObject = (*mIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*mIter);
	}
	for ( DROP_LIST::EIterator dIter = dropZones.Begin();
	!dIter.IsDone(); dIter++ )
	{
		const EditorObject* pObject = (*dIter);
		const ObjectAppearance* pAppearance = pObject ? pObject->appearance() : NULL;
		if (pAppearance && pAppearance->selected )				
			selectedObjects.Append(*dIter);
	}

	selectedObjectsNeedsToBeSynched = false;
}

void EditorObjectMgr::deleteSelectedObjects()
{
	for ( BUILDING_LIST::EIterator iter = buildings.End();
		!iter.IsDone();  )
		{
			// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; guard before ->selected
			// (mirror syncSelectedObjectPointerList above, ~line 2177).
			if ( (*iter)->appearance() && (*iter)->appearance()->selected )
			{
				// EDITOR-STATIC-DEDRAW: invalidate the static-prop recipe so the prop
				// stops drawing (the shared/undo-held appearance keeps it registered
				// otherwise -> ghost). undo re-registers (editor/Action.cpp:622).
				(*iter)->appearance()->invalidateStaticRegistration();
				// EDITOR-CRASH-HARDENING-1: scrub drag pointer for removed object (UAF guard).
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*iter);
				BUILDING_LIST::EIterator tmpIter = iter;
				iter --;
				buildings.Delete( tmpIter );
			}
			else
				iter--;
		}

		for ( UNIT_LIST::EIterator uIter = units.End();
		!uIter.IsDone(); )
		{
			// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; guard before ->selected.
			if ( (*uIter)->appearance() && (*uIter)->appearance()->selected )
			{
				// EDITOR-CRASH-HARDENING-1: scrub drag pointer for removed object (UAF guard).
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*uIter);
				UNIT_LIST::EIterator tmpIter = uIter;
				uIter --;
				units.Delete( tmpIter );
			}
			else
				uIter--;
		}

		for ( DROP_LIST::EIterator dIter = dropZones.End();
		!dIter.IsDone(); )
		{
			// EDITOR-CRASH-HARDENING-1: appearance() can be NULL; guard before ->selected.
			if ( (*dIter)->appearance() && (*dIter)->appearance()->selected )
			{
				// EDITOR-STATIC-DEDRAW: invalidate the static-prop recipe before removal.
				(*dIter)->appearance()->invalidateStaticRegistration();
				// EDITOR-CRASH-HARDENING-1: scrub drag pointer for removed object (UAF guard).
				if ( EditorInterface::instance() )
					EditorInterface::instance()->notifyObjectDeleted(*dIter);
				DROP_LIST::EIterator tmpIter = dIter;
				dIter --;
				dropZones.Delete( tmpIter );
			}
			else
				dIter--;
		
		}

		selectedObjects.Clear();
}

void EditorObjectMgr::adjustObjectsToNewTerrainHeights()
{
	for ( BUILDING_LIST::EIterator iter = buildings.End();
		!iter.IsDone(); iter++  )
	{
		float Z = land->getTerrainElevation( (*iter)->appearance()->position );
		(*iter)->appearance()->position.z = Z;
		(*iter)->appearance()->update();
	}

	for ( LINK_LIST::EIterator lIter = links.Begin(); !lIter.IsDone(); lIter++ )
	{
		(*lIter)->FixHeights();	
	}

	for ( UNIT_LIST::EIterator uIter = units.Begin(); !uIter.IsDone(); uIter++ )
	{
		float Z = land->getTerrainElevation( (*uIter)->appearance()->position );
		(*uIter)->appearance()->position.z = Z;
		(*uIter)->appearance()->update();
	}

}

int EditorObjectMgr::getNextAvailableForestID()
{
	int max = -1;

	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++)
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

	forests.Append( pForest );
	
	// here I need to go about adding the trees -- will do in a sec
	doForest( *pForest );

	return ID;

}

void EditorObjectMgr::editForest( long& oldID, const Forest& forest )
{
	land->unselectAll();

	for ( BUILDING_LIST::EIterator bIter = buildings.End(); !bIter.IsDone();  )
	{
		if ( (*bIter)->getForestID() == oldID )
		{
			long cellI, cellJ;
			(*bIter)->getCells( cellJ, cellI );
			long tileI = cellI/3;
			long tileJ = cellJ/3;

			land->selectVertex( tileJ, tileI, true );

			delete *bIter;

			BUILDING_LIST::EIterator tmpIter = bIter;
			bIter --;
			buildings.Delete( tmpIter );
		}
		else
			bIter--;
	}

	// update the forest object
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
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
	for ( BUILDING_LIST::EIterator bIter = buildings.End(); !bIter.IsDone();  )
	{
		if ( (*bIter)->getForestID() == forest.getID() )
		{
			delete *bIter;
			BUILDING_LIST::EIterator tmpIter = bIter;
			bIter--;
			buildings.Delete( tmpIter );
		}
		else
		{
			bIter--;
		}
	}

	// now remove the forest from the list
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getID() == forest.getID() )
		{
			delete (*iter);
			forests.Delete( iter );
			return;
		}
	}
}

void EditorObjectMgr::selectForest( long ID )
{
	unselectAll();

	for ( BUILDING_LIST::EIterator bIter = buildings.Begin(); !bIter.IsDone(); bIter++  )
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
						Stuff::Vector3D pos;

						if ( cellJ + l < 0 || cellJ + l > land->realVerticesMapSide * 3 )
							continue;
						if ( cellI + m < 0 || cellI + m > land->realVerticesMapSide * 3 )
							continue;

						EditorObject* pObject = getObjectAtCell( cellJ + l, cellI + m );
						if ( pObject&& pObject->getForestID() == -1 ) // only forests allow multiples in a cell
						{
							continue;
						}
						
						// figure out how far from center we are
						land->cellToWorld( cellJ + l, cellI + m, pos );
						pos.z = land->getTerrainElevation( pos );

						// distance 
						float deltaX = centerX - pos.x;
						float deltaY = centerY - pos.y;
						float tmpR  = deltaX * deltaX + deltaY * deltaY;
						tmpR = sqrt( tmpR );

						float fm = (minDensity - maxDensity)/forest.radius;
						float density = maxDensity + fm * ( tmpR );
						long lDensity = (long)density;

						float remainder = density - (float)lDensity;

						long count = (int)density + (RandomNumber( 100 ) < remainder * 100 ? 1 : 0);

						// OK, now we have the density randomly (if possible)
						// populate the tile with these...
						// might want to break down by cell
						
						for ( int t = 0; t < count; t++ )
						{
							// oh god, now we need to pick a tree
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

							Stuff::Vector3D treePos = pos;

							// now place equally randomly.
							treePos.x += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;
							treePos.y += (float)RandomNumber( Terrain::worldUnitsPerCell ) - Terrain::worldUnitsPerCell/2;

							int rotation = RandomNumber( 32 );

							float scale = RandomNumber( 100 * (forest.maxHeight - forest.minHeight) );
							scale = forest.minHeight + (scale)/100.f;

							if ((AppearanceTypeList::appearanceHeap->totalCoreLeft() < 1000/*arbitrary*/)
								|| (AppearanceTypeList::appearanceHeap->totalCoreLeft() < 0.01/*arbitrary*/ * AppearanceTypeList::appearanceHeap->size()))
							{
								AfxMessageBox(IDS_APPEARANCE_HEAP_EXHAUSTED);
								/*exit all loops*/
								t = count;
								m = 2;
								l = 2;
								i = land->realVerticesMapSide;
								j = land->realVerticesMapSide;
							}
							else
							{
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
}

const Forest* EditorObjectMgr::getForest( long ID )
{
	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
		{
			if ( (*iter)->getID() == ID )
				return (*iter);
		}

	gosASSERT( 0 ); // you shouldn't get here

	return NULL;
}

long EditorObjectMgr::getForests( Forest** pForests, long& count )
{

	if ( forests.Count() > count )
	{
		count = forests.Count();
		return -1;
	}

	count = 0;

	for ( FOREST_LIST::EIterator iter = forests.Begin(); !iter.IsDone(); iter++ )
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
	// figure out how many trees are in the group
	Group* pGroup = &groups[TREE_GROUP];
	if ( pGroup )
	{
		EList< Building, Building& >::EIterator buildIter;
		for( buildIter = groups[group].buildings.Begin();
			!buildIter.IsDone(); buildIter++ )
			{
				if ( (*buildIter).forestId == treeGroup )
					treeCount++;
			}

		int whichOne = RandomNumber( treeCount );
		treeCount = 0;

		int counter = 0;
		for( buildIter = groups[group].buildings.Begin();
			!buildIter.IsDone(); buildIter++, counter++ )
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
