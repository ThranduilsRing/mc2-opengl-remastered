// LogisticsData: components, mech purchase and sale, force groups, inventory lists and the vehicle pilot (with the infinite-money cheat gate).
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"


// Interactive cheat gates -- read once at startup, default OFF (unset = nullptr = false)
// The launcher writes this into launcher_env.json, which main() applies after static initialization, so it
// is read at first use rather than in a namespace-scope initializer.
static bool cheatInfiniteMoney() { static const bool on = mc2::gate::get<"MC2_CHEAT_INFINITE_MONEY">(); return on; }


int LogisticsData::getAvailableComponents( LogisticsComponent** pComps, int& maxCount )
{
	ensureMechBayDataLoaded("getAvailableComponents");
	int retVal = 0;

	int i = 0;
	for ( COMPONENT_LIST::EIterator iter = components.Begin(); 
		!iter.IsDone(); iter++ )
		{
			if ( (*iter).isAvailable()  )
			{
				if ( i < maxCount )
					pComps[i]	= &(*iter);	
				else 
					retVal = NEED_BIGGER_ARRAY;
				++i;

				
			}
		}

	maxCount = i;

	return retVal; 

}
int	LogisticsData::getAllComponents( LogisticsComponent** pComps, int& maxCount )
{
	int retVal = 0;
	
	int i = 0;
	for ( COMPONENT_LIST::EIterator iter = components.Begin(); 
		!iter.IsDone(); iter++ )
		{
			if ( i < maxCount )
					pComps[i]	= &(*iter);	
				else 
					retVal = NEED_BIGGER_ARRAY;
				++i;
		}

	maxCount = components.Count();

	return retVal; 
}




int LogisticsData::getPurchasableMechs( LogisticsVariant** array, int& count )
{
	ensureMechBayDataLoaded("getPurchasableMechs");
	long retVal = 0;
	long arraySize = count;

	count = 0;
	for( VARIANT_LIST::EIterator iter = instance->variants.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->isAvailable() )
		{
			if ( count >= arraySize )
			{
				retVal =  NEED_BIGGER_ARRAY; // keep counting
			}
			else
			{
				array[count] = (*iter);
			}

			(count)++;	
		}
	}

	return retVal;

}




int LogisticsData::purchaseMech( LogisticsVariant* pVariant )
{
	if ( !pVariant )
		return -1;

	int RP = pVariant->getCost();

	if ( cheatInfiniteMoney() || missionInfo->getCBills() - RP >= 0 )
	{
		int count = instance->createInstanceID( pVariant );
		LogisticsMech* pMech = new LogisticsMech( pVariant, count );
		instance->inventory.Append( pMech );
		if ( !cheatInfiniteMoney() )
			missionInfo->decrementCBills( pVariant->getCost() );
		return 0;
	}

	return NOT_ENOUGH_RESOURCE_POINTS;
}

int LogisticsData::canPurchaseMech( LogisticsVariant* pVar )
{
	if ( !pVar )
		return INVALID_ID;

	if ( cheatInfiniteMoney() )
		return 0;

	int RP = pVar->getCost();

	if ( missionInfo->getCBills() - RP >= 0 )
	{
		return 0;
	}

	return NOT_ENOUGH_RESOURCE_POINTS;

}



int LogisticsData::sellMech( LogisticsMech* pVar )
{
	if ( !pVar )
		return -1;

	for ( MECH_LIST::EIterator iter = instance->inventory.End(); !iter.IsDone(); iter-- )
	{
		if ( (*iter)->getForceGroup() )
			continue;
		if ( (*iter)->getVariant() == pVar->getVariant() )
		{
			int cost = ((*iter))->getCost();
			(*iter)->setPilot( nullptr );
			delete *iter;
			instance->inventory.Delete( iter );

			missionInfo->incrementCBills( cost );
			return 0;
		}
	}

	return -1;
}

