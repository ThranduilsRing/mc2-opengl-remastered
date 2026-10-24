// GameObjectManager: mover lists, object types, part ids, collision lists, captures, effects spawn and appearance-only update.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "objmgr_internal.h"
#include "sim_world_trace.h"
#include "impact_presentation.h"


//---------------------------------------------------------------------------

long GameObjectManager::buildMoverLists (void) {

	numMovers = 0;
	numGoodMovers = 0;
	numBadMovers = 0;

	for (long i = 0; i < numMechs; i++) {
		MoverPtr mover = dynamic_cast<MoverPtr>(mechs[i]);
		if (!mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	for (int i = 0; i < numVehicles; i++) {
		MoverPtr mover = dynamic_cast<MoverPtr>(vehicles[i]);
		if (!mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

bool GameObjectManager::modifyMoverLists (MoverPtr mover, long action) {
	MC2_WORLD_EVENT("mover_list", mover, action);

	switch (action) {
		case MOVERLIST_DELETE: {
			bool foundIt = false;
			if (mover->getObjectClass() == BATTLEMECH) {
				long i = 0;
				for (i = 0; i < numMechs; i++) {
					if (mechs[i] == mover)
						break;
				}
				if (i < numMechs) {
					foundIt = true;
					BattleMechPtr mech = mechs[i];
					memmove(&mechs[i], &mechs[i + 1], (maxMechs - i - 1) * sizeof(BattleMechPtr));
					mechs[maxMechs - 1] = mech;
					numMechs--;
				}
				}
			else if (mover->getObjectClass() == GROUNDVEHICLE) {
				long i = 0;
				for (i = 0; i < numVehicles; i++) {
					if (vehicles[i] == mover)
						break;
				}
				if (i < numVehicles) {
					foundIt = true;
					GroundVehiclePtr vehicle = vehicles[i];
					memmove(&vehicles[i], &vehicles[i + 1], (maxVehicles - i - 1) * sizeof(GroundVehiclePtr));
					vehicles[maxVehicles - 1] = vehicle;
					numVehicles--;
				}
			}

			for (long i = 0; i < numMovers; i++)
				if (moverList[i] == mover) {
					moverList[i] = moverList[--numMovers];
					break;
				}

			if (foundIt && mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home)) {
					long i = 0;
					for (i = 0; i < numGoodMovers; i++)
						if (mover == goodMoverList[i])
							break;
					if (i < numGoodMovers)
						goodMoverList[i] = goodMoverList[--numGoodMovers];
					}
				else if (mover->getTeam()->isEnemy(Team::home)) {
					long i = 0;
					for (i = 0; i < numBadMovers; i++)
						if (mover == badMoverList[i])
							break;
					if (i < numBadMovers)
						badMoverList[i] = badMoverList[--numBadMovers];
				}
			}
			return(foundIt);
		}
		case MOVERLIST_ADD:
			moverList[numMovers++] = mover;
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
			break;
		case MOVERLIST_TRADE:
			//-----------------------------------------------
			// First, remove it from whatever list it's on...
			long i = 0;
			for (i = 0; i < numGoodMovers; i++)
				if (mover == goodMoverList[i])
					break;
			if (i < numGoodMovers)
				goodMoverList[i] = goodMoverList[--numGoodMovers];
			else {
				long i = 0;
				for (i = 0; i < numBadMovers; i++)
					if (mover == badMoverList[i])
						break;
				if (i < numBadMovers)
					badMoverList[i] = badMoverList[--numBadMovers];
			}
			//----------------------------------
			// Now, add it to the proper list...
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
	}
	return(true);
}

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::loadObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->load(typeHandle));
}

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::getObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->get(typeHandle));
}

//---------------------------------------------------------------------------

void GameObjectManager::removeObjectType (ObjectTypeNumber typeHandle) {

	objTypeManager->remove(typeHandle);
}

//---------------------------------------------------------------------------

