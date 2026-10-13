// LogisticsData: removeMechsInForceGroup, pilot ranking, save / load, setMissionCompleted and the mech / pilot / variant lookups.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "objective_text.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <optional>


void LogisticsData::removeMechsInForceGroup()
{
	// Capture {variantName, pVariant*} for every deployed mech BEFORE deletion.
	// pVariant is owned by the variants list, not by LogisticsMech, so the pointer
	// survives the delete below and can be used by setMissionCompleted() as a fallback
	// for mod-purchased variants whose name is absent from the loaded variants list.
	deployedMechSnapshot.clear();
	for ( MECH_LIST::EIterator si = inventory.Begin(); !si.IsDone(); si++ )
	{
		if ( (*si)->getForceGroup() )
		{
			DeployedMechSnapshot snap;
			snap.variantName = (*si)->getName(); // a copy: variantName == LogisticsVariant::getName()
			snap.pVariant    = (*si)->getVariant();
			deployedMechSnapshot.push_back( snap );
		}
	}

	if ( !inventory.Count() )
		return;
	for ( MECH_LIST::EIterator iter = inventory.End(); !iter.IsDone();  )
	{
		if ( (*iter)->getForceGroup() )
		{
			MECH_LIST::EIterator tmpIter = iter;
			iter--;
			delete *tmpIter;
			inventory.Delete( tmpIter );
		}
		else
			iter--;
	}
}

const char*	LogisticsData::getBestPilot( long mechWeight )
{
	ensureMechBayDataLoaded("getBestPilot");

	LogisticsPilot** pPilots = (LogisticsPilot**)_alloca( pilots.Count() * sizeof( LogisticsPilot*) );
	memset( pPilots, 0, pilots.Count() * sizeof( LogisticsPilot*) );

	int counter = 0;
#ifndef VIEWER
	for ( PILOT_LIST::EIterator iter = pilots.Begin();
		!iter.IsDone(); iter++ )
		{
			const char *nameCheck = (*iter).getName().c_str();
			if ( (*iter).isAvailable() && (MPlayer || !MechWarrior::warriorInUse((char *)nameCheck)) )
				pPilots[counter++] = &(*iter);
		}
#endif

	long count = counter;

	for ( int i = 1; i < count; ++i )
	{
		LogisticsPilot* cur = pPilots[i];
		for ( int j = 0; j < i; ++j )
		{
			if ( comparePilots( cur, pPilots[j], mechWeight ) > 0 && j != i )
			{
				pPilots[i] = pPilots[j];
				pPilots[j] = cur;
				break;
			}
		}
	}

	for (int i = 0; i < count; i++ )
	{
		if ( pPilots[i]->isAvailable() && !pPilots[i]->isUsed() )
		{
			pPilots[i]->setUsed( 1 );
			return pPilots[i]->getFileName().c_str();   // the pilots list outlives the caller's use
		}
	}

	gosASSERT( !"We're out of pilots, shouldn't be here" );
	
	pPilots[0]->setUsed( true );
	return pPilots[0]->getFileName().c_str();


}

bool		LogisticsData::gotPilotsLeft()
{
	ensureMechBayDataLoaded("gotPilotsLeft");

	LogisticsPilot** pPilots = (LogisticsPilot**)_alloca( pilots.Count() * sizeof( LogisticsPilot*) );
	memset( pPilots, 0, pilots.Count() * sizeof( LogisticsPilot*) );

	int counter = 0;

	#ifndef VIEWER

	for ( PILOT_LIST::EIterator iter = pilots.Begin();
		!iter.IsDone(); iter++ )
		{
			const char *nameCheck = (*iter).getName().c_str();
			if ( (*iter).isAvailable() && ( MPlayer || !MechWarrior::warriorInUse((char *)nameCheck)) )
				pPilots[counter++] = &(*iter);
		}

	#endif
	long count = counter;

	for ( int i = 0; i < count; i++ )
	{
		if ( pPilots[i]->isAvailable() && !pPilots[i]->isUsed() )
		{
			return 1;
		}
	}

	return 0;

}

