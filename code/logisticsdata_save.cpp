// LogisticsData: removeMechsInForceGroup, pilot ranking, save / load, setMissionCompleted and the mech / pilot / variant lookups.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "pilot_pick.h"
#include "logisticsdata_internal.h"
#include "content_error.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "objective_text.h"
#include "purchase_catalog.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <optional>
#include <format>


void LogisticsData::removeMechsInForceGroup()
{
	// End the edit while its mech is alive; snapshots must retain the owned variant.
	if ( currentlyModifiedMech && currentlyModifiedMech->getForceGroup() )
		cancelMechModfications();

	// Capture {variantName, pVariant*} for every deployed mech BEFORE deletion.
	// pVariant is owned by the variants list, not by LogisticsMech, so the pointer
	// survives the delete below and can be used by setMissionCompleted() as a fallback
	// for mod-purchased variants whose name is absent from the loaded variants list.
	deployedMechSnapshot.clear();
	for ( auto si = inventory.begin(); si != inventory.end(); ++si )
	{
		if ( (*si)->getForceGroup() )
		{
			DeployedMechSnapshot snap;
			snap.variantName = (*si)->getName(); // a copy: variantName == LogisticsVariant::getName()
			snap.pVariant    = (*si)->getVariant();
			deployedMechSnapshot.push_back( snap );
		}
	}

	if ( !std::ssize(inventory) )
		return;
	// Newest first: the mechs of the force group are deleted from the back of the inventory forward.
	for ( auto iter = inventory.end(); iter != inventory.begin(); )
	{
		--iter;
		if ( (*iter)->getForceGroup() )
		{
			iter = inventory.erase( iter );
		}
	}
}

// A pilot already flying a live mech (by name). VIEWER builds never see the pool.
static bool pilotInLiveMech(LogisticsPilot& p)
{
#ifndef VIEWER
	return MechWarrior::warriorInUse(const_cast<char*>(p.getName().c_str()));
#else
	(void)p;
	return true;
#endif
}

std::optional<std::reference_wrapper<const std::string>> LogisticsData::getBestPilot( long mechWeight )
{
	ensureMechBayDataLoaded("getBestPilot");

	// Selection lives in pilot_pick.h (selectPoolPilot) so the unit tier runs the same code.
	LogisticsPilot* chosen = selectPoolPilot<LogisticsPilot>(pilots, pilotInLiveMech,
		[this, mechWeight](LogisticsPilot* a, LogisticsPilot* b) { return comparePilots(a, b, mechWeight); });
	if (!chosen)
	{
		mc2::content::error(mc2::content::Kind::MissionData, "pilots", "getBestPilot: no available pilot not already in a mech");
		return std::nullopt;
	}
	return std::cref(chosen->getFileName());   // the pilots list outlives the caller's use
}

bool		LogisticsData::gotPilotsLeft()
{
	ensureMechBayDataLoaded("gotPilotsLeft");
	return poolHasUnusedPilot<LogisticsPilot>(pilots, pilotInLiveMech);
}

int LogisticsData::comparePilots( LogisticsPilot* p1, LogisticsPilot* p2, long weight )
{
	if ( p1->isUsed() )
		return -1;
	else if ( p2->isUsed() )
		return 1;
	
		for ( auto mIter = instance->inventory.begin(); mIter != instance->inventory.end(); ++mIter )
		 {
			LogisticsPilot* pPilot = (*mIter)->getPilot();
			if ( pPilot )
			{
				if ( pPilot->getID() == p1->getID()  )
					return -1;
				else if ( pPilot->getID() == p2->getID() )
					return 1;
			}
		 }



	if ( p1->getRank() > p2->getRank() )
		return 1;
	
	else if ( p1->getRank() < p2->getRank() )
		return -1;

	// need to check specialty skill text for weight, not really done yet

	else if ( p2->getGunnery() > p1->getGunnery() )
		return -1;

	else if ( p1->getGunnery() > p2->getGunnery() )
		return 1;

	else if ( p1->getPiloting() > p2->getPiloting() )
		return 1;

	else if ( p2->getPiloting() > p1->getPiloting() )
		return -1;

	return 0;


}

