#include "tick_commit.h"
//***************************************************************************
//
//	carnage.cpp -- File contains the Carnage class code
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

#ifndef CARNAGE_H
#include"carnage.h"
#endif
#include <bit>
#include <cstring>
#include "mc2fmt.h"
#include "diagnostic_trace.h"

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#include<gameos.hpp>
#include "../RenderCore/axis_convert.h"

//---------------------------------------------------------------------------

constexpr auto BLIP_FRAME_RATE = 0.067;
constexpr double EXPLOSION_TIME = 0.5;
constexpr int EXPLOSION_DEPTH_FIXUP = -150;

extern unsigned long	NextIdNumber;
extern TeamPtr homeTeam;
extern bool useSound;
extern bool somethingOnFire;

float Carnage::maxFireBurnTime = 5.0;

extern MidLevelRenderer::MLRClipper * theClipper;
extern bool MLRVertexLimitReached;
//***************************************************************************
// FIRE TYPE class
//***************************************************************************

GameObjectPtr FireType::createInstance (void) {

	CarnagePtr newFire = new Carnage;
	if (!newFire)
		return(nullptr);

	newFire->init(true, this);

	return(newFire);
}

//---------------------------------------------------------------------------

void FireType::destroy (void) 
{
	if (fireOffsetX) 
	{
		std::free(fireOffsetX);
		fireOffsetX = nullptr;
	}

	if (fireOffsetY) 
	{
		std::free(fireOffsetY);
		fireOffsetY = nullptr;
	}
	
	if (fireDelay) 
	{
		std::free(fireDelay);
		fireDelay = nullptr;
	}

	if (fireRandomOffsetX) 
	{
		std::free(fireRandomOffsetX);
		fireRandomOffsetX = nullptr;
	}

	if (fireRandomOffsetY) 
	{
		std::free(fireRandomOffsetY);
		fireRandomOffsetY = nullptr;
	}

	if (fireRandomDelay) 
	{
		std::free(fireRandomDelay);
		fireRandomDelay = nullptr;
	}

	ObjectType::destroy();
}
		
//---------------------------------------------------------------------------

long FireType::init (FilePtr objFile, unsigned long fileSize) {

	long result = 0;
	
	FitIniFile explFile;
	result = explFile.open(objFile,fileSize);
	if (result != NO_ERR)
		return(result);
	
	result = explFile.seekBlock("FireData");
	if (result != NO_ERR)
		return(result);

	unsigned long dmgLevel;
	result = explFile.readIdULong("DmgLevel",dmgLevel);
	if (result != NO_ERR)
		return(result);
	damageLevel = dmgLevel;
		
	result = explFile.readIdULong("SoundEffectId",soundEffectId);
	if (result != NO_ERR)
		return(result);
	
	result = explFile.readIdULong("LightObjectId",lightObjectId);
	if (result != NO_ERR)
		lightObjectId = -1;

	result = explFile.readIdULong("startLoopFrame",startLoopFrame);
	if (result != NO_ERR)
		return(result);

	result = explFile.readIdULong("numLoops",numLoops);
	if (result != NO_ERR)
		return(result);
		
	result = explFile.readIdULong("endLoopFrame",endLoopFrame);
	if (result != NO_ERR)
		return(result);

	result = explFile.readIdFloat("maxExtentRadius",maxExtent);
	if (result != NO_ERR)		
		maxExtent = 0.0;
	
	result = explFile.readIdFloat("TimeToMaxExtent",timeToMax);
	if (result != NO_ERR)		
		timeToMax = 0.0;

	result = explFile.readIdLong("TotalFireShapes",totalFires);
	if (result != NO_ERR)
		totalFires = 1;

	//------------------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&explFile);

	objectClass = FIRE;

	return(result);
}
	
//---------------------------------------------------------------------------