int LogisticsData::comparePilots( LogisticsPilot* p1, LogisticsPilot* p2, long weight )
{
	if ( p1->isUsed() )
		return -1;
	else if ( p2->isUsed() )
		return 1;
	
		for ( MECH_LIST::EIterator mIter = instance->inventory.Begin(); !mIter.IsDone(); mIter++ )
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
	
	else if ( p2->getRank() < p1->getRank() )
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
	for ( VARIANT_LIST::EIterator vIter = variants.Begin();
		!vIter.IsDone();  vIter++ )
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
	file.writeIdLong( "PilotCount", pilots.Count() );
	file.writeIdLong( "InventoryCount", inventory.Count() );
	file.writeIdBoolean( "FirstTimeResults", MissionResults::FirstTimeResults);

	// save the campaign info
	missionInfo->save( file );

	int pilotCount = 0;
	// save the pilots
	for ( PILOT_LIST::EIterator pIter = pilots.Begin();
		!pIter.IsDone(); pIter++ )
		{
			(*pIter).save( file, pilotCount++ );
		}

	int mechCount = 0;
	// save the inventory
	for ( MECH_LIST::EIterator mIter = inventory.Begin();
		!mIter.IsDone(); mIter++ )
		{
			(*mIter)->save( file, mechCount++ );
		}

	return 0;
}

void LogisticsData::clearVariants()
{
	for ( VARIANT_LIST::EIterator iter = variants.End(); !iter.IsDone();  )
	{
		if ( !(*iter)->isDesignerMech() )
		{
			VARIANT_LIST::EIterator tmpIter = iter;
			iter --;
			delete *tmpIter;
			variants.Delete( tmpIter );
	
		}
		else
			iter--;
	}
		
}

