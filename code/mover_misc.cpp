#include "render_view.h"
// Mover: sensor text, waypoint drawing, optimal cells, mover placement, ejection and Save/CopyTo/Load.
// Declared in mover.h; shared names in mover_internal.h.
#include "mover_internal.h"

 
void Mover::drawSensorTextHelp (float screenX, float screenY, long resID, DWORD color, bool drawBOLD)
{
	DWORD width, height;
	Stuff::Vector4D moveHere;
	moveHere.x = screenX;
	moveHere.y = screenY;

	char buffer[256];
	cLoadString( resID, buffer, 255 );

	FloatHelp::getTextStringLength(buffer,color,1.0f,true,drawBOLD,true,false,width,height);

	moveHere.x -= width / 2;
	moveHere.z = width;
	moveHere.w = height;

	globalFloatHelp->setFloatHelp(buffer,moveHere,color,0x0,1.0f,true,drawBOLD,true,false);
}

void Mover::drawWaypointPath()
{
	mc2::render_view::drawHudWaypoints(mc2::render_view::hud().mover(this));
}

void Mover::drawOverviewMovePath( unsigned long color )
{
	mc2::render_view::drawHudOverviewPath(mc2::render_view::hud().mover(this), color);
}

bool Mover::getMoveDestination( glm::vec3& out )
{
	return getPilot() ? getPilot()->getMoveDestination( out ) : false;
}

void Mover::getWeaponBandsPresent( bool present[3] )
{
	present[0] = present[1] = present[2] = false;	// short / medium / long
	int total = static_cast<int>(numOther) + static_cast<int>(numWeapons) + static_cast<int>(numAmmos);
	if ( total > MAX_MOVER_INVENTORY_ITEMS ) total = MAX_MOVER_INVENTORY_ITEMS;
	for ( int j = 0; j < total; j++ )
	{
		MasterComponent& mc = MasterComponent::masterList[inventory[j].masterID];
		ComponentFormType f = mc.getForm();
		if ( f < COMPONENT_FORM_WEAPON || f > COMPONENT_FORM_WEAPON_MISSILE )
			continue;
		long rt = mc.getWeaponRange();
		int b = ( rt == WEAPON_RANGE_SHORT ) ? 0
		      : ( rt == WEAPON_RANGE_MEDIUM || rt == WEAPON_RANGE_SHORT_MEDIUM ) ? 1
		      : 2;	// LONG or MEDIUM_LONG
		present[b] = true;
	}
}

void Mover::updateDrawWaypointPath()
{
	getPilot()->updateDrawWaypointPath();
}

