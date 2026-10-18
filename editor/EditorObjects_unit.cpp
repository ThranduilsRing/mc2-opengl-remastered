// Editor objects: Unit: construction, copy, save / load, colours and the brain patrol import.
// Declared in the class headers; shared names in EditorObjects_internal.h.
#include "EditorObjects_internal.h"




Unit::Unit( int align )
{
	unsigned long squadNum = EditorObjectMgr::instance()->getNextAvailableSquadNum();
	setSquad(squadNum);
	EditorObjectMgr::instance()->registerSquadNum(squadNum);
	setSelfRepairBehaviorEnabled(true);
	pilot.info = align == 0 ? &Pilot::s_GoodPilots[0] : &Pilot::s_BadPilots[0];
	baseColor = 0x00ffffff;
	highlightColor = 0x00c0c0c0;
	highlightColor2 = 0x00808080;
	variant = 0;
	orderType = ORDER_NONE;
	stance = 2;   // AttitudeType ATTITUDE_NORMAL
	orderAuthored = false;
	importChecked = false;
	brainFsm[0] = 0;
	brainBehavior = BRAIN_UNKNOWN;
}

Unit::~Unit()
{
	alternatives.clear();
}

Unit::Unit( const Unit& src ) : EditorObject( src )
{
	
	if ( &src != this )
	{
		brain = src.brain;
		authoredPart = src.authoredPart;
		lance = src.lance;
		lanceIndex = src.lanceIndex;
		pilot = src.pilot;
		unsigned long color1, color2, color3;
		src.getColors(color1, color2, color3);
		setColors(color1, color2, color3);
		setSelfRepairBehaviorEnabled(src.getSelfRepairBehaviorEnabled());
		setVariant(src.getVariant());
		alternatives.insert(alternatives.end(), src.alternatives.begin(), src.alternatives.end());
		squad = src.squad;
		orderType = src.orderType;
		stance = src.stance;
		waypoints = src.waypoints;
		orderAuthored = src.orderAuthored;
		importChecked = src.importChecked;
		strncpy( brainFsm, src.brainFsm, sizeof( brainFsm ) ); brainFsm[sizeof( brainFsm ) - 1] = 0;
		brainBehavior = src.brainBehavior;
	}
}

void Unit::CastAndCopy(const EditorObject &master)
{
	const Unit *pCastedMaster = dynamic_cast<const Unit *>(&master);
	if (nullptr != pCastedMaster)
	{
		(*this) = (*pCastedMaster);
	}
	else
	{
		gosASSERT(false);
		(*((EditorObject *)this)) = (master);
	}
}

void Unit::refreshPartFromCatalog()
{
	const bool vehicle = dynamic_cast<GVAppearance*>(appearance()) != nullptr;
	const bool player = appearance()->teamId == EDITOR_TEAM1;
	const char* csv = EditorObjectMgr::instance()->getFileName(id);
	authoredPart = PartDefinition{vehicle ? 2ul : 1ul,
		vehicle ? (player ? "pv20600" : "pv20500") : (player ? "PM207300" : "PM101100"), csv ? csv : ""};
	for (auto it = alternatives.begin(); it != alternatives.end(); it++)
		(*it).refreshPartFromCatalog();
}

