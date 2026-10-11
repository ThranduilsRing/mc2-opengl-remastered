// LogisticsData: the front-end data load (components, pilots, variants, vehicles), the smart-load gate and the mission availability queries.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include "mc2fmt.h"


// SMART-LOAD-1: defer mech-bay data (components/pilots/variants) until first mech-bay accessor.
// SMART-LOAD-DEFAULT-ON-1: default flipped ON (was default-OFF). Kill-switch preserved --
// MC2_SMART_LOAD=0 (or any leading-zero/false value) restores legacy eager LogisticsData::init().
// Only affects the interactive splash->logistics->mech-bay flow (Logistics::initializeLogData(),
// code/logistics.cpp:248); smoke/tier1's justStartMission fast-path (mechcmd2.cpp:1813) calls
// Mission::init() directly and never touches LogisticsData, so this flip is a no-op for smoke
// mission_ready_ms -- validated via soak on the real splash/logistics path instead.
static const bool s_smartLoad      = mc2::gate::get<"MC2_SMART_LOAD">();   // default ON
static const bool s_smartLoadTrace = mc2::gate::get<"MC2_SMART_LOAD_TRACE">();

#define SMART_LOAD_TRACE_LOG(fmt, ...) do { \
    if (s_smartLoadTrace) { printf("[SMART_LOAD] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } \
} while(0)

//*************************************************************************************************
void LogisticsData::init()
{
	if ( s_smartLoad ) {
		// gate ON: full init on demand -- idempotent via phase flags
		initPhaseA();
		ensureMechBayDataLoaded("init");
		return;
	}

	if ( components.Count() ) // already been initialized
	{
		return;
	}

#define LOG_INIT_TIME(label) do { \
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() ) { \
		ULONGLONG _t = GetTickCount64(); \
		printf("[LOGISTICS_INIT] " label " t=%llums\n", (unsigned long long)_t); fflush(stdout); \
	} } while(0)

	LOG_INIT_TIME("begin");
	initComponents();
	LOG_INIT_TIME("after_initComponents");
	initPilots();
	LOG_INIT_TIME("after_initPilots");
	initVariants();
	LOG_INIT_TIME("after_initVariants");

	missionInfo = new LogisticsMissionInfo;

	FitIniFile file;
	if ( NO_ERR != file.open( "data" PATH_SEPARATOR "campaign" PATH_SEPARATOR "campaign.fit" ) )
	{
		Assert( 0, 0, "coudln't find the campaign file\n" );
	}
	missionInfo->init( file );

	// temporary, just so we can test
	int count = 32;
	const char* missionNames[32];
	missionInfo->getAvailableMissions( missionNames, count );

	setCurrentMission( missionNames[0] );
	LOG_INIT_TIME("after_setCurrentMission_updateAvailability");

#undef LOG_INIT_TIME
}

//*************************************************************************************************
// SMART-LOAD-1: Phase A only -- loads campaign tree (missionInfo), skips mech-bay data.
// Gate OFF falls through to full init().
void LogisticsData::initFast()
{
	if ( !s_smartLoad ) { init(); return; }
	SMART_LOAD_TRACE_LOG("initFast: Phase A only");
	initPhaseA();
}

void LogisticsData::initPhaseA()
{
	if ( m_phaseAComplete ) return;
	SMART_LOAD_TRACE_LOG("Phase A begin");

	missionInfo = new LogisticsMissionInfo;
	FitIniFile file;
	if ( NO_ERR != file.open( "data" PATH_SEPARATOR "campaign" PATH_SEPARATOR "campaign.fit" ) )
		Assert( 0, 0, "coudln't find the campaign file\n" );
	missionInfo->init( file );

	int count = 32;
	const char* missionNames[32];
	missionInfo->getAvailableMissions( missionNames, count );
	// setCurrentMission skips updateAvailability while !m_phaseBComplete
	setCurrentMission( missionNames[0] );

	m_phaseAComplete = true;
	SMART_LOAD_TRACE_LOG("Phase A done");
}

