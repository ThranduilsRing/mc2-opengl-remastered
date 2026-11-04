#include "Gates/preferences_legacy.h"
//***************************************************************************
//
//	artlry.cpp -- File contains the Artillery Strike Object code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"mclib.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif


#ifndef TEAM_H
#include"team.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

//---------------------------------------------------------------------------

//extern float worldUnitsPerMeter;

//extern ObjectMapPtr GameObjectMap;

//long NumCameraDrones = 0;

constexpr auto ARTILLERYCHUNK_COMMANDERID_BITS = 3;
constexpr auto ARTILLERYCHUNK_STRIKETYPE_BITS = 3;
constexpr auto ARTILLERYCHUNK_CELLPOS_BITS = 10;
constexpr auto ARTILLERYCHUNK_SECONDS_BITS = 6;

constexpr auto ARTILLERYCHUNK_COMMANDERID_MASK = 0x00000007;
constexpr auto ARTILLERYCHUNK_STRIKETYPE_MASK = 0x00000007;
constexpr auto ARTILLERYCHUNK_CELLPOS_MASK = 0x000003FF;
constexpr auto ARTILLERYCHUNK_SECONDS_MASK = 0x0000003F;

extern MidLevelRenderer::MLRClipper * theClipper;

constexpr char AIRSTRIKE_NAME[] = "airstrikemarker";
constexpr char SENSOR_NAME[] = "sensormarker";

constexpr auto GV_LEFT_DUST_ID = 0;
constexpr auto GV_RIGHT_DUST_ID = 1;

extern bool MLRVertexLimitReached;
//***************************************************************************
// MISC
//***************************************************************************

#ifdef MC2_ALLOC_STATS
// Proof build only: pooled-round evidence for the reload fixture (printed with [ALLOC_TICK]).
unsigned g_mc2ArtCreated = 0, g_mc2ArtReinit = 0, g_mc2ArtReinitFreed = 0, g_mc2ArtEnded = 0, g_mc2ArtBoomNoFx = 0;
#endif

// MC2_SOAK_SUPPORT_EVERY (test-only): periodic airstrikes drive artillery init/retire, which defers appearance destruction
// into the tick graveyard, so the AUDIT=2 graveyard-drain-after-join proof sees real work. Off (0) = no effect.
void soakSupportTick(float scenarioTimeNow) {
	static const float every = mc2::gate::get<"MC2_SOAK_SUPPORT_EVERY">();
	if (every <= 0.0f || !Team::home) return;
	static float s_next = 0.0f;
	static long s_n = 0;
	if (scenarioTimeNow < s_next - every) { s_next = 0.0f; s_n = 0; }   // new mission
	if (scenarioTimeNow < s_next) return;
	s_next = scenarioTimeNow + every;
	MoverPtr mover = Team::home->getMover(0);
	if (!mover) return;
	glm::vec3 loc = mover->getPosition();
	loc.x += 200.0f + 40.0f * static_cast<float>(s_n % 5);
	loc.y += 200.0f;
	CallArtillery(mover->getCommanderId(), s_n % 3, loc, 4, false);
	++s_n;
}

void CallArtillery (long commanderID, long strikeType, glm::vec3 strikeLoc, long secondsToImpact, bool randomOff) 
{

	//-------------------------------------------------------------------
	// If this commander already has 10 strikes going and I'm the server,
	// then no more! If I'm a client, trust the server...
	long numArtillery = 0;
	for (long i = 0; i < ObjectManager->getNumArtillery(); i++) 
	{
		ArtilleryPtr artillery = ObjectManager->getArtillery(i);
		if (artillery && artillery->getExists() && (artillery->getCommanderId() == commanderID))
			numArtillery++;
	}

	if (numArtillery >= 10) 
	{
		if (MPlayer && MPlayer->isServer())
			return;
	}

  	ArtilleryPtr artilleryStrike = ObjectManager->createArtillery(strikeType,strikeLoc);
	if (artilleryStrike)
	{
#ifdef MC2_ALLOC_STATS
		++g_mc2ArtCreated;
#endif
		artilleryStrike->iFacePosition = strikeLoc;
		artilleryStrike->setPosition( strikeLoc );
		
		//-----------------------------------------------------------
		// alignment only really matters for sensor & camera strikes.
		artilleryStrike->setTeamId(Commander::commanders[commanderID]->getTeam() ? Commander::commanders[commanderID]->getTeam()->getId() : -1, true);
		artilleryStrike->setCommanderId(commanderID);
	
		//-----------------------------------------------------------
		// Set Time to Impact here.  Only if timeToImpact is != -1.0
		if (secondsToImpact != -1)
			artilleryStrike->info.strike.timeToImpact = static_cast<float>(secondsToImpact);
	
		// Of course, then our friends the designers set
		// secondsToImpact to 0.  Well Done.  No warning.  Looks like
		// a bug don't it?
		// MINIMUM time to impact is now four!!!!!
		if (secondsToImpact < 2)
			artilleryStrike->info.strike.timeToImpact = -1;	//FULL Duration.
	
		//------------------------------------
		// We must be server if we got here...
		if (MPlayer) {
			if ((strikeType == ARTILLERY_SMALL) || (strikeType == ARTILLERY_LARGE))
				MPlayer->numAirStrikesUsed[commanderID]++;
			else
				MPlayer->numSensorProbesUsed[commanderID]++;
			if (MPlayer->isServer())
				MPlayer->addArtilleryChunk(commanderID, strikeType, strikeLoc, secondsToImpact);
		}
	
		artilleryStrike->update(); // call this so if in pause mode, there is something to draw
	}
}

//---------------------------------------------------------------------------