void Mover::initOptimalCells (long numIncrements) {

	numOptimalIncrements = numIncrements;
	double degreesToRadians = static_cast<double>(DEGREES_TO_RADS);
	double incrementDegrees = 360.0 / numOptimalIncrements;
	double doptimalCells[MAX_ATTACK_CELLRANGE][MAX_ATTACK_INCREMENTS][2];
	for (long r = 0; r < MAX_ATTACK_CELLRANGE; r++)
		for (long i = 0; i < numIncrements; i++) {
			double incrementRadians = incrementDegrees * static_cast<double>(i) * degreesToRadians;
			doptimalCells[r][i][0] = sin(incrementRadians) * static_cast<double>(r);
			doptimalCells[r][i][1] = cos(incrementRadians) * static_cast<double>(r);
		}

	// Due to double rounding errors, we munge the numbers a little before
	// casting to chars. NOTE: due to this rounding, it's possible that
	// an "optimal" cell may be a cell or more further from the target than
	// the desired range (i.e. there's an error of 10 or so meters in these
	// calcs, in some situations). Therefore, it's imperative that this is
	// acceptable (re: max fire range for the mover), otherwise the mover
	// may select an "optimal" cell from which it's out of range. As long
	// as these optimal ranges fall well within their weapon fire ranges,
	// this should be no problem.
	for (int r = 0; r < MAX_ATTACK_CELLRANGE; r++)
		for (long i = 0; i < numIncrements; i++) {
			if (doptimalCells[r][i][0] < 0.00)
				optimalCells[r][i][0] = static_cast<char>((doptimalCells[r][i][0] - 0.50));
			else
				optimalCells[r][i][0] = static_cast<char>((doptimalCells[r][i][0] + 0.50));
			if (doptimalCells[r][i][1] < 0.00)
				optimalCells[r][i][1] = static_cast<char>((doptimalCells[r][i][1] - 0.50));
			else
				optimalCells[r][i][1] = static_cast<char>((doptimalCells[r][i][1] + 0.50));
		}

	char rangedMap[MAX_ATTACK_CELLRANGE * 2 + 1][MAX_ATTACK_CELLRANGE * 2 + 1];
	for (int r = 0; r < (MAX_ATTACK_CELLRANGE * 2 + 1); r++)
		for (long c = 0; c < (MAX_ATTACK_CELLRANGE * 2 + 1); c++) {
			glm::vec3 start, goal;
			start.x = c;
			start.y = r;
			start.z = 0;
			goal.x = MAX_ATTACK_CELLRANGE;
			goal.y = MAX_ATTACK_CELLRANGE;
			goal.z = 0.0;
			double dist = distance_from(start, goal);
			rangedMap[r][c] = static_cast<char>((dist + 0.5));
		}
	long index = 0;
	for (long range = 0; range < MAX_ATTACK_CELLRANGE; range++) {
		rangedCellsIndices[range][0] = index;
		for (long r = 0; r < (MAX_ATTACK_CELLRANGE * 2 + 1); r++)
			for (long c = 0; c < (MAX_ATTACK_CELLRANGE * 2 + 1); c++)
				if (rangedMap[r][c] == range) {
					rangedCells[index][0] = r - MAX_ATTACK_CELLRANGE;
					rangedCells[index][1] = c - MAX_ATTACK_CELLRANGE;
					index++;
				}
		rangedCellsIndices[range][1] = index - 1;
	}
}


void GetBlockedDoorCells (int moveLevel, int door, char* openCells) {

	Assert((door > -1) && (door < GlobalMoveMap[moveLevel]->numDoors), 0, " FUDGE 1");
	Assert((GlobalMoveMap[moveLevel]->doors[door].direction[0] == 1) || (GlobalMoveMap[moveLevel]->doors[door].direction[0] == 2), 0, " FUDGE 2");
    //sebi length is char it is always < 1024
	//Assert((GlobalMoveMap[moveLevel]->doors[door].length > 0) && (GlobalMoveMap[moveLevel]->doors[door].length < 1024), 0, " FUDGE 3");
	Assert((GlobalMoveMap[moveLevel]->doors[door].length > 0), 0, " FUDGE 3");
	long doorWorldCellRec[4] = {-1, -1, -1, -1};
	if (GlobalMoveMap[moveLevel]->doors[door].direction[0] == 1) {
		doorWorldCellRec[0] = GlobalMoveMap[moveLevel]->doors[door].row;
		doorWorldCellRec[1] = GlobalMoveMap[moveLevel]->doors[door].col;
		doorWorldCellRec[2] = doorWorldCellRec[0] + GlobalMoveMap[moveLevel]->doors[door].length;
		doorWorldCellRec[3] = doorWorldCellRec[1] + 2;

		//NOTE: DO NOT set cells for elementals--only mechs and vehicles.
		long numMovers = ObjectManager->getNumMovers();
		for (long i = 0; i < numMovers; i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if ((mover->getWatchID() != PathFindMap[SECTOR_PATHMAP]->moverWID) && (mover->getWatchID() != RamObjectWID) && !mover->isDisabled()) {
				int cellPos[2];
				mover->getCellPosition (cellPos[0], cellPos[1]);
				int worldCellRow = cellPos[0];
				int worldCellCol = cellPos[1];
				if ((worldCellRow >= doorWorldCellRec[0]) && (worldCellRow < doorWorldCellRec[2]))
					if ((worldCellCol >= doorWorldCellRec[1]) && (worldCellCol < doorWorldCellRec[3])) {
						long cellIndex = worldCellRow - doorWorldCellRec[0];
						Assert((cellIndex > -1) && (cellIndex < GlobalMoveMap[moveLevel]->doors[door].length), 0, " Bad Cell Index ");
						openCells[cellIndex] = 0;
					}
			}
		}

		}
	else {
		doorWorldCellRec[0] = GlobalMoveMap[moveLevel]->doors[door].row;
		doorWorldCellRec[1] = GlobalMoveMap[moveLevel]->doors[door].col;
		doorWorldCellRec[2] = doorWorldCellRec[0] + 2;
		doorWorldCellRec[3] = doorWorldCellRec[1] + GlobalMoveMap[moveLevel]->doors[door].length;

		long numMovers = ObjectManager->getNumMovers();
		for (long i = 0; i < numMovers; i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if ((mover->getObjectClass() != ELEMENTAL) && (mover->getWatchID() != PathFindMap[SECTOR_PATHMAP]->moverWID) && (mover->getWatchID() != RamObjectWID) && !mover->isDisabled()) {
				int cellPos[2];
				mover->getCellPosition (cellPos[0], cellPos[1]);
				int worldCellRow = cellPos[0];
				long worldCellCol = cellPos[1];
				if ((worldCellRow >= doorWorldCellRec[0]) && (worldCellRow < doorWorldCellRec[2]))
					if ((worldCellCol >= doorWorldCellRec[1]) && (worldCellCol < doorWorldCellRec[3])) {
						long cellIndex = worldCellCol - doorWorldCellRec[1];
						Assert((cellIndex > -1) && (cellIndex < GlobalMoveMap[moveLevel]->doors[door].length), 0, " Bad Cell Index ");
						openCells[cellIndex] = 0;
					}
			}
		}
	}
}

