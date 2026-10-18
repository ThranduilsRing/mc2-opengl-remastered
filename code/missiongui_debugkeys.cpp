// MissionInterfaceManager: debug windows and cheats, damage keys, hotkey save/load and the chat keys.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include "mc2fmt.h"


int MissionInterfaceManager::quickDebugInfo() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		int row, col;
		land->worldToCell(wPos, row, col);
		char debugString[256];
		// NULL guards for bail'd-out GlobalMoveMap[] (mod content).
		int infoML = target ? target->getMoveLevel() : 0;
		int infoArea = (GlobalMoveMap[infoML]) ? GlobalMoveMap[infoML]->calcArea(row, col) : -1;
		int infoArea0 = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->calcArea(row, col) : -1;
		if (target)
			mc2::fmt::to_buf(debugString, "INFO = {}({:c}{:c}) {:d}(W{:d}) [{:d}, {:d}] ({:.4f}, {:.4f}, {:.4f}) #Pth={:d}({:d}) OBJ = {}\n", mc2::fmt::safe(terrainStr[GameMap->getTerrain(row, col)]), mc2::fmt::as_char(GameMap->getPassable(row, col) ? 'T' : 'F'), mc2::fmt::as_char(GameMap->getForest(row, col) ? 'T' : 'F'), mc2::fmt::as_i32(infoArea), mc2::fmt::as_i32(GameMap->getShallowWater(row, col) ? 1 : (GameMap->getDeepWater(row, col) ? 2 : 0)), mc2::fmt::as_i32(row), mc2::fmt::as_i32(col), wPos.x, wPos.y, wPos.z, mc2::fmt::as_i32(PathManager->numPaths), mc2::fmt::as_i32(PathManager->peakPaths), mc2::fmt::safe(target->getName()));
		else
			mc2::fmt::to_buf(debugString, "INFO = {}({:c}{:c}) {:d}(W{:d}) [{:d}, {:d}] ({:.4f}, {:.4f}, {:.4f}) #Pth={:d}({:d})\n", mc2::fmt::safe(terrainStr[GameMap->getTerrain(row, col)]), mc2::fmt::as_char(GameMap->getPassable(row, col) ? 'T' : 'F'), mc2::fmt::as_char(GameMap->getForest(row, col) ? 'T' : 'F'), mc2::fmt::as_i32(infoArea0), mc2::fmt::as_i32(GameMap->getShallowWater(row, col) ? 1 : (GameMap->getDeepWater(row, col) ? 2 : 0)), mc2::fmt::as_i32(row), mc2::fmt::as_i32(col), wPos.x, wPos.y, wPos.z, mc2::fmt::as_i32(PathManager->numPaths), mc2::fmt::as_i32(PathManager->peakPaths));
		DEBUGWINS_print(debugString);
		mc2::fmt::to_buf(debugString, "REQ =");
		char s[10];
		for (long i = 0; i < 25; i++) {
			mc2::fmt::to_buf(s, " {:02d}", mc2::fmt::as_i32(PathManager->sourceTally[i]));
			strcat(debugString, s);
		}
		DEBUGWINS_print(debugString);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::setGameObjectWindow() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		DEBUGWINS_setGameObject(-1, target);
		lastTime = gos_GetElapsedTime();
	}	
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow1() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[0] && DebugGameObject[0]->isMover())
			((MoverPtr)DebugGameObject[0])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow2() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[1] && DebugGameObject[1]->isMover())
			((MoverPtr)DebugGameObject[1])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow3() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[2] && DebugGameObject[2]->isMover())
			((MoverPtr)DebugGameObject[2])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::rotateObjectLeft () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			target->rotate(4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::rotateObjectRight () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			target->rotate(-4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject1() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(0);
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject2() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(1);
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject3() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(2);
	#endif
	return(1);
}

int MissionInterfaceManager::toggleDebugWins() {

	#ifndef FINAL
	static long debugWinsState = 0;
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		debugWinsState = (++debugWinsState % 6);
		bool debugStateFlags[6][7] = {
			{false, false, false, false, false, false, false},
			{false, false, false, false, false, true, false},
			{false, false, false, false, false, true, true},
			{false, true, true, true, false, true, true},
			{true, true, true, true, false, true, true},
			{false, false, false, false, true, false, true}
		};
		DEBUGWINS_display(debugStateFlags[debugWinsState]);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

extern bool ShowMovers;
int MissionInterfaceManager::showMovers() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		ShowMovers = !ShowMovers;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::cullPathAreas () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		CullPathAreas = !CullPathAreas;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::zeroHPrime() {

	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		ZeroHPrime = !ZeroHPrime;
		lastTime = gos_GetElapsedTime();
	}
	return(1);
}

int MissionInterfaceManager::calcValidAreaTable() {

	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		CalcValidAreaTable = !CalcValidAreaTable;
		lastTime = gos_GetElapsedTime();
	}
	return(1);
}