GameObjectHandle GameObjectManager::getHandle (GameObjectPtr obj) {

	for (long i = 1; i <= getMaxObjects(); i++)
		if (objList[i] == obj)
			return(i);
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::calcPartId (long objectClass, long param1, long param2, long param3) {

	//-------------------------------------------------------------------
	// This should be the only function used to calc partIds for objects.
	// This will allow easy modification of the formulas without making
	// changes everywhere...
	long partId = 0;
	switch (objectClass) {
		case MOVER:
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
			if (param1 == -1) {
				if (nextReinforcementPartId == (MAX_REINFORCEMENT_PART_ID + 1))
					return(0);
				partId = nextReinforcementPartId++;
				}
			else
				partId = MIN_MOVER_PART_ID +
						 param1 * MAX_MOVERGROUPS * MAX_MOVERGROUP_COUNT_START +
						 param2 * MAX_MOVERGROUP_COUNT_START +
						 param3;
			break;
		case TERRAINOBJECT:
		case TREE:
		case BUILDING:
		case TREEBUILDING:
		case TURRET:
		case GATE:
			partId = MIN_TERRAIN_PART_ID + param1 * gTerrainPartIdStride + param2;
			break;
		case TRAINCAR:
			Fatal(0, " TRAINS NEED PART IDS ");
			break;
	}
	return(partId);
}

//---------------------------------------------------------------------------

void GameObjectManager::setPartId (GameObjectPtr obj, long param1, long param2, long param3) {

	//------------------------------------------------------------------------
	// ALL game objects should have their partIds set through this function.
	// This simply allows easier modification and monitoring of the partIds,
	// should we have any reason to modify or test 'em as missions are loaded.
	long partId = calcPartId(obj->getObjectClass(), param1, param2, param3);
	obj->setPartId(partId);
}

//---------------------------------------------------------------------------

long GameObjectManager::initCollisionSystem (FitIniFile* missionFile) {

	collisionSystem = new CollisionSystem;
	gosASSERT(collisionSystem != nullptr);
		
	collisionSystem->init(missionFile);

	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::buildCollidableList (void) 
{
	rebuildCollidableList = false;
	
	// First, how many collidables are there?
	numCollidables = numMechs + numVehicles + numElementals + numTurrets + numGates + numCarnage + numArtillery;

	// B7o.8b: the previous list is released by the re-bind.
	collidableList = bindPoolArray(collidableStore, numCollidables);

	long curIndex = 0;
	for (long i = 0; i < numMechs; i++)
		collidableList[curIndex++] = mechs[i];

	for (int i = 0; i < numVehicles; i++)
		collidableList[curIndex++] = vehicles[i];

	for (int i = 0; i < numElementals; i++)
		collidableList[curIndex++] = reinterpret_cast<GameObjectPtr>(elementals[i]);

	for (int i=0;i<numTurrets;i++)
		collidableList[curIndex++] = turrets[i];

	for (int i=0;i<numGates;i++)
		collidableList[curIndex++] = gates[i];

	for (int i=0;i<numCarnage;i++)
		collidableList[curIndex++] = carnage[i];
		
	for (int i=0;i<numArtillery;i++)
		collidableList[curIndex++] = artillery[i];

	Assert(curIndex == numCollidables, curIndex, " GameObjectManager.buildCollidableList: oof ");
	
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::getCollidableList (GameObjectPtr*& objList) 
{
	if (rebuildCollidableList)
		buildCollidableList();
		
	objList = collidableList;
	return(numCollidables);
}

//---------------------------------------------------------------------------

long GameObjectManager::updateCollisions (void) {

	if (!collidableList)
		buildCollidableList();

	collisionSystem->checkObjects();

	return(0);
}

//---------------------------------------------------------------------------

void GameObjectManager::detectStaticCollision (GameObjectPtr obj1, GameObjectPtr obj2) {

	collisionSystem->detectStaticCollision(obj1, obj2);
}

//---------------------------------------------------------------------------

void GameObjectManager::updateCaptureList (void) {

	for (int i = 0; i < MAX_TEAMS; i++)
		numCaptures[i] = 0;
	for (int i = 0; i < getNumMovers(); i++) {
		MoverPtr mover = getMover(i);
		if (mover->isDisabled())
			continue;
		MechWarriorPtr pilot = mover->getPilot();
		if (pilot) {
			TacticalOrderPtr tacOrder;
			tacOrder = pilot->getCurTacOrder();
			if (tacOrder->code == TACTICAL_ORDER_CAPTURE) {
				captureList[mover->getTeamId()][numCaptures[mover->getTeamId()]++] = tacOrder->targetWID;
			}
		}
	}
}

//---------------------------------------------------------------------------

bool GameObjectManager::isTeamCapturing (TeamPtr team, GameObjectWatchID targetWID) {

	if (team) {
		long teamID = team->getId();
		for (long i = 0; i < numCaptures[teamID]; i++)
			if (targetWID == captureList[teamID][i])
				return(true);
		}
	else {
		for (long teamID = 0; teamID < MAX_TEAMS; teamID++)
			for (long i = 0; i < numCaptures[teamID]; i++)
				if (targetWID == captureList[teamID][i])
					return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createFire (ObjectTypeNumber fireObjTypeHandle,
										  GameObjectPtr owner,
										  glm::vec3& position,
										  float tonnage) {

	CarnagePtr fire = getCarnage(CARNAGE_FIRE);
	if (fire) {
		ObjectTypePtr objectType = getObjectType(fireObjTypeHandle);
		if (objectType) {
			//------------------------------------------------------------
			// Make sure the object type we loaded is really a fire object
			// type. If not, NULL out of here...
			if (objectType->getObjectClass() != FIRE)
				return(nullptr);
			fire->init(false, objectType);
			fire->setOwner(owner);
			fire->setTonnage(tonnage);
			fire->setPosition(position);
			fire->setExists(true);
		}
	}
	return(fire);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createExplosion (long effectId,
											   GameObjectPtr owner,
											   glm::vec3& position,
											   float damage,
											   float radius) 
{
	// Presentation only: impact decals for mines, deaths and airstrikes (bolt impacts emit their own event).
	impact_presentation::onExplosion(effectId, position, radius);
	long explosionObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
	if (explosionObjTypeHandle != INVALID_OBJECT) 
	{
		// FX-only entries may map to the reserved null type. Validate before
		// recycling a slot, or presentation can see an unbound explosion.
		ObjectTypePtr objectType = getObjectType(explosionObjTypeHandle);
		if (!objectType || (objectType->getObjectClass() != EXPLOSION && objectType->getObjectClass() != FIRE))
			return nullptr;
		CarnagePtr explosion = getCarnage(CARNAGE_EXPLOSION);
		if (explosion)
		{
			if (objectType && (objectType->getObjectClass() == EXPLOSION))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else if (objectType && (objectType->getObjectClass() == FIRE))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else
			{
				//Turn on when effects are officially working!
				// Don't really care if nothing happens.  Should be a bug not a crash!
				// -fs
			}
		}
		else
		{
			//Should we warn if we are out of explosions or just go on?
			//-fs
		}
	}

	return(nullptr);
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::createLight (ObjectTypeNumber lightObjTypeHandle) {

	LightPtr light = getLight();
	if (light) {
		ObjectTypePtr objectType = getObjectType(lightObjTypeHandle);
		light->init(false, objectType);
//		light->setOwner(owner);
//		light->setTonnage(tonnage);
		glm::vec3 startPosition;
		startPosition = glm::vec3(0.0f);
		light->setPosition(startPosition);
		light->setExists(true);
	}
	return(light);
}

//---------------------------------------------------------------------------
WeaponBoltPtr GameObjectManager::createWeaponBolt (long effectId) 
{
	WeaponBoltPtr weaponBolt = getWeapon();
	if (weaponBolt) 
	{
		ObjectTypeNumber weaponBoltObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
		ObjectTypePtr objectType = getObjectType(weaponBoltObjTypeHandle);
		if (!objectType)
		{
			STOP(("Object Type for a weapon Bolt was NULL.  EffectId: %d  ObjType: %d",effectId, weaponBoltObjTypeHandle));
			// STOP is a no-op in RelWithDebInfo, so guard the null-deref below
			// explicitly: drop the effect (caller handles a NULL weaponFX) instead
			// of crashing on objectType->getObjectClass() when an effect type has
			// no registered WEAPONBOLT object (e.g. asset gap on a given map).
			return(nullptr);
		}
		if (objectType->getObjectClass() != WEAPONBOLT)
			return(nullptr);
		//ALWAYS CALL IN THIS ORDER OR NO EFFECT!!!!!!!!!!!!!!!!!!!!
		weaponBolt->init(effectId);
		weaponBolt->init(false, objectType);
		weaponBolt->setExists(true);
	}

	return(weaponBolt);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::createArtillery (long artilleryType, glm::vec3& position) 
{
	ArtilleryPtr artillery = getArtillery();
	if (artillery) 
	{
		static long strikeObjectId[NUM_ARTILLERY_TYPES] = 
		{
			SMALL_ARTLRY, BIG_ARTLRY, SENSOR_ARTLRY
		};
		
 		ObjectTypePtr objectType = getObjectType(strikeObjectId[artilleryType]);
		artillery->init(false, objectType);
		artillery->setPosition(position);
		artillery->setExists(true);
	}
	
	return(artillery);
}

namespace {
// Where each mover stood when the latest sim tick began (indexed by object handle; the stamp says the entry is current).
struct InterpPose {
    glm::vec3 pos{}, current{};
    float rot = 0.0f, heading = 0.0f;
    uint32_t stamp = 0;
    mc2::sim::UnitAnimState previousAnimation, animation;
    mc2::sim::SimPose attachments;
};
std::vector<InterpPose> s_interpPose;
uint32_t s_interpStamp = 1;
float s_presentationAlpha = 1.0f;

// The pose a mover is DRAWN at: between its pose at the start of the latest tick and its pose now. Presentation only;
// the sim never sees it (Mission::updateObjectManager re-poses every mover exactly before the next tick).
void presentationPose(GameObject* object, glm::vec3& pos, float& rot)
{
	
	const long handle = object->getHandle();
	if (handle < 0 || static_cast<size_t>(handle) >= s_interpPose.size()) return;
	const InterpPose& prev = s_interpPose[handle];
	if (prev.stamp != s_interpStamp) return;   // spawned since the snapshot: nothing to blend from
	auto animation = prev.animation;
	if (animation.currentGestureId == prev.previousAnimation.currentGestureId &&
        animation.currentFrame >= prev.previousAnimation.currentFrame)
        animation.currentFrame = prev.previousAnimation.currentFrame +
            (animation.currentFrame - prev.previousAnimation.currentFrame) * s_presentationAlpha;
	object->getAppearance()->presentAnimation(animation);
	pos = prev.current; rot = prev.heading;
	const float a = s_presentationAlpha;
	pos.x = prev.pos.x + (pos.x - prev.pos.x) * a;
	pos.y = prev.pos.y + (pos.y - prev.pos.y) * a;
	pos.z = prev.pos.z + (pos.z - prev.pos.z) * a;
	float delta = std::fmod(rot - prev.rot, 360.0f);
	if (delta > 180.0f) delta -= 360.0f;
	else if (delta < -180.0f) delta += 360.0f;
	rot = prev.rot + delta * a;
}
}

void GameObjectManager::snapshotMoverPoses()
{
	if (s_interpPose.size() < static_cast<size_t>(getMaxObjects()) + 1)
		s_interpPose.resize(static_cast<size_t>(getMaxObjects()) + 1);
	++s_interpStamp;
	for (long i = 0; i < numMovers; i++)
	{
		MoverPtr mover = moverList[i];
		if (!mover || !mover->getExists()) continue;
		const long handle = mover->getHandle();
		if (handle < 0 || static_cast<size_t>(handle) >= s_interpPose.size()) continue;
		auto& pose = s_interpPose[handle];
        pose.pos = mover->getPosition(); pose.rot = mover->getRotation(); pose.stamp = s_interpStamp;
        pose.previousAnimation = *mover->getAppearance()->simulationAnimation();
	}
}

void GameObjectManager::extractRenderSnapshots() {
    for (long i = 0; i < numMovers; ++i) {
        auto* mover = moverList[i];
        if (!mover || !mover->getExists() || !mover->getAppearance()) continue;
        const long handle = mover->getHandle();
        if (handle < 0 || static_cast<size_t>(handle) >= s_interpPose.size()) continue;
        auto& proxy = s_interpPose[handle];
        proxy.current = mover->getPosition(); proxy.heading = mover->getRotation();
        proxy.animation = *mover->getAppearance()->simulationAnimation();
        proxy.attachments = *mover->getAppearance()->simPose;
        if (proxy.stamp != s_interpStamp) {
            proxy.pos = proxy.current; proxy.rot = proxy.heading;
            proxy.previousAnimation = proxy.animation;
            proxy.stamp = s_interpStamp;
        }
    }
}

void GameObjectManager::setPresentationAlpha( float alpha )
{
	s_presentationAlpha = alpha;
}

void GameObjectManager::updateAppearancesOnly( bool terrain, bool movers, bool other)
{
	if (mc2::world_trace::enabled()) mc2::world_trace::snapshot(*this, "gom_begin");
	MC2_WORLD_EVENT("appearance_only", nullptr, 0);

	if (terrain && renderObjects)
	{
        updateReadbackSnapshot();
        updateActiveSweep();
		int frameJobsPrePassCount = 0;
		updateFrameJobsPrepass(frameJobsPrePassCount);
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (object collect). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()] &&
						objList[objIndex]->getExists())
					{
						if (objList[objIndex]->getAppearance()->recalcBounds()) 
						{
							// Must force here as well.
							glm::vec3 pos = objList[objIndex]->getPosition();
							float rot = objList[objIndex]->getRotation();
							pos.z = TerrainRuntime::groundElevation(pos);
							long drawFlags = objList[objIndex]->drawFlags;
							int teamID = objList[objIndex]->getTeamId();
							objList[objIndex]->getAppearance()->setObjectParameters(pos,rot,drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

							MC2_WORLD_UPDATE("appearance_terrain", objList[objIndex]);

							auto* appearance = objList[objIndex]->getAppearance();
                            appearance->presentAnimation(*appearance->simulationAnimation());
                            appearance->presentEffects();
                            if (appearance->IsStaticNow() && !objList[objIndex]->getFlag(OBJECT_FLAG_FALLING))
                                appearance->touch();
                            else
                                appearance->update(false);
							objList[objIndex]->getAppearance()->setVisibility(true, true);	
						}
					}
				}
			}
		}
	}

	if (movers) 
	{
		if (mechs) 
		{
			for (long i = 0; i < numMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
				{
					bool inView = mechs[i]->getAppearance()->recalcBounds();
					if (inView)
						mechs[i]->windowsVisible = turn;
					
					// Must force here as well.
					glm::vec3 pos = mechs[i]->getPosition();
					float rot = mechs[i]->getRotation();
					presentationPose(mechs[i], pos, rot);
					bool selected = mechs[i]->isSelected();
					int teamID = mechs[i]->getTeamId();
					mechs[i]->getAppearance()->setObjectParameters(pos,rot,mechs[i]->drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

					MC2_WORLD_UPDATE("appearance_mechs", mechs[i]);


					mechs[i]->getAppearance()->update( false );
                    mechs[i]->getAppearance()->updateFootprints();
					mechs[i]->getAppearance()->setVisibility(inView,true);
				}
		}

		if (vehicles) 
		{
			for (long i = 0; i < numVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
				{
					bool inView = vehicles[i]->getAppearance()->recalcBounds();
					vehicles[i]->windowsVisible = turn;

					// Must force here as well.
					glm::vec3 pos = vehicles[i]->getPosition();
					float rot = vehicles[i]->getRotation();
					presentationPose(vehicles[i], pos, rot);
					bool selected = vehicles[i]->isSelected();
					int teamID = vehicles[i]->getTeamId();
					vehicles[i]->getAppearance()->setObjectParameters(pos, rot, vehicles[i]->drawFlags, teamID, Team::getRelation(teamID, Team::home->getId()));
					MC2_WORLD_UPDATE("appearance_vehicles", vehicles[i]);

					vehicles[i]->getAppearance()->update( false );
					vehicles[i]->getAppearance()->setInView( inView );
				}
		}

	}

	if (other) {
		//---------------------------------------
		// All other objects should be updated...
		if (turrets) 
		{
			for (long i=0;i<numTurrets;i++) 
			{
				if (turrets[i] && turrets[i]->getExists()) 
				{
                    turrets[i]->updatePresentationLighting();
					if (turrets[i]->getAppearance()->recalcBounds())
					{
						// Must force here as well.
						glm::vec3 pos = turrets[i]->getPosition();
						float rot = turrets[i]->getRotation();
						pos.z = TerrainRuntime::groundElevation(pos);
						bool selected = turrets[i]->isSelected();
						int teamID = turrets[i]->getTeamId();
						turrets[i]->getAppearance()->setObjectParameters(pos,rot,selected,teamID,Team::getRelation(teamID, Team::home->getId()));

						MC2_WORLD_UPDATE("appearance_turrets", turrets[i]);

						turrets[i]->getAppearance()->presentAnimation(*turrets[i]->getAppearance()->simulationAnimation());
                            turrets[i]->getAppearance()->update( false );
						turrets[i]->getAppearance()->setInView( 1 );
					}
					else
						turrets[i]->getAppearance()->setInView( 0 );
				}
			}
		}
		
		if (weapons) {
			for (long i=0;i<numWeapons;i++) {
				if (weapons[i] && weapons[i]->hasPresentation()) {
					weapons[i]->updatePresentation();
				}
			}
		}

		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->carnageType == CARNAGE_EXPLOSION &&
				    !carnage[i]->getFlag(OBJECT_FLAG_JUSTCREATED) &&
				    (carnage[i]->getExists() || !carnage[i]->presentationStarted || carnage[i]->gosEffect)) {
					carnage[i]->updatePresentation();
				}
			}
		}

		if (lights) 
		{
			for (long i = 0; i < numLights; i++) 
			{
				if (lights[i] && lights[i]->getExists()) 
				{
					if (lights[i]->getAppearance()->recalcBounds() )
					{
						MC2_WORLD_UPDATE("appearance_lights", lights[i]);
						lights[i]->getAppearance()->update( false );
						lights[i]->getAppearance()->setInView( 1 );
					}
					else
						lights[i]->getAppearance()->setInView(0);
				}
			}
		}

		if (artillery) 
		{
			for (long i = 0; i < numArtillery; i++) 
			{
				if (artillery[i] && (artillery[i]->getExists() || artillery[i]->hasPresentation()))
				{
					artillery[i]->updatePresentation();
				}
			}
		}
	}
}