void IfaceCallStrike (long strikeID,
					  glm::vec3* strikeLoc,
					  GameObjectPtr strikeTarget,
					  bool playerStrike,
					  bool clanStrike,
					  float timeToImpact) 
{

	if ((strikeID != ARTILLERY_LARGE) && (strikeID != ARTILLERY_SMALL) && (strikeID != ARTILLERY_SENSOR))
		return;

	if (!strikeLoc && !strikeTarget)
		return;

	glm::vec3 strikeLocation;
	if (strikeLoc)
		strikeLocation = *strikeLoc;
	else
		strikeLocation = strikeTarget->getPosition();

	bool bRandom = 0;

	long commanderID = -1;
	if (playerStrike) 
	{
		commanderID = Commander::home->getId();
		bRandom = Team::home->teamLineOfSight(*strikeLoc,0.0f) ? 0 : 1;
	}
	else if (clanStrike) 
	{
		if (MPlayer)
			Fatal(0, " Iface.CallStrike: Need more info than clanStrike in MPlayer ");
		//------------------------------------------------------------
		// In campaign game, clans can strike into unrevealed terrain.
		bRandom = Team::teams[1]->teamLineOfSight(*strikeLoc,0.0f) ? 0 : 1;
		commanderID = 1;
	}

	switch (strikeID) 
	{
		case ARTILLERY_LARGE:
		case ARTILLERY_SMALL:
			if (playerStrike && Team::home->teamLineOfSight(strikeLocation,0.0f))
			{
				soundSystem->playDigitalSample(MAPBUTTONS_GUI);
			}
			break;
	}

	long secondsToImpact = static_cast<long>(timeToImpact);

	if (strikeID == ARTILLERY_SMALL)
		strikeID = ARTILLERY_LARGE;

	if (strikeID == ARTILLERY_LARGE && bRandom)
		strikeID = ARTILLERY_SMALL;

	if (MPlayer) 
	{
		if (MPlayer->isServer())
		{
			CallArtillery(commanderID, strikeID, strikeLocation, secondsToImpact, bRandom);
		}
		else
		{
			MPlayer->sendPlayerArtillery(strikeID, strikeLocation, secondsToImpact);
		}
	}
	else
	{
		CallArtillery(commanderID, strikeID, strikeLocation, secondsToImpact, bRandom);
	}
}

//***************************************************************************
// ARTILLERY CHUNK
//***************************************************************************

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------

void ArtilleryChunk::build (long _commanderId,
							long _strikeType,
							glm::vec3 location,
							long _seconds) {

	commanderId = _commanderId;

	strikeType = _strikeType;

	land->worldToCell(location, cellRC[0], cellRC[1]);

	secondsToImpact = _seconds;

	data = 0;
}

//---------------------------------------------------------------------------

void ArtilleryChunk::pack (void) {

	data = 0;

	data |= (secondsToImpact + 1);

	data <<= ARTILLERYCHUNK_CELLPOS_BITS;
	data |= cellRC[0];

	data <<= ARTILLERYCHUNK_CELLPOS_BITS;
	data |= cellRC[1];

	data <<= ARTILLERYCHUNK_STRIKETYPE_BITS;
	data |= strikeType;

	data <<= ARTILLERYCHUNK_COMMANDERID_BITS;
	data |= commanderId;
}

//---------------------------------------------------------------------------
		
void ArtilleryChunk::unpack (void) {

	unsigned long tempData = data;

	commanderId = (tempData & ARTILLERYCHUNK_COMMANDERID_MASK);
	tempData >>= ARTILLERYCHUNK_COMMANDERID_BITS;

	strikeType = (tempData & ARTILLERYCHUNK_STRIKETYPE_MASK);
	tempData >>= ARTILLERYCHUNK_STRIKETYPE_BITS;

	cellRC[1] = (tempData & ARTILLERYCHUNK_CELLPOS_MASK);
	tempData >>= ARTILLERYCHUNK_CELLPOS_BITS;

	cellRC[0] = (tempData & ARTILLERYCHUNK_CELLPOS_MASK);
	tempData >>= ARTILLERYCHUNK_CELLPOS_BITS;

	secondsToImpact = (tempData & ARTILLERYCHUNK_SECONDS_MASK) - 1;
}

//---------------------------------------------------------------------------

bool ArtilleryChunk::equalTo (ArtilleryChunkPtr chunk) {

	if (commanderId != chunk->commanderId)
		return(false);

	if (strikeType != chunk->strikeType)
		return(false);

	if (cellRC[0] != chunk->cellRC[0])
		return(false);

	if (cellRC[1] != chunk->cellRC[1])
		return(false);

	if (secondsToImpact != chunk->secondsToImpact)
		return(false);

	return(true);
}

//***************************************************************************
// class ArtilleryType
//***************************************************************************

GameObjectPtr ArtilleryType::createInstance (void) {

	ArtilleryPtr newArtillery = new Artillery;
	if (!newArtillery)
		return(nullptr);

	newArtillery->init(true, this);

	return(newArtillery);
}

//---------------------------------------------------------------------------

void ArtilleryType::init (void) {

	ObjectType::init();
	objectTypeClass = ARTILLERY_TYPE;
	objectClass = ARTILLERY;
}

//---------------------------------------------------------------------------

void ArtilleryType::destroy (void) 
{
	if (explosionOffsetX) 
	{
		std::free(explosionOffsetX);
		explosionOffsetX = nullptr;
	}

	if (explosionOffsetY) 
	{
		std::free(explosionOffsetY);
		explosionOffsetY = nullptr;
	}

	if (explosionDelay) 
	{
		std::free(explosionDelay);
		explosionDelay = nullptr;
	}

	ObjectType::destroy();
}
		
//---------------------------------------------------------------------------