long	LogisticsData::save( FitIniFile& file )
{
		
	int variantCount = 0;
	// save the player created variants
	for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
		{
			if ( !(*vIter)->isDesignerMech() )
			{
				(*vIter)->save( file, variantCount );
				variantCount++;
			}
		}

	file.writeBlock( "Version" );
	file.writeIdLong( "VersionNumber", SaveGameVersionNumber);

	file.writeBlock( "General" );	
	
	file.writeIdLong( "VariantCount", variantCount );
	file.writeIdLong( "PilotCount", static_cast<long>(std::ssize(pilots)) );
	file.writeIdLong( "InventoryCount", static_cast<long>(std::ssize(inventory)) );
	file.writeIdBoolean( "FirstTimeResults", MissionResults::FirstTimeResults);

	// save the campaign info
	missionInfo->save( file );

	int pilotCount = 0;
	// save the pilots
	for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
		{
			(*pIter).save( file, pilotCount++ );
		}

	int mechCount = 0;
	// save the inventory
	for ( auto mIter = inventory.begin(); mIter != inventory.end(); ++mIter )
		{
			(*mIter)->save( file, mechCount++ );
		}

	return 0;
}

void LogisticsData::clearVariants()
{
	// Newest first: the player-made variants are deleted from the back of the list forward.
	for ( auto iter = variants.end(); iter != variants.begin(); )
	{
		--iter;
		if ( !(*iter)->isDesignerMech() )
		{
			iter = variants.erase( iter );
		}
	}
}