int LogisticsData::removeVariant( const char* varName )
{
	if ( !varName )
		return -1;

	LogisticsVariant* pVar = nullptr;

	if ( currentlyModifiedMech->getName() == varName || oldVariant->getName() == varName )
		return -1;

    VARIANT_LIST::EIterator vIter = variants.Begin();
	for ( ; !vIter.IsDone(); vIter++ )
	{
		if ( (*vIter)->getName().Compare( varName, 0 ) == 0 )
		{
			pVar = (*vIter );
			break;
		}
	}

	if ( !pVar )
	{
		return INVALID_VARIANT;
	}

    
	for (MECH_LIST::EIterator iter = instance->inventory.End(); !iter.IsDone(); iter-- )
	{
		if ( (*iter)->getVariant() == pVar )
		{
			return VARIANT_IN_USE;
		}
	}

	delete pVar;
	variants.Delete( vIter );

	return 0;
}



int LogisticsData::createInstanceID( LogisticsVariant* pVar )
{
	int count = -1;
	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( pVar->getVariantID() == (*iter)->getVariantID() ) 
		{
			int tmp = (*iter)->getInstanceID();
			if ( tmp > count )
				count = tmp;
		}
	}
	return count + 1;
}

LogisticsVariant* LogisticsData::getVariant( int ID )
{
	for ( VARIANT_LIST::EIterator iter = variants.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getID() == (ID & 0x00ffffff) )
		{
			return *iter;
		}
	}

	if ( instance->currentlyModifiedMech && ID == instance->currentlyModifiedMech->getID() )
		return instance->currentlyModifiedMech->getVariant();

	return nullptr;
}

LogisticsMech*	LogisticsData::getMech( int ID )
{
	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getID() == ID )
			return (*iter );
	}

	return nullptr;
}




int LogisticsData::addMechToForceGroup( LogisticsMech* pMech, int slot )
{
	if ( !pMech )
		return -1;

	if ( slot > 12 )
		return -1;

	if ( pMech && !pMech->getForceGroup() )
	{
		pMech->setForceGroup( slot );
		return 0;
	}
	else // find another of the same variant
	{

		LogisticsMech* pNewMech = getMechWithoutForceGroup( pMech );
		if ( pNewMech )
		{
			pNewMech->setForceGroup( slot );
			return 0;
		}
	}
	return -1;
}

int		LogisticsData::removeMechFromForceGroup( int slot )
{
	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getForceGroup() == slot )
		{
			(*iter)->setForceGroup( 0 );
			(*iter)->setPilot( nullptr );
			return 0;
		}
	}

	return -1;

}

LogisticsMech*		LogisticsData::getMechWithoutForceGroup( LogisticsMech* pMech )
{
	if ( !pMech->getForceGroup() )
		return pMech;

	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
		{
			if ( (*iter)->getVariant() == pMech->getVariant() && !(*iter)->getForceGroup() )
			{
				return (*iter);
				
			}
		}
	
	return nullptr;
}
int LogisticsData::removeMechFromForceGroup( LogisticsMech* pMech, bool bRemovePilot )
{
	if ( !pMech )
		return -1;

	if ( pMech && pMech->getForceGroup() )
	{
		pMech->setForceGroup( 0 );
		// no mechs in inventory have pilots
		if ( bRemovePilot )
			pMech->setPilot( nullptr );
		return 0;
	}

	// find similar one
	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getVariant() == pMech->getVariant() && (*iter)->getForceGroup() )
		{
			(*iter)->setForceGroup( 0 );

			if ( bRemovePilot )
				pMech->setPilot( nullptr );

			return 0;
		}
	}
	return -1;
}

LogisticsPilot* LogisticsData::getFirstAvailablePilot()
{
	ensureMechBayDataLoaded("getFirstAvailablePilot");
	for ( PILOT_LIST::EIterator iter = pilots.Begin(); !iter.IsDone(); iter++  )
	{
		 bool bIsUsed = false;
		 for ( MECH_LIST::EIterator mIter = inventory.Begin(); !mIter.IsDone(); mIter++ )
		 {
			LogisticsPilot* pPilot = (*mIter)->getPilot();
			if ( pPilot && pPilot->getID() == (*iter).getID() )
			{
				bIsUsed = true;
				break;
			}
		 }
		 if ( !bIsUsed )
		 {
			 return &(*iter);
		 }
	}

	return nullptr;

}