long ArtilleryType::init (FilePtr objFile, unsigned long fileSize) {

	long result = 0;
	
	FitIniFile miFile;
	result = miFile.open(objFile,fileSize);
	if (result != NO_ERR)
		return(result);

	//---------------------------------------------------------------
	// Load up the artillery data.
	result = miFile.seekBlock("Artillery");
	if (result != NO_ERR)
		return(result);
	
	char artillerySpriteName[80];	
	result = miFile.readIdString("ArtillerySpriteName",artillerySpriteName,79);
	if (result != NO_ERR)
		return(result);
		
	result = miFile.readIdULong("FrameCount",frameCount);
	if (result != NO_ERR)
		return(result);
		
	result = miFile.readIdULong("StartFrame",startFrame);
	if (result != NO_ERR)
		return(result);
		
	result = miFile.readIdFloat("FrameRate",frameRate);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalTimeToImpact",nominalTimeToImpact);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalTimeToLaunch",nominalTimeToLaunch);
	if (result != NO_ERR)
		nominalTimeToLaunch = nominalTimeToImpact - 10;

	result = miFile.readIdFloat("NominalDamage",nominalDamage);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalMajorRange",nominalMajorRange);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalMajorHits",nominalMajorHits);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalMinorRange",nominalMinorRange);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalMinorHits",nominalMinorHits);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalSensorTime",nominalSensorTime);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("NominalSensorRange",nominalSensorRange);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("fontScale",fontScale);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("fontXOffset",fontXOffset);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdFloat("fontYOffset",fontYOffset);
	if (result != NO_ERR)
		return(result);

	result = miFile.readIdULong("fontColor",fontColor);
	if (result != NO_ERR)
		return(result);
	
	if (nominalDamage)
	{
		result = miFile.readIdLong("NumExplosions",numExplosions);
		if (result != NO_ERR)
			return(result);
			
		explosionOffsetX = static_cast<float *>(std::malloc(sizeof(float)*numExplosions));
		gosASSERT(explosionOffsetX != nullptr);

		explosionOffsetY = static_cast<float *>(std::malloc(sizeof(float)*numExplosions));
		gosASSERT(explosionOffsetY != nullptr);

		explosionDelay   = static_cast<float *>(std::malloc(sizeof(float)*numExplosions));
		gosASSERT(explosionDelay != nullptr);

		for (long i=0;i<numExplosions;i++)
		{
			char explosionId[50];

			mc2::fmt::to_buf(explosionId,"ExplosionDelay{:d}",mc2::fmt::as_i32(i));
			result = miFile.readIdFloat(explosionId,explosionDelay[i]);
			if (result != NO_ERR)
				return(result);
				
			mc2::fmt::to_buf(explosionId,"ExplosionOffsetX{:d}",mc2::fmt::as_i32(i));
			result = miFile.readIdFloat(explosionId,explosionOffsetX[i]);
			if (result != NO_ERR)
				return(result);

			mc2::fmt::to_buf(explosionId,"ExplosionOffsetY{:d}",mc2::fmt::as_i32(i));
			result = miFile.readIdFloat(explosionId,explosionOffsetY[i]);
			if (result != NO_ERR)
				return(result);
		}

		result = miFile.readIdLong("ExplosionsPerExplosion",numExplosionsPerExplosion);
		if (result != NO_ERR)
			return(result);

		result = miFile.readIdLong("ExplosionRandomOffsetX",explosionRandomX);
		if (result != NO_ERR)
			return(result);
		
		result = miFile.readIdLong("ExplosionRandomOffsetY",explosionRandomY);
		if (result != NO_ERR)
			return(result);

		result = miFile.readIdLong("MinArtilleryHeadRange",minArtilleryHeadRange);
		if (result != NO_ERR)
			minArtilleryHeadRange = 5.0;

	}
	else
	{
		explosionOffsetX = explosionOffsetY = explosionDelay = nullptr;
	}

	//-------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&miFile);
	return(result);
}
	
//---------------------------------------------------------------------------
bool ArtilleryType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) 
{
	if (MPlayer && !MPlayer->isServer())
		return(false);

	//----------------------------------------------------------
	// Artillery counts as damage IF AND ONLY IF the artillery 
	// strike arrives during the collision time.  At all other
	// times, the artillery doesn't matter.
	ArtilleryPtr artillery = static_cast<ArtilleryPtr>(collidee);
	ArtilleryTypePtr artilleryType = static_cast<ArtilleryTypePtr>(artillery->getObjectType());
	if (artillery->getFlag(OBJECT_FLAG_BOOM)) 
	{
		//---------------------
		// Get Range to target.
		glm::vec3 distance = collider->getPosition();
		distance -= collidee->getPosition();
		distance.z = 0.0;			//Do NOT use elevation!!!  Will make it look like its not WORKING!!!!
		float range = glm::length(distance) * metersPerWorldUnit;
		if ((collider->getObjectClass() == GATE) || (collider->getObjectClass() == TURRET))
		{
			switch (collider->getObjectClass()) 
			{
				case GATE: 
				{
					//---------------------------------------------------
					// An explosion may NOT damage a gate or turret unless
					// is is within the "little" extent radius.
					float littleExtent = (static_cast<GatePtr>(collider))->getLittleExtent() * metersPerWorldUnit;
					if (littleExtent < range)
					{
						float realRange = range - littleExtent;
						if (realRange > (static_cast<ArtilleryTypePtr>(artillery->getObjectType()))->nominalMajorRange)
							return FALSE;
					}
				}
				break;
				
				case TURRET: 
				{
					//---------------------------------------------------
					// An explosion may NOT damage a gate or turret unless
					// is is within the "little" extent radius.
					float littleExtent = (static_cast<TurretPtr>(collider))->getLittleExtent() * metersPerWorldUnit;
					if (littleExtent < range) 
					{
						float realRange = range - littleExtent;
						if (realRange > artilleryType->nominalMajorRange)
							return(false);
					}
				}
				break;
			}
		}
		else if ((collider->getObjectClass() == BATTLEMECH) && 
				((static_cast<MoverPtr>(collider))->getMoveType() == MOVETYPE_AIR) &&
				(collider->getStatus() != OBJECT_STATUS_SHUTDOWN))
		{
			//DO Nothing.  Helicopters are immune.
			return false;
		}
		
		bool isMajorHit = (range <= artilleryType->nominalMajorRange);
		long numHits = artilleryType->nominalMajorHits; 
		if (!isMajorHit)
			numHits = artilleryType->nominalMinorHits;
			
		for (long i = 0; i < numHits; i++) 
		{
			WeaponShotInfo shot;
			shot.init(collidee->getWatchID(), -3, artilleryType->nominalDamage, 0, 0);
			if (collider->isMover()) 
			{
				if (range <= minArtilleryHeadRange)
					shot.hitLocation = collider->calcHitLocation(collidee, -1, ATTACKSOURCE_DFA, 0);
				else
					shot.hitLocation = collider->calcHitLocation(collidee, -1, ATTACKSOURCE_ARTILLERY, 0);
				shot.setEntryAngle(collider->relFacingTo(collidee->getPosition()));
			}
			collider->handleWeaponHit(&shot, (MPlayer != nullptr));
		}
	}
	
	return(false);
}

//---------------------------------------------------------------------------
bool ArtilleryType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) 
{
	//-------------------------------------------------------
	// Artillery goes away when it hits.  Nothing happens as
	// a result of a collision.
	return(false);
}

