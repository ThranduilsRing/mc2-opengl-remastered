#include "utils/mc2log.h"
// LogisticsData: updateAvailability, the front-end state dump and the availability append.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "package_chassis.h"
#include "purchase_catalog.h"
#include "unitprofile.h"
#include "equipment_registry.h"
#include <filesystem>

#include <format>
#include <string>

std::string mc2purchase::chassisSource(std::string_view csvPath)
{
	const auto* package = packageChassisByStem(std::filesystem::path(csvPath).stem().string());
	const char* owner = LookupModOwner(std::string(csvPath).c_str());
	return source(package ? package->modId : std::string_view{}, owner ? owner : "");
}

long LogisticsData::updateAvailability()
{



	bNewWeapons = 0;

	// Existing launcher sandbox option: all loaded chassis and components.
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
	for (auto & pilot : pilots)
	{
		if ( pilot.bAvailable )
			oldPilotAvailableCount++;

		pilot.setAvailable( 0 );
	}


	FitIniFile file;
	if ( NO_ERR != file.open( purchaseFileName.c_str() ) )
	{
		const std::string error = std::format( "Couldn't open {}", purchaseFileName );
		PAUSE((const_cast<char*>(error.c_str()) ));
		return NO_PURCHASE_FILE;
	}

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

			if ( component >= 0 && component < 255 )
				available[component] = 1;
		}


	}

	for (auto & component : components)
	{
		if ( available[component.getID()] || bAll )
		{
			if ( !component.isAvailable() )
				bNewWeapons = true;
			component.setAvailable( 1 );
		}
	}

	const char* pFileNames[512];
	long count = 512;
 	missionInfo->getAdditionalPurachaseFiles( pFileNames, count );


	for (auto & variant : variants)
	{
		if ( variant->isAvailable()  && !(variant->getID() >> 16 ))
			oldMechAvailableCount++;
		variant->setAvailable(0);
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
	}

	// Explicit equipment availability also permits variants that use those components.
	{
		size_t enableWireForced = applyEquipmentEnableWireIfEnabled( available, 255 );
		if ( enableWireForced > 0 )
		{
			for (auto & component : components)
			{
				if ( available[component.getID()] && !component.isAvailable() )
				{
					bNewWeapons = true;
					component.setAvailable( 1 );
				}
			}
		}
	}

	for (auto & component : components)
	{
		if ( !available[component.getID()]  )
		{
			component.setAvailable( 0 );
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

		for (auto & variant : variants)
		{
			const std::string& mechName = variant->getFileName();
			char realName[1024];
			_splitpath( mechName.c_str(), nullptr, nullptr, realName, nullptr );
			if ( S_stricmp( realName, chassisFileName ) == 0 )
			{
				componentCount = 255;
				bool bRight = true;
				variant->getComponents( componentCount, componentArray );
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
					variant->setAvailable( true );

					if (  !(variant->getID() >> 16 ) )
						newMechAvailableCount++;
				}
			}
		}
	}

	// Stock additions follow the purchase-file mission; MW5 chassis are always available.
	std::vector<mc2purchase::Unlock> unlocks;
	FitIniFile unlockFile;
	if (unlockFile.open("data/defs/purchase/unlocks.fit") == NO_ERR && unlockFile.seekBlock("Unlocks") == NO_ERR)
		for (int i = 0; ; ++i) {
			char key[32], stem[256];
			mc2::fmt::to_buf(key, "Unit{:d}", i);
			if (unlockFile.readIdString(key, stem, sizeof(stem)) != NO_ERR) break;
			mc2::fmt::to_buf(key, "Mission{:d}", i);
			long mission = 0;
			if (unlockFile.readIdLong(key, mission) == NO_ERR) unlocks.push_back({stem, mission});
		}
	const int purchaseMission = mc2purchase::missionNumber(purchaseFileName);
	for (auto & variant : variants)
	{
		const auto stem = std::filesystem::path(variant->getFileName()).stem().string();
		if ( variant->isAvailable() || !(s_purchaseAll || mc2purchase::supplementalAvailable(stem,
			!mc2purchase::chassisSource(variant->getFileName()).empty(), purchaseMission, unlocks)) )
			continue;
		variant->setAvailable( true );
		if ( !(variant->getID() >> 16) )
			newMechAvailableCount++;
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
		for (auto & pilot : pilots)
		{
			if ( mc2::str::iequals( pilot.getFileName(), pilotKey ) )
			{
				pilot.setAvailable( true );
			}

		}
	}

	for (auto & pilot : pilots)
	{
		
		if ( pilot.bAvailable )
			newPilotAvailableCount++;
	}


	if ( oldPilotAvailableCount != newPilotAvailableCount && newPilotAvailableCount > oldPilotAvailableCount )
		bNewPilots = true;
	else
		bNewPilots = 0;


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
void LogisticsData::dumpFrontEndState( mc2::czstring screenName )
{
	if ( !mc2_diag::tagEnabled( "LOGISTICS" ) )
		return;

	static uint64_t s_dumpSeq = 0;
	++s_dumpSeq;

	// Null inputs are empty; non-null inputs must be NUL-terminated.
	auto esc = []( mc2::czstring s ) -> std::string {
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
		j += ",\"inventoryCount\":" + std::to_string( static_cast<int>(std::ssize(inv)) );
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
		j += ",\"forceGroupCount\":" + std::to_string( static_cast<int>(std::ssize(fg)) );
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
		j += ",\"pilotCount\":" + std::to_string( static_cast<int>(pcount) );
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
		j += ",\"missionListCount\":" + std::to_string( static_cast<int>(mc) );
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

void LogisticsData::appendAvailability(mc2::czstring pFileName, bool* availableArray )
{
	auto* observation = objectiveObservation();
	auto accepted = [&](mc2::czstring stage, mc2::czstring result, std::string_view payload) {
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
		if ( false )
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

				// Unknown / out-of-range ids (mod or stale purchase data) are skipped, not fatal.
				LogisticsComponent* pComp = (component >= 0 && component < 255) ? getComponent( component ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr) : nullptr;
				if ( !pComp )
				{
					MC2_LOG(Ui, Warn, "[LOGISTICS] appendAvailability: unknown component id {:d} in '{}' -- skipped", mc2::fmt::as_i32(component), mc2::fmt::safe(pFileName ? pFileName : "(null)"));
					continue;
				}
				availableArray[component] = true;

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
		for (auto & pilotElem : pilots)
		{
			if ( mc2::str::iequals( pilotElem.getFileName(), pilotKey ) )
			{
				pilotElem.setAvailable( true );
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

		for (auto & variant : variants)
		{
			const std::string& mechName = variant->getFileName();
			char realName[255];
			_splitpath( mechName.c_str(), nullptr, nullptr, realName, nullptr );
			if ( S_stricmp( realName, chassisFileName ) == 0 )
			{
				long componentCount = 255;
				long componentArray[256];
				bool bRight = true;
				variant->getComponents( componentCount, componentArray );
				for ( int i = 0; i < componentCount; i++ )
				{
					LogisticsComponent* pComp = getComponent(componentArray[i]).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
					if ( !pComp || !pComp->isAvailable() ) // unavailable componets
					{
						bRight= false;
						break;
					}
				}

				if ( bRight )
				{
					variant->setAvailable( true );
					newAvailableCount++;
					if (observation) { accepted("mech", "available", objectiveObservationQuote(mechName)); }
				}

				break;
			}
		}
	}
}