long	LogisticsData::load( FitIniFile& file )
{
	clearInventory();
	resourcePoints = 0;
	pilots.Clear();
	initPilots();
	clearVariants();

	if ( !missionInfo )
		missionInfo = new LogisticsMissionInfo;

	missionInfo->load( file );

	long result = file.seekBlock( "Version" );
	if (result != NO_ERR)
	{
		PAUSE(("SaveGame has no version number.  Not Loading"));
		return -1;
	}

	long testVersionNum = 0;
	result = file.readIdLong( "VersionNumber", testVersionNum);
	if (result != NO_ERR)
	{
		PAUSE(("SaveGame has no version number.  Not Loading"));
		return -1;
	}

	if (testVersionNum != SaveGameVersionNumber)
	{
		PAUSE(("SaveGame is not Version %d.  It was Version %d which is not valid!",SaveGameVersionNumber,testVersionNum));
		return -1;
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
		result = loadVariant( file );
		if ( result != NO_ERR )
		{
			gosASSERT( 0 );
			return -1;
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
		for ( PILOT_LIST::EIterator pIter = pilots.Begin(); !pIter.IsDone(); pIter++ )
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

	return 0;
}

long LogisticsData::loadVariant( FitIniFile& file )
{
	char tmp[256] = {};

	const std::optional<std::string> chassisKey = objective_text::readPlain( file, "Chassis" );
	if ( !chassisKey )
	{
		MC2_LOG(Core, Warn, "logistics: a saved variant has no Chassis and is rejected");
		return INVALID_FILE_NAME;
	}

	const LogisticsChassis* pChassis  = nullptr;
	// go out and find that chassis
	for ( VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone(); vIter++ )
	{
		if ( mc2::str::iequals( (*vIter)->getFileName(), *chassisKey ) )
		{
			pChassis = (*vIter)->getChassis();
		}
	}

	if ( !pChassis ) // we can always try and make it ourseleves, but it should have been loaded by now
	{
		gosASSERT( 0 );
		return INVALID_FILE_NAME;
	}

	// A variant without a name would take the chassis file name, so it is rejected.
	const std::optional<std::string> variantName = objective_text::readPlain( file, "VariantName" );
	if ( !variantName )
	{
		MC2_LOG(Core, Warn, "logistics: a saved variant of chassis '{}' has no VariantName and is rejected", *chassisKey);
		return INVALID_FILE_NAME;
	}

	// create the variant, add to the list
	LogisticsVariant* pVariant = new LogisticsVariant( pChassis, 0 );	

	variants.Append( pVariant );
	
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

	return 0;
}

long LogisticsData::loadMech( FitIniFile& file, int& count )
{
	const std::string variantKey = objective_text::readPlain( file, "Variant" ).value_or( std::string() );
	for ( VARIANT_LIST::EIterator mIter = variants.Begin(); !mIter.IsDone(); mIter++ )
	{
		if ( mc2::str::iequals( (*mIter)->getName(), variantKey ) )
		{
			
			LogisticsMech* pMech = new LogisticsMech( (*mIter), count );
			// A mech without a Pilot key has no pilot: the key is never read as the variant text.
			const std::optional<std::string> pilotKey = objective_text::readPlain( file, "Pilot" );
			inventory.Append( pMech );

			for ( PILOT_LIST::EIterator pIter = pilots.Begin(); pilotKey && !pIter.IsDone(); pIter++ )
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

			// it could have had no pilot
			return 0;
		}
	}

	return -1; // failed in finding the variant
}



void	LogisticsData::setMissionCompleted( )
{
#ifndef VIEWER
	missionInfo->setMissionComplete();

	rpJustAdded = 0;

	// first set all pilots as not just dead
	for ( PILOT_LIST::EIterator iter = pilots.Begin(); !iter.IsDone();
		iter++ )
		{
			(*iter).setUsed( 0 );
		}

	for ( EList< CObjective*, CObjective* >::EIterator oIter =  Team::home->objectives.Begin();
			!oIter.IsDone(); oIter++ )
	{
		if ( (*oIter)->Status(Team::home->objectives) == OS_SUCCESSFUL )
		{
			addCBills( (*oIter)->ResourcePoints() );
		}
	}

	// need to go find out which pilots died.
	Team* pTeam = Team::home;

	int ForceGroupCount = 1;

	if ( pTeam )
	{
		for ( int i = pTeam->getRosterSize() - 1; i > -1; i-- )
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );

			// LOGISTICS-PILOT-SKIP-UNIT-LOSS-RECON-1 (T4): per-unit carry-over decision.
			// Emits, for every roster unit at mission completion, the exact reason it is
			// re-added or dropped — so we can see which check a vanished TM vehicle fails
			// (on_gui / type_class / move_type AIR). Gate MC2_LOG_LOGISTICS_FLOW +
			// MC2_DIAG_TAGS=LOGISTICS; read via get_diagnostic_events("LOGISTICS"). No behavior change.
			{
				static const bool s_logiFlowTrace = mc2::gate::get<"MC2_LOG_LOGISTICS_FLOW">();
				if ( s_logiFlowTrace && pMover && mc2_diag::tagEnabled("LOGISTICS") )
				{
					int  _lg_otc  = pMover->getObjectType() ? (int)pMover->getObjectType()->getObjectTypeClass() : -1;
					bool _lg_gui  = pMover->isOnGUI();
					int  _lg_mt   = (int)pMover->getMoveType();
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

			//Must check if we ever linked up with the mech!!
			if ( pMover->isOnGUI() &&
				 (pMover->getObjectType()->getObjectTypeClass() == BATTLEMECH_TYPE) && 
				 (pMover->getCommanderId() == Commander::home->getId()) &&
				 (pMover->getMoveType() != MOVETYPE_AIR))
			{
				LogisticsMech* pMech = getMech( pMover->getName(), pMover->getPilot()->getName() );

				DWORD base, highlight1, highlight2;
				((Mech3DAppearance*)pMover->getAppearance())->getPaintScheme( base, highlight1, highlight2 );
				LogisticsPilot* pPilot = getPilot( pMover->getPilot()->getName() );

				// LOGISTICS T4b: re-add resolution outcome for a filter-passing mech.
				// Shows whether the inventory mech (pMech) and the pilot round-trip
				// (pPilot) survive — a pilotless re-add or a getMech miss is how a
				// surviving Mad Dog still vanishes. Gate MC2_LOG_LOGISTICS_FLOW.
				{
					static const bool s_lfResolve = mc2::gate::get<"MC2_LOG_LOGISTICS_FLOW">();
					if ( s_lfResolve && mc2_diag::tagEnabled("LOGISTICS") )
					{
						const char* _r_pn = (pMover->getPilot() && pMover->getPilot()->getName()) ? pMover->getPilot()->getName() : "";
						const char* _r_vn = ((BattleMech*)pMover)->variantName ? ((BattleMech*)pMover)->variantName : "";
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
						LogisticsVariant* pVar = getVariant( ((BattleMech*)pMover)->variantName );
						addMechToInventory( pVar, ForceGroupCount++, pPilot, base, highlight1, highlight2 );
						if ( pPilot )
							pPilot->setUsed( true );

					}
				}
				else // mech was recovered during the mission (or was a deployed mech removed by removeMechsInForceGroup)
				{
					if ( !pMover->isDestroyed() && !pMover->isDisabled() )
					{
						// find the variant with this mech's info
						const char* vname = ((BattleMech*)pMover)->variantName;
						LogisticsVariant* pVariant = getVariant( vname );
						if ( !pVariant )
						{
							// getVariant failed: variantName not in the loaded variants list.
							// This happens for mod-purchased mechs whose variant was added at
							// runtime (not in the campaign's base variant CSV).  Fall back to
							// the pre-removal snapshot captured by removeMechsInForceGroup().
							// The pVariant pointer in the snapshot is still valid -- LogisticsVariant
							// objects are owned by the variants list, not by LogisticsMech.
							for ( const DeployedMechSnapshot& snap : deployedMechSnapshot )
							{
								// Variant names match ignoring ASCII case, like every other variant lookup;
								// BattleMech::variantName is set from LogisticsVariant::getName() at deploy.
								if ( mc2::str::iequals( snap.variantName, mc2::str::from_c( vname ) ) && snap.pVariant )
								{
									pVariant = snap.pVariant;
									break;
								}
							}
							if ( !pVariant )
							{
								// Still NULL: genuine missing variant (corrupt save / unknown mech).
								// Log and skip rather than Assert no-op (Assert is a no-op in RelWithDebInfo).
								mc2::fmt::print_out( "[LOGISTICS] setMissionCompleted: no variant for surviving mech '{}' -- mech lost\n",
								        mc2::fmt::safe(vname) );
								fflush( stdout );
								// LOGISTICS T4c: the actual deletion — variant unresolved, mech dropped.
								if ( mc2::gate::get<"MC2_LOG_LOGISTICS_FLOW">() && mc2_diag::tagEnabled("LOGISTICS") )
								{
									char _ml_vn[80]; const char* _ml_s = vname ? vname : "";
									size_t _ml_k; for(_ml_k=0;_ml_k<79 && _ml_s[_ml_k];++_ml_k) _ml_vn[_ml_k]=(_ml_s[_ml_k]=='\\'||_ml_s[_ml_k]=='"')?'_':_ml_s[_ml_k]; _ml_vn[_ml_k]='\0';
									char _ml_buf[160];
									mc2::fmt::to_buf(_ml_buf, "{{\"site\":\"mech_lost\",\"variant\":\"{}\"}}", mc2::fmt::safe(_ml_vn));
									mc2_diag::writeEvent("LOGISTICS", 1, 0, _ml_buf);
								}
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
//					if ( pMover->isDestroyed() || pMover->isDisabled() )
//						pPilot->setUsed( false );

				}
			}
		}
	}

#endif
}

LogisticsMech*  LogisticsData::getMech( const char* MechName, const char* pilotName )
{
	const std::string wanted = mc2::str::from_c( MechName );
	for( MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( mc2::str::iequals( (*iter)->getName(), wanted ) )
		{
			if ( !pilotName )
			{
				if ( !(*iter)->getPilot() )
					return (*iter);
			}

			else
			{
				if ( (*iter)->getPilot() && mc2::str::iequals( (*iter)->getPilot()->getName(), mc2::str::from_c( pilotName ) ) )
					return (*iter);
			}
		}
	}

	return nullptr;
}

void LogisticsData::removeMechFromInventory( const char* mechName, const char* pilotName )
{
	LogisticsMech* pMech = getMech( mechName, pilotName );

	gosASSERT( pMech );

	if ( pMech )
	{
		inventory.Delete( inventory.Find(pMech) );
		delete pMech;
	}
}

LogisticsPilot*	LogisticsData::getPilot( const char* pilotName )
{
	const std::string wanted = mc2::str::from_c( pilotName );
	// look for available ones first
	for( PILOT_LIST::EIterator iter = pilots.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter).isAvailable() )
		{
			if ( mc2::str::iequals( (*iter).getName(), wanted ) )
			{
				return &(*iter);
			}
		}
	}
	
	for( PILOT_LIST::EIterator iter = pilots.Begin(); !iter.IsDone(); iter++ )
	{
		if ( mc2::str::iequals( (*iter).getName(), wanted ) )
		{
			return &(*iter);
		}
	}

	return nullptr;
}

LogisticsVariant* LogisticsData::getVariant( const char* mechName )
{
	const std::string wanted = mc2::str::from_c( mechName );
	for ( VARIANT_LIST::EIterator iter = variants.Begin(); !iter.IsDone(); iter++ )
	{
		if ( mc2::str::iequals( (*iter)->getName(), wanted ) )
			return (*iter);
	}

	return nullptr;
}