//***************************************************************************
// class Artillery 
//***************************************************************************
void Artillery::init (bool create) 
{
	// Pool reuse (create=false) arrives holding the previous round's appearance/bomber/effects; free them before
	// the resets below orphan them. destroy() is idempotent (nulls what it frees). The constructor (create=true) has nothing yet.
#ifdef MC2_ALLOC_STATS
	if (!create)
	{
		++g_mc2ArtReinit;
		if (appearance)
			++g_mc2ArtReinitFreed;
	}
#endif
	if (!create)
		destroy();

	presentationInitialized = presentationImpactStarted = false;
	artilleryType = ARTILLERY_SMALL;

	initFlags();
	setFlag(OBJECT_FLAG_JUSTCREATED, true);
				
	info.strike.timeToImpact = -1.0;
	info.strike.timeToLaunch = -1.0;
			
	info.strike.sensorRange = 0.0;
	info.strike.contactUpdate = scenarioTime;
	info.strike.sensorSystemIndex = -1;

	info.strike.timeToBlind = 0.0;
	
	hitEffect = nullptr;
	leftContrail = nullptr;
	rightContrail = nullptr;
			
	bomber = nullptr;
	// A fresh round owns no appearance yet; destroy() frees it (the heap that used to sweep it up is gone).
	adoptAppearance(nullptr);

	setFlag(OBJECT_FLAG_RANDOM_OFFSET, 0);
}

//---------------------------------------------------------------------------
void Artillery::init (bool create, long _artilleryType) 
{
	init(create);

	artilleryType = _artilleryType;
	if (isStrike()) 
	{
	}
	else if (isSensor()) 
	{
	}
}

//---------------------------------------------------------------------------
void Artillery::handleStaticCollision (void) 
{
	if (getFlag(OBJECT_FLAG_TANGIBLE)) 
	{
		//-----------------------------------------------------
		// What is our block and vertex number?
		int blockNumber = 0;
		int vertexNumber = 0;
		
		getBlockAndVertexNumber(blockNumber,vertexNumber);

		int CellRow, CellCol;
		land->worldToCell(getPosition(), CellRow, CellCol);
		
		//-------------------------------------------------------
		// MUST figure out radius to set off mines in CELLS now.
		// -fs
		int startCellRow = CellRow - 4;
		int startCellCol = CellCol - 4;
			
		int i=0;
		for (i = startCellRow; i < startCellRow + 9; i++) 
		{
			for (int j = startCellCol; j < startCellCol + 9; j++) 
			{
				if (GameMap->inBounds(i,j)) 
				{
					int mineResult = 0;
					mineResult = GameMap->getMine(i,j);
					
					if (mineResult == 1)
					{
						glm::vec3 minePosition;
						land->cellToWorld(i,j,minePosition);
						GameMap->setMine(i,j,2);
						if (MPlayer) 
						{
							MPlayer->addMineChunk(i,
												  j,
												  1,
												  2,
												  2);
						}
						ObjectManager->createExplosion(MINE_EXPLOSION_ID, nullptr, minePosition, MineSplashDamage, MineSplashRange * worldUnitsPerMeter);
					}
				}
			}
		}
		
		//-------------------------------------------------------------------------
		// We must now move out into other tiles for the artillery strike to work.
		// Remember, Its pretty big!
		// Just grab the nine vertices around this one.  Problems arise when on Block border.  Handle it.
		blockNumber = 0;
		vertexNumber = 0;
		getBlockAndVertexNumber(blockNumber,vertexNumber);
			
		//-------------------------------------------------------------------------
		// We must now move out into other tiles for the artillery strike to work.
		// Remember, Its pretty big!
		// Just grab the nine vertices around this one.  Problems arise when on Block border.  Handle it.
		int topLeftBlockNumber = blockNumber - Terrain::blocksMapSide - 1;
		int currentBlockNumber = topLeftBlockNumber;
		int totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

		for (i = 0; i < 3; i++) 
		{
			for (int j = 0; j < 3; j++) 
			{
				if ((currentBlockNumber >= 0) && (currentBlockNumber < totalBlocks))
				{
					int numObjectsInBlock = ObjectManager->getObjBlockNumObjects(currentBlockNumber);
					for (int objIndex = 0; objIndex < numObjectsInBlock; objIndex++) 
					{
						GameObjectPtr obj = ObjectManager->getObjBlockObject(currentBlockNumber, objIndex);
						if (!obj)
							STOP(("Object Number %d in terrain Block %d was NULL!",objIndex,currentBlockNumber));

						if (obj->getExists())
							ObjectManager->detectStaticCollision(this, obj);
					}
				}
				currentBlockNumber++;
			}
			currentBlockNumber = topLeftBlockNumber + (Terrain::blocksMapSide * (i + 1));
		}
	}
}	

//---------------------------------------------------------------------------
void Artillery::setJustCreated (void) 
{
	if (getFlag(OBJECT_FLAG_JUSTCREATED)) 
	{
		ArtilleryTypePtr type = static_cast<ArtilleryTypePtr>(getObjectType());
		setFlag(OBJECT_FLAG_JUSTCREATED, false);
		if (info.strike.timeToImpact == -1.0)
			info.strike.timeToImpact = type->nominalTimeToImpact;
			
		info.strike.timeToLaunch = type->nominalTimeToLaunch;
		info.strike.sensorRange = type->nominalSensorRange;
		info.strike.timeToBlind	= 0.0;
		setFlag(OBJECT_FLAG_TANGIBLE, false);
		setFlag(OBJECT_FLAG_SENSORS_GOING, false);

		if (info.strike.sensorRange)
		{
			int cellR, cellC;
			land->worldToCell(position,cellR, cellC);
			if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
				effectId--;
		}
		
		if (info.strike.sensorRange)
		{
			SensorSystemPtr sensor = SensorManager->newSensor();
			info.strike.sensorSystemIndex = sensor->id;
			setSensorData(getTeam());

		}
	}
}