bool FireType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	if (MPlayer && !MPlayer->isServer())
		return(false);

	if (!collider->isDestroyed()) 
	{
		switch (collider->getObjectClass()) 
		{
			case BATTLEMECH:
				//-------------------------------------------------------------
				// We should induce heat in a battlemech if it is in the fire.
				break;
			case BUILDING:
				//----------------------------------------------------------------------
				// We should light a building on fire if we have expanded to engulf it.
				// Not anymore.  Fires are undefined at present.
				break;
			case TERRAINOBJECT:
				if ((static_cast<TerrainObjectPtr>(collider))->getSubType() == TERROBJ_TREE)
				{
					if (RollDice(10)) 
					{
						(static_cast<TerrainObjectPtr>(collider))->lightOnFire(15.0);
						if (MPlayer)
							MPlayer->addLightOnFireChunk(collider, 15);
					}
				}
				break;
		}
	}
	
	return(false);
}

//---------------------------------------------------------------------------

bool FireType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) {
	return(FALSE);
}

//***************************************************************************
// EXPLOSION TYPE class
//***************************************************************************

GameObjectPtr ExplosionType::createInstance (void) {

	CarnagePtr newExplosion = new Carnage;
	if (!newExplosion)
		return(nullptr);

	newExplosion->init(true, this);

	return(newExplosion);
}

//---------------------------------------------------------------------------

void ExplosionType::destroy (void) 
{
	ObjectType::destroy();
}
		
//---------------------------------------------------------------------------

long ExplosionType::init (FilePtr objFile, unsigned long fileSize) {

	long result = 0;
	
	FitIniFile explFile;
	result = explFile.open(objFile,fileSize);
	if (result != NO_ERR)
		return(result);
	
	//------------------------------------------------------------------
	// Read in the data needed to fire the laser
	result = explFile.seekBlock("ExplosionData");
	if (result != NO_ERR)
		return(result);

	unsigned long dmgLevel;
	result = explFile.readIdULong("DmgLevel",dmgLevel);
	if (result != NO_ERR)
		return(result);
	damageLevel = dmgLevel;
		
	result = explFile.readIdULong("SoundEffectId",soundEffectId);
	if (result != NO_ERR)
		return(result);
		
	result = explFile.readIdLong("ExplosionRadius",explRadius);
	if (result != NO_ERR)
		explRadius = 0;
		
	result = explFile.readIdULong("LightObjectId",lightObjectId);
	if (result != NO_ERR)
		lightObjectId = -1;

	result = explFile.readIdFloat("DamageChunkSize",chunkSize);
	if (result != NO_ERR)
		chunkSize = 5.0;

	result = explFile.readIdFloat("DelayUntilCollidable",delayUntilCollidable);
	if (result != NO_ERR)
		delayUntilCollidable = EXPLOSION_TIME;

	result = explFile.seekBlock("LightData");
	if (result == NO_ERR)
	{
		result = explFile.readIdFloat("LightMinMaxRadius",lightMinMaxRadius);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("LightMaxMaxRadius",lightMaxMaxRadius);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("LightOutMinRadius",lightOutMinRadius);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("LightOutMaxRadius",lightOutMaxRadius);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdULong("LightRGB",lightRGB);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("MaxIntensity",maxIntensity);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("MinIntensity",minIntensity);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));

		result = explFile.readIdFloat("Duration",duration);
		if (result != NO_ERR)
			STOP(("Light data for explosion BAD"));
	}

	//------------------------------------------------------------------
	// Initialize the base object Type from the current file.
	result = ObjectType::init(&explFile);

	objectClass = EXPLOSION;

	return(result);
}
	
//---------------------------------------------------------------------------