bool Unit::save( FitIniFile* file, int WarriorNumber )
{
	if (!authoredPart) return EditorFit::reject("Part definition missing");
	const auto& definition = *authoredPart;
	const char* objectProfile = definition.objectProfile.c_str();
	if (mechAsset)
	{
		const char * iniFilename = EditorObjectMgr::instance()->getFileName( id );
		char buf[512] = {0};

		if (iniFilename && iniFilename[0])
		{
			strcpy(buf, "Data\\TGL\\");
			strcat(buf, iniFilename);
			strcat(buf, ".ini");

			IProviderAssetPtr objAssetPtr = armProvider->OpenAsset(buf,
								AssetType_Physical, ProviderType_Primary);

			if (getDisplayName()[0])
			{
				objAssetPtr->AddProperty("DisplayName", getDisplayName());
			}

			objAssetPtr->AddProperty("ObjectType", "Mech");
			objAssetPtr->Close();
		}

		mechAsset->AddProperty("Type", "Warrior");

		const std::string profilePath = std::string("Data\\Missions\\Profiles\\") + objectProfile + ".fit";
		mechAsset->AddRelationship("ObjectProfile", profilePath.c_str());

		// Asset links use the authored CSV identity too, so catalog refresh stays
		// an explicit authoring decision even for asset-provider metadata.
		if (!definition.csvFile.empty())
		{
			const std::string csvPath = std::string("Data\\Objects\\") + definition.csvFile + ".csv";
			mechAsset->AddRelationship("CSVFile", csvPath.c_str());
		}
		if (iniFilename && iniFilename[0])
		{
			strcpy(buf, "Data\\TGL\\");
			strcat(buf, iniFilename);
			strcat(buf, ".ini");
			mechAsset->AddRelationship("AppearanceFile", buf);
		}
	}

	pilot.save( file, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0 );
	brain.save( file, WarriorNumber, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0);

	char tmp[256];
	sprintf( tmp, "Part%ld", WarriorNumber );
	file->writeBlock( tmp );
	file->writeIdULong( "ObjectNumber", EditorObjectMgr::instance()->getFitID( id ) );
	file->writeIdULong( "ControlType", 2 );
	int playerNum = appearance()->teamId;
	file->writeIdBoolean( "PlayerPart", playerNum == EDITOR_TEAM1 ? true : false );
	file->writeIdChar( "MyIcon", 0 );
	int teamNum = EditorData::instance->PlayersRef().PlayerRef(playerNum).DefaultTeam();
	file->writeIdChar( "TeamID", teamNum );
	file->writeIdChar( "CommanderID",playerNum );
	file->writeIdULong( "Pilot", WarriorNumber );
	file->writeIdFloat( "PositionX", appearance()->position.x  );
	file->writeIdFloat( "PositionY", appearance()->position.y );
	file->writeIdFloat( "Rotation", appearance()->rotation );
	file->writeIdLong( "Active", 1 );
	file->writeIdLong( "Exists", 1 );
	file->writeIdFloat( "Damage", (float)((int)getDamage()) );
	file->writeIdULong( "BaseColor", baseColor );
	file->writeIdULong( "HighlightColor1", highlightColor );
	file->writeIdULong( "HighlightColor2", highlightColor2 );

	unsigned long tmpULong = 0;
	if (getSelfRepairBehaviorEnabled())
	{
		tmpULong = 1;
	}
	file->writeIdULong( "SelfRepairBehavior", tmpULong );

	file->writeIdULong( "ControlDataType", definition.controlDataType );
	file->writeIdString( "ObjectProfile", objectProfile );
	file->writeIdString( "CSVFile", definition.csvFile.c_str() );
	file->writeIdULong( "VariantNumber", variant );

	file->writeIdULong( "SquadNum", getSquad() );
	file->writeIdULong( "NumAlternatives", static_cast<unsigned long>(alternatives.size()) );

	// Patrol/Move order authoring (additive — only written when the user has
	// AUTHORED an order in-editor, so stock missions whose patrol lives in the
	// brain .abl and was only imported for display are NOT rewritten).
	if ( orderAuthored && ( orderType != ORDER_NONE || !waypoints.empty() ) )
	{
		file->writeIdLong( "OrderType", orderType );
		file->writeIdLong( "OrderStance", stance );
		file->writeIdULong( "WaypointCount", (unsigned long)waypoints.size() );
		for ( size_t i = 0; i < waypoints.size(); ++i )
		{
			char k[64];
			sprintf( k, "Waypoint%uX", (unsigned)i ); file->writeIdFloat( k, waypoints[i].x );
			sprintf( k, "Waypoint%uY", (unsigned)i ); file->writeIdFloat( k, waypoints[i].y );
			sprintf( k, "Waypoint%uZ", (unsigned)i ); file->writeIdFloat( k, waypoints[i].z );
		}
	}

	return true;
}