// Phase B: loads components/pilots/variants and runs the first availability pass.
// No-op if gate OFF or already done. Only marks success at the end (retry-safe).
void LogisticsData::ensureMechBayDataLoaded( const char* trigger )
{
	if ( !s_smartLoad || m_phaseBComplete ) return;
	initPhaseA(); // must be done before Phase B
	SMART_LOAD_TRACE_LOG("Phase B begin trigger=%s", trigger ? trigger : "?");

	initComponents();
	initPilots();
	initVariants();

	// First availability pass now that variants/components are populated
	updateAvailability();
	resourcePoints = missionInfo->getCurrentRP();
	removeDeadWeight();

	m_phaseBComplete = true;
	SMART_LOAD_TRACE_LOG("Phase B done");
}

//*************************************************************************************************
void LogisticsData::initComponents()
{
	char componentPath[256];
	strcpy( componentPath, objectPath );
	strcat( componentPath, "compbas.csv" );

	
	File componentFile;
#ifdef _DEBUG
	int result = 
#endif
		componentFile.open( componentPath );
	gosASSERT( result == NO_ERR );

	BYTE* data = new BYTE[componentFile.getLength()];

	componentFile.read( data, componentFile.getLength() );


	File dataFile;
	dataFile.open( (char*)data, componentFile.getLength() );

	componentFile.close();
	
	BYTE line[1024];
	char* pLine = (char*)line;

	// skip the first line
	dataFile.readLine(line, 1024);

	int		Ammo[512];
	memset( Ammo, 0, sizeof ( int ) * 512 );


	LogisticsComponent tmpComp;
	int counter = 0;
	while(true)
	{
		int result = dataFile.readLine( line, 1023 );

		if ( result < 2 || result == 0xBADF0008 || result > 1024  )
			break;

		components.Append( tmpComp );

		LogisticsComponent& tmp = components.GetTail();

		if ( -1 == tmp.init( pLine ) ) // failure
		{
			Ammo[counter] = (long)tmp.getRecycleTime();
			components.DeleteTail();
		}

		counter++;
	}
	
	// fix up ammo
	for ( COMPONENT_LIST::EIterator iter = components.Begin(); !iter.IsDone(); iter++ )
	{
		if ((*iter).getAmmo() )
		{
			(*iter).setAmmo( Ammo[(*iter).getAmmo()] );
		}
	}

	delete [] data;
	data = nullptr;

	// EQUIP-ADD-FILL-1 (S2): second consumer of the ADD pass -- the filled rows
	// are NOT in compbas.csv, so append them here too (gate OFF -> empty list).
	appendEquipmentAddFillComponents();
}

//*************************************************************************************************
// EQUIP-ADD-FILL-1 (S2): append a LogisticsComponent for every row the gated
// ADD pass materialized in masterList, parsed from the SAME synthesized compbas
// line (so both consumers see identical stats), with the mechlab display name
// taken from the def's DisplayName (a new id has no string-table entry).
// IDEMPOTENT (skips ordinals already present) and re-called from
// updateAvailability, because the ADD pass runs at the loadMasterList seam which
// can land AFTER the one-shot initComponents() in the splash->logistics flow.
// Gate OFF or pass not run -> equipmentAddFillRows() empty -> exact no-op.
void LogisticsData::appendEquipmentAddFillComponents()
{
	const std::vector<EquipmentAddFillRow>& rows = equipmentAddFillRows();
	if ( rows.empty() )
		return;

	for ( size_t i = 0; i < rows.size(); ++i )
	{
		const EquipmentAddFillRow& r = rows[i];

		bool present = false;
		for ( COMPONENT_LIST::EIterator iter = components.Begin(); !iter.IsDone(); iter++ )
		{
			if ( (*iter).getID() == r.ordinal ) { present = true; break; }
		}
		if ( present )
			continue;

		char lineBuf[1024];
		strncpy( lineBuf, r.csvLine.c_str(), 1023 );
		lineBuf[1023] = 0;

		LogisticsComponent blank;
		components.Append( blank );
		LogisticsComponent& tmp = components.GetTail();
		if ( -1 == tmp.init( lineBuf ) )
		{
			// icon column is validated by the ADD pass, so this is unexpected --
			// surface it rather than half-materializing the item.
			printf( "[EQUIP-ADD] logistics consumer REJECTED synthesized row (id=%s ordinal=%ld) -- item absent from mechlab\n",
					r.qualifiedId.c_str(), r.ordinal );
			components.DeleteTail();
			continue;
		}
		tmp.setDisplayName( r.displayName.c_str() );

		// Mirror of the initComponents() ammo fixup: the CSV Ammo column holds
		// the ammo row's ordinal; logistics wants ammo-per-ton from that row.
		if ( tmp.getAmmo() && MasterComponent::masterList &&
			 r.ammoMasterId > 0 && r.ammoMasterId < MasterComponent::numComponents )
		{
			tmp.setAmmo( MasterComponent::masterList[r.ammoMasterId].getAmmoPerTon() );
		}

		mc2::fmt::print_out("[EQUIP-ADD] mechlab row appended: id={} ordinal={:d} name=\"{}\"\n", r.qualifiedId.c_str(), mc2::fmt::as_i32(r.ordinal), r.displayName.c_str());
	}
}