bool ExplosionType::handleCollision (GameObjectPtr collidee, GameObjectPtr collider) {

	if (MPlayer && !MPlayer->isServer())
		return(false);

	float damageLeft = collidee->getExplDmg();

	//-------------------------------------------------------
	// Explosions CAN damage objects around them if they have
	// a damage level associated with them.
	if (damageLeft != 0.0) {
		float damageAmount = 0.0;
		WeaponShotInfo shot;
		if (damageLeft > (static_cast<CarnagePtr>(collidee))->info.explosion.chunkSize)
			damageAmount = (static_cast<CarnagePtr>(collidee))->info.explosion.chunkSize;
		else
			damageAmount = damageLeft;

		if (collider->isMover()) {
			shot.init(0, -1, damageAmount, 0, 0);		 
			while (damageLeft > 0.0) {
				shot.hitLocation = collider->calcHitLocation(collidee,-1,ATTACKSOURCE_ARTILLERY,0);
				(static_cast<CarnagePtr>(collidee))->traceCombat("damage", "collision", collider, &shot);
				collider->handleWeaponHit(&shot, (MPlayer != nullptr));
				damageLeft -= (static_cast<CarnagePtr>(collidee))->info.explosion.chunkSize;
			}
			}
		else {
			switch (collider->getObjectClass()) 
			{
				case GATE: 
				{
					//---------------------------------------------------
					// An explosion may NOT damage a gate or turret unless
					// is is within the "little" extent radius.
					glm::vec3 distance = collider->getPosition();
					distance -= collidee->getPosition();
					float range = glm::length(distance);

					float littleExtent = (static_cast<TurretPtr>(collider))->getLittleExtent();
					if (littleExtent < range) 
					{
						float realRange = range - littleExtent;
						if (realRange > collidee->getExtentRadius())
							return(false);
					}
					
					shot.init(0, -1, damageAmount, 0, 0);		 
					while (damageLeft > 0.0) 
					{
						shot.hitLocation = 0;
						(static_cast<CarnagePtr>(collidee))->traceCombat("damage", "collision", collider, &shot);
						collider->handleWeaponHit(&shot, (MPlayer != nullptr));
						damageLeft -= chunkSize;
					}
				}
				break;

				case TURRET: 
				{
					//---------------------------------------------------
					// An explosion may NOT damage a gate or turret unless
					// is is within the "little" extent radius.
					glm::vec3 distance = collider->getPosition();
					distance -= collidee->getPosition();
					float range = glm::length(distance);

					float littleExtent = (static_cast<TurretPtr>(collider))->getLittleExtent();
					if (littleExtent < range) 
					{
						float realRange = range - littleExtent;
						if (realRange > collidee->getExtentRadius())
							return(FALSE);
					}
					
					float damageLeft = collidee->getExplDmg();
					WeaponShotInfo shot;
					shot.init(0, -1, damageAmount, 0, 0);		 
					while (damageLeft > 0.0) 
					{
						shot.hitLocation = 0;
						(static_cast<CarnagePtr>(collidee))->traceCombat("damage", "collision", collider, &shot);
						collider->handleWeaponHit(&shot, (MPlayer != nullptr));
						damageLeft -= chunkSize;

					}
							
					}
					break;

				default: 
				{
					WeaponShotInfo shot;
					shot.init(0, -1, collidee->getExplDmg(), 0, 0);
					(static_cast<CarnagePtr>(collidee))->traceCombat("damage", "collision", collider, &shot);
					collider->handleWeaponHit(&shot, (MPlayer != nullptr));
				}
			}
		}
	}
	
	return(false);
}

//---------------------------------------------------------------------------

bool ExplosionType::handleDestruction (GameObjectPtr collidee, GameObjectPtr collider) {

	//-------------------------------------------------------
	// Retirement follows the authoritative collision pulse.
	// always return FALSE or the collision will make it
	// go away!
	
	return(false);
}

//***************************************************************************
// CARNAGE class
//***************************************************************************
void Carnage::init (bool create) 
{
	carnageType = CARNAGE_NONE;
	ownerWID = 0;
	info.fire.timeToBurn = 0.0;
	info.fire.radius = 0.0;
	info.fire.reallyVisible = 0;
	info.explosion.timer = EXPLOSION_TIME;
	info.explosion.chunkSize = 0.0;
	gosEffect = nullptr;

	intensity = 0.0f;
	inRadius = 0.0f;
	outRadius = 0.0f;
	duration = 0.0f;

	pointLight = nullptr;
	lightId = 0xffffffff;
}

//---------------------------------------------------------------------------
void Carnage::init (CarnageEnumType _carnageType) 
{
    mc2::tick::releaseEffect(gosEffect);
    mc2::tick::releaseLight(pointLight, eye, lightId);
	//--------------------------------------------------------------------
	// This function not only sets the carnage type, but also re-inits the
	// object so it may be used again in the game without re-allocating
	// it...
	carnageType = _carnageType;
	if (carnageType == CARNAGE_FIRE) 
	{
	
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
		intensity = 0.0f;
		inRadius = 0.0f;
		outRadius = 0.0f;
		duration = 0.0f;

		pointLight = nullptr;
		lightId = 0xffffffff;
	}
}