bool Unit::load( FitIniFile* file, int warriorNumber )
{
	PartDefinition definition;
	long control = 0;
	if (!EditorFit::longValue(*file, "ControlDataType", control, true)
		|| !EditorFit::stringValue(*file, "ObjectProfile", definition.objectProfile)
		|| !EditorFit::stringValue(*file, "CSVFile", definition.csvFile))
		return EditorFit::reject("Part control/profile/CSVFile");
	definition.controlDataType = static_cast<unsigned long>(control);
	authoredPart = std::move(definition);
	long result = 0;
	char tmp;
	file->readIdChar( "CommanderID", tmp );
	appearance()->teamId = tmp;
	file->readIdFloat( "PositionX", appearance()->position.x  );
	file->readIdFloat( "PositionY", appearance()->position.y );
	file->readIdFloat( "Rotation", appearance()->rotation );

	int tmpCellRow = 0;
	int tmpCellColumn = 0;
	land->worldToCell( appearance()->position, tmpCellRow, tmpCellColumn );
	cellRow = tmpCellRow;
	cellColumn = tmpCellColumn;

	float fDamage = 0.0;
	result = file->readIdFloat( "Damage", fDamage );
	if ((NO_ERR == result) && (0.0 != fDamage))
	{
		setDamage(true);
	}

	file->readIdULong( "BaseColor", baseColor );
	file->readIdULong( "HighlightColor1", highlightColor );
	file->readIdULong( "HighlightColor2", highlightColor2 );

	unsigned long tmpULong = 1;
	result = file->readIdULong( "SelfRepairBehavior", tmpULong );
	if ((NO_ERR == result) && (0 == tmpULong))
	{
		setSelfRepairBehaviorEnabled(false);
	}
	else
	{
		setSelfRepairBehaviorEnabled(true);
	}

	file->readIdULong( "VariantNumber", variant );

	unsigned long squadNum = 1;
	result = file->readIdULong("SquadNum", squadNum);
	if (NO_ERR != result) {
		// the unit should already have a valid default squad assigned at construction
	} else {
		setSquad(squadNum);
		EditorObjectMgr::instance()->registerSquadNum(squadNum);
	}

	tmpNumAlternativeInstances = 0;
	long alternatives = 0;
	if (!EditorFit::longValue(*file, "NumAlternatives", alternatives, true) || alternatives > 15)
		return EditorFit::reject("Part alternative count");
	tmpNumAlternativeInstances = static_cast<unsigned long>(alternatives);
	tmpAlternativeStartIndex = 0;
	if (0 < tmpNumAlternativeInstances)
	{
		long start = 0;
		if (!EditorFit::longValue(*file, "AlternativeStartIndex", start, true) || start == 0)
			return EditorFit::reject("Part alternative index");
		tmpAlternativeStartIndex = static_cast<unsigned long>(start);
	}

	// Patrol/Move order authoring (additive — absent in stock missions; defaults
	// to ORDER_NONE / Normal stance / no waypoints). Read from the Part block,
	// before the Warrior seekBlock below.
	orderType = ORDER_NONE;
	stance = 2;
	waypoints.clear();
	orderAuthored = false;
	importChecked = false;
	{
		long tmpOrder = ORDER_NONE;
		if ( NO_ERR == file->readIdLong( "OrderType", tmpOrder ) )
			orderType = (int)tmpOrder;
		long tmpStance = 2;
		if ( NO_ERR == file->readIdLong( "OrderStance", tmpStance ) )
			stance = (int)tmpStance;
		unsigned long wpCount = 0;
		if ( NO_ERR == file->readIdULong( "WaypointCount", wpCount ) )
		{
			// Editor-authored order present in the .fit -> it owns persistence.
			// (Leave importChecked false so brain fsm/behavior still get analyzed;
			// the patrol-import is guarded by orderAuthored/non-empty waypoints.)
			orderAuthored = true;
			for ( unsigned long i = 0; i < wpCount; ++i )
			{
				glm::vec3 wp; wp.x = wp.y = wp.z = 0.0f;
				char k[64];
				sprintf( k, "Waypoint%luX", i ); file->readIdFloat( k, wp.x );
				sprintf( k, "Waypoint%luY", i ); file->readIdFloat( k, wp.y );
				sprintf( k, "Waypoint%luZ", i ); file->readIdFloat( k, wp.z );
				waypoints.push_back( wp );
			}
		}
	}

	char blockId[256];
	sprintf(blockId,"Warrior%d",warriorNumber);
	file->seekBlock(blockId);
	pilot.load( file, appearance()->teamId == EDITOR_TEAM1 ? 1 : 0 );

	brain.load( file, warriorNumber);
	
	setColors(baseColor,highlightColor,highlightColor2);
	
	return true;
}

