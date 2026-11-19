#include "utils/mc2log.h"
// LogisticsData: the front-end data load (components, pilots, variants, vehicles), the smart-load gate and the mission availability queries.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include <memory>
#include <cstdio>
#include "logisticsdata_internal.h"
#include "mc2fmt.h"
#include "content_error.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "package_chassis.h"
#include "../GameOS/gameos/gos_render.h"
#include "../GameOS/gameos/loading_pump_meter.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <chrono>
#include <tracy/Tracy.hpp>


// SMART-LOAD-1: defer mech-bay data (components/pilots/variants) until first mech-bay accessor.
// SMART-LOAD-DEFAULT-ON-1: default flipped ON (was default-OFF). Kill-switch preserved --
// MC2_SMART_LOAD=0 (or any leading-zero/false value) restores legacy eager LogisticsData::init().
// Only affects the interactive splash->logistics->mech-bay flow (Logistics::initializeLogData(),
// code/logistics.cpp:248); smoke/tier1's justStartMission fast-path (mechcmd2.cpp:1813) calls
// Mission::init() directly and never touches LogisticsData, so this flip is a no-op for smoke
// mission_ready_ms -- validated via soak on the real splash/logistics path instead.
static const bool s_smartLoad      = mc2::gate::get<"MC2_SMART_LOAD">();   // default ON
static const bool s_smartLoadTrace = false;

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

	if ( std::ssize(components) ) // already been initialized
	{
		return;
	}

#define LOG_INIT_TIME(label) do { \
	if ( false ) { \
		ULONGLONG _t = GetTickCount64(); \
		printf("[LOGISTICS_INIT] " label " t=%llums\n", static_cast<unsigned long long>(_t)); fflush(stdout); \
	} } while(0)

	LOG_INIT_TIME("begin");
	initComponents();
	LOG_INIT_TIME("after_initComponents");
	initPilots();
	LOG_INIT_TIME("after_initPilots");
	initVariants();
	LOG_INIT_TIME("after_initVariants");

	missionInfo = std::make_unique_for_overwrite<LogisticsMissionInfo>();

	FitIniFile file;
	if ( NO_ERR != file.open( "data" PATH_SEPARATOR "campaign" PATH_SEPARATOR "campaign.fit" ) )
	{
		Assert( 0, 0, "coudln't find the campaign file\n" );
	}
	// A campaign file that does not init or lists no mission leaves the current mission unset (missionNames would be uninitialised).
	if ( !missionInfo->init( file ) )
		MC2_LOG(Io, Error, "campaign: the default campaign file could not be initialized, no current mission is set");
	else
	{
		int count = 32;
		const char* missionNames[32];
		missionInfo->getAvailableMissions( missionNames, count );
		if ( count < 1 )
			MC2_LOG(Io, Error, "campaign: the default campaign lists no available mission, no current mission is set");
		else
			setCurrentMission( missionNames[0] );
	}
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
	startPhaseB();
}

void LogisticsData::initPhaseA()
{
	if ( m_phaseAComplete ) return;
	SMART_LOAD_TRACE_LOG("Phase A begin");

	missionInfo = std::make_unique_for_overwrite<LogisticsMissionInfo>();
	FitIniFile file;
	if ( NO_ERR != file.open( "data" PATH_SEPARATOR "campaign" PATH_SEPARATOR "campaign.fit" ) )
		Assert( 0, 0, "coudln't find the campaign file\n" );
	if ( !missionInfo->init( file ) )
		MC2_LOG(Io, Error, "campaign: the default campaign file could not be initialized, no current mission is set");
	else
	{
		int count = 32;
		const char* missionNames[32];
		missionInfo->getAvailableMissions( missionNames, count );
		if ( count < 1 )
			MC2_LOG(Io, Error, "campaign: the default campaign lists no available mission, no current mission is set");
		else
			// setCurrentMission skips updateAvailability while !m_phaseBComplete
			setCurrentMission( missionNames[0] );
	}

	m_phaseAComplete = true;
	SMART_LOAD_TRACE_LOG("Phase A done");
}