std::expected<void, long> LogisticsData::load( FitIniFile& file )
{
	clearInventory();
	resourcePoints = 0;
	clearPilots();
	initPilots();
	clearVariants();

	if ( !missionInfo )
		missionInfo = std::make_unique_for_overwrite<LogisticsMissionInfo>();

	missionInfo->load( file );

	long result = file.seekBlock( "Version" );
	if (result != NO_ERR)
	{
		PAUSE(("SaveGame has no version number.  Not Loading"));
		return std::unexpected(-1L);
	}

	long testVersionNum = 0;
	result = file.readIdLong( "VersionNumber", testVersionNum);
	if (result != NO_ERR)
	{
		PAUSE(("SaveGame has no version number.  Not Loading"));
		return std::unexpected(-1L);
	}

	if (testVersionNum != SaveGameVersionNumber)
	{
		PAUSE(("SaveGame is not Version %d.  It was Version %d which is not valid!",SaveGameVersionNumber,testVersionNum));
		return std::unexpected(-1L);
	}

	file.seekBlock( "General" );	
	
	long variantCount, pilotCount, inventoryCount;
	variantCount = pilotCount = inventoryCount = 0;

	file.readIdLong( "VariantCount", variantCount );
	file.readIdLong( "PilotCount", pilotCount );
	file.readIdLong( "InventoryCount", inventoryCount );
	file.readIdBoolean( "FirstTimeResults", MissionResults::FirstTimeResults);

	char tmp[64];

	// load variants
	for ( int i = 0; i < variantCount; i++ )
	{
		mc2::fmt::to_buf(tmp, "Variant{:d}", mc2::fmt::as_i32(i));
		file.seekBlock( tmp );
		if ( !loadVariant( file ) )
		{
			gosASSERT( 0 );
			return std::unexpected(-1L);
		}
	}

	// load pilots
	for (int i = 0; i < pilotCount; i++ )
	{
		mc2::fmt::to_buf(tmp, "Pilot{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.seekBlock( tmp ) )
		{
			gosASSERT( 0 );
		}

		// A pilot block without a FileName matches no pilot.
		const std::string pilotKey = objective_text::readPlain( file, "FileName" ).value_or( std::string() );

		// pilot should already exist
		for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
		{
			if ( mc2::str::iequals( (*pIter).getFileName(), pilotKey ) )
			{
				(*pIter).load( file );
			}

			(*pIter).setAvailable( true );
		}
	}

	// load inventory
	int count = 0;
	for (int i = 0; i < inventoryCount; i++ )
	{
		mc2::fmt::to_buf(tmp, "Inventory{:d}", mc2::fmt::as_i32(i)) ;
		if ( NO_ERR != file.seekBlock( tmp ) )
		{
			gosASSERT( 0 );
		}

		loadMech( file, count );
	}

	updateAvailability();


	//Start finding the Leaks
	//systemHeap->dumpRecordLog();

	return {};
}

std::expected<void, long> LogisticsData::loadVariant( FitIniFile& file )
{
	char tmp[256] = {};

	const std::optional<std::string> chassisKey = objective_text::readPlain( file, "Chassis" );
	if ( !chassisKey )
	{
		MC2_LOG(Core, Warn, "logistics: a saved variant has no Chassis and is rejected");
		return std::unexpected(INVALID_FILE_NAME);
	}

	const LogisticsChassis* pChassis  = nullptr;
	const auto chassisPack = objective_text::readPlain(file, "PurchasePack");
	// go out and find that chassis
	for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
	{
		if ( mc2purchase::identityMatches(mc2purchase::chassisSource((*vIter)->getFileName()),
			(*vIter)->getFileName(), chassisPack, *chassisKey) )
		{
			pChassis = (*vIter)->getChassis();
		}
	}

	if ( !pChassis ) // we can always try and make it ourseleves, but it should have been loaded by now
	{
		gosASSERT( 0 );
		return std::unexpected(INVALID_FILE_NAME);
	}

	// A variant without a name would take the chassis file name, so it is rejected.
	const std::optional<std::string> variantName = objective_text::readPlain( file, "VariantName" );
	if ( !variantName )
	{
		MC2_LOG(Core, Warn, "logistics: a saved variant of chassis '{}' has no VariantName and is rejected", *chassisKey);
		return std::unexpected(INVALID_FILE_NAME);
	}

	// create the variant, add to the list
	auto variant = std::make_unique<LogisticsVariant>( pChassis, 0 );
	LogisticsVariant* pVariant = variant.get();
	variants.push_back( std::move(variant) );
	
	pVariant->setName( *variantName );

	long componentCount = 0;
	long x = 0;
	long y = 0;
	long location = 0;
	long id = 0;

	char tmp2[256];
	
	// read number of components
	file.readIdLong( "ComponentCount", componentCount );

	// add those components
	for ( int i = 0; i < componentCount; i++ )
	{
		mc2::fmt::to_buf(tmp, "Component{:d}", mc2::fmt::as_i32(i));
		file.readIdLong(tmp, id );
		
		strcpy( tmp2, tmp );
		strcat( tmp2, "x" );
		file.readIdLong( tmp2, x );

		strcpy( tmp2, tmp );
		strcat( tmp2, "y" );
		file.readIdLong( tmp2, y );

		strcpy( tmp2, tmp );
		strcat( tmp2, "Location" );
		file.readIdLong( tmp2, location );

		pVariant->addComponent( id, x, y );
	}

	return {};
}

std::expected<void, long> LogisticsData::loadMech( FitIniFile& file, int& count )
{
	const std::string variantKey = objective_text::readPlain( file, "Variant" ).value_or( std::string() );
	const auto packKey = objective_text::readPlain(file, "PurchasePack");
	for ( auto mIter = variants.begin(); mIter != variants.end(); ++mIter )
	{
		if ( mc2purchase::identityMatches(mc2purchase::chassisSource((*mIter)->getFileName()),
			(*mIter)->getName(), packKey, variantKey) )
		{
			
			auto mech = std::make_unique<LogisticsMech>( mIter->get(), count );
			LogisticsMech* pMech = mech.get();
			// A mech without a Pilot key has no pilot: the key is never read as the variant text.
			const std::optional<std::string> pilotKey = objective_text::readPlain( file, "Pilot" );
			inventory.push_back( std::move(mech) );

			if ( pilotKey )
			{
				for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
				{
					if ( mc2::str::iequals( (*pIter).getFileName(), *pilotKey ) )
					{
						pMech->setPilot( &(*pIter ) );
						count++;
						if ( count > -1 && count < 13 )
							pMech->setForceGroup( count );
						break;
					}
				}
			}

			// it could have had no pilot
			return {};
		}
	}

	return std::unexpected(-1L); // failed in finding the variant
}



void	LogisticsData::setMissionCompleted( )
{
#ifndef VIEWER
	const auto observation = objectiveObservation();
	const int beforeMission = observation ? getCBills() : 0;
	missionInfo->setMissionComplete();
	if (observation) {
		observation->Emit("resolve", 0, std::format("{{\"scope\":\"cbills\",\"stage\":\"mission\",\"old\":{},\"new\":{}}}", beforeMission, getCBills()));
	}

	rpJustAdded = 0;

	for ( auto iter = pilots.begin(); iter != pilots.end(); ++iter )
		{
			(*iter).setUsed( 0 );
		}

	for ( CObjective* pObjective : Team::home->objectives )
	{
		if ( pObjective->Status(Team::home->objectives) == OS_SUCCESSFUL )
		{
			const int beforeObjective = observation ? getCBills() : 0;
			addCBills( pObjective->ResourcePoints() );
			if (observation) {
				pObjective->ObserveCBills(beforeObjective, getCBills());
			}
		}
	}

	if (observation) {
		observation->settled = true;
		if (missionInfo->campaignOver()) { observation->Finish(); }
	}
	// Recover surviving pilots and their vehicles for the next mission.
	Team* pTeam = Team::home;

	int ForceGroupCount = 1;

	if ( pTeam )
	{
		for ( int i = pTeam->getRosterSize() - 1; i > -1; i-- )
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));

			// Carry-over diagnostics distinguish filtering from inventory recovery failure.
			{
				static const bool s_logiFlowTrace = false;
				if ( s_logiFlowTrace && pMover && mc2_diag::tagEnabled("LOGISTICS") )
				{
					int  _lg_otc  = pMover->getObjectType() ? static_cast<int>(pMover->getObjectType()->getObjectTypeClass()) : -1;
					bool _lg_gui  = pMover->isOnGUI();
					int  _lg_mt   = static_cast<int>(pMover->getMoveType());
					bool _lg_cmd  = (pMover->getCommanderId() == Commander::home->getId());
					bool _lg_pass = _lg_gui && (_lg_otc == BATTLEMECH_TYPE) && _lg_cmd && (_lg_mt != MOVETYPE_AIR);
					char _lg_nm[64]; const char* _lg_src = pMover->getName() ? pMover->getName() : "";
					size_t _lg_k;
					for (_lg_k=0; _lg_k<sizeof(_lg_nm)-1 && _lg_src[_lg_k]; ++_lg_k)
						_lg_nm[_lg_k] = (_lg_src[_lg_k]=='\\' || _lg_src[_lg_k]=='"') ? '_' : _lg_src[_lg_k];
					_lg_nm[_lg_k] = '\0';
					char _lg_buf[320];
					mc2::fmt::to_buf(_lg_buf, "{{\"site\":\"cull\",\"name\":\"{}\",\"on_gui\":{:d},\"type_class\":{:d},\"move_type\":{:d},\"cmd_match\":{:d},\"is_air\":{:d},\"dead\":{:d},\"carry_over\":{:d}}}", mc2::fmt::safe(_lg_nm), mc2::fmt::as_i32(_lg_gui?1:0), mc2::fmt::as_i32(_lg_otc), mc2::fmt::as_i32(_lg_mt), mc2::fmt::as_i32(_lg_cmd?1:0), mc2::fmt::as_i32((_lg_mt==MOVETYPE_AIR)?1:0), mc2::fmt::as_i32((pMover->isDestroyed()||pMover->isDisabled())?1:0), mc2::fmt::as_i32(_lg_pass?1:0));
					mc2_diag::writeEvent("LOGISTICS", 1, 0, _lg_buf);
				}
			}

			if ( pMover->isOnGUI() &&
				 (pMover->getObjectType()->getObjectTypeClass() == BATTLEMECH_TYPE) && 
				 (pMover->getCommanderId() == Commander::home->getId()) &&
				 (pMover->getMoveType() != MOVETYPE_AIR))
			{
				LogisticsMech* pMech = getMech( pMover->getName(), pMover->getPilot()->getName() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);

				DWORD base, highlight1, highlight2;
				(static_cast<Mech3DAppearance*>(pMover->getAppearance()))->getPaintScheme( base, highlight1, highlight2 );
				LogisticsPilot* pPilot = getPilot( pMover->getPilot()->getName() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);

				{
					static const bool s_lfResolve = false;
					if ( s_lfResolve && mc2_diag::tagEnabled("LOGISTICS") )
					{
						const char* _r_pn = (pMover->getPilot() && pMover->getPilot()->getName()) ? pMover->getPilot()->getName() : "";
						const char* _r_vn = (static_cast<BattleMech*>(pMover))->variantName ? (static_cast<BattleMech*>(pMover))->variantName : "";
						char _r_nm[64], _r_pnm[64], _r_vnm[64];
						const char* _r_srcs[3] = { pMover->getName() ? pMover->getName() : "", _r_pn, _r_vn };
						char* _r_dsts[3] = { _r_nm, _r_pnm, _r_vnm };
						for (int _r_s=0; _r_s<3; ++_r_s) { size_t _r_k; for(_r_k=0;_r_k<63 && _r_srcs[_r_s][_r_k];++_r_k) _r_dsts[_r_s][_r_k]=(_r_srcs[_r_s][_r_k]=='\\'||_r_srcs[_r_s][_r_k]=='"')?'_':_r_srcs[_r_s][_r_k]; _r_dsts[_r_s][_r_k]='\0'; }
						char _r_buf[400];
						mc2::fmt::to_buf(_r_buf, "{{\"site\":\"resolve\",\"name\":\"{}\",\"pilot\":\"{}\",\"variant\":\"{}\",\"mech_found\":{:d},\"pilot_found\":{:d},\"dead\":{:d}}}", mc2::fmt::safe(_r_nm), mc2::fmt::safe(_r_pnm), mc2::fmt::safe(_r_vnm), mc2::fmt::as_i32(pMech?1:0), mc2::fmt::as_i32(pPilot?1:0), mc2::fmt::as_i32((pMover->isDestroyed()||pMover->isDisabled())?1:0));
						mc2_diag::writeEvent("LOGISTICS", 1, 0, _r_buf);
					}
				}
				if ( pMech )
				{
					if ( pMover->isDestroyed() || pMover->isDisabled() )
					{
						removeMechFromInventory( pMech->getName().c_str(), pMover->getPilot()->getName() );
					}
					else
					{
						removeMechFromInventory( pMech->getName().c_str(), pMover->getPilot()->getName() );
						LogisticsVariant* pVar = getVariant( (static_cast<BattleMech*>(pMover))->variantName ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
						addMechToInventory( pVar, ForceGroupCount++, pPilot, base, highlight1, highlight2 );
						if ( pPilot )
							pPilot->setUsed( true );

					}
				}
				else // mech was recovered during the mission (or was a deployed mech removed by removeMechsInForceGroup)
				{
					if ( !pMover->isDestroyed() && !pMover->isDisabled() )
					{
						const char* vname = (static_cast<BattleMech*>(pMover))->variantName;
						LogisticsVariant* pVariant = getVariant( vname ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
						if ( !pVariant )
						{
							// Runtime variants can be absent from the base CSVs. Deployed snapshots
							// retain variant owners after their inventory mechs are removed.
							for ( const DeployedMechSnapshot& snap : deployedMechSnapshot )
							{
								if ( mc2::str::iequals( snap.variantName, mc2::str::from_c( vname ) ) && snap.pVariant )
								{
									pVariant = snap.pVariant;
									break;
								}
							}
							if ( !pVariant )
							{
								// A missing variant cannot be restored to inventory.
								mc2::fmt::print_out( "[LOGISTICS] setMissionCompleted: no variant for surviving mech '{}' -- mech lost\n",
								        mc2::fmt::safe(vname) );
								fflush( stdout );
							}
						}
						if ( pVariant )
						{
							addMechToInventory( pVariant, ForceGroupCount++, pPilot, base, highlight1, highlight2 );
							if ( pPilot )
								pPilot->setUsed( true );

						}
					}
				}

  				if ( pPilot )
				{
					pPilot->update( pMover->getPilot() );

				}
			}
		}
	}

#endif
}

