// MissionInterfaceManager: attack, jump, guard, aim, air-strike, camera, zoom, rotate and draw-toggle commands, plus move, patrol and force-group orders.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "missiongui_shared_internal.h"
#include "net/runtime.h"
#include "render_view.h"
#include "mc2fmt.h"
#include "camera_elevation.h"
#include <algorithm>
#include <span>
#include <string_view>
#include <vector>


// The menu opens, and dispatches, on exactly what a classic left-click attack (doAttack) accepts:
// not friendly to the home team and not a capture target, alive and not disabled (doAttack's caller gates on !isDisabled).
MissionInterfaceManager::MenuTarget MissionInterfaceManager::classifyMenuTarget(GameObject& obj)
{
    return mc2::order_menu::classify({obj.isDestroyed(), obj.isDisabled(), obj.isCaptureable(Team::home->getId()),
                                      Team::home->isFriendly(obj.getTeam())}) == mc2::order_menu::TargetClass::Attackable
        ? MenuTarget::Attackable : MenuTarget::Rejected;
}

// Walks the home roster in order and keeps the movers in `wanted` (null: every selected one), as doAttack always has.
static void collectHomeMovers(std::vector<Mover*>& out, const std::vector<std::uint32_t>* wanted)
{
	for (long i = 0; i < Team::home->getRosterSize(); i++)
	{
		Mover* pMover = static_cast<Mover*>(Team::home->getMover( i ));
		if (!commanderIsHome(pMover->getCommander())) continue;
		if (wanted) {
			if (std::find(wanted->begin(), wanted->end(), pMover->getWatchID()) == wanted->end()) continue;
			if (pMover->isDestroyed() || pMover->isDisabled()) continue;
		} else if (!pMover->isSelected()) continue;
		out.push_back(pMover);
	}
}

void orderMenuLogLine(std::string_view text); // missiongui_style.cpp

void MissionInterfaceManager::dispatchMenuOrder(const mc2::order_menu::Order& captured)
{
    ZoneScopedN("MIF.OrderMenuDispatch");
    auto* enemy = ObjectManager->getByWatchID(captured.target);
    if (!enemy || classifyMenuTarget(*enemy) != MenuTarget::Attackable) return; // one shared predicate with the menu gate
    long range = -1; // Attack (default): each mover's own attackRange, as classic doAttack
    switch (captured.range) {
    case mc2::order_menu::Range::Short: range = FIRERANGE_SHORT; break;
    case mc2::order_menu::Range::Medium: range = FIRERANGE_MEDIUM; break;
    case mc2::order_menu::Range::Long: range = FIRERANGE_LONG; break;
    default: break;
    }
    std::vector<Mover*> recipients;
    collectHomeMovers(recipients, &captured.recipients);
    if (mc2_diag::tagRequested("ORDER_MENU")) {
        char line[128];
        std::snprintf(line, sizeof line, "dispatch recipients=%zu issued=%zu target=%u range=%ld",
            captured.recipients.size(), recipients.size(), static_cast<unsigned>(captured.target), range);
        orderMenuLogLine(line);
    }
    if (recipients.empty()) return;
    issueAttack(*enemy, recipients, range);
}

