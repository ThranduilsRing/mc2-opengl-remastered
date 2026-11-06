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
	for (auto & component : components)
		{
			if ( component.isAvailable()  )
			{
				if ( i < maxCount )
					pComps[i]	= &component;	
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
	for (auto & component : components)
		{
			if ( i < maxCount )
					pComps[i]	= &component;	
				else 
					retVal = NEED_BIGGER_ARRAY;
				++i;
		}

	maxCount = static_cast<int>(std::ssize(components));

	return retVal; 
}




int LogisticsData::getPurchasableMechs( LogisticsVariant** array, int& count )
{
	ensureMechBayDataLoaded("getPurchasableMechs");
	long retVal = 0;
	long arraySize = count;

	count = 0;
	for (auto & variant : instance->variants)
	{
		if ( variant->isAvailable() )
		{
			if ( count >= arraySize )
			{
				retVal =  NEED_BIGGER_ARRAY; // keep counting
			}
			else
			{
				array[count] = variant;
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
		instance->inventory.push_back( pMech );
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

	// Newest first: the last copy of the variant that is not in the force group is the one that is sold.
	for ( auto iter = instance->inventory.rbegin(); iter != instance->inventory.rend(); ++iter )
	{
		if ( (*iter)->getForceGroup() )
			continue;
		if ( (*iter)->getVariant() == pVar->getVariant() )
		{
			int cost = ((*iter))->getCost();
			(*iter)->setPilot( nullptr );
			delete *iter;
			instance->inventory.erase( std::next( iter ).base() );

			missionInfo->incrementCBills( cost );
			return 0;
		}
	}

	return -1;
}

int LogisticsData::removeVariant( mc2::czstring varName )
{
	if ( !varName )
		return -1;

	LogisticsVariant* pVar = nullptr;

	const std::string wanted = mc2::str::from_c( varName );
	if ( mc2::str::iequals( currentlyModifiedMech->getName(), wanted ) || mc2::str::iequals( oldVariant->getName(), wanted ) )
		return -1;

    auto vIter = variants.begin();
	for ( ; vIter != variants.end(); ++vIter )
	{
		if ( mc2::str::iequals( (*vIter)->getName(), wanted ) )
		{
			pVar = (*vIter );
			break;
		}
	}

	if ( !pVar )
	{
		return INVALID_VARIANT;
	}

    
	for ( auto iter = instance->inventory.rbegin(); iter != instance->inventory.rend(); ++iter )
	{
		if ( (*iter)->getVariant() == pVar )
		{
			return VARIANT_IN_USE;
		}
	}

	delete pVar;
	variants.erase( vIter );

	return 0;
}



int LogisticsData::createInstanceID( LogisticsVariant* pVar )
{
	int count = -1;
	for (auto & iter : inventory)
	{
		if ( pVar->getVariantID() == iter->getVariantID() ) 
		{
			int tmp = iter->getInstanceID();
			if ( tmp > count )
				count = tmp;
		}
	}
	return count + 1;
}

LogisticsVariant* LogisticsData::getVariant( int ID )
{
	for (auto & variant : variants)
	{
		if ( variant->getID() == (ID & 0x00ffffff) )
		{
			return variant;
		}
	}

	if ( instance->currentlyModifiedMech && ID == instance->currentlyModifiedMech->getID() )
		return instance->currentlyModifiedMech->getVariant();

	return nullptr;
}

LogisticsMech*	LogisticsData::getMech( int ID )
{
	for (auto & iter : inventory)
	{
		if ( iter->getID() == ID )
			return iter;
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
	for (auto & iter : inventory)
	{
		if ( iter->getForceGroup() == slot )
		{
			iter->setForceGroup( 0 );
			iter->setPilot( nullptr );
			return 0;
		}
	}

	return -1;

}

LogisticsMech*		LogisticsData::getMechWithoutForceGroup( LogisticsMech* pMech )
{
	if ( !pMech->getForceGroup() )
		return pMech;

	for (auto & iter : inventory)
		{
			if ( iter->getVariant() == pMech->getVariant() && !iter->getForceGroup() )
			{
				return iter;
				
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
	for (auto & iter : inventory)
	{
		if ( iter->getVariant() == pMech->getVariant() && iter->getForceGroup() )
		{
			iter->setForceGroup( 0 );

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
	for ( auto iter = pilots.begin(); iter != pilots.end(); ++iter )
	{
		 bool bIsUsed = false;
		 for ( auto mIter = inventory.begin(); mIter != inventory.end(); ++mIter )
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
		auto p = std::make_unique_for_overwrite<LogisticsPilot>();
		char nm[] = "PMWScout";
		if ( p->init( nm ) == -1 )
		{
			p.reset();
		}
		else
		{
			p->setAvailable( false );
		}
		s_vehiclePilot = p.release();
	}
	return s_vehiclePilot;
}


LogisticsData::MechList	LogisticsData::getForceGroup()
{
	ensureMechBayDataLoaded("getForceGroup");
	MechList newList;
	int count = 0;

	for ( auto iter = inventory.begin(); iter != inventory.end(); ++iter )
	{
		if ( count > 11 )
			break;

		if ( (*iter)->getForceGroup() )
		{
			newList.push_back( (*iter) );
			count++;
		}
	}
	return newList;
}

LogisticsData::MechList	LogisticsData::getInventory()
{
	ensureMechBayDataLoaded("getInventory");
	return inventory;
}


void	LogisticsData::addMechToInventory( LogisticsVariant* pVar, LogisticsPilot* pPilot, int ForceGroup,
										  bool bSubtractPts)
{
	if ( pVar )
	{
		int count = instance->createInstanceID( pVar );
		LogisticsMech* pMech = new LogisticsMech( pVar, count );
		inventory.push_back( pMech );
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
		inventory.push_back( pMech );
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

LogisticsVariant* LogisticsData::getVariant( mc2::czstring pCSVFileName, int VariantNum )
{
	const std::string lowerCase = mc2::str::ascii_lower( mc2::str::from_c( pCSVFileName ) );
	for (auto & variant : variants)
	{
		if ( variant->getFileName().find( lowerCase ) != std::string::npos
			&& ((variant->getVariantID()>>16)&0xff) == VariantNum )
		{
			return variant;
		}
	}

	return nullptr;
}