//---------------------------------------------------------------------------
long Artillery::update(void)
{
	if (getFlag(OBJECT_FLAG_JUSTCREATED)) setJustCreated();
	else { info.strike.timeToImpact -= frameLength; info.strike.timeToLaunch -= frameLength; }
	auto* type = static_cast<ArtilleryTypePtr>(getObjectType());
	if (type->nominalDamage > 0.0f) {
		if (getFlag(OBJECT_FLAG_BOOM)) {
			setFlag(OBJECT_FLAG_TANGIBLE, false);
			return false;
		}
		if (info.strike.timeToImpact <= 0.0f) {
			setFlag(OBJECT_FLAG_BOOM, true);
			setFlag(OBJECT_FLAG_TANGIBLE, true);
		}
	}
	if (info.strike.timeToImpact <= 5.5f && !getFlag(OBJECT_FLAG_WHOOSH)) {
		setFlag(OBJECT_FLAG_WHOOSH, true);
		if (soundSystem && type->nominalDamage > 0.0f) soundSystem->playDigitalSample(INCOMING_AIRSTRIKE, position, true);
	}
	//----------------------------------------------------------------------------
	// Sensor round is ticking now.  Update Everything.
	if ((info.strike.timeToBlind > 0.0) && getFlag(OBJECT_FLAG_SENSORS_GOING))
	{
		info.strike.timeToBlind -= frameLength;
		info.strike.sensorRange = info.strike.timeToBlind / type->nominalSensorTime;
		info.strike.sensorRange *= type->nominalSensorRange;
		info.strike.sensorRange *= worldUnitsPerMeter;
		SensorSystemPtr sensor = SensorManager->getSensor(info.strike.sensorSystemIndex);
		Assert(sensor != nullptr, 0, " Artillery.update: NULL sensor ");
		sensor->setRange(info.strike.sensorRange * metersPerWorldUnit);

		//--------------------------------------
		// Actual scan is done in team update...
	}
	else if ((info.strike.timeToBlind <= 0.0) && getFlag(OBJECT_FLAG_SENSORS_GOING))
	{
		//---------------------------------------------------
		// All done, return FALSE;
		SensorSystemPtr sensor = SensorManager->getSensor(info.strike.sensorSystemIndex);
		SensorManager->removeTeamSensor(getTeam()->getId(), sensor);
		SensorManager->freeSensor(sensor);
		return(0);
	}

	//-------------------------------------------------------------------------------------------
	// If we are a sensor round, start the sensor countdown.  ALWAYS on, even before effect hits.
	// time to blind is being reset everytime without this else
	else if (type->nominalSensorTime > 0)
	{
		info.strike.timeToBlind = type->nominalSensorTime;
		setFlag(OBJECT_FLAG_SENSORS_GOING, true);
	}

	return true;
}

void Artillery::updatePresentation()
{
	if (getFlag(OBJECT_FLAG_JUSTCREATED)) return;
	auto* type = static_cast<ArtilleryTypePtr>(getObjectType());
	recalcBounds(eye);
	if (inView) windowsVisible = turn;
	if (!presentationInitialized) {
		presentationInitialized = true;
		appearance->presentAnimation(*appearance->simulationAnimation());
		if (bomber) bomber->presentAnimation(*bomber->simulationAnimation());
		//Create the GOSFX here.
		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				hitEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(hitEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
		if (strcmp(weaponEffects->GetEffectName(JET_CONTRAIL_ID),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(JET_CONTRAIL_ID));
			
			if (gosEffectSpec)
			{
				//leftContrail = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				//gosASSERT(leftContrail != NULL);
			
				rightContrail = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(rightContrail != nullptr);
				
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
	}
	if ((info.strike.timeToImpact > 0.0f) || (type->nominalDamage == 0.0))
	{
		bool oldShadows = useShadows;
		useShadows = false;
		appearance->setObjectParameters(iFacePosition,(static_cast<ObjectAppearance*>(appearance))->rotation,false,getTeamId(),Team::getRelation(getTeamId(), Team::home->getId()));
		appearance->recalcBounds();
		appearance->update();
		useShadows = oldShadows;
	}
	
	if (bomber && (info.strike.timeToImpact <= 2.5f) && !bombRunStarted)
	{
		bomber->setObjectParameters(position,0.0f,false,getTeamId(),Team::getRelation(getTeamId(), Team::home->getId()));
		bomber->setMoverParameters(0.0f);
		bomber->setGesture(0);
		bomber->setObjStatus(OBJECT_STATUS_DESTROYED);
		bomber->recalcBounds();
		bomber->setVisibility(true,true);
		bomber->update();		   //Force it to try and draw or stuff will not work!
		bombRunStarted = true;
		
		if (rightContrail && rightContrail->IsExecuted())
		{
			glm::mat4 	shapeOrigin;
			
			glm::vec3 lPosition = bomber->getNodeNamePosition("activity_node");
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
			
 			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
			rightContrail->Start(&info);
		}
		
		if (leftContrail && leftContrail->IsExecuted())
		{
			glm::mat4 	shapeOrigin;
			
			glm::vec3 lPosition = bomber->getDustNodePosition(GV_LEFT_DUST_ID);
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
 
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
			leftContrail->Start(&info);
		}
	}
	
	if (bomber && bombRunStarted)
	{
		bomber->setObjectParameters(position,0.0f,false,getTeamId(),Team::getRelation(getTeamId(), Team::home->getId()));
		bomber->recalcBounds();
		bomber->setVisibility(true,true);
		bomber->update();
		
		if (bomber && leftContrail && leftContrail->IsExecuted())
		{
			glm::vec3 lPosition = bomber->getDustNodePosition(GV_LEFT_DUST_ID);
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
		
			glm::mat4 	shapeOrigin;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			leftContrail->Execute(&info);
		}
		
		if (bomber && rightContrail && rightContrail->IsExecuted())
		{
			glm::vec3 lPosition = bomber->getNodeNamePosition("activity_node");
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
 		
			glm::mat4 	shapeOrigin;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			rightContrail->Execute(&info);
		}
	}
	
	if ((getFlag(OBJECT_FLAG_BOOM) || type->nominalDamage == 0.0f) && hitEffect) {
		glm::vec3 at; mc2ToRender(at, position);
		glm::mat4 origin(1.0f); mc2::math::buildTranslation(origin, at);
		mc2::math::Obb bounds;
		gosFX::Effect::ExecuteInfo fx((Stuff::Time)scenarioTime, &origin, &bounds);
		if (!presentationImpactStarted) { presentationImpactStarted = true; hitEffect->Start(&fx); }
		if (!hitEffect->Execute(&fx)) {
			hitEffect->Kill(); hitEffect.reset();
			bomber.reset(); leftContrail.reset(); rightContrail.reset();
		}
	}
}

//---------------------------------------------------------------------------
bool Artillery::recalcBounds (CameraPtr myEye)
{
	if (myEye)
	{
		//--------------------------------------------------
		// First, if we are using perspective, figure out
		// if object too far from camera.  Far Clip Plane.
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 objPosition(position);
			glm::vec3 eyePosition(eye->getPosition());
	
			Distance = objPosition - eyePosition;
			float eyeDistance = glm::length(Distance);
			if (eyeDistance > Camera::MaxClipDistance)
			{
				inView = false;
			}
			else
			{
				inView = true;
			}
			
			//-----------------------------------------------------------------
			// If inside farClip plane, check if behind camera.
			// Find angle between lookVector of Camera and vector from camPos
			// to Target.  If angle is less then halfFOV, object is visible.
			if (inView)
			{
				glm::vec3 Distance;
				glm::vec3 objPosition;
				glm::vec3 eyePosition(eye->getCameraOrigin());
				mc2ToRender(objPosition, position);
		
				Distance = objPosition - eyePosition;
				Distance = glm::normalize(Distance);
				
				float cosine = glm::dot(Distance, eye->getLookVector());
 				if (cosine > eye->cosHalfFOV)
					inView = true;
				else
					inView = false;
			}
		}
		else
		{
			inView = true;
		}
		
		if (inView)
		{
			// [PROJECTZ:ScreenXYOracle id=artlry_position_screen]
			eye->projectForScreenXY(position,screenPos);
			Stuff::Vector4D iFaceScreen;
			// [PROJECTZ:ScreenXYOracle id=artlry_iface_screen]
			eye->projectForScreenXY(iFacePosition, iFaceScreen );
	
			if (((screenPos.x >= 0) && (screenPos.y >= 0) &&
				(screenPos.x <= eye->getScreenResX()) &&
				(screenPos.y <= eye->getScreenResY())) 
				|| (((iFaceScreen.x >= 0) && (iFaceScreen.y >= 0) &&
				(iFaceScreen.x <= eye->getScreenResX()) &&
				(iFaceScreen.y <= eye->getScreenResY()))))
			{
				inView = true;
			}
			else
			{
				inView = false;
			}
		}
	}
	
	return (inView);
}

//---------------------------------------------------------------------------
TeamPtr Artillery::getTeam (void) 
{

	if (teamId == -1)
		return(nullptr);
	return(Team::teams[teamId]);
}

//---------------------------------------------------------------------------
void Artillery::setCommanderId (long _commanderId) 
{
	commanderId = _commanderId;
}

//---------------------------------------------------------------------------
long Artillery::setTeamId (long _teamId, bool setup) 
{

	teamId = _teamId;
	Assert(teamId > -1, teamId, " Mover.setTeamId: bad teamId ");

	return(NO_ERR);
}

//---------------------------------------------------------------------------

bool Artillery::isFriendly (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_FRIENDLY);
	return(false);
}

//---------------------------------------------------------------------------

bool Artillery::isEnemy (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_ENEMY);
	return(false);
}