int MissionInterfaceManager::teleport() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		Team* pTeam = Team::home;
		glm::vec3 moveGoals[MAX_MOVERS];
		long numMovers = 0;
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = pTeam->getMover(i);
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		MoverGroup::calcMoveGoals(wPos, numMovers, moveGoals);
		numMovers = 0;
		for (int i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = (Mover*)pTeam->getMover( i );
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId() )
				((MoverPtr)mover)->setTeleportPosition(moveGoals[numMovers++]);
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

extern GameLog*	LRMoveLog;

extern bool quitGame;

int MissionInterfaceManager::globalMapLog () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if (MPlayer)		//Otherwise we crash in singlePlayer!!
	{
		if ((lastTime + 0.5) < gos_GetElapsedTime()) 
		{
			lastTime = gos_GetElapsedTime();
		}
	}
	#endif
	return(1);
}

int MissionInterfaceManager::brainDead () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		for (long i = 1; i < MAX_TEAMS; i++)
			MechWarrior::brainsEnabled[i] = !MechWarrior::brainsEnabled[i];
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::goalPlan() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		Team* pTeam = Team::home;
		long numMovers = 0;
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = pTeam->getMover(i);
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		for (int i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = (Mover*)pTeam->getMover( i );
			if (mover->isSelected() && mover ->getCommander()->getId() == Commander::home->getId()) {
				((MoverPtr)mover)->getPilot()->setUseGoalPlan(!((MoverPtr)mover)->getPilot()->getUseGoalPlan());
				((MoverPtr)mover)->getPilot()->setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
			}
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::enemyGoalPlan() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		EnemiesGoalPlan = !EnemiesGoalPlan;
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			Mover* mover = ObjectManager->getMover(i);
			if (mover->getTeam() != Team::home) {
				((MoverPtr)mover)->getPilot()->setUseGoalPlan(EnemiesGoalPlan);
				((MoverPtr)mover)->getPilot()->setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
			}
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::showVictim () {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (DebugGameObject[0] && DebugGameObject[0]->isMover()) 
		{
			GameObjectPtr targetObj = ((MoverPtr)DebugGameObject[0])->getPilot()->getCurTacOrder()->getTarget();
			if (targetObj)
				DEBUGWINS_setGameObject(-1, targetObj);
		}
		lastTime = gos_GetElapsedTime();
	}	
	#endif
	return(1);
}

void damageObject (GameObjectPtr victim, float damage) {

	WeaponShotInfo shot;
	switch (victim->getObjectClass()) {
		case BATTLEMECH:
		case GROUNDVEHICLE:
			shot.init(0, -2, damage, victim->calcHitLocation(nullptr,-1,ATTACKSOURCE_WEAPONFIRE,0), 0);
			victim->handleWeaponHit(&shot, (MPlayer != nullptr));
			break;
		default:
			shot.init(0, -1, damage, 0, 0);
			victim->handleWeaponHit(&shot, (MPlayer != nullptr));
			break;
	}
}