void PlaceMovers (void) {

	long numMovers = ObjectManager->getNumMovers();
	for (long i = 0; i < numMovers; i++) {
		MoverPtr mover = static_cast<MoverPtr>(ObjectManager->getMover(i));
		if (mover->getUseMe()) {
			int cellRow, cellCol;
			mover->getCellPosition(cellRow, cellCol);
			if (!GameMap->getPreserved(cellRow, cellCol))
				GameMap->setPreserved(cellRow, cellCol, true);
			GameMap->placeObject(mover->getPosition(), mover->getExtentRadius());
			GameMap->setPassable(cellRow, cellCol, false);
		}
	}
}

void adjustMoveMapCellCost (MoveMapNodePtr cell, long costAdj);

void PlaceStationaryMovers (MoveMap* map) {

	long numMovers = ObjectManager->getNumMovers();
	for (long i = 0; i < numMovers; i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if ((mover->getObjectClass() != ELEMENTAL) && (mover->getWatchID() != map->moverWID) && (mover->getWatchID() != RamObjectWID) && !mover->isDisabled()) {
			int cellRow, cellCol;
			mover->getCellPosition(cellRow, cellCol);
			if ((cellRow >= 0) && (cellRow < map->height) && (cellCol >= 0) && (cellCol < map->width)) {
	
				if ((cellRow != map->startR) || (cellCol != map->startC)) {
					if ((map->map[cellRow * map->maxWidth + cellCol].flags & MOVEFLAG_GOAL) == 0) {
						MovePathPtr path = mover->getPilot() ? mover->getPilot()->getMovePath() : nullptr; // a pilotless mover has no path
						if (path && (path->numSteps == 0)) {
							map->map[cellRow * map->maxWidth + cellCol].setFlag(MOVEFLAG_MOVER_HERE);
#ifdef USE_OVERLAYS
							long bridgeCost = COST_BLOCKED / 3;
							long overlay = GameMap->getOverlay(cellRow, cellCol);
							if (OverlayIsBridge[overlay])
								map->adjustCellCost(cellRow, cellCol, bridgeCost);
							else
#endif
								map->adjustCost(cellRow, cellCol, COST_BLOCKED * 2);
						}
					}
				}
			}
		}
	}
}