//---------------------------------------------------------------------------

bool Artillery::isNeutral (TeamPtr team) {

	if (teamId > -1)
		return(Team::relations[teamId][team->getId()] == RELATION_NEUTRAL);
	return(true);
}

//---------------------------------------------------------------------------
void Artillery::setSensorRange (float range) 
{
	SensorSystemPtr sensor = SensorManager->getSensor(info.strike.sensorSystemIndex);
	if (sensor)
		sensor->setRange(range);
}

//---------------------------------------------------------------------------
void Artillery::setSensorData (TeamPtr team, float sensorTime, float range) 
{
	if (sensorTime != -1.0)
		info.strike.timeToBlind = sensorTime;
	
	if (range != -1.0)	
		info.strike.sensorRange = range;

	SensorSystemPtr sensor = SensorManager->getSensor(info.strike.sensorSystemIndex);
	Assert(sensor != nullptr, info.strike.sensorSystemIndex, " Artillery.setSensorData: NULL sensor ");
	sensor->setOwner(this);
	SensorManager->addTeamSensor(team->getId(), sensor);
	sensor->setRange(range);
	sensor->setLOSCapability(false);
}

//---------------------------------------------------------------------------
void Artillery::drawSelectBox (unsigned char color) 
{
}
	
//---------------------------------------------------------------------------
// G7b K3: the old render() split at the render extract. The strike timer, its screen anchor, the impact position and the
// windowsVisible stamp are taken here; the draw (hit effect, timer text, appearance, bomber) reads the view.
void Artillery::extractRenderView (mc2::render_view::RenderView& view)
{
	GameObject::extractRenderView(view);
	if (!inView)
		return;
	view.flags |= mc2::render_view::Visible;
	if ( (!MPlayer && (info.strike.timeToImpact > 0.f)) ||
		 (MPlayer &&  info.strike.timeToImpact > 0.f
		 && ((teamId == Team::home->getId()) || info.strike.timeToImpact < 3.0f)))
	{
		view.flags |= mc2::render_view::Timer;
		view.value[0] = info.strike.timeToImpact;
		view.aux[0] = iFacePosition;
		if (appearance->canBeSeen())
		{
			view.flags |= mc2::render_view::SeenAtExtract;
			windowsVisible = turn;
		}
	}
}

