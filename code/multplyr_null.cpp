// Multiplayer compiled out (CMake MC2_MULTIPLAYER=OFF, the default).
//
// WHAT: the definitions the rest of the game links against when code/multplyr*.cpp are not in the build: MPlayer, which stays
// null for the life of the process, the class statics, and no-op bodies for the MultiPlayer members the single-player code
// names behind `if (MPlayer)` checks and in the lobby screens.
// WHY: the network session layer is frozen out of the modernization (owner ruling 2026-10-11) and should cost a default build
// nothing, while the 90-odd files that mention MPlayer keep compiling unchanged; the main menu's multiplayer button is disabled,
// and LogisticsData::startMultiPlayer, the one site that would create a MultiPlayer, is compiled out with the layer, so no
// body below ever runs. Values returned are the "no session" answers.
#include "multplyr.h"

MultiPlayer* MPlayer = nullptr;
DWORD ServerPlayerNum = 1;
float ResourceBuildingRefreshRate = 60.0f;

bool MultiPlayer::launchedFromLobby = false;
bool MultiPlayer::registerZone = false;
long MultiPlayer::presetDropZones[MAX_MC_PLAYERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
long MultiPlayer::colors[MAX_COLORS] = {0};

// GENERATED-STUBS-BEGIN (tools/dev/gen_multplyr_null.py from the declarations in multplyr.h)
long MultiPlayer::addLightOnFireChunk(GameObjectPtr object, long seconds) { return long(); }
long MultiPlayer::addMineChunk(long tileR, long tileC, long teamId, long mineState, long explosionType) { return long(); }
void MultiPlayer::addToLocalMovers(MoverPtr mover) {}
void MultiPlayer::addToMoverRoster(MoverPtr mover) {}
void MultiPlayer::addToPlayerMoverRoster(long playerCommanderID, MoverPtr mover) {}
long MultiPlayer::addWeaponHitChunk(GameObjectPtr target, WeaponShotInfoPtr shotInfo, bool isRefit) { return long(); }
long MultiPlayer::beginSessionScan(char* ipAddress, bool persistent) { return long(); }
long MultiPlayer::bootPlayer(NETPLAYER bootedPlayer) { return long(); }
bool MultiPlayer::calcMissionStatus() { return false; }
void MultiPlayer::calcPlayerRanks() {}
long MultiPlayer::closeSession() { return long(); }
void MultiPlayer::destroy() {}
long MultiPlayer::endSessionScan() { return long(); }
void MultiPlayer::getChatMessages(char** buffer, long* playerIDs, long& count) {}
const MC2Player* MultiPlayer::getPlayers(long& playerCount) { return nullptr; }
MC2Session* MultiPlayer::getSessions(long& sessionCount) { return nullptr; }
bool MultiPlayer::hostSession(char* sessionName, char* playerName, long mxPlayers) { return false; }
void MultiPlayer::initParametersScreen() {}
void MultiPlayer::initSpecialBuildings(char commandersToLoad[8][3]) {}
bool MultiPlayer::isServerMissing() { return false; }
long MultiPlayer::joinSession(MC2Session* session, char* playerName) { return long(); }
bool MultiPlayer::launchBrowser(const char* link) { return false; }
bool MultiPlayer::playersReadyToLoad() { return false; }
void MultiPlayer::redistributeRP() {}
void MultiPlayer::removeFromLocalMovers(MoverPtr mover) {}
void MultiPlayer::removeFromMoverRoster(MoverPtr mover) {}
void MultiPlayer::removeFromPlayerMoverRoster(MoverPtr mover) {}
void MultiPlayer::sendChat(NETPLAYER receiver, char team, char* chatString) {}
void MultiPlayer::sendHoldPosition() {}
void MultiPlayer::sendMissionSetup(NETPLAYER receiver, long stage, CompressedMech* mechData) {}
void MultiPlayer::sendPlayerArtillery(long strikeType, Stuff::Vector3D location, long seconds) {}
void MultiPlayer::sendPlayerInsignia(char* insigniaFileName, unsigned char* insigniaData, long insigniaDataSize) {}
void MultiPlayer::sendPlayerOrder(TacticalOrderPtr tacOrder, bool needsSelection, long numMovers, MoverPtr* moverList, long numGroups, MoverGroupPtr* groupList, bool queuedOrder) {}
void MultiPlayer::sendPlayerUpdate(NETPLAYER receiver, long stage, long commanderID) {}
void MultiPlayer::sendReinforcement(long vehicleID, long rosterIndex, const char pilotName[16], long commanderID, Stuff::Vector3D pos, unsigned char stage) {}
void MultiPlayer::setInProgress(bool set) {}
void MultiPlayer::setLocked(bool set) {}
long MultiPlayer::update() { return long(); }
bool MultiPlayer::waitTillMechDataReceived() { return false; }
bool MultiPlayer::waitTillMissionLoaded() { return false; }
bool MultiPlayer::waitTillMissionSetup() { return false; }
bool MultiPlayer::waitTillStartLoading() { return false; }
// GENERATED-STUBS-END

// The class allocators (the stock ones route through systemHeap); only `delete MPlayer` sites link against them here and
// MPlayer is null at every one of them.
void* MultiPlayer::operator new (size_t ourSize)
{
	return ::operator new(ourSize);
}

void MultiPlayer::operator delete (void* us)
{
	::operator delete(us);
}

// The engine's startup and shutdown hooks (code/mechcmd2.cpp); the stock body returns false too (no transport was ever live).
bool StartupNetworking (void)
{
	return false;
}

void ShutdownNetworking (void)
{
}