//*************************************************************************************************
void LogisticsData::initPilots()
{

	pilots.Clear();

	char pilotPath[256];
	strcpy( pilotPath, objectPath );
	strcat( pilotPath, "pilots.csv" );

	
	File pilotFile;
	pilotFile.open( pilotPath );

	BYTE pilotFileName[256];

	int id = 1;

	while( true )
	{
		int bytesRead = pilotFile.readLine( pilotFileName, 256 );
		
		if ( bytesRead < 2 )
			break;

		LogisticsPilot tmpPilot;
		pilots.Append( tmpPilot );
		LogisticsPilot& pilot = pilots.GetTail();
		pilot.id = id;

		if ( -1 == pilot.init( (char*)pilotFileName ) )
			pilots.DeleteTail();

		id++;

	}

	// PILOTS-ROSTER diagnostic (MC2_LOG_PILOTS=1): how many pilots loaded + the
	// roster path. Stock ~52; an MC2X/MCO compat roster (mc2x-compat/mco-compat
	// data/objects/pilots.csv, VFS-overlaid) should report ~100+. Confirms whether
	// the campaign/compat roster won over base stock.
	if ( mc2::gate::get<"MC2_LOG_PILOTS">() )
	{
		mc2::fmt::print_out("[PILOTS] loaded={:d} roster={}\n", mc2::fmt::as_i32((int)pilots.Count()), mc2::fmt::safe(pilotPath));
		fflush(stdout);
	}
}

void LogisticsData::initVariants()
{
	char variantPath[256];
	strcpy( variantPath, artPath );
	strcat( variantPath, "buildings.csv" );

	// ---- MC2_LOG_LOGISTICS diagnostics (instrumentation only, no behavior change) ----
	static FILE* s_logFile = nullptr;
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() && !s_logFile ) {
		s_logFile = fopen("logistics_debug.log", "a");
	}
