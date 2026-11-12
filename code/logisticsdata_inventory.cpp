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
				array[count] = variant.get();
			}

			(count)++;	
		}
	}

	return retVal;

}




std::expected<void, int> LogisticsData::purchaseMech( LogisticsVariant* pVariant )
{
	if ( !pVariant )
		return std::unexpected(-1);

	int RP = pVariant->getCost();

	if ( cheatInfiniteMoney() || missionInfo->getCBills() - RP >= 0 )
	{
		int count = instance->createInstanceID( pVariant );
		instance->inventory.push_back( std::make_unique<LogisticsMech>( pVariant, count ) );
		if ( !cheatInfiniteMoney() )
			missionInfo->decrementCBills( pVariant->getCost() );
		return {};
	}

	return std::unexpected(NOT_ENOUGH_RESOURCE_POINTS);
}

std::expected<void, int> LogisticsData::canPurchaseMech( LogisticsVariant* pVar )
{
	if ( !pVar )
		return std::unexpected(INVALID_ID);

	if ( cheatInfiniteMoney() )
		return {};

	int RP = pVar->getCost();

	if ( missionInfo->getCBills() - RP >= 0 )
	{
		return {};
	}

	return std::unexpected(NOT_ENOUGH_RESOURCE_POINTS);

}



std::expected<void, int> LogisticsData::sellMech( LogisticsMech* pVar )
{
	if ( !pVar )
		return std::unexpected(-1);

	// Newest first: the last copy of the variant that is not in the force group is the one that is sold.
	for ( auto iter = instance->inventory.rbegin(); iter != instance->inventory.rend(); ++iter )
	{
		if ( (*iter)->getForceGroup() )
			continue;
		if ( (*iter)->getVariant() == pVar->getVariant() )
		{
			if ( instance->currentlyModifiedMech == iter->get() )
				instance->cancelMechModfications();
			int cost = ((*iter))->getCost();
			(*iter)->setPilot( nullptr );
			instance->inventory.erase( std::next( iter ).base() );

			missionInfo->incrementCBills( cost );
			return {};
		}
	}

	return std::unexpected(-1);
}

std::expected<void, int> LogisticsData::removeVariant( mc2::czstring varName )
{
	if ( !varName )
		return std::unexpected(-1);

	LogisticsVariant* pVar = nullptr;

	const std::string wanted = mc2::str::from_c( varName );
	if ( mc2::str::iequals( currentlyModifiedMech->getName(), wanted ) || mc2::str::iequals( oldVariant->getName(), wanted ) )
		return std::unexpected(-1);

    auto vIter = variants.begin();
	for ( ; vIter != variants.end(); ++vIter )
	{
		if ( mc2::str::iequals( (*vIter)->getName(), wanted ) )
		{
			pVar = vIter->get();
			break;
		}
	}

	if ( !pVar )
	{
		return std::unexpected(INVALID_VARIANT);
	}

    
	for ( auto iter = instance->inventory.rbegin(); iter != instance->inventory.rend(); ++iter )
	{
		if ( (*iter)->getVariant() == pVar )
		{
			return std::unexpected(VARIANT_IN_USE);
		}
	}

	variants.erase( vIter );

	return {};
}



int LogisticsData::createInstanceID( LogisticsVariant* pVar )
{
	int count = -1;
	for (auto & iter : inventory)
	{
		if ( pVar->getVariantID() == iter->getVariantID() ) 
		{
			int tmp = iter->getInstanceID();
			count = std::max(count, tmp);
		}
	}
	return count + 1;
}

std::optional<std::reference_wrapper<LogisticsVariant>> LogisticsData::getVariant( int ID )
{
	for (auto & variant : variants)
	{
		if ( variant->getID() == (ID & 0x00ffffff) )
		{
			return *variant;
		}
	}

	if ( instance->currentlyModifiedMech && ID == instance->currentlyModifiedMech->getID() )
		return *instance->currentlyModifiedMech->getVariant();

	return std::nullopt;
}

std::optional<std::reference_wrapper<LogisticsMech>> LogisticsData::getMech( int ID )
{
	for (auto & iter : inventory)
	{
		if ( iter->getID() == ID )
			return *iter;
	}

	return std::nullopt;
}