int MissionInterfaceManager::damageObject1 () 
{
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (target)
			target->repairAll();

		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject2 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject3 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 9.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject4 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 16.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject5 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 25.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject6 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 36.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject7 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 49.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject8 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 64.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject9 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 81);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject0 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (target)
		{
			if (target->isMover())
				((MoverPtr)target)->disable(0);
			else
				damageObject(target, 100.0);
		}

		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::toggleCompass()
{
	((GameCamera*)eye)->toggleCompass();
	return 0;
}

bool MissionInterfaceManager::selectionIsHelicopters( )
{
	Team* pTeam = Team::home;
   	for (long i=0;i<pTeam->getRosterSize();i++)
   	{
   		Mover* pMover = (Mover*)pTeam->getMover( i );
   		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
   		{
			if ( pMover->getMoveType() != MOVETYPE_AIR )
			{
				return 0;
			}
   		}
   	}

	return true;
}

int MissionInterfaceManager::saveHotKeys( FitIniFile& file )
{
	file.writeBlock( "Keyboard" );
	// Version sentinel so stale saves from a different MAX_COMMAND are rejected on load.
	file.writeIdLong( "KeyboardVersion", MAX_COMMAND );
	for ( int i = 0; i < MAX_COMMAND; i++ )
	{
		char header[256];
		mc2::fmt::to_buf(header, "Key{:d}", mc2::fmt::as_i32(i));
		file.writeIdLong( header, commands[i].key );
	}

	file.writeIdLong( "WayPointKey", WAYPOINT_KEY );
	file.writeIdBoolean( "UseLeftRightMouseProfile", useLeftRightMouseProfile );

	return 0;

}
int MissionInterfaceManager::loadHotKeys( FitIniFile& file )
{
	if ( OldKeys[0] == -1 )
	{
		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			OldKeys[i] = commands[i].key;
		}
	}

	if ( NO_ERR == file.seekBlock( "Keyboard" ) )
	{
		long version = -1;
		file.readIdLong( "KeyboardVersion", version );
		if ( version != MAX_COMMAND )
		{
			// Stale save (different command count — e.g. a key was added/removed).
			// Silently discard and keep compiled-in defaults.
			return -1;
		}

		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			char header[256];
			mc2::fmt::to_buf(header, "Key{:d}", mc2::fmt::as_i32(i));
			file.readIdLong( header, commands[i].key );
		}

		long tmp;
		file.readIdLong( "WayPointKey", tmp );
		WAYPOINT_KEY = gosEnum_KeyIndex( tmp );

		file.readIdBoolean( "UseLeftRightMouseProfile", useLeftRightMouseProfile );
		return 0;
	}


	return -1;
}

int MissionInterfaceManager::setHotKey( int whichCommand, gosEnum_KeyIndex newKey, bool bShift, bool bControl, bool bAlt )
{
	gosASSERT( whichCommand < MAX_COMMAND );
	long oldKey = commands[whichCommand].key;
	long key = newKey;
	if ( bShift )
		key |= SHIFT;
	if ( bControl )
		key |= CTRL;
	if ( bAlt )
		key |= ALT;

	if ( commands[whichCommand].key & WAYPT )
		key |= WAYPT;
	else
	{
		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			if ( ((commands[i].key & 0x0000ffff) == oldKey)  && (commands[i].key & WAYPT) )
			{
				commands[i].key = key | WAYPT;
			}
		}
	}

	commands[whichCommand].key = key;

	return 0;
}

int MissionInterfaceManager::getHotKey( int whichCommand, gosEnum_KeyIndex& newKey, bool& bShift, bool& bControl, bool& bAlt )
{
	gosASSERT( whichCommand < MAX_COMMAND );
	
	long key = commands[whichCommand].key;

	if ( key & SHIFT )
		bShift = true;
		
	if ( key & CTRL )
		bControl = true;
	
	if ( key & ALT )
		bAlt = true;

	key &= 0x0000ffff;
	newKey = (gosEnum_KeyIndex)key;

	return 0;
}

int MissionInterfaceManager::setWayPointKey( gosEnum_KeyIndex key )
{
	WAYPOINT_KEY = key;

	return 0;
}