void Unit::getColors( unsigned long& color1, unsigned long& color2, unsigned long& color3 ) const
{
	color1 = baseColor;
	color2 = highlightColor;
	color3 = highlightColor2;
}

void Unit::setColors( unsigned long color1, unsigned long color2, unsigned long color3 ) 
{
	baseColor = color1;
	highlightColor = color2;
	highlightColor2 = color3;
	
	appearance()->resetPaintScheme(color2,color3,color1);
}

Unit& Unit::operator=( const Unit& src )
{
	if ( &src != this )
	{
		brain = src.brain;
		authoredPart = src.authoredPart;
		lance = src.lance;
		lanceIndex = src.lanceIndex;
		pilot = src.pilot;
		unsigned long color1, color2, color3;
		src.getColors(color1, color2, color3);
		setColors(color1, color2, color3);
		setSelfRepairBehaviorEnabled(src.getSelfRepairBehaviorEnabled());
		setVariant(src.getVariant());
		alternatives.clear();
		alternatives.insert(alternatives.end(), src.alternatives.begin(), src.alternatives.end());
		squad = src.squad;
		orderType = src.orderType;
		stance = src.stance;
		waypoints = src.waypoints;
		orderAuthored = src.orderAuthored;
		importChecked = src.importChecked;
		strncpy( brainFsm, src.brainFsm, sizeof( brainFsm ) ); brainFsm[sizeof( brainFsm ) - 1] = 0;
		brainBehavior = src.brainBehavior;

		EditorObject::operator=( src );
	}

	return *this;
}

