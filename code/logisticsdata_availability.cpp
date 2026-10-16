// LogisticsData: updateAvailability, the front-end state dump and the availability append.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"

#include <format>
#include <string>


long LogisticsData::updateAvailability()
{
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

	std::string purchaseFileName = mc2::str::ascii_lower( missionInfo->getCurrentPurchaseFile() );

	if ( purchaseFileName.empty() )
	{
		purchaseFileName = std::string( missionPath ) + "purchase.fit";
	}

	int oldMechAvailableCount= 0;
	int newMechAvailableCount = 0;
	int oldPilotAvailableCount = 0;
	int newPilotAvailableCount= 0;
	for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
	{
		if ( (*pIter).bAvailable )
			oldPilotAvailableCount++;

		(*pIter).setAvailable( 0 );
	}

	LOG_LOGISTICS_UA("updateAvailability: purchaseFile='%s'", purchaseFileName.c_str());

	FitIniFile file;
	if ( NO_ERR != file.open( purchaseFileName.c_str() ) )
	{
		LOG_LOGISTICS_UA("updateAvailability: purchase file OPEN FAILED");
		const std::string error = std::format( "Couldn't open {}", purchaseFileName );
		PAUSE(((char*)error.c_str() ));
		return NO_PURCHASE_FILE;
	}
	LOG_LOGISTICS_UA("updateAvailability: purchase file open OK");

	// Master-list additions can arrive after component initialization.
	appendEquipmentAddFillComponents();

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

	for ( auto cIter = components.begin(); cIter != components.end(); ++cIter )
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


	for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
	{
		if ( (*vIter)->isAvailable()  && !((*vIter)->getID() >> 16 ))
			oldMechAvailableCount++;
		(*vIter)->setAvailable(0);
	}

	for (int i = 0; i < count; i++ )
	{
		appendAvailability( pFileNames[i], available );
	}

	// Purchase-all includes each campaign mission's pilots and chassis, so the
	// roster remains bounded by campaign content.
	if ( s_purchaseAll )
	{
		const char* allPF[1024];
		long pfCount = 1024;
		missionInfo->getAllPurchaseFiles( allPF, pfCount );
		for ( long i = 0; i < pfCount && i < 1024; i++ )
			appendAvailability( allPF[i], available );
		LOG_LOGISTICS_UA( "updateAvailability: MC2_PURCHASE_ALL campaign-wide (%ld purchase files)", pfCount );
	}

	// Explicit equipment availability also permits variants that use those components.
	{
		size_t enableWireForced = applyEquipmentEnableWireIfEnabled( available, 255 );
		if ( enableWireForced > 0 )
		{
			LOG_LOGISTICS_UA( "updateAvailability: enable-wire forced %zu component(s)", enableWireForced );
			for ( auto cIter = components.begin(); cIter != components.end(); ++cIter )
			{
				if ( available[(*cIter).getID()] && !(*cIter).isAvailable() )
				{
					bNewWeapons = true;
					(*cIter).setAvailable( 1 );
				}
			}
		}
	}

	for ( auto cIter = components.begin(); cIter != components.end(); ++cIter )
	{
		if ( !available[(*cIter).getID()]  )
		{
			(*cIter).setAvailable( 0 );
		}
	}



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

		for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
		{
			const std::string& mechName = (*vIter)->getFileName();
			char realName[1024];
			_splitpath( mechName.c_str(), nullptr, nullptr, realName, nullptr );
			if ( S_stricmp( realName, chassisFileName ) == 0 )
			{
				componentCount = 255;
				bool bRight = true;
				(*vIter)->getComponents( componentCount, componentArray );
				for ( int i = 0; i < componentCount; i++ )
				{
					if ( !available[componentArray[i]] ) // unavailable componets
					{
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

	char pilotName[255];
	file.seekBlock( "Pilots" );
	for (int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Pilot{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, pilotName, 254 ) )
			break;

		// The purchase file names a pilot in its own case (PMWChopper) and the pilot FIT is lower case (pmwchopper): the match ignores ASCII case.
		const std::string pilotKey = mc2::str::from_c( pilotName );
		for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
		{
			if ( mc2::str::iequals( (*pIter).getFileName(), pilotKey ) )
			{
				(*pIter).setAvailable( true );
			}

		}
	}

	for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
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

	if (auto* observation = objectiveObservation()) { observation->Finish(); }
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
	j += ",\"campaign\":\"" + esc( getCampaignName().c_str() ) + "\"";

	// Mission name fields -- guard on a valid current mission (campaign-select
	// screen may have none yet).
	const std::string& curMission = getCurrentMission();
	if ( !curMission.empty() ) {
		j += ",\"mission\":\"" + esc( curMission.c_str() ) + "\"";
		j += ",\"missionFriendly\":\"" + esc( getCurrentMissionFriendlyName().c_str() ) + "\"";
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
			j += "\"" + esc( pv[i] ? pv[i]->getName().c_str() : "" ) + "\"";
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
		const MechList inv = getInventory();
		j += ",\"inventoryCount\":" + std::to_string( (int)std::ssize(inv) );
		j += ",\"inventory\":[";
		int i = 0;
		for ( auto it = inv.begin(); it != inv.end() && i < 64; ++it, ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( ( *it )->getName().c_str() ) + "\"";
		}
		j += "]";
	}

	// Force group (the deployed loadout / launch roster).
	{
		const MechList fg = getForceGroup();
		j += ",\"forceGroupCount\":" + std::to_string( (int)std::ssize(fg) );
		j += ",\"forceGroup\":[";
		int i = 0;
		for ( auto it = fg.begin(); it != fg.end() && i < 24; ++it, ++i ) {
			if ( i ) j += ",";
			j += "\"" + esc( ( *it )->getName().c_str() ) + "\"";
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
			j += "{\"name\":\"" + esc( pp[i] ? pp[i]->getName().c_str() : "" )
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
	auto* observation = objectiveObservation();
	auto accepted = [&](const char* stage, const char* result, const std::string& payload) {
		if (!observation) { return; }
		for (const auto& technology : observation->technologies) {
			if (technology.path == pFileName) {
				observation->SinkResult("technology", stage, result, payload, technology.dispatch);
			}
		}
	};
	FitIniFile file;
	if ( NO_ERR != file.open( pFileName ) )
	{
		if (observation) { accepted("purchase_file", "rejected", objectiveObservationQuote(pFileName)); }
		if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() )
		{
			mc2::fmt::print_out("[LOGISTICS] appendAvailability: open FAILED '{}' -- purchase file skipped\n", mc2::fmt::safe(pFileName ? pFileName : "(null)"));
			fflush( stdout );
		}
		return;
	}
	if (observation) { accepted("purchase_file", "opened", objectiveObservationQuote(pFileName)); }
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
				if (observation) { accepted("component", "available", std::to_string(component)); }
			}
		}
	}

	char pilotName[255];
	char tmp[256];
	file.seekBlock( "Pilots" );
	for ( int i = 0; i < 255; i++ )
	{
		mc2::fmt::to_buf(tmp, "Pilot{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.readIdString( tmp, pilotName, 254 ) )
			break;

		const std::string pilotKey = mc2::str::from_c( pilotName );
		for ( auto pIter = pilots.begin(); pIter != pilots.end(); ++pIter )
		{
			if ( mc2::str::iequals( (*pIter).getFileName(), pilotKey ) )
			{
				(*pIter).setAvailable( true );
				bNewPilots = true;
				if (observation) { accepted("pilot", "available", objectiveObservationQuote(pilotKey)); }
				
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

		for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
		{
			const std::string& mechName = (*vIter)->getFileName();
			char realName[255];
			_splitpath( mechName.c_str(), nullptr, nullptr, realName, nullptr );
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
						bRight= false;
						break;
					}
				}

				if ( bRight )
				{
					(*vIter)->setAvailable( true );
					newAvailableCount++;
					if (observation) { accepted("mech", "available", objectiveObservationQuote(mechName)); }
				}

				break;
			}
		}
	}
}
