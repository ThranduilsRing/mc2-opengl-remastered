// LogisticsData: updateAvailability, the front-end state dump and the availability append.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include "mc2fmt.h"


long LogisticsData::updateAvailability()
{
	// ---- MC2_LOG_LOGISTICS diagnostics ----
	static FILE* s_logFileUA = nullptr;
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() && !s_logFileUA ) {
		s_logFileUA = fopen("logistics_debug.log", "a");
	}
#define LOG_LOGISTICS_UA(fmt, ...) do { \
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() ) { \
		printf("[LOGISTICS] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
		if (s_logFileUA) { fprintf(s_logFileUA, "[LOGISTICS] " fmt "\n", ##__VA_ARGS__); fflush(s_logFileUA); } \
	} } while(0)

	bNewWeapons = 0;

	// MC2_PURCHASE_ALL (default OFF; launcher Engine Option): unlock every
	// COMPONENT/weapon so modded gear (e.g. the Magic clan double heat sinks)
	// is buyable. It only forces AllComponents below — it does NOT blanket-mark
	// mechs/pilots, so the mission's own [Mechs]/[Pilots] lists still bound the
	// roster (no random modded chassis like the imported Marauder leaking in;
	// stock campaign mechs all become buildable since every component is now
	// available).
	static const bool s_purchaseAll = mc2::gate::get<"MC2_PURCHASE_ALL">();

	EString purchaseFileName = missionInfo->getCurrentPurchaseFile();
	purchaseFileName.MakeLower();

	if ( purchaseFileName.Length() < 1 )
	{
		purchaseFileName = missionPath;
		purchaseFileName += "purchase.fit";
	}

	int oldMechAvailableCount= 0;
	int newMechAvailableCount = 0;
	int oldPilotAvailableCount = 0;
	int newPilotAvailableCount= 0;
	for ( PILOT_LIST::EIterator pIter = pilots.Begin(); !pIter.IsDone(); pIter++ )
	{
		if ( (*pIter).bAvailable )
			oldPilotAvailableCount++;

		(*pIter).setAvailable( 0 );
	}

	LOG_LOGISTICS_UA("updateAvailability: purchaseFile='%s'", (const char*)purchaseFileName);

	// make sure its around and you can open it
	FitIniFile file;
	if ( NO_ERR != file.open( (char*)(const char*)purchaseFileName ) )
	{
		LOG_LOGISTICS_UA("updateAvailability: purchase file OPEN FAILED");
		EString error;
		error.Format( "Couldn't open %s", (char*)(const char*)purchaseFileName );
		PAUSE(((char*)(const char*)error ));
		return NO_PURCHASE_FILE;
	}
	LOG_LOGISTICS_UA("updateAvailability: purchase file open OK");

	// EQUIP-ADD-FILL-1 (S2): the ADD pass runs at the loadMasterList seam, which
	// can land after the one-shot initComponents(); re-run the idempotent append
	// here so filled rows exist in `components` before the availability loops.
	appendEquipmentAddFillComponents();

	// read in available components
	bool available[255];
	memset( available, 0, sizeof( bool ) * 255 );

	long result = file.seekBlock( "Components" );
	if ( result != NO_ERR )
	{
		Assert( 0, 0, "No components in the purchase file" );
	}

	char tmp[32];
	long component;

	bool bAll = 0;
	file.readIdBoolean( "AllComponents", bAll );
	if ( s_purchaseAll )
		bAll = true;   // MC2_PURCHASE_ALL: unlock all components/weapons (mechs stay file-bounded)
	for ( int i = 0; i < 255; i++ )
	{
		if ( bAll )
			available[i] = 1;
		else
		{
			mc2::fmt::to_buf(tmp, "Component{:d}", mc2::fmt::as_i32(i));
			if ( NO_ERR != file.readIdLong( tmp, component ) )
				break;

			available[component] = 1;
		}


	}

	// go through comonent list, and set 'em
	for ( COMPONENT_LIST::EIterator cIter = components.Begin(); !cIter.IsDone(); cIter++ )
	{
		if ( available[(*cIter).getID()] || bAll )
		{
			if ( !(*cIter).isAvailable() )
				bNewWeapons = true;
			(*cIter).setAvailable( 1 );
		}
	}

	const char* pFileNames[512];
	long count = 512;
 	missionInfo->getAdditionalPurachaseFiles( pFileNames, count );


	// reset all variants to unavailable
	for ( VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone(); vIter++ )
	{
		if ( (*vIter)->isAvailable()  && !((*vIter)->getID() >> 16 ))
			oldMechAvailableCount++;
		(*vIter)->setAvailable(0);
	}

	for (int i = 0; i < count; i++ )
	{
		appendAvailability( pFileNames[i], available );
	}

	// MC2_PURCHASE_ALL: campaign-level roster. Fold in the [Mechs]/[Pilots] of
	// EVERY mission's purchase file across all stages, not just this mission's,
	// so the buyable roster is the union the campaign ever offers. Components are
	// already all available (bAll above), so every listed chassis is buildable.
	if ( s_purchaseAll )
	{
		const char* allPF[1024];
		long pfCount = 1024;
		missionInfo->getAllPurchaseFiles( allPF, pfCount );
		for ( long i = 0; i < pfCount && i < 1024; i++ )
			appendAvailability( allPF[i], available );
		LOG_LOGISTICS_UA( "updateAvailability: MC2_PURCHASE_ALL campaign-wide (%ld purchase files)", pfCount );
	}

	// EQUIP-ENABLE-WIRE-1 (equipment arc, ADD-ladder S1): first consumer of the
	// E2 equipment registry `enabled` flag. ENABLE-ONLY union on top of the
	// purchase.fit verdicts: a populated masterList row whose def sidecar declares
	// `[Equipment] b Enabled=TRUE` is forced into available[] here, BEFORE the
	// disable sweep and the [Mechs] variant loop below (so its components and any
	// variants using them resolve normally). Gate MC2_UNIT_PROFILE_DATA; gate-OFF
	// or zero mods -> returns 0 and this block is byte-identical no-op.
	{
		size_t enableWireForced = applyEquipmentEnableWireIfEnabled( available, 255 );
		if ( enableWireForced > 0 )
		{
			LOG_LOGISTICS_UA( "updateAvailability: enable-wire forced %zu component(s)", enableWireForced );
			for ( COMPONENT_LIST::EIterator cIter = components.Begin(); !cIter.IsDone(); cIter++ )
			{
				if ( available[(*cIter).getID()] && !(*cIter).isAvailable() )
				{
					bNewWeapons = true;
					(*cIter).setAvailable( 1 );
				}
			}
		}
	}

	// go through comonent list, and set 'em
	for (COMPONENT_LIST::EIterator cIter = components.Begin(); !cIter.IsDone(); cIter++ )
	{
		if ( !available[(*cIter).getID()]  )
		{
			(*cIter).setAvailable( 0 );
		}
	}



	// go through each variant, and see if it's available
	char chassisFileName[255];
	long componentArray[255];
	long componentCount;

	file.seekBlock( "Mechs" );
	for (int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Mech{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, chassisFileName, 254 ) )
			break;
		LOG_LOGISTICS_UA("updateAvailability: [Mechs] Mech%d='%s'", i, chassisFileName);

		// go through each variant, if it has the same chassis, check and see if all of its components are valid
		for (VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone(); vIter++ )
		{
			EString mechName = (*vIter)->getFileName();
			char realName[1024];
			_splitpath( mechName, nullptr, nullptr, realName, nullptr );
			if ( S_stricmp( realName, chassisFileName ) == 0 )
			{
				componentCount = 255;
				bool bRight = true;
				(*vIter)->getComponents( componentCount, componentArray );
				for ( int i = 0; i < componentCount; i++ )
				{
					if ( !available[componentArray[i]] ) // unavailable componets
					{
						//char errorStr[256];
						//sprintf( errorStr, "mech %s discarded because it contains a %ld", 
						//	chassisFileName, componentArray[i] );
						//PAUSE(( errorStr ));
						bRight= false;
						break;
					}
				}

				if ( bRight )
				{
					(*vIter)->setAvailable( true );

					if (  !((*vIter)->getID() >> 16 ) )
						newMechAvailableCount++;
				}
			}
		}
	}

	if ( newMechAvailableCount != oldMechAvailableCount )
		bNewMechs = true;
	else
		bNewMechs = false;

	// add new pilots
	char pilotName[255];
	file.seekBlock( "Pilots" );
	for (int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Pilot{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, pilotName, 254 ) )
			break;

		for ( PILOT_LIST::EIterator pIter = pilots.Begin(); !pIter.IsDone(); pIter++ )
		{
			if ( (*pIter).getFileName().Compare( pilotName, 0 ) == 0 )
			{
				(*pIter).setAvailable( true );
			}

		}
	}

	for (PILOT_LIST::EIterator pIter = pilots.Begin(); !pIter.IsDone(); pIter++ )
	{
		
		if ( (*pIter).bAvailable )
			newPilotAvailableCount++;
	}


	if ( oldPilotAvailableCount != newPilotAvailableCount && newPilotAvailableCount > oldPilotAvailableCount )
		bNewPilots = true;
	else
		bNewPilots = 0;

	LOG_LOGISTICS_UA("updateAvailability: newMechAvailableCount=%d oldMechAvailableCount=%d",
		newMechAvailableCount, oldMechAvailableCount);