// Phase B: loads components/pilots/variants and runs the first availability pass.
// No-op if gate OFF or already done. Only marks success at the end (retry-safe).
void LogisticsData::ensureMechBayDataLoaded( mc2::czstring trigger )
{
	ZoneScopedN("LogisticsData::ensureMechBayDataLoaded");
	if ( !s_smartLoad || m_phaseBComplete ) return;
	initPhaseA(); // must be done before Phase B
	SMART_LOAD_TRACE_LOG("Phase B begin trigger=%s", trigger ? trigger : "?");
	startPhaseB();
	{
		ZoneScopedN("Prewarm.Finish");
		mc2::prewarm::finish(m_phaseBJob, [] { graphics::pump_loading_events(); });
	}
	if (mc2_diag::tagRequested("MECH_PARSE"))
		std::printf("[PREWARM_LOGISTICS v1] trigger=%s max_step_ms=%.3f\n", trigger ? trigger : "?", m_phaseBJob->maxStepMs);
}

void LogisticsData::startPhaseB()
{
	if (m_phaseBComplete || m_phaseBJob) return;
	auto sequence = std::make_shared<mc2::prewarm::Sequence>(loadPhaseB());
	m_phaseBJob = mc2::prewarm::service().submitMain([sequence] {
		ZoneScopedN("Prewarm.LogisticsStep");
		return sequence->step();
	});
}

mc2::prewarm::Sequence LogisticsData::loadPhaseB()
{

	auto components = loadComponents();
	while (components.step()) co_yield 0;
	auto pilots = loadPilots();
	while (pilots.step()) co_yield 0;
	auto sequence = loadVariants();
	while (sequence.step()) co_yield 0;

	// First availability pass now that variants/components are populated
	updateAvailability();
	resourcePoints = missionInfo->getCurrentRP();

	m_phaseBComplete = true;
	SMART_LOAD_TRACE_LOG("Phase B done");
}

//*************************************************************************************************
void LogisticsData::initComponents()
{
	auto sequence = loadComponents();
	while (sequence.step()) {}
}