std::expected<void, int> LogisticsData::addMechToForceGroup( LogisticsMech* pMech, int slot )
{
	if ( !pMech )
		return std::unexpected(-1);

	if ( slot > 12 )
		return std::unexpected(-1);

	if ( pMech && !pMech->getForceGroup() )
	{
		pMech->setForceGroup( slot );
		return {};
	}
	else // find another of the same variant
	{

		auto pNewMech = getMechWithoutForceGroup( pMech );
		if ( pNewMech )
		{
			pNewMech->get().setForceGroup( slot );
			return {};
		}
	}
	return std::unexpected(-1);
}

std::expected<void, int> LogisticsData::removeMechFromForceGroup( int slot )
{
	for (auto & iter : inventory)
	{
		if ( iter->getForceGroup() == slot )
		{
			iter->setForceGroup( 0 );
			iter->setPilot( nullptr );
			return {};
		}
	}

	return std::unexpected(-1);

}

std::optional<std::reference_wrapper<LogisticsMech>> LogisticsData::getMechWithoutForceGroup( LogisticsMech* pMech )
{
	if ( !pMech->getForceGroup() )
		return *pMech;

	for (auto & iter : inventory)
		{
			if ( iter->getVariant() == pMech->getVariant() && !iter->getForceGroup() )
			{
				return *iter;
				
			}
		}
	
	return std::nullopt;
}
std::expected<void, int> LogisticsData::removeMechFromForceGroup( LogisticsMech* pMech, bool bRemovePilot )
{
	if ( !pMech )
		return std::unexpected(-1);

	if ( pMech && pMech->getForceGroup() )
	{
		pMech->setForceGroup( 0 );
		// no mechs in inventory have pilots
		if ( bRemovePilot )
			pMech->setPilot( nullptr );
		return {};
	}

	// find similar one
	for (auto & iter : inventory)
	{
		if ( iter->getVariant() == pMech->getVariant() && iter->getForceGroup() )
		{
			iter->setForceGroup( 0 );

			if ( bRemovePilot )
				pMech->setPilot( nullptr );

			return {};
		}
	}
	return std::unexpected(-1);
}

std::optional<std::reference_wrapper<LogisticsPilot>> LogisticsData::getFirstAvailablePilot()
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
			 return *iter;
		 }
	}

	return std::nullopt;

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
	if ( !vehiclePilotTried )
	{
		vehiclePilotTried = true;
		auto p = std::make_unique_for_overwrite<LogisticsPilot>();
		char nm[] = "PMWScout";
		if ( !p->init( nm ) )
		{
			p.reset();
		}
		else
		{
			p->setAvailable( false );
		}
		vehiclePilot = std::move(p);
	}
	return vehiclePilot.get();
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
			newList.push_back( iter->get() );
			count++;
		}
	}
	return newList;
}

LogisticsData::MechList	LogisticsData::getInventory()
{
	ensureMechBayDataLoaded("getInventory");
	MechList snapshot;
	snapshot.reserve( inventory.size() );
	for ( const auto& mech : inventory )
		snapshot.push_back( mech.get() );
	return snapshot;
}


void	LogisticsData::addMechToInventory( LogisticsVariant* pVar, LogisticsPilot* pPilot, int ForceGroup,
										  bool bSubtractPts)
{
	if ( pVar )
	{
		int count = instance->createInstanceID( pVar );
		auto mech = std::make_unique<LogisticsMech>( pVar, count );
		LogisticsMech* pMech = mech.get();
		inventory.push_back( std::move(mech) );
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
		auto mech = std::make_unique<LogisticsMech>( pVar, count );
		LogisticsMech* pMech = mech.get();
		inventory.push_back( std::move(mech) );
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

std::optional<std::reference_wrapper<LogisticsVariant>> LogisticsData::getVariant( mc2::czstring pCSVFileName, int VariantNum )
{
	const std::string lowerCase = mc2::str::ascii_lower( mc2::str::from_c( pCSVFileName ) );
	for (auto & variant : variants)
	{
		if ( variant->getFileName().find( lowerCase ) != std::string::npos
			&& ((variant->getVariantID()>>16)&0xff) == VariantNum )
		{
			return *variant;
		}
	}

	return std::nullopt;
}