#undef LOG_LOGISTICS_UA

	return 0;

}

//*****************************************************************************
// Front-end screen state capture (MCP-readable LOGISTICS diagnostic event).
// One hook in MissionBegin::update keyed on the current screen drives this for
// every front-end screen: campaign select, briefing, mech bay, purchase,
// loadout (mech lab), and the pilot/launch screen. Self-gates on the LOGISTICS
// diag tag so it costs nothing unless explicitly enabled (MC2_DIAG_TAGS).
//*****************************************************************************
void LogisticsData::dumpFrontEndState( const char* screenName )
{
	if ( !mc2_diag::tagEnabled( "LOGISTICS" ) )
		return;

	static uint64_t s_dumpSeq = 0;
	++s_dumpSeq;

	auto esc = []( const char* s ) -> std::string {
		std::string o;
		if ( !s ) return o;
		for ( const char* p = s; *p; ++p ) {
			char c = *p;
			if ( c == '"' || c == '\\' ) { o.push_back( '\\' ); o.push_back( c ); }
			else if ( c == '\n' || c == '\r' || c == '\t' ) o.push_back( ' ' );
			else o.push_back( c );
		}
		return o;
	};

	std::string j = "{";
	j += "\"screen\":\"" + esc( screenName ) + "\"";
	j += ",\"campaign\":\"" + esc( (const char*)getCampaignName() ) + "\"";

	// Mission name fields -- guard on a valid current mission (campaign-select
	// screen may have none yet).
	const EString& curMission = getCurrentMission();
	if ( curMission.Length() > 0 ) {
		j += ",\"mission\":\"" + esc( (const char*)curMission ) + "\"";
		j += ",\"missionFriendly\":\"" + esc( getCurrentMissionFriendlyName() ) + "\"";
		j += ",\"dropWeight\":" + std::to_string( getCurrentDropWeight() );
		j += ",\"maxDropWeight\":" + std::to_string( getMaxDropWeight() );
	}

	j += ",\"cbills\":" + std::to_string( getCBills() );
	j += ",\"resourcePoints\":" + std::to_string( getResourcePoints() );

	// Purchasable mechs -- the bay/purchase population. Empty here = the POAR bug.
	{
		LogisticsVariant* pv[512];
		int pc = 512;
		getPurchasableMechs( pv, pc );
		j += ",\"purchasableCount\":" + std::to_string( pc );
		j += ",\"purchasable\":[";
		int lim = pc < 64 ? pc : 64;
		for ( int i = 0; i < lim; ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( pv[i] ? (const char*)pv[i]->getName() : "" ) + "\"";
		}
		j += "]";
	}

	// Vehicles available to the campaign (POAR early missions sell these).
	{
		const LogisticsVehicle* vv[256];
		int vc = 256;
		getVehicles( vv, vc );
		j += ",\"vehicleCount\":" + std::to_string( vc );
	}

	// Inventory (owned mechs).
	{
		EList<LogisticsMech*, LogisticsMech*> inv;
		getInventory( inv );
		j += ",\"inventoryCount\":" + std::to_string( (int)inv.Count() );
		j += ",\"inventory\":[";
		int i = 0;
		for ( EList<LogisticsMech*, LogisticsMech*>::EIterator it = inv.Begin();
		      !it.IsDone() && i < 64; it++, ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( (const char*)( *it )->getName() ) + "\"";
		}
		j += "]";
	}

	// Force group (the deployed loadout / launch roster).
	{
		EList<LogisticsMech*, LogisticsMech*> fg;
		getForceGroup( fg );
		j += ",\"forceGroupCount\":" + std::to_string( (int)fg.Count() );
		j += ",\"forceGroup\":[";
		int i = 0;
		for ( EList<LogisticsMech*, LogisticsMech*>::EIterator it = fg.Begin();
		      !it.IsDone() && i < 24; it++, ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( (const char*)( *it )->getName() ) + "\"";
		}
		j += "]";
	}

	// Pilots (name + availability).
	{
		LogisticsPilot* pp[256];
		long pcount = 256;
		getPilots( pp, pcount );
		int avail = 0;
		for ( long i = 0; i < pcount; ++i )
			if ( pp[i] && pp[i]->isAvailable() ) ++avail;
		j += ",\"pilotCount\":" + std::to_string( (int)pcount );
		j += ",\"pilotsAvailable\":" + std::to_string( avail );
		j += ",\"pilots\":[";
		long lim = pcount < 48 ? pcount : 48;
		for ( long i = 0; i < lim; ++i ) {
			if ( i ) j += ",";
			j += "{\"name\":\"" + esc( pp[i] ? (const char*)pp[i]->getName() : "" )
			   + "\",\"available\":" + ( ( pp[i] && pp[i]->isAvailable() ) ? "true" : "false" ) + "}";
		}
		j += "]";
	}

	// Available missions (campaign-select screen).
	{
		const char* mn[256];
		long mc = 256;
		getCurrentMissions( mn, mc );
		j += ",\"missionListCount\":" + std::to_string( (int)mc );
		j += ",\"missionList\":[";
		long lim = mc < 48 ? mc : 48;
		for ( long i = 0; i < lim; ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( mn[i] ) + "\"";
		}
		j += "]";
	}

	j += "}";
	mc2_diag::writeEvent( "LOGISTICS", 1, s_dumpSeq, j.c_str() );
}