#define LOG_LOGISTICS(fmt, ...) do { \
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() ) { \
		printf("[LOGISTICS] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
		if (s_logFile) { fprintf(s_logFile, "[LOGISTICS] " fmt "\n", ##__VA_ARGS__); fflush(s_logFile); } \
	} } while(0)

	LOG_LOGISTICS("initVariants: artPath='%s' objectPath='%s'", artPath, objectPath);
	LOG_LOGISTICS("initVariants: buildings.csv path='%s'", variantPath);

	CSVFile variantFile;
	long csvOpenResult = variantFile.open( variantPath );
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() ) {
		const char* modOwner = LookupModOwner(variantPath);
		unsigned long fsz = (csvOpenResult == NO_ERR) ? variantFile.fileSize() : 0;
		LOG_LOGISTICS("initVariants: buildings.csv open=%s size=%lu tier=%s",
			(csvOpenResult == NO_ERR) ? "OK" : "FAIL",
			(unsigned long)fsz,
			modOwner ? modOwner : "base-or-fastfile");
	}

	FullPathFileName pakPath;
	pakPath.init( objectPath, "Object2", ".pak" );

	PacketFile pakFile;
	
	if ( NO_ERR !=pakFile.open( pakPath ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe((const char*)pakPath));
		Assert( 0, 0, errorStr );
	}


	char variantFileName[256];
	char variantFullPath[1024];

	int chassisID = 0;

	char tmpStr[256];

	struct MechJob {
		std::string path;
		long fitID;
		float scale;
		int chassisId;
		LogisticsChassis* chassis = nullptr;
		std::vector<LogisticsVariant*> varList;
	};
	std::vector<MechJob> mechJobs;

	int i = 1;
	while( true )
	{
		long fitID;
		long nameID = 0;
		variantFile.readLong( i, 3, nameID );

		int retVal = variantFile.readString( i, 4, tmpStr, 256 );
		
		if ( retVal != 0 )
			break;

		if ( S_stricmp( tmpStr, "VEHICLE" ) == 0 )
		{
			float scale;
			variantFile.readFloat( i, 11, scale );
			if ( scale )
			{
				variantFile.readLong( i, 5, fitID );
				addVehicle( fitID, pakFile, scale, nameID );
			}
			i++;
			continue;
		}
		if ( S_stricmp( tmpStr, "MECH" ) != 0 )
		{
		
			float scale;
			if ( NO_ERR == variantFile.readFloat( i, 11, scale ) && scale )
			{
				variantFile.readLong( i, 5, fitID );
				addBuilding( fitID, pakFile, scale );
			}
			
			i++;
			continue;

		}
		
		float scale;
		long scaleReadResult = variantFile.readFloat( i, 11, scale );
		// ENCYCLO-3D-2: an EMPTY "Mechlopedia Scale" cell reads back as
		// NO_ERR + 0.0 (stock Buildings.csv leaves it blank for every mech),
		// so the != NO_ERR fallback never fired and every chassis carried
		// scale 0 -> SimpleCamera framed a zero-scaled mech -> black preview.
		// Blank/zero means "default", same as the buildings branch's `&& scale`.
		if ( NO_ERR != scaleReadResult || scale <= 0.0f )
			scale = 1.0;

		variantFile.readString( i, 1, variantFileName, 256 );

		// ENCYCLO-3D-2 diagnostic: what does the buildings.csv scale column
		// actually yield per mech row? (fresh-boot chassis scale observed 0.0)
		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			if ( FILE* f = fopen("preview_debug.log","a") )
			{
				mc2::fmt::print(f, "[ENCYCLO] initVariants row={:d} mech={} readResult={:d} scale={:.3f}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(variantFileName), mc2::fmt::as_i32(scaleReadResult), scale);
				fclose(f);
			}
		}

		variantFile.readLong( i, 5, fitID );

		strcpy( variantFullPath, objectPath );
		strcat( variantFullPath, variantFileName );
		strcat(  variantFullPath, ".csv" );
		S_strlwr( variantFullPath );

		mechJobs.push_back({ variantFullPath, fitID, scale, chassisID++ });

		i++;

	}

	// --- Parallel phase: open + parse each mech CSV concurrently ---
	// Safety: CSVFile/File use read-only FastFile globals and g_modIndex.
	// LogisticsChassis::init() writes only to 'this'. LogisticsVariant::init()
	// reads LogisticsData::components (read-only after initComponents()).
	// pChassis->refCount++ is per-chassis so only one thread touches each chassis.
	std::vector<std::future<void>> mechFutures;
	mechFutures.reserve(mechJobs.size());
	for (auto& job : mechJobs) {
		mechFutures.push_back(std::async(std::launch::async, [&job, this]() {
			CSVFile mechFile;
			if (NO_ERR != mechFile.open(job.path.c_str()))
				return;

			job.chassis = new LogisticsChassis();
			job.chassis->init(&mechFile, job.chassisId);
			job.chassis->setFitID(job.fitID);
			job.chassis->setScale(job.scale);

			int row = 23;
			char buffer[256];
			int varCount = 0;
			while (NO_ERR == mechFile.readString(row, 2, buffer, 256)) {
				LogisticsVariant* pVariant = new LogisticsVariant;
				if (0 == pVariant->init(&mechFile, job.chassis, varCount++))
					job.varList.push_back(pVariant);
				else
					delete pVariant;
				row += 97;
			}
		}));
	}
	for (auto& f : mechFutures) f.get();

	// Serial merge in original buildings.csv order.
	for (auto& job : mechJobs) {
		if (job.chassis) {
			for (auto* v : job.varList)
				variants.Append(v);
		}
	}

	// ---- post-load diagnostics ----
	if ( mc2::gate::get<"MC2_LOG_LOGISTICS">() ) {
		int totalVariants = 0;
		bool shadowhawkFound = false;
		for ( VARIANT_LIST::EIterator it = variants.Begin(); !it.IsDone(); it++ ) {
			totalVariants++;
			EString fn = (*it)->getFileName();
			char fnLow[1024];
			strncpy(fnLow, (const char*)fn, sizeof(fnLow)-1);
			fnLow[sizeof(fnLow)-1] = '\0';
			S_strlwr(fnLow);
			if ( strstr(fnLow, "shadowhawk") ) shadowhawkFound = true;
		}
		LOG_LOGISTICS("initVariants: total variants loaded=%d", totalVariants);
		if ( shadowhawkFound )
			LOG_LOGISTICS("SHADOWHAWK VARIANT LOADED");
		else
			LOG_LOGISTICS("SHADOWHAWK NOT IN VARIANTS");
	}
#undef LOG_LOGISTICS
}