bool Mover::isCloseToFirstTacOrder( glm::vec3& pos )
{
	return getPilot()->isCloseToFirstTacOrder( pos );
}

void Mover::removeFromUnitGroup( int id )
{
	unitGroup = -1;
}
void Mover::addToUnitGroup( int id )
{
	unitGroup = (1 << id); // now you only get to be in one at a time
}
bool Mover::isInUnitGroup( int id )
{
	if ( unitGroup == -1 )
		return false;
	return unitGroup & ( 1 << id ) ? true : false;
}
bool Mover::handleEjection()
{

	if (pilot && ((pilot->getStatus() == WARRIOR_STATUS_NORMAL) || (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWING))) {
		getPilot()->eject();

		ejectOrderGiven = true;
	


 		disable(EJECTION_DEATH);

#ifdef USE_IFACE
		theInterface->RemoveMech(getPartId());
#endif

#ifdef USE_MOODMUSIC
		if (alignment == homeTeam->getAlignment())
			friendlyDestroyed = true;
		else
			enemyDestroyed = true;
#endif
	}

	return(true);
}

bool	 Mover::isWeaponWorking (long weaponIndex)
{
	if (inventory[weaponIndex].disabled)
		return(FALSE);
	else if (getWeaponShots(weaponIndex) == 0)
		return (FALSE);
	return(TRUE);
}

#include "save_struct_zero.h"
void Mover::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(MoverData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&data),sizeof(MoverData),STORAGE_TYPE_ZLIB);
}