// Parse a patrol brain .abl for its startPatrolPath[i,0/1] waypoint literals.
// The unit's brain file (brainName.abl at warriorPath) is exactly the script the
// engine loads, so existing stock patrols live here as explicit XY coordinates:
//   startPatrolState[1] = <count>;
//   startPatrolPath[0, 0] = <x>;  startPatrolPath[0, 1] = <y>;  ...
// (also matches base variants like startBase1PatrolPath[i,j]). z is left 0; the
// overlay re-samples terrain elevation when drawing.
// Analyze a unit's brain .abl (the exact script the engine loads from
// <warriorPath><brainName>.abl): extract any patrol path (startPatrolPath[i,0/1]
// literals), the fsm name, and a coarse behavior tag. Returns true if the file
// opened (even with no patrol). fsmOut/behaviorOut are always written.
static bool EditorAnalyzeBrain( const char* brainName, std::vector<glm::vec3>& wpOut,
	char* fsmOut, size_t fsmCap, int& behaviorOut )
{
	wpOut.clear();
	if ( fsmOut && fsmCap ) fsmOut[0] = 0;
	behaviorOut = Unit::BRAIN_UNKNOWN;
	if ( !brainName || !brainName[0] )
		return false;

	char path[512];
	sprintf( path, "%s%s.abl", warriorPath, brainName );
	File f;
	long openRc = f.open( path );
	if ( mc2::gate::get<"MC2_EDITOR_TRACE">() )
	{
		fprintf( stderr, "[PATROL] open '%s' rc=%ld (warriorPath='%s' brain='%s')\n",
			path, openRc, warriorPath, brainName );
		fflush( stderr );
	}
	if ( NO_ERR != openRc )
		return false;

	std::vector<float> xs, ys;
	std::vector<unsigned char> hasX, hasY;
	bool sawPatrol = false, sawGuard = false, sawAttack = false;
	char line[1024];
	while ( !f.eof() )
	{
		long n = f.readLine( (MemoryPtr)line, (long)sizeof( line ) - 1 );
		if ( n <= 0 )
			break;
		if ( n >= (long)sizeof( line ) ) n = (long)sizeof( line ) - 1;
		line[n] = 0;

		// fsm name: "fsm <name>;" (first occurrence).
		if ( fsmOut && fsmCap && !fsmOut[0] )
		{
			const char* fp = strstr( line, "fsm " );
			if ( fp == line || ( fp && ( fp == line || fp[-1] == '\t' || fp[-1] == ' ' ) ) )
			{
				fp += 4;
				while ( *fp == ' ' || *fp == '\t' ) ++fp;
				size_t k = 0;
				while ( fp[k] && fp[k] != ';' && fp[k] != ' ' && fp[k] != '\t' && fp[k] != '\r' && fp[k] != '\n' && k < fsmCap - 1 )
				{ fsmOut[k] = fp[k]; ++k; }
				fsmOut[k] = 0;
			}
		}

		if ( strstr( line, "PATROL_TYPE" ) || strstr( line, "PatrolPath" ) ) sawPatrol = true;
		if ( strstr( line, "guard" ) || strstr( line, "Guard" ) )            sawGuard  = true;
		if ( strstr( line, "attack" ) || strstr( line, "Attack" ) )          sawAttack = true;

		const char* p = strstr( line, "PatrolPath[" );
		if ( !p )
			continue;
		p += 11;   // strlen("PatrolPath[")
		int idx = -1, comp = -1;
		if ( sscanf( p, "%d , %d", &idx, &comp ) != 2 &&
			 sscanf( p, "%d,%d", &idx, &comp ) != 2 )
			continue;
		const char* eq = strchr( p, '=' );
		if ( !eq )
			continue;
		float val = (float)atof( eq + 1 );
		if ( idx < 0 || idx > 4096 )
			continue;
		if ( comp == 0 )
		{
			if ( (int)xs.size() <= idx ) { xs.resize( idx + 1, 0.f ); hasX.resize( idx + 1, 0 ); }
			xs[idx] = val; hasX[idx] = 1;
		}
		else if ( comp == 1 )
		{
			if ( (int)ys.size() <= idx ) { ys.resize( idx + 1, 0.f ); hasY.resize( idx + 1, 0 ); }
			ys[idx] = val; hasY[idx] = 1;
		}
	}

	size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	for ( size_t i = 0; i < count; ++i )
		if ( i < hasX.size() && i < hasY.size() && hasX[i] && hasY[i] )
		{
			// .abl PatrolPath coords are raw world coords, same frame as unit
			// PositionX/Y (verified: mc2_01_Pat1 unit (3434,1983) sits next to its
			// startBase1PatrolPath[0]=(3008,1472); both mc2_01 and mc2_03 patrol coords
			// span the same +/- world range as their unit positions). No transform.
			glm::vec3 wp; wp.x = xs[i]; wp.y = ys[i]; wp.z = 0.0f;
			wpOut.push_back( wp );
		}

	// Classify: patrol path wins; else guard/attack by keyword; else idle.
	if ( sawPatrol || !wpOut.empty() ) behaviorOut = Unit::BRAIN_PATROL;
	else if ( sawGuard )               behaviorOut = Unit::BRAIN_GUARD;
	else if ( sawAttack )              behaviorOut = Unit::BRAIN_ATTACK;
	else                               behaviorOut = Unit::BRAIN_IDLE;
	return true;
}