void Artillery::renderView (const mc2::render_view::RenderView& view)
{
	if (view.has(mc2::render_view::Visible))
	{
		if (hitEffect )
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			//mlrState.SetBackFaceOn();
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			//mlrState.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
			//mlrState.SetAlphaMode(MidLevelRenderer::MLRState::AlphaInvAlphaMode);
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, view.position);

			glm::mat4 	shapeOrigin;

			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);

			drawInfo.m_parentToWorld = &shapeOrigin;

			if (!MLRVertexLimitReached)
				hitEffect->Draw(&drawInfo);
		}
		if (view.has(mc2::render_view::Timer))
		{
			// We are drawing the timer.
			// Draw it.
			char text[256];

			switch (artilleryType)
			{
				case ARTILLERY_LARGE:
					mc2::fmt::to_buf(text,"{:02.2f}",view.value[0]);
				break;

				case ARTILLERY_SMALL:
					mc2::fmt::to_buf(text,"{:02.2f}",view.value[0]);
				break;
				
				case ARTILLERY_SENSOR:
					text[0] = 0; // players are confused by sensor count down
				break;
			}
			
			DWORD width, height;
			Stuff::Vector4D moveHere;
			// [PROJECTZ:ScreenXYOracle id=artlry_iface_float_help]
			glm::vec3 iFace = view.aux[0];
			eye->projectForScreenXY( iFace, moveHere );

			gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, false, false, false);
			gos_TextStringLength(&width,&height,text);

			moveHere.y += 10.0f;
			moveHere.x -= width / 2;
			moveHere.z = width;
			moveHere.w = height;
		
			globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,0x0,gosFontScale,false,false,false,false);
		
			if (view.has(mc2::render_view::SeenAtExtract))
			{
				appearance->setVisibility(true,true);
				appearance->render(-1);
			}
		}
	}
	
	if (bomber && bombRunStarted)
	{
		bomber->setVisibility(true,true);
		bomber->render();
		bomber->renderShadows();
	
		gosFX::Effect::DrawInfo drawInfo;
		drawInfo.m_clipper = theClipper;
		
		MidLevelRenderer::MLRState mlrState;
		//mlrState.SetBackFaceOn();
		mlrState.SetDitherOn();
		mlrState.SetTextureCorrectionOn();
		mlrState.SetZBufferCompareOn();
		mlrState.SetZBufferWriteOn();

		//mlrState.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);
		//mlrState.SetAlphaMode(MidLevelRenderer::MLRState::AlphaInvAlphaMode);

		drawInfo.m_state = mlrState;
		drawInfo.m_clippingFlags = 0x0;
			
 		if (leftContrail)
		{
			glm::vec3 lPosition = bomber->getDustNodePosition(GV_LEFT_DUST_ID);
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
 			
			glm::mat4 	shapeOrigin;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			if (!MLRVertexLimitReached)
				leftContrail->Draw(&drawInfo);
		}
		
		if (rightContrail)
		{
			glm::vec3 lPosition = bomber->getNodeNamePosition("activity_node");
			
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, lPosition);
 			
			glm::mat4 	shapeOrigin;
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
	 
			if (!MLRVertexLimitReached)
				rightContrail->Draw(&drawInfo);
		}
	}
}

//---------------------------------------------------------------------------
long Artillery::handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk) 
{
	return(NO_ERR);
}

//---------------------------------------------------------------------------
void Artillery::destroy (void) 
{
	//Must delete here too in case effect was NOT over when mission ended!
	if (hitEffect)
	{
		hitEffect->Kill();
		hitEffect.reset();
	}
				
	if (leftContrail)
	{
		leftContrail->Kill();
		leftContrail.reset();
	}
	
	if (rightContrail)
	{
		rightContrail->Kill();
		rightContrail.reset();
	}

	bomber.reset();

	// The round owns its strike appearance; the per-mission appearance heap no longer reclaims it in bulk.
	if (appearance)
	{
		releaseAppearance();
	}
}