void LogisticsData::appendAvailability(const char* pFileName, bool* availableArray )
{
	FitIniFile file;
	if ( NO_ERR != file.open( pFileName ) )
	{
		// ISSUE-8: this silent return is how a purchase-file miss (e.g. the
		// pre-fb4d2e52 backslash-vs-elfHash FST lookup failure) ate pilots /
		// weapons unlocks with zero evidence. Env-gated trace so future
		// purchase-file misses are visible in the field.
		if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() )
		{
			mc2::fmt::print_out("[LOGISTICS] appendAvailability: open FAILED '{}' -- purchase file skipped\n", mc2::fmt::safe(pFileName ? pFileName : "(null)"));
			fflush( stdout );
		}
		return;
	}
	long result = file.seekBlock( "Components" );
	if ( result == NO_ERR )
	{

		char tmp[32];
		long component;

		bool bAll = 0;
		file.readIdBoolean( "AllComponents", bAll );
		for ( int i = 0; i < 255; i++ )
		{
			{
				mc2::fmt::to_buf(tmp, "Component{:d}", mc2::fmt::as_i32(i));
				if ( NO_ERR != file.readIdLong( tmp, component ) )
					break;

				availableArray[component] = true;

				LogisticsComponent* pComp = getComponent( component );
				if ( !pComp->isAvailable() )
					bNewWeapons = true;
				pComp->setAvailable( true );
			}
		}
	}

	// add new pilots
	char pilotName[255];
	char tmp[256];
	file.seekBlock( "Pilots" );
	for ( int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Pilot{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, pilotName, 254 ) )
			break;

		for ( PILOT_LIST::EIterator pIter = pilots.Begin(); !pIter.IsDone(); pIter++ )
		{
			if ( (*pIter).getFileName().Compare( pilotName, 0 ) == 0 )
			{
				(*pIter).setAvailable( true );
				bNewPilots = true;
				
			}

		}
	}

	file.seekBlock( "Mechs" );
	int newAvailableCount = 0;
	char chassisFileName[256];
	for (int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Mech{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, chassisFileName, 255 ) )
			break;

		// go through each variant, if it has the same chassis, check and see if all of its components are valid
		for ( VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone(); vIter++ )
		{
			EString mechName = (*vIter)->getFileName();
			char realName[255];
			_splitpath( mechName, nullptr, nullptr, realName, nullptr );
			if ( S_stricmp( realName, chassisFileName ) == 0 )
			{
				long componentCount = 255;
				long componentArray[256];
				bool bRight = true;
				(*vIter)->getComponents( componentCount, componentArray );
				for ( int i = 0; i < componentCount; i++ )
				{
					LogisticsComponent* pComp = getComponent(componentArray[i]);
					if ( !pComp->isAvailable() ) // unavailable componets
					{
						//char errorStr[256];
						//sprintf( errorStr, "mech %s discarded because it contains a %ld", 
						//	chassisFileName, componentArray[i] );
						//PAUSE(( errorStr ));
						bRight= false;
						break;
					}
				}

				if ( bRight )
				{
					(*vIter)->setAvailable( true );
					newAvailableCount++;
				}

				break;
			}
		}
	}
}