// Classic left-click attack: same guard, same recipients in roster order, same behaviour as before the menu existed.
void MissionInterfaceManager::doAttack()
{
	if ( userInput->getKeyDown( WAYPOINT_KEY ) || !target )
	{
		soundSystem->playDigitalSample( INVALID_GUI );
		return; // don't do if in waypoint mode
	}

	// TARGETING-CURRENTNESS-GUARD-1 (non-fatal, recon — FRAME-CURRENTNESS-GUARDS-1).
	// Consumer-boundary observer: at the instant an attack order is about to issue,
	// measure whether `target` is actually current — its watch-ID still resolves to
	// the same object, it projects in front of the camera, and it lands near the
	// cursor. This is the "eligible across the whole map" symptom (a stale/offscreen
	// target accepted because doAttack only checks `!target` + friendly). No behavior
	// change; this only counts/traces so we can prove whether bad orders actually
	// issue before adding any suppression. Gate MC2_TARGETING_GUARD +
	// MC2_DIAG_TAGS=TARGETING. Read via get_diagnostic_events("TARGETING").
	{
		static const bool s_tgtGuard = false;
		if ( s_tgtGuard && eye && mc2_diag::tagEnabled("TARGETING") )
		{
			static uint64_t s_tg_orders = 0, s_tg_staleWID = 0, s_tg_behind = 0, s_tg_far = 0;
			++s_tg_orders;
			const bool _tg_staleWID = (ObjectManager->getByWatchID( target->getWatchID() ) != target);
			glm::vec3 _tg_pos = target->getPosition();
			Stuff::Vector4D _tg_sp;
			eye->projectForScreenXY( _tg_pos, _tg_sp );
			const bool  _tg_behind = (_tg_sp.w <= 1e-4f);
			const float _tg_dx = _tg_sp.x - static_cast<float>(mouseX);
			const float _tg_dy = _tg_sp.y - static_cast<float>(mouseY);
			const float _tg_distSq = _tg_dx*_tg_dx + _tg_dy*_tg_dy;
			const bool  _tg_far = (_tg_distSq > 90000.0f);   // >300px from cursor
			if (_tg_staleWID) ++s_tg_staleWID;
			if (_tg_behind)   ++s_tg_behind;
			if (_tg_far)      ++s_tg_far;
			if (_tg_staleWID || _tg_behind || _tg_far)
			{
				char _tg_buf[384];
				mc2::fmt::to_buf(_tg_buf, "{{\"site\":\"doAttack\",\"stale_wid\":{:d},\"behind\":{:d},\"far_cursor\":{:d},\"target_screen\":[{:.1f},{:.1f}],\"w\":{:.4f},\"mouse\":[{:d},{:d}],\"dist_sq\":{:.0f},\"is_mover\":{:d},\"watch_id\":{:d},\"totals\":{{\"orders\":{:d},\"stale_wid\":{:d},\"behind\":{:d},\"far\":{:d}}}}}", mc2::fmt::as_i32(_tg_staleWID?1:0), mc2::fmt::as_i32(_tg_behind?1:0), mc2::fmt::as_i32(_tg_far?1:0), _tg_sp.x, _tg_sp.y, _tg_sp.w, mc2::fmt::as_i32(static_cast<long>(mouseX)), mc2::fmt::as_i32(static_cast<long>(mouseY)), _tg_distSq, mc2::fmt::as_i32(target->isMover()?1:0), mc2::fmt::as_i32(static_cast<int>(target->getWatchID())), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_orders)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_staleWID)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_behind)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_tg_far)));
				mc2_diag::writeEvent("TARGETING", 1, 0, _tg_buf);
			}
		}
	}

	std::vector<Mover*> recipients;
	collectHomeMovers(recipients, nullptr);
	issueAttack(*target, recipients, -1);
}

// rangeOverride: -1 = each mover's own attackRange (classic); else a FireRangeType (order menu).
void MissionInterfaceManager::issueAttack(GameObject& tgt, std::span<Mover* const> recipients, long rangeOverride)
{

	TacticalOrder tacOrder;
	bool bCapture =  tgt.isCaptureable(Team::home->getId());
	bool bFireFromCurrentPos = controlGui.getCurrentRange() == FIRERANGE_CURRENT ? 1 : 0;
	if ( controlGui.getFireFromCurrentPos() )
		bFireFromCurrentPos = true;

	if (!bCapture)
	{
		if (!Team::home->isFriendly( tgt.getTeam() ))
		{
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT );
			tacOrder.targetWID = tgt.getWatchID();
			tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
			tacOrder.attackParams.pursue = !bFireFromCurrentPos;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;

			tgt.getAppearance()->flashBuilding(1.3f,0.2f,0xffff0000);
		}
		else
		{
			//Do NOT Attack friendlies unless force Fire.
			// That TacOrder is issued elsewhere!!
			return;
		}
	}
	else
	{
		for (Mover* pMover : recipients)
		{
			{
				if ((pMover->getObjectClass() == BATTLEMECH) && 
					(pMover->getMoveType() == MOVETYPE_GROUND))
				{
					if ( tgt.getCaptureBlocker(pMover) )
					{
						soundSystem->playDigitalSample( INVALID_GUI );
						return;
					}
				}
				else
				{
					//Can't capture player has at least one support thing selected
					// At least until design tells me what they REALLY want!
					// -fs
					soundSystem->playDigitalSample( INVALID_GUI );
					return;
				}
			}
		}
		
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_CAPTURE );
		tacOrder.targetWID = tgt.getWatchID();
		tacOrder.attackParams.type = ATTACK_NONE;
		tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
		tacOrder.attackParams.pursue = true;
		tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	}
	
	soundSystem->playDigitalSample(BUTTON5);

	for (Mover* pMover : recipients)
	{
		{
			tacOrder.attackParams.range = static_cast<FireRangeType>(rangeOverride >= 0 ? rangeOverride : pMover->attackRange);
			if (tacOrder.attackParams.range == FIRERANGE_CURRENT)
				tacOrder.attackParams.pursue = false;
			else if (controlGui.getFireFromCurrentPos())
				tacOrder.attackParams.pursue = false;
			else
				tacOrder.attackParams.pursue = true;
			tacOrder.pack(nullptr, nullptr);

			if (MPlayer && !MPlayer->isServer())
				MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
			else
				pMover->handleTacticalOrder(tacOrder);
		}
	}

	controlGui.setFireFromCurrentPos( 0 );
}