//---------------------------------------------------------------------------
void Artillery::init (bool create, ObjectTypePtr _type)
{
	// A re-init must not orphan what the previous init built (appearance, bomber, effects).
	destroy();

	GameObject::init(create, _type);

	initFlags();
	
	switch (_type->getObjectTypeClass()) 
	{
		case ARTILLERY_TYPE:
			objectClass = ARTILLERY;
			setFlag(OBJECT_FLAG_EXISTS, true);
			setFlag(OBJECT_FLAG_JUSTCREATED, true);
			setFlag(OBJECT_FLAG_BOOM, false);
			info.strike.timeToImpact = -1.0;
			info.strike.explosionOffFlags = 0;
			break;
	}

	effectId = -1;
	switch (_type->whatAmI())
	{
		case BIG_ARTLRY:
		{
			effectId = LARGE_AIRSTRIKE_ID;
			init(ARTILLERY_LARGE);
			
			//--------------------------------------------
			//Load up the strike appearance before hit.
			char appearName[1024];
			strcpy(appearName,AIRSTRIKE_NAME);
		
			//--------------------------------------------------------------
			// New code!!!
			// We need to append the sprite type to the appearance num now.
			// The MechEdit tool does not assume a sprite type, nor should it.
			// MechCmdr2 features much simpler objects which only use 1 type of sprite!
			long appearanceType = (BLDG_TYPE << 24);
		
			AppearanceTypePtr buildingAppearanceType = nullptr;
			if (!appearName[0])
			{
				//------------------------------------------------------
				// LOAD a dummy appearance until real ones are available
				// for this building!
				appearanceType = (BLDG_TYPE << 24);
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TESTBLDG");
			}
			else
			{
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearName);
				if (!buildingAppearanceType)
				{
					char msg[1024];
					mc2::fmt::to_buf(msg,"No Building Appearance Named {}",mc2::fmt::safe(appearName));
					Fatal(0,msg);
				}
			}
			  
			adoptAppearance(std::make_unique_for_overwrite<BldgAppearance>());
			gosASSERT(appearance != nullptr);
		
			//--------------------------------------------------------------
			// The only appearance type for buildings is MLR_APPEARANCE.
			gosASSERT(buildingAppearanceType->getAppearanceClass() == BLDG_TYPE);
			
			appearance->init(static_cast<BldgAppearanceType*>(buildingAppearanceType), static_cast<GameObjectPtr>(this));
			
			//--------------------------------------------------------------
			appearanceType = (GV_TYPE << 24);
		
			AppearanceTypePtr bomberAppearanceType = appearanceTypeList->getAppearance(appearanceType,"Shilone");
			if (!bomberAppearanceType)
			{
				STOP(("Unable to create Bomber for air strike Shilone"));
			}
		
			if ((appearanceType>>24) == GV_TYPE)
			{
				bomber = std::make_unique<GVAppearance>();
				if (!bomber)
					STOP((" Artillery.init: unable to create appearance for Shilone"));
		
				//-----------------------------------------------------------------
				bomber->init(static_cast<GVAppearanceType*>(bomberAppearanceType), static_cast<GameObjectPtr>(this));
			}
			
			bombRunStarted = false;
			presentationInitialized = presentationImpactStarted = false;
		}
		break;
		
		case SMALL_ARTLRY:
		{
			effectId = SMALL_AIRSTRIKE_ID;
			init(ARTILLERY_SMALL);
			//--------------------------------------------
			//Load up the strike appearance before hit.
			char appearName[1024];
			strcpy(appearName,AIRSTRIKE_NAME);
		
			//--------------------------------------------------------------
			// New code!!!
			// We need to append the sprite type to the appearance num now.
			// The MechEdit tool does not assume a sprite type, nor should it.
			// MechCmdr2 features much simpler objects which only use 1 type of sprite!
			long appearanceType = (BLDG_TYPE << 24);
		
			AppearanceTypePtr buildingAppearanceType = nullptr;
			if (!appearName[0])
			{
				//------------------------------------------------------
				// LOAD a dummy appearance until real ones are available
				// for this building!
				appearanceType = (BLDG_TYPE << 24);
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TESTBLDG");
			}
			else
			{
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearName);
				if (!buildingAppearanceType)
				{
					char msg[1024];
					mc2::fmt::to_buf(msg,"No Building Appearance Named {}",mc2::fmt::safe(appearName));
					Fatal(0,msg);
				}
			}
			  
			adoptAppearance(std::make_unique_for_overwrite<BldgAppearance>());
			gosASSERT(appearance != nullptr);
		
			//--------------------------------------------------------------
			// The only appearance type for buildings is MLR_APPEARANCE.
			gosASSERT(buildingAppearanceType->getAppearanceClass() == BLDG_TYPE);
			
			appearance->init(static_cast<BldgAppearanceType*>(buildingAppearanceType), static_cast<GameObjectPtr>(this));
			//--------------------------------------------------------------
			appearanceType = (GV_TYPE << 24);
		
			AppearanceTypePtr bomberAppearanceType = appearanceTypeList->getAppearance(appearanceType,"shilone");
			if (!bomberAppearanceType)
			{
				STOP(("Unable to create Bomber for air strike Shilone"));
			}
		
			if ((appearanceType>>24) == GV_TYPE)
			{
				bomber = std::make_unique<GVAppearance>();
				if (!bomber)
					STOP((" Artillery.init: unable to create appearance for Shilone"));
		
				//-----------------------------------------------------------------
				bomber->init(static_cast<GVAppearanceType*>(bomberAppearanceType), static_cast<GameObjectPtr>(this));
			}
			
			bombRunStarted = false;
			presentationInitialized = presentationImpactStarted = false;
		}
		break;
		
		case SENSOR_ARTLRY:
		{
			effectId = SENSOR_AIRSTRIKE_ID;
			init(create, ARTILLERY_SENSOR);
			//--------------------------------------------
			//Load up the strike appearance before hit.
			
			char appearName[1024];
			strcpy(appearName,SENSOR_NAME);
		
			//--------------------------------------------------------------
			// New code!!!
			// We need to append the sprite type to the appearance num now.
			// The MechEdit tool does not assume a sprite type, nor should it.
			// MechCmdr2 features much simpler objects which only use 1 type of sprite!
			long appearanceType = (BLDG_TYPE << 24);
		
			AppearanceTypePtr buildingAppearanceType = nullptr;
			if (!appearName[0])
			{
				//------------------------------------------------------
				// LOAD a dummy appearance until real ones are available
				// for this building!
				appearanceType = (BLDG_TYPE << 24);
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,"TESTBLDG");
			}
			else
			{
				buildingAppearanceType = appearanceTypeList->getAppearance(appearanceType,appearName);
				if (!buildingAppearanceType)
				{
					char msg[1024];
					mc2::fmt::to_buf(msg,"No Building Appearance Named {}",mc2::fmt::safe(appearName));
					Fatal(0,msg);
				}
			}
			  
			adoptAppearance(std::make_unique_for_overwrite<BldgAppearance>());
			gosASSERT(appearance != nullptr);
		
			//--------------------------------------------------------------
			// The only appearance type for buildings is MLR_APPEARANCE.
			gosASSERT(buildingAppearanceType->getAppearanceClass() == BLDG_TYPE);
			
			appearance->init(static_cast<BldgAppearanceType*>(buildingAppearanceType), static_cast<GameObjectPtr>(this));
		}
		break;
	}
}	

//---------------------------------------------------------------------------
#include "save_struct_zero.h"
#include "artillery_value_codec.h"
void Artillery::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(ArtilleryData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	auto bytes = mc2::save::encodeTaggedRecord(mc2::save::ArtilleryRecord{data}, mc2::save::artilleryTag);
	if (bytes.empty()) Fatal(0, "Invalid artillery ObjectSlot");
	file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_ZLIB);
}

//---------------------------------------------------------------------------
void Artillery::CopyTo (ArtilleryData *data)
{
	data->artilleryType = artilleryType;
	data->teamId = teamId;
	data->commanderId = commanderId;
	data->info = info;
	data->effectId = effectId;

	data->bombRunStarted = bombRunStarted;
	data->inView = inView;
	data->iFacePosition = iFacePosition;

	GameObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void Artillery::Load (ArtilleryData *data)
{
	GameObject::Load(data);

	artilleryType = data->artilleryType;
	commanderId = data->commanderId;
	teamId = data->teamId;
	info = data->info;
	effectId = data->effectId;

	bombRunStarted = data->bombRunStarted;
	inView = data->inView;
	iFacePosition = data->iFacePosition;

	// if we're not a true artillery round, we're a sensor probe.
	// This checks to see if we've hit yet.
	// If not, create the impact effect and move on.
	if ((data->info.strike.sensorRange != 0.0f) && (data->info.strike.timeToImpact <= 0.0) && !getFlag(OBJECT_FLAG_SENSORS_GOING))	
	{
		int cellR, cellC;
		land->worldToCell(position,cellR, cellC);
		if (GameMap->getDeepWater(cellR, cellC) || GameMap->getShallowWater(cellR, cellC))
			effectId--;
		
		//Create the GOSFX here.
		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				hitEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(hitEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}

		if (hitEffect)
		{ 
			glm::mat4 	shapeOrigin;

			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, position);

			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);

			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
			hitEffect->Start(&info);
		} 
	}

	if (!getFlag(OBJECT_FLAG_BOOM) && (data->info.strike.timeToImpact > 0.0) && (data->info.strike.sensorRange == 0.0f)) 
	{
		//Create the GOSFX here.
		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag;
	
			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				hitEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(hitEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}

	if (data->info.strike.sensorSystemIndex != -1)
	{
		SensorSystemPtr sensor = SensorManager->newSensor();
		info.strike.sensorSystemIndex = sensor->id;
		setSensorData(getTeam());
	}
}

//---------------------------------------------------------------------------