void Mover::CopyTo (MoverData *data)
{																	   
	data->killed						 	  	= killed;
	data->lost									= lost;
	data->positionNormal						= positionNormal;
	data->velocity							    = velocity;                                   

    ArrayCopy(data->name,name);

	data->chassis							    = chassis;                                    
	data->startDisabled							= startDisabled;                              
	data->creationTime							= creationTime;                               
																							
	data->moveType                              = moveType;                                   
	data->moveLevel                             = moveLevel;                                  
	data->followRoads                           = followRoads;                                
																							
    ArrayCopy(data->lastMapCell, lastMapCell);
																							
	data->damageRateTally					    = damageRateTally;                            
	data->damageRateCheckTime				    = damageRateCheckTime;                        
	data->pilotCheckDamageTally 				= pilotCheckDamageTally;                      
																							
    ArrayCopy(data->body, body);

	data->numBodyLocations					    = numBodyLocations;                           
	data->fieldedCV                             = fieldedCV;                                  
																							
	data->attackRange 						    = attackRange;                                
																							
    ArrayCopy(data->armor, armor);

	data->numArmorLocations                     = numArmorLocations;                          

    ArrayCopy(data->longName, longName);
																							
    ArrayCopy(data->inventory,inventory);

	data->numOther                              = numOther;                                   
	data->numWeapons                            = numWeapons;                                 
	data->numAmmos                              = numAmmos;                                   

    ArrayCopy(data->ammoTypeTotal,ammoTypeTotal);

	data->numAmmoTypes					        = numAmmoTypes;                               
	data->pilotHandle                           = pilotHandle;                                
																							
	data->cockpit 								= cockpit;                                    		
	data->engine								= engine;                                     			
	data->lifeSupport							= lifeSupport;                                		
	data->sensor								= sensor;                                     			
	data->ecm									= ecm;                                        		
	data->probe 								= probe;                                      			
	data->jumpJets								= jumpJets;                                   		
	data->nullSignature 						= nullSignature;                              			
	data->maxWeaponEffectiveness    			= maxWeaponEffectiveness;                     			
	data->weaponEffectiveness					= weaponEffectiveness;                        		
																							
	data->minRange 								= minRange;                                   		
	data->maxRange 								= maxRange;                                   		
	data->optimalRange 							= optimalRange;                               		
	data->numFunctionalWeapons 					= numFunctionalWeapons;                       		
																							
	data->numAntiMissileSystems 				= numAntiMissileSystems;                      			
    ArrayCopy(data->antiMissileSystem,antiMissileSystem);

	data->engineBlowTime                        = engineBlowTime;                             
	data->maxMoveSpeed                          = maxMoveSpeed;                               
	data->shutDownThisFrame                     = shutDownThisFrame;                          
	data->startUpThisFrame                      = startUpThisFrame;                           
	data->disableThisFrame                      = disableThisFrame;                           
																							
	data->teamId                                = teamId;                                     
	data->groupId                               = groupId;                                    
	data->squadId                               = squadId;                                    
	data->selectionIndex 					    = selectionIndex;                             
	data->teamRosterIndex 				        = teamRosterIndex;                            
	data->commanderId                           = commanderId;                                
	data->unitGroup 						    = unitGroup;                                  
																							
	data->iconPictureIndex 				        = iconPictureIndex;                           
	data->suppressionFire 				        = suppressionFire;                            
																							
	data->pilotCheckModifier                    = pilotCheckModifier;                         
	data->prevPilotCheckModifier                = prevPilotCheckModifier;                     
	data->prevPilotCheckDelta                   = prevPilotCheckDelta;                        
	data->prevPilotCheckUpdate                  = prevPilotCheckUpdate;                       
	data->failedPilotingCheck 			        = failedPilotingCheck;                        
	data->lastWeaponEffectivenessCalc 	        = lastWeaponEffectivenessCalc;                
	data->lastOptimalRangeCalc 			        = lastOptimalRangeCalc;                       
	data->challengerWID                         = challengerWID;                              
																							
	data->lastGesture                           = lastGesture;                                
																							
	data->control 						        = control;                                    
	data->dynamics 						        = dynamics;                                   
	mc2SaveClearVptr(data->control);
	mc2SaveClearVptr(data->dynamics);
																							
	data->numWeaponHitsHandled                  = numWeaponHitsHandled;                       
	data->timeLeft 						        = timeLeft;                                   
	data->exploding                             = exploding;                                  
	data->withdrawing                           = withdrawing;                                
																							
	data->yieldTimeLeft 					    = yieldTimeLeft;                              
	data->lastValidPosition 				    = lastValidPosition;                          
	data->pivotDirection 					    = pivotDirection;                             
	data->lastHustleTime 					    = lastHustleTime;                             
																							
	data->salvageVehicle                        = salvageVehicle;                             
																							
	data->markDistanceMoved 				    = markDistanceMoved;                          
																							
	data->refitBuddyWID                         = refitBuddyWID;                              
	data->recoverBuddyWID                       = recoverBuddyWID;                            
																							
	data->crashAvoidSelf                        = crashAvoidSelf;                             
	data->crashAvoidPath                        = crashAvoidPath;                             
	data->crashBlockSelf                        = crashBlockSelf;                             
	data->crashBlockPath                        = crashBlockPath;                             
	data->crashYieldTime                        = crashYieldTime;                             
	data->pathLockLength                        = pathLockLength;                             

    ArrayCopy2(data->pathLockList,pathLockList);

	data->moveCenter                            = moveCenter;                                 
	data->moveRadius                            = moveRadius;                                 
																							
	data->overlayWeightClass                    = overlayWeightClass;                         
																							
	data->timeToClearSelection                  = timeToClearSelection;                       
																							
	data->timeSinceMoving                       = timeSinceMoving;                            
																							
	data->timeSinceFiredLast 				    = timeSinceFiredLast;                         
																							
	data->lastMovingTargetWID                   = lastMovingTargetWID;                        
																							
	data->mechSalvage 					        = mechSalvage;                                
																							
	data->teleportPosition 				        = teleportPosition;                           
	data->debugPage                             = debugPage;                                  
																							
	data->pathLocks 						    = pathLocks;                                  
	data->isOnGui 						        = isOnGui;                                    
																							
	data->conStat 						        = conStat;                                    
	data->fadeTime 						        = fadeTime;                                   
	data->alphaValue 						    = alphaValue;                                 
	data->causeOfDeath                          = causeOfDeath;                               
																							
	data->lowestWeaponNodeID                    = lowestWeaponNodeID;                         

	if (getAppearance())
	{
		getAppearance()->getPaintScheme(data->psRed,data->psGreen,data->psBlue);
	}

	GameObject::CopyTo(dynamic_cast<GameObjectData *>(data));
}