int MissionInterfaceManager::attackShort()
{
	controlGui.setRange( FIRERANGE_SHORT );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::attackMedium()
{
	controlGui.setRange( FIRERANGE_MEDIUM );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::attackLong()
{
	controlGui.setRange( FIRERANGE_LONG );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::alphaStrike()
{
	return 0;
}
int MissionInterfaceManager::defaultAttack()
{
	
	controlGui.setRange( FIRERANGE_OPTIMAL );

	return 0;
}

int  MissionInterfaceManager::jump()
{
	if ( !canJump() )
	{
		soundSystem->playDigitalSample( INVALID_GUI );
		return 0;
	}
	
	if ( !controlGui.getJump() )
		controlGui.toggleJump();
	return 0;
}

int MissionInterfaceManager::stopJump()
{
	if ( controlGui.getJump() )
		controlGui.toggleJump();
	return 1;

}
void MissionInterfaceManager::doJump()
{
	bool passable = 0;

	if ( !target )
	{
		if (Terrain::IsGameSelectTerrainPosition(wPos))
		{
			int cellR, cellC;
			land->worldToCell(wPos, cellR, cellC);
			passable = GameMap->getPassable(cellR,cellC);
		}
			
		bool canJump = canJumpToWPos();	
		if (canJump)
		{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_JUMPTO_POINT, false);
		
			tacOrder.moveParams.wait = false;
			// The tick ingress copies only counted waypoints. setWayPoint alone
			// left numPoints at zero and admission rejected order_goal_missing.
			tacOrder.addWayPoint(wPos, TRAVEL_MODE_JUMP);
			tacOrder.pack(nullptr, nullptr);

			handleOrders(tacOrder);

			controlGui.setDefaultSpeed();
			
			soundSystem->playDigitalSample(BUTTON5);
			controlGui.setDefaultSpeed();
		}
		else
		{
			userInput->setMouseCursor(mState_DONT);
			soundSystem->playDigitalSample( INVALID_GUI );
		}
		
			
	}
	else
	{
		userInput->setMouseCursor(mState_DONT);
		soundSystem->playDigitalSample( INVALID_GUI );
	}
	
}
		
int MissionInterfaceManager::fireFromCurrentPos()
{
	if ( !controlGui.getFireFromCurrentPos() )
		controlGui.toggleFireFromCurrentPos( );

	return forceShot();
}
int MissionInterfaceManager::stopFireFromCurrentPos()
{
	if ( controlGui.getFireFromCurrentPos() )
		controlGui.toggleFireFromCurrentPos();
	return 1;
}

void MissionInterfaceManager::doGuard(GameObject* who)
{
	if ( who )
	{
		TacticalOrder tacOrder;
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
		tacOrder.targetWID = who->getWatchID();
		tacOrder.pack(nullptr, nullptr);	
		handleOrders( tacOrder );

		controlGui.setDefaultSpeed();
	}
	else
	{
		LocationNode path;
		path.location = wPos;
		path.run = true;
		path.next = nullptr;

		TacticalOrder tacOrder;
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
		tacOrder.initWayPath( &path );
		tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
		tacOrder.pack(nullptr, nullptr);	
		handleOrders( tacOrder );

		controlGui.setDefaultSpeed();
	}

	soundSystem->playDigitalSample(BUTTON5);
}
int MissionInterfaceManager::guard()
{
	if ( !controlGui.getGuard() )
		controlGui.toggleGuard();
	return 0;
}
int MissionInterfaceManager::stopGuard()
{
	if ( controlGui.getGuard() )
		controlGui.toggleGuard();
	return 0;
}
int MissionInterfaceManager::conserveAmmo()
{
	return 0;
}
int MissionInterfaceManager::selectVisible()
{
	Team* pTeam = Team::home;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if (commanderIsHome(pMover->getCommander()))
		{
			pMover->setSelected( false );
		}
	}
		
	return addVisibleToSelection();

}

int MissionInterfaceManager::addVisibleToSelection()
{
	dragStart = glm::vec3(0.0f);		
	dragEnd = eye->getScreenRes();
	
	for (int i=0;i<ObjectManager->getNumMovers();i++)
	{
		if (ObjectManager->moverInRect(i,dragStart,dragEnd))
		{
			GameObjectPtr	mover = ObjectManager->getMover(i);
			if (mover && (mover->getCommanderId() == Commander::home->getId()) && !mover->isDisabled())
			{
				mover->setSelected( true );
			}
		}
	}

	dragStart = glm::vec3(0.0f);
	dragEnd = glm::vec3(0.0f);

	return 1;
}
int MissionInterfaceManager::aimLeg()
{
	if ( target )
	{
		BattleMech* pMech = dynamic_cast<BattleMech*>(target );
		
		if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
		{
			if (!anySelectedWithoutAreaEffect())
			{
				//Can't aim shot.  No weapon which is not area effect available!
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else if ( pMech->body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED 
				&& pMech->body[MECH_BODY_LOCATION_RLEG].damageState == IS_DAMAGE_DESTROYED )
			{
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else  if ( !commandClicked )// cursor already set above, however if in doubt 
				return 1;
			else
			{
				TacticalOrder tacOrder;
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
				tacOrder.targetWID = target->getWatchID();
				tacOrder.attackParams.type = ATTACK_TO_DESTROY;
				tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
				tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
				tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
				tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				
				bool bRight = pMech->body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED;
				tacOrder.attackParams.aimLocation = bRight ? MECH_BODY_LOCATION_RLEG :  MECH_BODY_LOCATION_LLEG;
				tacOrder.pack( nullptr, nullptr);
				handleOrders( tacOrder );

				soundSystem->playDigitalSample(BUTTON5);

				bAimedShot = true;
				return 1;
			}
			
		}
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}

	return 0;
}

int MissionInterfaceManager::aimArm()
{
	BattleMech* pMech = dynamic_cast<BattleMech*>(target);
		
	if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
	{
		if ( anySelectedWithoutAreaEffect() && commandClicked )
		{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
			tacOrder.targetWID = target->getWatchID();
			tacOrder.attackParams.type = ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
			tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
			bool bRight = pMech->body[MECH_BODY_LOCATION_LARM].damageState == IS_DAMAGE_DESTROYED;
			tacOrder.attackParams.aimLocation = bRight ? MECH_BODY_LOCATION_RARM :  MECH_BODY_LOCATION_LARM;
			tacOrder.pack( nullptr, nullptr);
			handleOrders( tacOrder );

			soundSystem->playDigitalSample(BUTTON5);

			bAimedShot = true;
			return 1;
		}

		if (!anySelectedWithoutAreaEffect())
		{
			//Can't aim shot.  No weapon which is not area effect available!
			userInput->setMouseCursor( mState_DONT );
			return 1;		
		}
		else if ( pMech->body[MECH_BODY_LOCATION_LARM].damageState == IS_DAMAGE_DESTROYED 
			&& pMech->body[MECH_BODY_LOCATION_RARM].damageState == IS_DAMAGE_DESTROYED )
		{
			userInput->setMouseCursor( mState_DONT );
			return 1;		
		}
		else // cursor already set above, however if in doubt 
			return 1;
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}
}

int MissionInterfaceManager::aimHead()
{
	if ( target )
	{
		BattleMech* pMech = dynamic_cast<BattleMech*>(target);
		
		if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
		{
			if (anySelectedWithoutAreaEffect() && commandClicked )
			{
				TacticalOrder tacOrder;

				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
				tacOrder.targetWID = target->getWatchID();
				tacOrder.attackParams.type = ATTACK_TO_DESTROY;
				tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
				tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
				tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
				tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				tacOrder.attackParams.aimLocation = MECH_BODY_LOCATION_HEAD;
				tacOrder.pack( nullptr, nullptr);
				handleOrders( tacOrder );

				bAimedShot = true;
				soundSystem->playDigitalSample(BUTTON5);
			}
			else if (!anySelectedWithoutAreaEffect())
			{
				//Can't aim shot.  No weapon which is not area effect available!
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else if ( pMech->body[MECH_BODY_LOCATION_HEAD].damageState == IS_DAMAGE_DESTROYED )
			{
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else // cursor already set above, however if in doubt 
				return 1;
		}
		else
		{
			userInput->setMouseCursor( mState_DONT );
			return 1;
		}
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}

	return 0;
}

int MissionInterfaceManager::removeCommand()
{
	controlGui.doStop();
	return 1;
}
int MissionInterfaceManager::powerUp()
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERUP, true);
	tacOrder.pack(nullptr, nullptr);					
	handleOrders( tacOrder );
	return 1;
}
int MissionInterfaceManager::powerDown()
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERDOWN, true);
	tacOrder.pack(nullptr, nullptr);
	handleOrders( tacOrder );
	return 1;
					
}
int MissionInterfaceManager::changeSpeed()
{
 	if ( controlGui.isDefaultSpeed() )
		controlGui.toggleDefaultSpeed();
	return 0;
}

int MissionInterfaceManager::stopChangeSpeed()
{
	if ( !controlGui.isDefaultSpeed() )
		controlGui.toggleDefaultSpeed();
	return 1;
}
int MissionInterfaceManager::eject()
{
	if ( target && target->isMover() && target->getCommanderId() == Commander::home->getId() && !invulnerableON)
	{
		if (  commandClicked )
		{
			doEject( target );
		}

		userInput->setMouseCursor( mState_EJECT );
	
	}
	else if ( !controlGui.forceGroupBar.inRegion( userInput->getMouseX(), userInput->getMouseY() ) )
		userInput->setMouseCursor( mState_XEJECT );
	return 1;
}

int MissionInterfaceManager::bigAirStrike()
{
	if (mc2::net::active()) return 0;
	glm::vec3 v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_LARGE,&v,nullptr);
		soundSystem->playSupportSample(SUPPORT_AIRSTRIKE);
	return 1;
}
int MissionInterfaceManager::smlAirStrike()
{
	if (mc2::net::active()) return 0;
	glm::vec3 v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_SMALL,&v,nullptr);
		soundSystem->playSupportSample(SUPPORT_AIRSTRIKE);
	return 1;
}
int MissionInterfaceManager::snsAirStrike()
{
	if (mc2::net::active()) return 0;
	glm::vec3 v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_SENSOR,&v,nullptr);
		soundSystem->playSupportSample(SUPPORT_PROBE);
	return 1;
}