void MissionInterfaceManager::setAOEStyle()
{
	useLeftRightMouseProfile = true;
}
void MissionInterfaceManager::setMCStyle()
{
	useLeftRightMouseProfile = false;
}

bool MissionInterfaceManager::isAOEStyle()
{
	return useLeftRightMouseProfile == true;
}
bool MissionInterfaceManager::isMCStyle()
{
	return useLeftRightMouseProfile == false;
}

void MissionInterfaceManager::doEject( GameObject* who )
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_EJECT, true);
	tacOrder.pack(nullptr, nullptr);
	MoverPtr pMover = (MoverPtr)who;
	if (MPlayer && !MPlayer->isServer())
		MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
	else
		pMover->handleTacticalOrder(tacOrder);
}

bool MissionInterfaceManager::hotKeyIsPressed( int whichCommand )
{
	long key = commands[whichCommand].key;
	bool bShift = 0;
	bool bControl = 0;
	bool bAlt = 0;

	if ( key & SHIFT )
		bShift = true;
		
	if ( key & CTRL )
		bControl = true;
	
	if ( key & ALT )
		bAlt = true;

	key &= 0x0000ffff;

	bool shiftDn = userInput->shift();
	bool ctrlDn = userInput->ctrl();
	bool altDn = userInput->alt();

	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(key&0x000000ff) ) == KEY_PRESSED ||
		gos_GetKeyStatus( (gosEnum_KeyIndex)(key&0x000000ff) ) == KEY_HELD )
	{
		if ( shiftDn == bShift 
			&& ctrlDn == bControl
			&& altDn == bAlt )
		{
			return true;
		}
	}

	return false;

}

void testKeyStuff()
{
	bool bShift, bCtrl, bAlt;
	gosEnum_KeyIndex key = KEY_F9;
	MissionInterfaceManager::instance()->getHotKey( 5, key, bShift, bCtrl, bAlt );
	MissionInterfaceManager::instance()->setHotKey( 5, KEY_L, 0, 0, 0 );
	MissionInterfaceManager::instance()->getHotKey( 6, key, bShift, bCtrl, bAlt );

	FitIniFile file;
	file.open( "keyboards.fit" );
	MissionInterfaceManager::instance()->loadHotKeys( file );
	MissionInterfaceManager::instance()->getHotKey( 6, key, bShift, bCtrl, bAlt );

}

int MissionInterfaceManager::toggleHotKeys()
{
	bDrawHotKeys ^= 1;
	if ( bDrawHotKeys )
	{
		keyboardRef->reseed( commands );

		if ( !isPaused() )
			togglePauseWithoutMenu();
	}
	else if ( !bDrawHotKeys && isPaused() && isPausedWithoutMenu() )
		togglePauseWithoutMenu();
	return 1;
}

void MissionInterfaceManager::drawHotKeys()
{

	keyboardRef->render();
	return;


}

void MissionInterfaceManager::drawHotKey( const char* keyString, const char* descString, long x, long y )
{
	hotKeyFont.render( keyString, 0, y, x, Environment.screenHeight, 0xffffffff, 0, 1 );

	hotKeyFont.render( descString,  x + (10 * Environment.screenWidth/640.f), y, 
		Environment.screenWidth, Environment.screenHeight, 0xffffffff, 0, 0 );

}

void MissionInterfaceManager::doGuardTower()
{

	for (long i = 0; i < Team::home->getRosterSize(); i++)
	{
		Mover* pMover = Team::home->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
		{
			pMover->suppressionFire = true;
		}
	}
			
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_POINT);
	tacOrder.attackParams.targetPoint = wPos;			
	tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
	tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
	tacOrder.attackParams.range = FIRERANGE_CURRENT;
	tacOrder.attackParams.pursue = false;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	controlGui.setDefaultSpeed();
}

int		MissionInterfaceManager::toggleHoldPosition()
{
	controlGui.toggleHoldPosition();

	return 1;
}

int MissionInterfaceManager::handleChatKey()
{
	controlGui.toggleChat(0);
	return 1;
}

int	MissionInterfaceManager::handleTeamChatKey()
{
	controlGui.toggleChat(1);
	return 1;
}