mc2::prewarm::Sequence LogisticsData::loadComponents()
{
	char componentPath[256];
	strcpy( componentPath, objectPath );
	strcat( componentPath, "compbas.csv" );

	
	File componentFile;
	if ( componentFile.open( componentPath ) != NO_ERR )
		STOP(( "LogisticsData::initComponents: unable to open %s", componentPath ));

	auto data = std::make_unique_for_overwrite<BYTE[]>(componentFile.getLength());

	componentFile.read( data.get(), componentFile.getLength() );


	File dataFile;
	dataFile.open( reinterpret_cast<char*>(data.get()), componentFile.getLength() );

	componentFile.close();
	
	BYTE line[1024];
	char* pLine = reinterpret_cast<char*>(line);

	// skip the first line
	dataFile.readLine(line, 1024);

	int		Ammo[512];
	memset( Ammo, 0, sizeof ( int ) * 512 );


	int counter = 0;
	while(true)
	{
		int result = dataFile.readLine( line, 1023 );

		if ( result < 2 || result == 0xBADF0008 || result > 1024  )
			break;

		components.emplace_back();

		LogisticsComponent& tmp = components.back();

		if ( !tmp.init( pLine ) ) // failure
		{
			Ammo[counter] = static_cast<long>(tmp.getRecycleTime());
			components.pop_back();
		}

		counter++;
		co_yield 0;
	}
	
	// fix up ammo
	for (auto & component : components)
	{
		if (component.getAmmo() )
		{
			component.setAmmo( Ammo[component.getAmmo()] );
		}
	}

	data.reset();

	// EQUIP-ADD-FILL-1 (S2): second consumer of the ADD pass -- the filled rows
	// are NOT in compbas.csv, so append them here too (gate OFF -> empty list).
	// Cold logistics constructs variants before Mission::init. Materialize ADD
	// rows now using the same table, sensor scale and overlay order as missions.
	// Without an ADD-enabled package (or explicit legacy gates), stock is untouched.
	const bool equipmentAdd = equipmentPackageAccessAllowed(
		mc2UnitProfileDataEnabled() && mc2EquipmentAddEnabled(),
		ModCapabilityMounted("equipment_add"),
		mc2::gate::state<"MC2_EQUIPMENT_ADD">() == mc2::gate::Tri::Off);
	if (equipmentAdd && !MasterComponent::masterList)
	{
		FullPathFileName gameSystemPath;
		gameSystemPath.init(missionPath, "gamesys", ".fit");
		FitIniFile gameSystem;
		float sensorRange = 0.0f;
		if (gameSystem.open(gameSystemPath) != NO_ERR ||
			gameSystem.seekBlock("General") != NO_ERR ||
			gameSystem.readIdFloat("BaseSensorRange", sensorRange) != NO_ERR)
			STOP(("LogisticsData::initComponents: missing BaseSensorRange in %s", static_cast<const char*>(gameSystemPath)));
		if (MasterComponent::loadMasterList(componentPath, 255, sensorRange) != NO_ERR)
			STOP(("LogisticsData::initComponents: unable to load %s", componentPath));
		overlayGeneratedEquipmentStatsIfEnabled();
		applyEquipmentAddFillIfEnabled(sensorRange);
		buildAndDumpEquipmentRegistryIfEnabled();
		dumpFireExecutorProofIfEnabled();
	}

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
		for ( auto iter = components.begin(); iter != components.end(); ++iter )
		{
			if ( (*iter).getID() == r.ordinal ) { present = true; break; }
		}
		if ( present )
			continue;

		char lineBuf[1024];
		strncpy( lineBuf, r.csvLine.c_str(), 1023 );
		lineBuf[1023] = 0;

		components.emplace_back();
		LogisticsComponent& tmp = components.back();
		if ( !tmp.init( lineBuf ) )
		{
			// icon column is validated by the ADD pass, so this is unexpected --
			// surface it rather than half-materializing the item.
			mc2::fmt::print_out( "[EQUIP-ADD] logistics consumer REJECTED synthesized row (id={} ordinal={:d}) -- item absent from mechlab\n",
					r.qualifiedId.c_str(), mc2::fmt::as_i32(r.ordinal) );
			components.pop_back();
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
// WHAT: empties the pilot roster. WHY: every mech of the inventory holds a LogisticsPilot* into it, so the roster may only go once the inventory is empty; a caller
// that breaks that order would leave the mechs pointing at freed elements, so the pointers are cleared first and the event is logged.
void LogisticsData::clearPilots()
{
	if ( !inventory.empty() )
	{
		MC2_LOG(Io, Warn, "logistics: the pilot roster is cleared while the inventory holds {} mechs; their pilots are unassigned", std::ssize( inventory ));
		for ( const auto& pMech : inventory )
			pMech->setPilot( nullptr );
	}
	pilots.clear();
}

//*************************************************************************************************
void LogisticsData::initPilots()
{
	auto sequence = loadPilots();
	while (sequence.step()) {}
}

mc2::prewarm::Sequence LogisticsData::loadPilots()
{

	clearPilots();

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
		pilots.push_back( std::move(tmpPilot) );
		LogisticsPilot& pilot = pilots.back();
		pilot.id = id;

		if ( !pilot.init( reinterpret_cast<char*>(pilotFileName) ) )
			pilots.pop_back();

		id++;
		co_yield 0;

	}

	// PILOTS-ROSTER diagnostic (MC2_LOG_PILOTS=1): how many pilots loaded + the
	// roster path. Stock ~52; an MC2X/MCO compat roster (mc2x-compat/mco-compat
	// data/objects/pilots.csv, VFS-overlaid) should report ~100+. Confirms whether
	// the campaign/compat roster won over base stock.
	if ( false )
	{
		mc2::fmt::print_out("[PILOTS] loaded={:d} roster={}\n", mc2::fmt::as_i32(static_cast<int>(std::ssize(pilots))), mc2::fmt::safe(pilotPath));
		fflush(stdout);
	}
}

// Reads the "File Name" cell; false (row to skip) when it is >= 256 chars, since copyString then leaves the buffer unterminated.
static bool readCsvFileName( CSVFile& f, int row, char* out, size_t cap )
{
	if ( f.readString( row, 1, out, cap ) != -2 )
		return true;
	out[cap - 1] = '\0';
	mc2::content::error( mc2::content::Kind::MissionData, "Buildings.csv", "File Name cell over 255 chars, row skipped" );
	return false;
}

void LogisticsData::initVariants()
{
	auto sequence = loadVariants();
	while (sequence.step()) {}
}

mc2::prewarm::Sequence LogisticsData::loadVariants()
{
	graphics::begin_variant_load();
	struct PumpReport { ~PumpReport() { graphics::end_variant_load(); } } pumpReport;
	char variantPath[256];
	strcpy( variantPath, artPath );
	strcat( variantPath, "buildings.csv" );

	// ---- MC2_LOG_LOGISTICS diagnostics (instrumentation only, no behavior change) ----




	CSVFile variantFile;
	variantFile.open( variantPath );

	FullPathFileName pakPath;
	pakPath.init( objectPath, "Object2", ".pak" );

	PacketFile pakFile;
	
	if ( NO_ERR !=pakFile.open( pakPath ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(pakPath)));
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
		std::vector<std::unique_ptr<LogisticsVariant>> varList;
		bool package = false;
	};
	std::vector<MechJob> mechJobs;

	int i = 1;
	while( true )
	{
		graphics::pump_loading_events();
		co_yield 0;
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
				char csvName[256] = "";
				if ( readCsvFileName( variantFile, i, csvName, sizeof( csvName ) ) )
					addVehicle( fitID, pakFile, scale, nameID, csvName );
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
				char csvName[256] = "";
				if ( readCsvFileName( variantFile, i, csvName, sizeof( csvName ) ) )
					addBuilding( fitID, pakFile, scale, csvName );
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
			if ( bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info) )
			{
				MC2_LOG(Ui, Info, "[ENCYCLO] initVariants row={:d} mech={} readResult={:d} scale={:.3f}", mc2::fmt::as_i32(i), mc2::fmt::safe(variantFileName), mc2::fmt::as_i32(scaleReadResult), scale);

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

	do { if (false && mc2_diag::tagRequested("LOGISTICS")) MC2_LOG(Ui, Info, "[LOGISTICS] initVariants: vehicles={:d} buildings={:d} object2.pak packets={:d}", mc2::fmt::as_i32(static_cast<int>(std::ssize(vehicles))), mc2::fmt::as_i32(static_cast<int>(std::ssize(buildings))), mc2::fmt::as_i32(static_cast<int>(pakFile.getNumPackets()))); } while (0);

	// BT2018-FIRST-CLASS P3: package chassis (mods with the package_chassis capability) append
	// after every buildings.csv row, so stock chassis keep their ids and order. Their fitID is
	// the runtime object-type number the registry allocated after the last Object2.pak packet.
	packageChassisEnsureRegistry(pakFile.getNumPackets(), mc2pkg::kPackageTypeLimit);
	for (const mc2pkg::PackageChassis& pc : packageChassisAll())
	{
		if (pc.objTypeNum < 0)
			continue;
		std::string csvPath = std::string(objectPath) + pc.stem + ".csv";
		for (char& ch : csvPath) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		mechJobs.push_back({ csvPath, pc.objTypeNum, pc.scale, chassisID++ });
		mechJobs.back().package = true;
	}

	// The mech CSVs are parsed on this thread, one after another. WHY: a FastFile archive reads through one shared FILE*
	// (seek then read) and one shared LZ buffer, so parsing them concurrently let one thread's seek land between another's
	// seek and read. Under host load a chassis file then failed to parse, its variants were missing, and every mech of that
	// variant got the 10 C-Bill fallback in Mech::init (mc2_16 save, BUGS-FOUND-FIXED).
	const auto parseStart = std::chrono::steady_clock::now();
	using Clock = std::chrono::steady_clock;
	struct ParseLaps { Clock::duration open{}, chassis{}, variants{}, maxStep{}; int files = 0; };
	ParseLaps laps[2]; // Stock and package timings remain separately attributable.
	auto& pumpScope = mc2::loadpump::meter.variants;
	pumpScope.mark(SDL_GetTicks64(), "initVariants.parse");
	for (auto& job : mechJobs) {
		auto& lap = laps[job.package ? 1 : 0];
		++lap.files;
		graphics::pump_loading_events();
		co_yield 0;
		const auto openStart = Clock::now();
		CSVFile mechFile;
		const long openResult = mechFile.open(job.path.c_str());
		const auto chassisStart = Clock::now();
		lap.open += chassisStart - openStart;
		lap.maxStep = std::max(lap.maxStep, chassisStart - openStart);
		if (openResult != NO_ERR)
		{
			MC2_LOG(Io, Warn, "logistics: mech file {} did not open (error {:#x}); its chassis and variants are skipped", job.path, static_cast<unsigned long>(openResult));
			continue;
		}

		job.chassis = new LogisticsChassis();
		job.chassis->init(&mechFile, job.chassisId);
		job.chassis->setFitID(job.fitID);
		job.chassis->setScale(job.scale);
		const auto variantStart = Clock::now();
		lap.chassis += variantStart - chassisStart;
		lap.maxStep = std::max(lap.maxStep, variantStart - chassisStart);

		int row = 23;
		char buffer[256];
		int varCount = 0;
		while (NO_ERR == mechFile.readString(row, 2, buffer, 256)) {
			graphics::pump_loading_events();
			const auto singleVariantStart = Clock::now();
			auto pVariant = std::make_unique_for_overwrite<LogisticsVariant>();
			if (0 == pVariant->init(&mechFile, job.chassis, varCount))
				job.varList.push_back(std::move(pVariant));
			else
			{
				MC2_LOG(Io, Warn, "logistics: mech file {} variant {} (CSV row {}) is malformed and is skipped", job.path, varCount, row);
			}
			varCount++;
			row += 97;
			const auto elapsed = Clock::now() - singleVariantStart;
			lap.maxStep = std::max(lap.maxStep, elapsed);
			lap.variants += elapsed;
			co_yield 0;
		}
	}
	// One line per load: the serial parse is on the startup (or first mech-bay) path, so its cost stays visible.
	MC2_LOG(Io, Info, "logistics: parsed {} mech files in {} ms", mechJobs.size(),
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parseStart).count());
	if (mc2_diag::tagRequested("MECH_PARSE")) for (int group = 0; group < 2; ++group) {
		const auto& lap = laps[group];
		MC2_LOG(Io, Info, "[MECH_PARSE v1] kind={} files={} open_ms={} chassis_ms={} variant_ms={} max_step_ms={}",
			group ? "package" : "stock", lap.files,
			std::chrono::duration_cast<std::chrono::milliseconds>(lap.open).count(),
			std::chrono::duration_cast<std::chrono::milliseconds>(lap.chassis).count(),
			std::chrono::duration_cast<std::chrono::milliseconds>(lap.variants).count(),
			std::chrono::duration_cast<std::chrono::milliseconds>(lap.maxStep).count());
	}

	// Serial merge in original buildings.csv order.
	for (auto& job : mechJobs) {
		if (job.chassis) {
			for (auto& v : job.varList)
				variants.push_back(std::move(v));
		}
	}

	// ---- post-load diagnostics ----
}