glm::vec3 MissionInterfaceManager::makeAirStrikeTarget( const glm::vec3& pos )
{
	glm::vec3 newPos = pos;
/*	bool 	lineOfSight = Team::home->teamLineOfSight(pos);
	if ( !lineOfSight )
	{
		// need to offset x, y, random amounts up to five tiles... 
		long randX = rand();
		long randY = rand();
		long offsetX = randX % (128*5);
		long offsetY = randY % (128*5);
		if ( randX % 2 )
			offsetX = -offsetX;
		if ( randY % 2 )
			offsetY = -offsetY;

		newPos.x += offsetX;
		newPos.y += offsetY;
	}*/

	return newPos;
}

int MissionInterfaceManager::cameraAssign0()
{
	Camera::cameraZoom[0] = eye->cameraAltitude;
	Camera::cameraTilt[0] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign1()
{
	Camera::cameraZoom[1] = eye->cameraAltitude;
	Camera::cameraTilt[1] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign2()
{
	Camera::cameraZoom[2] = eye->cameraAltitude;
	Camera::cameraTilt[2] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign3()
{
	Camera::cameraZoom[3] = eye->cameraAltitude;
	Camera::cameraTilt[3] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraNormal()
{
	if (eye)
		eye->allNormal();

	return 1;
}


int MissionInterfaceManager::cameraDefault()
{
	if (eye)
		eye->setCameraView(0);
 		
	return 1;
}
int MissionInterfaceManager::cameraMaxIn()
{
	if (eye)
		eye->setCameraView(1);

	return 1;
}
int MissionInterfaceManager::cameraMaxOut()
{
	if (eye)
		eye->ZoomMin();

	return 1;
}
int MissionInterfaceManager::cameraTight()
{
	if (eye)
		eye->setCameraView(2);
		
 	return 1;
}
int MissionInterfaceManager::cameraFour()
{
	if (eye)
		eye->setCameraView(3);
		
	return 1;
}

bool MissionInterfaceManager::anySelectedWithoutAreaEffect (void)
{
	//Scan the list of selected movers and see if any have at least one non-area effect weapon
	// Used for Called shots which cannot be called if weapon is area effect!!
	Team* pTeam = Team::home;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pMover->isSelected() && pMover->hasNonAreaWeapon() && commanderIsHome(pMover->getCommander()))
		   	return true;
	}

	return false;
}

int MissionInterfaceManager::handleOrders( TacticalOrder& order)
{
	Team* pTeam = Team::home;

	bool isMoveOrder = false;
	glm::vec3 moveGoals[MAX_MOVERS];
	long numMovers = 0;
	if (order.code == TACTICAL_ORDER_JUMPTO_POINT) {
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* pMover = pTeam->getMover(i);
			if (pMover->isSelected() && commanderIsHome(pMover->getCommander()))
				numMovers++;
		}
		MoverGroup::calcJumpGoals(order.getWayPoint(0), numMovers, moveGoals, nullptr);
		isMoveOrder = true;
		}
	else if (order.code == TACTICAL_ORDER_MOVETO_POINT) {
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* pMover = pTeam->getMover(i);
			if (pMover->isSelected() && commanderIsHome(pMover->getCommander()))
				numMovers++;
		}
		MoverGroup::calcMoveGoals(order.getWayPoint(0), numMovers, moveGoals);
		isMoveOrder = true;
	}

	numMovers = 0;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pMover->isSelected() && commanderIsHome(pMover->getCommander()) )
		{
			if (isMoveOrder ) {
				order.setWayPoint(0, moveGoals[numMovers++]);
				order.pack(nullptr, nullptr);
			}
			if (MPlayer && !MPlayer->isServer()) {
				MPlayer->sendPlayerOrder(&order, false, 1, &pMover, 0, nullptr, userInput->getKeyDown(WAYPOINT_KEY));
				}
			else
			{
				order.attackParams.range = static_cast<FireRangeType>(pMover->attackRange);
				order.pack( nullptr, nullptr );
				if ( userInput->getKeyDown( WAYPOINT_KEY ) ) // way point one
				{
					pMover->handleTacticalOrder(order, 0, true);
				}
				else if ( controlGui.getMines() )
				{
					pMover->handleTacticalOrder(order, 0, true);
				}
				else
					pMover->handleTacticalOrder(order);
			}

		}
	}

	return 1;
}