std::optional<std::reference_wrapper<LogisticsMech>> LogisticsData::getMech( mc2::czstring MechName, mc2::czstring pilotName )
{
	const std::string wanted = mc2::str::from_c( MechName );
	for ( auto iter = inventory.begin(); iter != inventory.end(); ++iter )
	{
		if ( mc2::str::iequals( (*iter)->getName(), wanted ) )
		{
			if ( !pilotName )
			{
				if ( !(*iter)->getPilot() )
					return **iter;
			}

			else
			{
				if ( (*iter)->getPilot() && mc2::str::iequals( (*iter)->getPilot()->getName(), mc2::str::from_c( pilotName ) ) )
					return **iter;
			}
		}
	}

	return std::nullopt;
}

void LogisticsData::removeMechFromInventory( mc2::czstring mechName, mc2::czstring pilotName )
{
	auto pMech = getMech( mechName, pilotName );

	gosASSERT( pMech );

	if ( pMech )
	{
		if ( currentlyModifiedMech == &pMech->get() )
			cancelMechModfications();
		std::erase_if( inventory, [pMech](const auto& mech) { return mech.get() == &pMech->get(); } );
	}
}

std::optional<std::reference_wrapper<LogisticsPilot>> LogisticsData::getPilot( mc2::czstring pilotName )
{
	const std::string wanted = mc2::str::from_c( pilotName );
	// look for available ones first
	for ( auto iter = pilots.begin(); iter != pilots.end(); ++iter )
	{
		if ( (*iter).isAvailable() )
		{
			if ( mc2::str::iequals( (*iter).getName(), wanted ) )
			{
				return *iter;
			}
		}
	}
	
	for ( auto iter = pilots.begin(); iter != pilots.end(); ++iter )
	{
		if ( mc2::str::iequals( (*iter).getName(), wanted ) )
		{
			return *iter;
		}
	}

	return std::nullopt;
}

std::optional<std::reference_wrapper<LogisticsVariant>> LogisticsData::getVariant( mc2::czstring mechName )
{
	const std::string wanted = mc2::str::from_c( mechName );
	for ( auto iter = variants.begin(); iter != variants.end(); ++iter )
	{
		if ( mc2::str::iequals( (*iter)->getName(), wanted ) )
			return **iter;
	}

	return std::nullopt;
}