//---------------------------------------------------------------------------
GameObjectPtr Carnage::getOwner (void) 
{
	return(ObjectManager->getByWatchID(ownerWID));
}

//---------------------------------------------------------------------------
void Carnage::handleStaticCollision (void) 
{
	traceCombat("static_collision_pass");
	if (getTangible()) 
	{
		//-----------------------------------------------------
		// What is our block and vertex number?
		int blockNumber = 0;
		int vertexNumber = 0;
		getBlockAndVertexNumber(blockNumber,vertexNumber);
			
		//-------------------------------------------------------------------------
		// We must now move out into other tiles for the artillery strike to work.
		// Remember, Its pretty big!
		// Just grab the nine vertices around this one.  Problems arise when on Block border.  Handle it.
		int topLeftBlockNumber = blockNumber - Terrain::blocksMapSide - 1;
		int currentBlockNumber = topLeftBlockNumber;
		int totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

		for (int i = 0; i < 3; i++) 
		{
			for (int j = 0; j < 3; j++) 
			{
				if ((currentBlockNumber >= 0) && (currentBlockNumber < totalBlocks))
				{
					int numObjectsInBlock = ObjectManager->getObjBlockNumObjects(currentBlockNumber);
					for (int objIndex = 0; objIndex < numObjectsInBlock; objIndex++) 
					{
						GameObjectPtr obj = ObjectManager->getObjBlockObject(currentBlockNumber, objIndex);
						if (obj->getExists() && (obj->getObjectClass() != GATE) && (obj->getObjectClass() != TURRET)) 
							ObjectManager->detectStaticCollision(this, obj);
					}
				}
				currentBlockNumber++;
			}
			currentBlockNumber = topLeftBlockNumber + (Terrain::blocksMapSide * (i + 1));
		}

		//-------------------------------------------------------
		// MUST figure out radius to set off mines in CELLS now.
		// UNLESS WE ARE A MINE EXPLOSION!!!
		// -fs
		if ((effectId != MINE_EXPLOSION_ID) && (info.explosion.radius > 0.0f))
		{
			int CellRow, CellCol;
			land->worldToCell(getPosition(), CellRow, CellCol);
	
			int startCellRow = CellRow - 4;
			int startCellCol = CellCol - 4;
				
			for (int i = startCellRow; i < startCellRow + 9; i++) 
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
							glm::vec3 distance;
							distance = getPosition() - minePosition;
							distance.z = 0.0f;
							float dist = glm::length(distance);
							if (dist < info.explosion.radius)
							{
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
			}
		}
	}
}	

//---------------------------------------------------------------------------
bool Carnage::onScreen (void) 
{
	//----------------------------------------------------------------------
	// Need a real check here against the OBB Radius of the effect
	
	return(true);
}

//---------------------------------------------------------------------------
void Carnage::finishNow (void) 
{
	//When the fire goes out, it may spread.
	if (carnageType == CARNAGE_FIRE) 
	{
		setFlag(OBJECT_FLAG_SPREAD, true);
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
		traceCombatRetire("forced_finish");
        mc2::tick::releaseEffect(gosEffect);
        mc2::tick::releaseLight(pointLight, eye, lightId);
		init(false);
	}
}	
		
//---------------------------------------------------------------------------
void Carnage::addTimeLeft (float timeLeft) 
{
	if (carnageType == CARNAGE_FIRE) 
	{
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
	}
}

//---------------------------------------------------------------------------
long Carnage::update(void)
{
	if (carnageType != CARNAGE_EXPLOSION) return true;
	setFlag(OBJECT_FLAG_JUSTCREATED, false);
	if (auto* owner = getOwner()) position = owner->getPosition();
	// Remain tangible for exactly one collision pass, independent of FX lifetime.
	if (getTangible()) {
		setFlag(OBJECT_FLAG_TANGIBLE, false);
		return false;
	}
	info.explosion.timer -= frameLength;
	if (info.explosion.timer <= 0.0f) {
		setFlag(OBJECT_FLAG_TANGIBLE, true);
		traceCombat("collision_armed");
	}
	return true;
}