int MissionInterfaceManager::scrollUp()
{
	float scrollFactor = mc2cam::panStep(scrollInc, eye->getScaleFactor(), frameLength, baseFrameLength);
	eye->moveUp(scrollFactor);
	return 1;
}

int MissionInterfaceManager::scrollDown()
{
	float scrollFactor = mc2cam::panStep(scrollInc, eye->getScaleFactor(), frameLength, baseFrameLength);
	eye->moveDown(scrollFactor);
	return 1;
}
int MissionInterfaceManager::scrollLeft()
{
	float scrollFactor = mc2cam::panStep(scrollInc, eye->getScaleFactor(), frameLength, baseFrameLength);
	eye->moveLeft(scrollFactor);
	return 1;
}
int MissionInterfaceManager::scrollRight()
{
	float scrollFactor = mc2cam::panStep(scrollInc, eye->getScaleFactor(), frameLength, baseFrameLength);
	eye->moveRight(scrollFactor);
	return 1;
}
// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] Mouse-anchored zoom helper. Gated by
// MC2_LOWCAM_ZOOM_ANCHOR (default OFF -> legacy fixed-pivot zoom unchanged).
// Capture cursor world point A, scroll-target T and pre-step altitude h0 BEFORE
// the zoom, run the supplied legacy zoom (calling the member fn), then shift the
// scroll target so A stays under the cursor: T_new = T + (A - T)*(1 - h1/h0).
namespace {
	bool lowcamZoomAnchorEnabled()
	{
		static int cached = -1;
		if (cached < 0)
		{
			// Default OFF: anchored zoom still reads as a pan in practice; use the
			// legacy fixed-pivot wheel zoom. Opt in with MC2_LOWCAM_ZOOM_ANCHOR=1.
			cached = false;
		}
		return cached != 0;
	}
}