void LogisticsData::addVehicle( long fitID, PacketFile& objectFile, float scale, long nameID )
{
	if ( NO_ERR != objectFile.seekPacket(fitID) )
		return;

	int fileSize = objectFile.getPacketSize();

	if ( fileSize )
	{
		LogisticsVehicle* pVehicle = new LogisticsVehicle;

		FitIniFile file;
		 file.open(&objectFile, fileSize);

		pVehicle->init( file );
		pVehicle->setScale( scale );
			if ( nameID ) pVehicle->setNameID( nameID );
		vehicles.Append( pVehicle );
	}
}


//*************************************************************************************************
void LogisticsData::RegisterFunctions()
{
	

}

//*************************************************************************************************
void LogisticsData::UnRegisterFunctions()
{

}



// GetAvailableMissions( char** missionNames, long& count )
int LogisticsData::getAvailableMissions( const char** missionNames, long& count )
{
	int numberOfEm = 0;

	// first figure out how many there are
	missionInfo->getAvailableMissions( nullptr, numberOfEm );

	// next make sure the array is big enough
	if ( count < numberOfEm )
		return NEED_BIGGER_ARRAY;

	missionInfo->getAvailableMissions( missionNames, numberOfEm );
	count= numberOfEm;
	
	return 0;



}

int LogisticsData::getCurrentMissions( const char** missionNames, long& count )
{
	int numberOfEm = 0;

	// first figure out how many there are
	missionInfo->getCurrentMissions( nullptr, numberOfEm );

	// next make sure the array is big enough
	if ( count < numberOfEm )
		return NEED_BIGGER_ARRAY;

	numberOfEm = count;

	missionInfo->getCurrentMissions( missionNames, numberOfEm );
	count= numberOfEm;
	
	return 0;



}

bool LogisticsData::getMissionAvailable( const char* missionName )
{
	return missionInfo->getMissionAvailable( missionName );
}


// SetCurrentMission( char* missionName )
int LogisticsData::setCurrentMissionAnyStage( const char* missionName )
{
	long result = missionInfo->setMissionAnyStage( missionName );
	if ( result == NO_ERR )
	{
		if ( !s_smartLoad || m_phaseBComplete ) {
			updateAvailability();
			removeDeadWeight();
		}
		resourcePoints = missionInfo->getCurrentRP();
	}
	return result;
}

int LogisticsData::setCurrentMission( const char* missionName )
{
	long result = missionInfo->setNextMission( missionName );

	if ( result == NO_ERR )
	{
		if ( !s_smartLoad || m_phaseBComplete ) {
			updateAvailability();
			removeDeadWeight();
		}
		resourcePoints = missionInfo->getCurrentRP();
	}


	

	return result;
}

void LogisticsData::removeDeadWeight()
{
	int maxDropWeight = getMaxDropWeight();
	int curDropWeight = getCurrentDropWeight();

	int i = 12;
	while ( curDropWeight > maxDropWeight )
	{
		LogisticsData::removeMechFromForceGroup( i );
		i--;

		curDropWeight = getCurrentDropWeight();

		if ( i == 0 )
			break;
	}
}

int		LogisticsData::setCurrentMission( const EString& missionName )
{
	return setCurrentMission( (const char*)missionName );
}