void Carnage::updatePresentation()
{
	if (carnageType == CARNAGE_FIRE) 
	{
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
		if (!presentationStarted)
		{
			presentationStarted = true;
			createPresentationEffect();
				
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
				
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, position);
	
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			
			if (gosEffect && gosEffect->IsExecuted())
			{
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
				gosEffect->Start(&info);
			}

			//------------------------------------------
			// If just created, play our sound effect...
			if ((static_cast<ExplosionTypePtr>(getObjectType()))->soundEffectId != -1)
				soundSystem->playDigitalSample((static_cast<ExplosionTypePtr>(getObjectType()))->soundEffectId,position,true);

			if ((static_cast<ExplosionTypePtr>(getObjectType()))->duration > 0.0f)
			{
				// Explosion Has Point Light Source
				// Make and Init it here.
				pointLight = static_cast<TG_LightPtr>(std::malloc(sizeof(TG_Light)));
				pointLight->init(TG_LIGHT_POINT);

				pointLight->SetaRGB((static_cast<ExplosionTypePtr>(getObjectType()))->lightRGB);
				intensity = (static_cast<ExplosionTypePtr>(getObjectType()))->maxIntensity;
				outRadius = (static_cast<ExplosionTypePtr>(getObjectType()))->lightOutMaxRadius;
				inRadius = (static_cast<ExplosionTypePtr>(getObjectType()))->lightMaxMaxRadius;
				pointLight->SetIntensity(intensity);
				pointLight->SetFalloffDistances(inRadius,outRadius);

				lightId = eye->addWorldLight(pointLight);
				duration = (static_cast<ExplosionTypePtr>(getObjectType()))->duration;

				if (lightId == -1)
				{
					//NO LIGHT ALLOWED!  TOO Many in World!
					// LightId is now -1 which will cause this to NEVER make a light!
					std::free(pointLight);
					pointLight = nullptr;
					duration = 0.0f;
				}
			}
		}
		
		//------------------------------------------------
		// Update pointLight if its not NULL
		if (pointLight)
		{
			if (duration > 0.0f)
			{
				float timeFactor = duration / (static_cast<ExplosionTypePtr>(getObjectType()))->duration;
				intensity = (static_cast<ExplosionTypePtr>(getObjectType()))->minIntensity +
							((static_cast<ExplosionTypePtr>(getObjectType()))->maxIntensity - (static_cast<ExplosionTypePtr>(getObjectType()))->minIntensity) * timeFactor;
				outRadius = (static_cast<ExplosionTypePtr>(getObjectType()))->lightOutMinRadius +
							((static_cast<ExplosionTypePtr>(getObjectType()))->lightOutMaxRadius - (static_cast<ExplosionTypePtr>(getObjectType()))->lightOutMinRadius) * timeFactor;
				inRadius = (static_cast<ExplosionTypePtr>(getObjectType()))->lightMinMaxRadius +
							((static_cast<ExplosionTypePtr>(getObjectType()))->lightMaxMaxRadius - (static_cast<ExplosionTypePtr>(getObjectType()))->lightMinMaxRadius) * timeFactor;

				pointLight->SetIntensity(intensity);
				pointLight->SetFalloffDistances(inRadius,outRadius);

				duration -= frameLength;
				
				glm::vec3 ourPosition;
				mc2ToRender(ourPosition, position);
			
				pointLight->direction = ourPosition;
			
				glm::mat4 lightToWorldMatrix;
				mc2::math::buildTranslation(lightToWorldMatrix, ourPosition);
				mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f,0.0f,0.0f));
				pointLight->SetLightToWorld(&lightToWorldMatrix);
				pointLight->SetPosition(&position);
			}
			else
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
				pointLight = nullptr;
			}
		}

		//------------------------------------------------
		// Update GOSFX
		if (gosEffect && gosEffect->IsExecuted())
		{
			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, position);
			
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
			   
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);
			localToWorld = shapeOrigin * gosFX::Effect_Into_Motion;
		
 			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
			
			bool result = gosEffect->Execute(&info);
			if (!result)
			{
				gosEffect->Kill();		//Effect is over.
				gosEffect.reset();

				if (pointLight)
				{
					eye->removeWorldLight(lightId,pointLight);
					std::free(pointLight);
					pointLight = nullptr;
				}
		
				return;			// The visual tail has finished; gameplay retirement is independent.
			}
		}
		else	// An absent cosmetic effect has no authority over collision.
		{
			if (pointLight)
			{
				eye->removeWorldLight(lightId,pointLight);
				std::free(pointLight);
				pointLight = nullptr;
			}

			return;
		}
	}
	
	return;
}