void MissionInterfaceManager::anchoredZoom(long mx, long my, int (MissionInterfaceManager::*zoomFn)())
{
	if (!lowcamZoomAnchorEnabled() || !eye || !eye->getUsePerspective())
	{
		(this->*zoomFn)();
		return;
	}

	// [MOUSE-ANCHORED-ZOOM-1 v2] The prior single-shot position shift PANNED:
	// it used h1=cameraAltitudeDesired (the instant setpoint) but the camera's
	// ACTUAL altitude eases toward it over ~5 frames (Camera::update), so the
	// pivot snapped the full distance on frame 0 while altitude barely moved.
	// Fix: capture cursor-world A, the ACTUAL altitude h0, and pivot T0 here,
	// then let Camera::update() shift the pivot each frame LOCKED to the eased
	// altitude (frac = 1 - cameraAltitude/h0) until it settles. ZOOMs, no pan.
	glm::vec3 A;
	bool haveA = eye->screenToTerrainApprox(mx, my, A);

	(this->*zoomFn)();   // updates cameraAltitudeDesired (the zoom target)

	// Fallback: cursor->world failed -> leave legacy fixed-pivot zoom, no jump.
	if (!haveA)
		return;
	eye->beginZoomAnchor(A, eye->getCameraAltitude(), eye->getPosition());
}