void Mover::Load (MoverData *data)
{
	GameObject::Load(data);

	killed						 	  		= data->killed;                                              
	lost						 	  		= data->lost;                                              
	positionNormal							= data->positionNormal;
	velocity							    = data->velocity;                                   

	memcpy(name,data->name, sizeof(char) * MAXLEN_MOVER_NAME);

	chassis							    	= data->chassis;                                    
	startDisabled							= data->startDisabled;                              
	creationTime							= data->creationTime;                               
																						
	moveType                              	= data->moveType;                                   
	moveLevel                             	= data->moveLevel;                                  
	followRoads                           	= data->followRoads;                                
																							
	memcpy(lastMapCell,data->lastMapCell,sizeof(int32_t) * 2);
																							
	damageRateTally					    = data->damageRateTally;                            
	damageRateCheckTime				    = data->damageRateCheckTime;                        
	pilotCheckDamageTally 				= data->pilotCheckDamageTally;                      
																							
	memcpy(body,data->body,sizeof(BodyLocation) * MAX_MOVER_BODY_LOCATIONS);

	numBodyLocations					  = data->numBodyLocations;                           
	fieldedCV                             = data->fieldedCV;                                  
																							
	attackRange 						    = data->attackRange;                                
																							
	memcpy(armor,data->armor, sizeof(ArmorLocation) * MAX_MOVER_ARMOR_LOCATIONS);

	numArmorLocations                     = data->numArmorLocations;                          

	memcpy(longName,data->longName, sizeof(char) * MAXLEN_MECH_LONGNAME);
																							
	memcpy(inventory,data->inventory,sizeof(InventoryItem) * MAX_MOVER_INVENTORY_ITEMS);

	numOther                              = data->numOther;                                   
	numWeapons                            = data->numWeapons;                                 
	numAmmos                              = data->numAmmos;                                   
	memcpy(ammoTypeTotal,data->ammoTypeTotal,sizeof(AmmoTally) * MAX_AMMO_TYPES);
	numAmmoTypes		   		        = data->numAmmoTypes;                               
	pilotHandle                           = data->pilotHandle;                                
																							
	cockpit 							= data->cockpit;                                    		
	engine								= data->engine;                                     			
	lifeSupport							= data->lifeSupport;                                		
	sensor								= data->sensor;                                     			
	ecm									= data->ecm;                                        		
	probe 								= data->probe;                                      			
	jumpJets							= data->jumpJets;                                   		
	nullSignature 						= data->nullSignature;                              			
	maxWeaponEffectiveness    			= data->maxWeaponEffectiveness;                     			
	weaponEffectiveness					= data->weaponEffectiveness;                        		
																							
	minRange 								= data->minRange;                                   		
	maxRange 								= data->maxRange;                                   		
	optimalRange 							= data->optimalRange;                               		
	numFunctionalWeapons 					= data->numFunctionalWeapons;                       		
																							
	numAntiMissileSystems 				= data->numAntiMissileSystems;                      			
	memcpy(antiMissileSystem,data->antiMissileSystem,sizeof(unsigned char) * MAX_ANTI_MISSILE_SYSTEMS);
																							
	engineBlowTime                        = data->engineBlowTime;                             
	maxMoveSpeed                          = data->maxMoveSpeed;                               
	shutDownThisFrame                     = data->shutDownThisFrame;                          
	startUpThisFrame                      = data->startUpThisFrame;                           
	disableThisFrame                      = data->disableThisFrame;                           
																						
	teamId                                = data->teamId;                                     
	groupId                               = data->groupId;                                    
	squadId                               = data->squadId;                                    
	selectionIndex 					      = data->selectionIndex;                             
	teamRosterIndex 				      = data->teamRosterIndex;                            
	commanderId                           = data->commanderId;                                
	unitGroup 						      = data->unitGroup;                                  
																						
	iconPictureIndex 				      = data->iconPictureIndex;                           
	suppressionFire 				      = data->suppressionFire;                            
																						
	pilotCheckModifier                    = data->pilotCheckModifier;                         
	prevPilotCheckModifier                = data->prevPilotCheckModifier;                     
	prevPilotCheckDelta                   = data->prevPilotCheckDelta;                        
	prevPilotCheckUpdate                  = data->prevPilotCheckUpdate;                       
	failedPilotingCheck 			      = data->failedPilotingCheck;                        
	lastWeaponEffectivenessCalc 	      = data->lastWeaponEffectivenessCalc;                
	lastOptimalRangeCalc 			      = data->lastOptimalRangeCalc;                       
	challengerWID                         = data->challengerWID;                              
																						
	lastGesture                           = data->lastGesture;                                
																						
	control 						      = data->control;                                    
	dynamics 						      = data->dynamics;                                   
																						
	numWeaponHitsHandled                  = data->numWeaponHitsHandled;                       
	timeLeft 						      = data->timeLeft;                                   
	exploding                             = data->exploding;                                  
	withdrawing                           = data->withdrawing;                                
																						
	yieldTimeLeft 					      = data->yieldTimeLeft;                              
	lastValidPosition 				      = data->lastValidPosition;                          
	pivotDirection 					      = data->pivotDirection;                             
	lastHustleTime 					      = data->lastHustleTime;                             
																						
	salvageVehicle                        = data->salvageVehicle;                             
																						
	markDistanceMoved 				      = data->markDistanceMoved;                          
																						
	refitBuddyWID                         = data->refitBuddyWID;                              
	recoverBuddyWID                       = data->recoverBuddyWID;                            
																						
	crashAvoidSelf                        = data->crashAvoidSelf;                             
	crashAvoidPath                        = data->crashAvoidPath;                             
	crashBlockSelf                        = data->crashBlockSelf;                             
	crashBlockPath                        = data->crashBlockPath;                             
	crashYieldTime                        = data->crashYieldTime;                             
	pathLockLength                        = data->pathLockLength;                             

	memcpy(pathLockList,data->pathLockList, sizeof(int32_t) * MAX_LOCK_RANGE * 2);

	moveCenter                            = data->moveCenter;                                 
	moveRadius                            = data->moveRadius;                                 
																							
	overlayWeightClass                    = data->overlayWeightClass;                         
																						
	timeToClearSelection                  = data->timeToClearSelection;                       
																						
	timeSinceMoving                       = data->timeSinceMoving;                            
																						
	timeSinceFiredLast 				      = data->timeSinceFiredLast;                         
																						
	lastMovingTargetWID                   = data->lastMovingTargetWID;                        
																						
	mechSalvage 					      = data->mechSalvage;                                
																					
	teleportPosition 				      = data->teleportPosition;                           
	debugPage                             = data->debugPage;                                  
																						
	pathLocks 						      = data->pathLocks;                                  
	isOnGui 						      = data->isOnGui;                                    
																						
	conStat 						      = data->conStat;                                    
	fadeTime 						      = data->fadeTime;                                   
	alphaValue 						      = data->alphaValue;                                 
	causeOfDeath                          = data->causeOfDeath;                               
																						
	lowestWeaponNodeID                    = data->lowestWeaponNodeID;                         

	//OK.  Must get pilotPtr back.
	pilot = MechWarrior::warriorList[pilotHandle];
	if (!pilot)
		STOP(("Could not find pilot for mover after in-mission load"));

	if (getAppearance())
	{
		getAppearance()->resetPaintScheme(data->psRed,data->psGreen,data->psBlue);
	}
}