//---------------------------------------------------------------------------

// G7b K3: the sim reads (type, JUSTCREATED, position) at the render extract; the draw reads the view.
void Carnage::extractRenderView (mc2::render_view::RenderView& view)
{
	GameObject::extractRenderView(view);
	view.effects[0] = gosEffect.get();
	if (carnageType == CARNAGE_EXPLOSION && !getFlag(OBJECT_FLAG_JUSTCREATED))
		view.flags |= mc2::render_view::Visible;
}

void Carnage::renderView (const mc2::render_view::RenderView& view)
{
	auto* const gosEffect = view.effects[0];
	if (view.has(mc2::render_view::Visible))
	{
		if (gosEffect && gosEffect->IsExecuted())
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

			glm::vec3 actualPosition;
			mc2ToRender(actualPosition, view.position);

			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;

			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, actualPosition);

			drawInfo.m_parentToWorld = &shapeOrigin;

			if (!MLRVertexLimitReached)
				gosEffect->Draw(&drawInfo);
		}
	}
}

//---------------------------------------------------------------------------

// Combat diagnostics retain object identity and event order without owning gameplay state.
void Carnage::traceCombat(std::string_view event, std::string_view reason,
                              GameObjectPtr target, const WeaponShotInfo* shot)
{
    combatTrace.write("carnage", getHandle(), turn, event, reason, position,
        [] { return static_cast<const glm::vec3*>(nullptr); }, target, shot);
}

void Carnage::traceCombatRetire(std::string_view reason)
{
    traceCombat("retire", reason);
    combatTrace.live = false;
}

void Carnage::destroy (void) 
{
	traceCombatRetire("teardown");
	if (carnageType == CARNAGE_FIRE) 
	{
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
        mc2::tick::releaseEffect(gosEffect);
        mc2::tick::releaseLight(pointLight, eye, lightId);
	}
}

//---------------------------------------------------------------------------

void Carnage::init (bool create, ObjectTypePtr _type) 
{
    mc2::tick::releaseEffect(gosEffect);
    mc2::tick::releaseLight(pointLight, eye, lightId);
	traceCombatRetire("reinit");
	GameObject::init(true, _type);

	gosEffect = nullptr;
	presentationStarted = false;
	pointLight = nullptr;
	setFlag(OBJECT_FLAG_JUSTCREATED, true);
	
	if (carnageType == CARNAGE_FIRE)
	{
	}
	else if (carnageType == CARNAGE_EXPLOSION) 
	{
		info.explosion.chunkSize = (static_cast<ExplosionType *>(_type))->chunkSize;
		info.explosion.timer = (static_cast<ExplosionType *>(_type))->delayUntilCollidable;

		setFlag(OBJECT_FLAG_TANGIBLE, false);
	}
    combatTrace.begin(_type->whatAmI(), effectId);
    traceCombat("birth");
}	

//---------------------------------------------------------------------------
#include "save_struct_zero.h"
void Carnage::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(CarnageData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&data),sizeof(CarnageData),STORAGE_TYPE_ZLIB);
}

//---------------------------------------------------------------------------
void Carnage::CopyTo (CarnageData *data)
{
	data->carnageType = carnageType;
	data->ownerWID = ownerWID;
	data->info = info;

	data->effectId = effectId;

	GameObject::CopyTo(data);
}

//---------------------------------------------------------------------------
void Carnage::Load (CarnageData *data)
{
	GameObject::Load(data); 

	carnageType = data->carnageType;
	ownerWID = data->ownerWID;
	info = data->info;

	effectId = data->effectId;
	combatTrace.effect = effectId;
	traceCombat("restore");
	if (!getExists()) traceCombatRetire("loaded_inactive");
}

//---------------------------------------------------------------------------

void Carnage::createPresentationEffect()
{
		if (S_stricmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
		{
			//--------------------------------------------------------------
			// We need to create the GOSFX we need for this explosion here.
			// The name of the GOSFX is stored as the appearanceName in the Type.
			// ONLY Execute.  Explosions just run once!
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));

			gosEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(gosEffect != nullptr);

			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}

}