void LogisticsData::addVehicle( long fitID, PacketFile& objectFile, float scale, long nameID, std::string_view csvName )
{
	// MCO buildings.csv VEHICLE rows use FitIDs (2000+) past the mod's object2.pak; like ObjectTypeManager, take
	// their definition from the loose data/objects/<File Name>.fit named by the CSV row instead of the pak packet.
	FitIniFile file;
	if ( fitID >= 0 && fitID < objectFile.getNumPackets() )
	{
		if ( NO_ERR != objectFile.seekPacket(fitID) )
			return;
		const int fileSize = objectFile.getPacketSize();
		if ( !fileSize || NO_ERR != file.open(&objectFile, fileSize) )
			return;
	}
	else if ( !openLooseChassisFit( file, csvName ) )
		return;

	auto pVehicle = std::make_unique<LogisticsVehicle>();
	pVehicle->init( file );
	pVehicle->setScale( scale );
	if ( nameID ) pVehicle->setNameID( nameID );
	vehicles.push_back( std::move(pVehicle) );
}

bool LogisticsData::openLooseChassisFit( FitIniFile& file, std::string_view csvName ) const
{
	if ( csvName.empty() )
		return false;
	char path[1024];
	mc2::fmt::to_buf(path, "{}{}.fit", mc2::fmt::safe(static_cast<const char*>(objectPath)), mc2::fmt::safe(std::string(csvName).c_str()));
	S_strlwr( path );
	return NO_ERR == file.open( path );
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

bool LogisticsData::getMissionAvailable( mc2::czstring missionName )
{
	return missionInfo->getMissionAvailable( missionName );
}


// SetCurrentMission( char* missionName )
std::expected<void, long> LogisticsData::setCurrentMissionAnyStage( mc2::czstring missionName )
{
	auto result = missionInfo->setMissionAnyStage( missionName );
	if ( result )
	{
		if ( !s_smartLoad || m_phaseBComplete ) {
			updateAvailability();
		}
		resourcePoints = missionInfo->getCurrentRP();
	}
	return result;
}

std::expected<void, long> LogisticsData::setCurrentMission( mc2::czstring missionName )
{
	auto result = missionInfo->setNextMission( missionName );

	if ( result )
	{
		if ( !s_smartLoad || m_phaseBComplete ) {
			updateAvailability();
		}
		resourcePoints = missionInfo->getCurrentRP();
	}


	

	return result;
}