//---------------------------------------------------------------------------
// VEHICLE-AUTO-PILOT-1: a deployed unit is a vehicle/support unit (not a BattleMech)
// when its object type class != BATTLEMECH_TYPE. Bridge: chassis fitID -> ObjectType.
bool LogisticsData::isVehicleUnit( LogisticsMech* pMech )
{
	// Logistics-safe vehicle detection (NO ObjectManager->loadObjectType -- objTypeManager
	// is null at the logistics/pilot-ready screen and crashes, objmgr.cpp:4049).
	if ( !pMech || !pMech->getVariant() )
		return false;
	// The chassis carries a logistics-safe vehicle flag, computed once from CSV-visible
	// signals in LogisticsChassis::init (objTypeManager is null here so the real object class
	// is unavailable). Covers both registry vehicles and force-group support units / copters.
	const LogisticsChassis* ch = pMech->getVariant()->getChassis();
	return ( ch && ch->isVehicle() );
}

//---------------------------------------------------------------------------
// Shared standard vehicle pilot (PMWScout, resolved via VFS from the cveg dep's
// data/missions/warriors). Assigned to deployed vehicles so they satisfy the launch
// pilot-requirement without consuming a roster mech-pilot, and so they deploy with a
// vehicle pilot file. setAvailable(false) keeps it out of the AVAILABLE PILOTS list.
// Lazily created + cached; null if the file does not resolve (campaign without cveg)
// -> the vehicle stays unpiloted, no worse than before.
LogisticsPilot* LogisticsData::getVehiclePilot()
{
	static LogisticsPilot* s_vehiclePilot = nullptr;
	static bool s_tried = false;
	if ( !s_tried )
	{
		s_tried = true;
		LogisticsPilot* p = new LogisticsPilot;
		char nm[] = "PMWScout";
		if ( p->init( nm ) == -1 )
		{
			delete p;
			p = nullptr;
		}
		else
		{
			p->setAvailable( false );
		}
		s_vehiclePilot = p;
	}
	return s_vehiclePilot;
}


void	LogisticsData::getForceGroup( EList<LogisticsMech*, LogisticsMech*>& newList )
{
	ensureMechBayDataLoaded("getForceGroup");
	int count = 0;

	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( count > 11 )
			break;

		if ( (*iter)->getForceGroup() )
		{
			newList.Append( (*iter) );
			count++;
		}
	}
}

void	LogisticsData::getInventory( EList<LogisticsMech*, LogisticsMech*>& newList )
{
	ensureMechBayDataLoaded("getInventory");
	for ( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{

		newList.Append( (*iter) );
	}
}


void	LogisticsData::addMechToInventory( LogisticsVariant* pVar, LogisticsPilot* pPilot, int ForceGroup,
										  bool bSubtractPts)
{
	if ( pVar )
	{
		int count = instance->createInstanceID( pVar );
		LogisticsMech* pMech = new LogisticsMech( pVar, count );
		inventory.Append( pMech );
		pMech->setForceGroup( ForceGroup );
		if ( pPilot && !pPilot->isDead() )
			pMech->setPilot( pPilot );
		if ( ForceGroup > -1 && ForceGroup < 13 )
			pMech->setForceGroup( ForceGroup );
		if ( bSubtractPts )
			missionInfo->decrementCBills( pMech->getCost() );
	}
}
void	LogisticsData::addMechToInventory( LogisticsVariant* pVar, int addToForceGroup, 
										  LogisticsPilot* pPilot,
										  unsigned long baseColor,
										  unsigned long highlight1,
										  unsigned long highlight2 )
{
	if ( pVar )
	{
		int count = instance->createInstanceID( pVar );
		LogisticsMech* pMech = new LogisticsMech( pVar, count );
		inventory.Append( pMech );
		if ( addToForceGroup > -1 && addToForceGroup < 13 )
			pMech->setForceGroup( addToForceGroup );
		if ( pPilot && !pPilot->isDead() )
			pMech->setPilot( pPilot );
		pMech->setColors( baseColor, highlight1, highlight2 );
		return;
	}
	else
	{
		gosASSERT(!"couldn't add the mech to the inventory" );
	}
}

LogisticsVariant* LogisticsData::getVariant( const char* pCSVFileName, int VariantNum )
{
	EString lowerCase = pCSVFileName;
	lowerCase.MakeLower();
	for( VARIANT_LIST::EIterator iter = variants.Begin(); !iter.IsDone(); iter++ )
	{
		if ( -1 !=( (*iter)->getFileName().Find( lowerCase, -1 ) )
			&& (((*iter)->getVariantID()>>16)&0xff) == VariantNum )
		{
			return *iter;
		}
	}

	return nullptr;
}