int MissionInterfaceManager::zoomOut()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomOut(zoomInc * frameFactor * eye->getScaleFactor());
	return 1;
}
int MissionInterfaceManager::zoomIn()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomIn(zoomInc * frameFactor * eye->getScaleFactor());
	return 1;
}
int MissionInterfaceManager::zoomChoiceOut()
{
	eye->ZoomOut(mc2cam::wheelZoomAmount(zoomInc, baseFrameLength, eye->getScaleFactor()));

	return 1;
}
int MissionInterfaceManager::zoomChoiceIn()
{
	eye->ZoomIn(mc2cam::wheelZoomAmount(zoomInc, baseFrameLength, eye->getScaleFactor()));
 	return 1;
}
int MissionInterfaceManager::rotateLeft()
{
	float frameFactor = frameLength / baseFrameLength;
	// [CAMERA-SMOOTHING-1] Legacy held-rotate is QUANTIZED: the accumulator
	// fires an instant 11.25-deg (RotationIncrement) snap every time it
	// crosses the threshold (~every 2-3 frames at 60-80fps) — the visible
	// rotation jerk. With smoothing on, feed the same average rate
	// (degPerSecRot per baseFrameLength) continuously; Camera's exponential
	// smoothing shapes onset/release. Knob=0 keeps the legacy snaps verbatim.
	if (Camera::inputSmoothingEnabled())
	{
		eye->rotateLeft(degPerSecRot * frameFactor);
		return 1;
	}
	realRotation += degPerSecRot * frameFactor;
	if (realRotation >= rotationInc)
	{
		eye->rotateLeft(rotationInc);
		realRotation = 0;
	}

	return 1;
}
int MissionInterfaceManager::rotateRight()
{
	float frameFactor = frameLength / baseFrameLength;
	if (Camera::inputSmoothingEnabled())
	{
		eye->rotateRight(degPerSecRot * frameFactor);
		return 1;
	}
	realRotation -= degPerSecRot * frameFactor;
	if (realRotation <= -rotationInc)
	{
		eye->rotateRight(rotationInc);
		realRotation = 0;
	}

	return 1;
}
int MissionInterfaceManager::tiltUp()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->tiltDown(scrollFactor * 15.0f);
	return 1;
}
int MissionInterfaceManager::tiltDown()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->tiltUp(scrollFactor * 15.0f);
	return 1;
	
}
int MissionInterfaceManager::centerCamera()
{
	return 0;
}
int MissionInterfaceManager::rotateLightLeft()
{
	#ifndef FINAL
	eye->rotateLightLeft(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightRight()
{
	#ifndef FINAL
	eye->rotateLightRight(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightUp()
{
	#ifndef FINAL
	eye->rotateLightUp(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightDown()
{
	#ifndef FINAL
	eye->rotateLightDown(rotationInc);
	#endif
	return 1;
}

int MissionInterfaceManager::drawTerrain()
{
	#ifndef FINAL
	drawTerrainTiles ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::drawOverlays()
{
	#ifndef FINAL
	drawTerrainOverlays ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::drawBuildings()
{
	#ifndef FINAL
	renderObjects ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::showGrid()
{
	#ifndef FINAL
	drawTerrainGrid = !drawTerrainGrid;
	#endif
	return 1;
}
int MissionInterfaceManager::recalcLights()
{
	#ifndef FINAL
	Terrain::recalcLight ^= TRUE;
	Terrain::recalcShadows = false;
	#endif
	return 1;
}
int MissionInterfaceManager::drawFog()
{
	#ifndef FINAL
	useFog ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::usePerspective()
{
	#ifndef FINAL
	eye->usePerspective ^= true;
	#endif
	return 1;
}
int MissionInterfaceManager::toggleGUI()
{
	#ifndef FINAL
	drawGUIOn ^= true;
	toggledGUI = drawGUIOn;
	#endif
	return 1;
}
int MissionInterfaceManager::drawTGLShapes()
{
	#ifndef FINAL
	renderTGLShapes ^= true;
	terrainLineChanged = turn;
	#endif
	return 1;
}
int MissionInterfaceManager::drawWaterEffects()
{
	#ifndef FINAL
	useWaterInterestTexture ^= true;
	terrainLineChanged = turn;
	#endif
	return 1;
}
int MissionInterfaceManager::recalcWater()
{
	#ifndef FINAL
	Terrain::mapData->recalcWater();
	#endif
	return 1;
}
int MissionInterfaceManager::drawShadows()
{
	#ifndef FINAL
	useShadows ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::changeLighting()
{
	#ifndef FINAL
	drawLOSGrid ^= true;
	#endif
	return 1;
}

void MissionInterfaceManager::doMove(const glm::vec3& pos)
{
	/*if ( controlGui.getGuardTower()  )
	{
		doGuardTower();
		return;
	}*/
	if ( controlGui.isAddingAirstrike() &&  controlGui.isButtonPressed( ControlGui::SENSOR_PROBE ))
	{
		glm::vec3 tmp = wPos;
		wPos = pos;
		addAirstrike( );
		wPos = tmp;

		return;
	}
	
	LocationNode path;
	path.location = pos;
	path.run = true;
	path.next = nullptr;

	TacticalOrder tacOrder;
	if ( controlGui.getJump() )
	{
		doJump();
		return;
	}
	else
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
	tacOrder.initWayPath(&path);
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.wayPath.mode[0] =  controlGui.getWalk() ? TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	if ( controlGui.getMines() )
		tacOrder.moveParams.mode = MOVE_MODE_MINELAYING;
	tacOrder.pack(nullptr, nullptr);


	handleOrders(tacOrder);
	
	soundSystem->playDigitalSample(BUTTON5);
	// Reuse the stock spinning waypoint art for ordinary move-click feedback.
	if (!moveOrderMarker) {
		auto* type = appearanceTypeList->getAppearance(BLDG_TYPE << 24,
			controlGui.getWalk() ? "WalkWayPoint" : "RunWayPoint");
		moveOrderMarker = std::make_unique<BldgAppearance>();
		moveOrderMarker->init(type);
		moveOrderMarker->setIsHudElement();
	}
	moveOrderMarker->setObjectParameters(pos, moveOrderMarker->rotation, false, 0, 0);
	moveOrderMarkerUntil = mc2::render_view::clock().scenarioTime + 2.0f;
	if (mc2_diag::tagRequested("MOVE_MARKER"))
		mc2::fmt::print_out("[MOVE_MARKER] order pos={},{},{} until={}\n", pos.x, pos.y, pos.z, moveOrderMarkerUntil);

	controlGui.setDefaultSpeed();
}

bool MissionInterfaceManager::makePatrolPath()
{
	if (mc2::net::active()) return false;
	Team* pTeam = Team::home;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pMover->isSelected() && pMover->getPilot()->getNumTacOrdersQueued() && commanderIsHome(pMover->getCommander()) )
		{
			if ( pMover->isCloseToFirstTacOrder( wPos ) )
			{
				userInput->setMouseCursor( mState_LINK );
				if ( commandClicked )
				{
					pMover->getPilot()->setTacOrderQueueLooping( true );
				}

				return 1;
			}
			else 
			{
				glm::vec3 tmp;
				tmp = wPos - pMover->getPosition();
				if ( glm::length(tmp) < 25 ) // random amount
				{
					userInput->setMouseCursor( mState_LINK );
					if ( commandClicked )
					{
						doMove(wPos);
						pMover->getPilot()->setTacOrderQueueLooping( true );
					}
					return 1;
				}
			}
			
		}
	}

	return 0;
}

int MissionInterfaceManager::forceShot()
{
	bForcedShot = true;

	userInput->setMouseCursor( mState_GENERIC_ATTACK );
	if ( commandClicked )
	{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER,  target ? TACTICAL_ORDER_ATTACK_OBJECT : TACTICAL_ORDER_ATTACK_POINT);
			tacOrder.targetWID = target ? target->getWatchID() : 0;
			if ( !target )
				tacOrder.attackParams.targetPoint = wPos;
			tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.range = static_cast<FireRangeType>(FIRERANGE_OPTIMAL);
			tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
			
			soundSystem->playDigitalSample(BUTTON5);

			if (target)
				target->getAppearance()->flashBuilding(1.3f,0.2f,0xffff0000);

			handleOrders( tacOrder );
			return 1;
	}

	return 0;

	

}

void MissionInterfaceManager::selectForceGroup( int forceGroup, bool deselect )
{
	Team* pTeam = Team::home;
	bool bAllSelected = deselect ? false : true;
	if ( deselect )
	{

		for (long i=0;i<pTeam->getRosterSize();i++)
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if (pMover && pMover->isOnGUI() && (commanderIsHome(pMover->getCommander())))
			{
				pMover->setSelected( 0 );
			}
		}					
	}
	else // see if the whole force group is already seleced
	{
		for (long i=0;i<pTeam->getRosterSize();i++)
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if (pMover && pMover->isInUnitGroup( forceGroup ) && !pMover->isDisabled() && !pMover->isSelected() && pMover->isOnGUI() && (commanderIsHome(pMover->getCommander())))
			{
				bAllSelected = false;
			}
		}
	}

	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if (pMover && pMover->isInUnitGroup( forceGroup ) && !pMover->isDisabled() && pMover->isOnGUI() && (commanderIsHome(pMover->getCommander())))
			pMover->setSelected( !bAllSelected );
	}
}
void MissionInterfaceManager::makeForceGroup( int forceGroup )
{
	if (mc2::net::active()) return;
	Team* pTeam = Team::home;

	for (int i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if (pMover && (commanderIsHome(pMover->getCommander())))
		{
			if ( pMover->isInUnitGroup( forceGroup ) )
				pMover->removeFromUnitGroup( forceGroup );
		}
	}					

	for (int i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if (pMover && (commanderIsHome(pMover->getCommander())))
		{
			if (pMover->isSelected() )
				pMover->addToUnitGroup( forceGroup );
		}
	}					
}