// declarative mission.fit Brain{} Primary-OPORD Patrol import.
// Matches this unit's spawn (ux,uy) to a parsed Brain record via MissionFitBrainSource --
// the SAME shared cache + position-match tolerance the "Declarative Brain" panel section
// uses (editor/UnitBrainPanel.cpp), so what the panel shows for a unit and what this seeds
// onto its waypoint list (and therefore what editor/EditorDebugOverlay.cpp's drawOnePatrol
// draws on the map) can never disagree. Returns true (and fills wpOut/loopOut) only when the
// matched brain's Primary OPORD is a waypointed Patrol; false leaves the caller to fall back
// to the legacy .abl PatrolPath[] literal scan (EditorAnalyzeBrain above), which remains the
// only source for missions with no declarative Brain{} block.
static bool EditorImportDeclarativePatrol( float ux, float uy,
	std::vector<glm::vec3>& wpOut, bool& loopOut )
{
	wpOut.clear();
	loopOut = false;
	MissionFitBrainSource::Refresh();
	const MissionFitBrain* b = MissionFitBrainSource::MatchForPos( ux, uy );
	if ( !b || 0 != strcmp( b->primary.type, "Patrol" ) || b->primary.waypointCount <= 0 )
		return false;
	for ( int i = 0; i < b->primary.waypointCount; ++i )
	{
		glm::vec3 wp;
		wp.x = b->primary.waypoints[i].x;
		wp.y = b->primary.waypoints[i].y;
		wp.z = 0.0f;   // overlay re-samples terrain elevation when drawing, same as the .abl path
		wpOut.push_back( wp );
	}
	loopOut = b->primary.loop;
	return true;
}

void Unit::importPatrolFromBrainIfNeeded()
{
	if ( importChecked )
		return;
	importChecked = true;

	// .abl analysis always runs: it supplies brainFsm + a coarse brainBehavior tag for the
	// "AI Brain" panel section regardless of which source ends up supplying the patrol path.
	std::vector<glm::vec3> wps;
	EditorAnalyzeBrain( brain.getBrainName(), wps, brainFsm, sizeof( brainFsm ), brainBehavior );
	bool loop = true;   // legacy .abl patrols: always closed-loop (unchanged prior behavior)

	// Declarative mission.fit Brain{} Primary-OPORD Patrol is AUTHORITATIVE when this unit's
	// spawn position matches one (carver_v_enhanced content) -- overrides the .abl scan above.
	if ( appearance() )
	{
		std::vector<glm::vec3> declWps;
		bool declLoop = false;
		if ( EditorImportDeclarativePatrol( (float)appearance()->position.x,
			(float)appearance()->position.y, declWps, declLoop ) )
		{
			wps = declWps;
			loop = declLoop;
			brainBehavior = BRAIN_PATROL;
		}
	}

	// Import the patrol for display only when there is no editor-authored / existing order.
	if ( !( orderAuthored || !waypoints.empty() || orderType != ORDER_NONE ) && !wps.empty() )
	{
		waypoints = wps;
		orderType = loop ? ORDER_PATROL : ORDER_MOVE;
		// orderAuthored stays false -> display only; not rewritten on save.
	}

	// MC2_EDITOR_TRACE: dump unit position vs first imported waypoint so any
	// coordinate-frame mismatch can be read off directly.
	if ( mc2::gate::get<"MC2_EDITOR_TRACE">() && !wps.empty() && appearance() )
	{
		fprintf( stderr, "[PATROL] brain=%s unitPos=(%.0f,%.0f) wp0=(%.0f,%.0f) wpN=%u loop=%d\n",
			brain.getBrainName(), appearance()->position.x, appearance()->position.y,
			wps[0].x, wps[0].y, (unsigned)wps.size(), loop ? 1 : 0 );
		fflush( stderr );
	}
}

void Unit::setSquad(unsigned long newSquad) {
	squad = newSquad;
	auto iter = alternatives.begin();
	while (iter != alternatives.end())
	{
		(*iter).setSquad(newSquad);
		iter++;
	}
}
