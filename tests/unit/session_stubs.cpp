#include "Gates/preferences_legacy.h"
#include "doctest.h"
#include "mission_init_frame.h"
#include "objtype.h"
#include "terrain.h"
#include <cstdlib>

// The FIT/heap spine is real; these are the surrounding session services used by the init phase.
MultiPlayer* MPlayer = nullptr;
float loadProgress = 0;
CPrefs prefs;
long RandomNumber(long) { return 0; }
MechWarrior* MechWarrior::warriorList[MAX_WARRIORS] = {};
void MechWarrior::init(bool) {}
void MechWarrior::destroy() {}
MechWarrior::~MechWarrior() {}
void TacticalOrder::init() {}
void TacticalOrder::destroy() {}
void TargetPriorityList::init() {}
void TargetPriorityList::destroy() {}
void GameObjectManager::init() {}
void GameObjectManager::destroy() {}
GameObjectManager::GameObjectManager() { init(); }
GameObjectManager::~GameObjectManager() { destroy(); }
void ObjectTypeManager::destroy() {}
void ObjectType::operator delete(void*) {}
long ObjectType::init(FilePtr, unsigned long) { return NO_ERR; }
void ObjectType::destroy() {}
GameObjectPtr ObjectType::createInstance() { return nullptr; }
bool ObjectType::handleCollision(GameObjectPtr, GameObjectPtr) { return false; }
bool ObjectType::handleDestruction(GameObjectPtr, GameObjectPtr) { return false; }

// Object-table fixtures provide session services; archive validation remains production code.
void GameObject::init(bool) {}
void GameObject::destroy() {}
void GameObject::verifyAppearanceView(std::string_view) {}
void GameObject::operator delete(void*) {}
void GameObject::getBlockAndVertexNumber(int&, int&) {}
ObjectTypePtr GameObject::getObjectType() { return nullptr; }
void GameObject::init(bool, ObjectTypePtr) {}
glm::vec3 GameObject::relativePosition(float, float, unsigned long) { return {}; }
void GameObject::setPosition(const glm::vec3&, bool) {}
void GameObject::setExists(bool set) { setFlag(OBJECT_FLAG_EXISTS, set); }
float GameObject::distanceFrom(glm::vec3) { return 0; }
long GameObject::cellDistanceFrom(glm::vec3) { return 0; }
long GameObject::cellDistanceFrom(GameObjectPtr) { return 0; }
long GameObject::getLineOfSightNodes(long, long, long*) { return 0; }
bool GameObject::lineOfSight(glm::vec3) { return false; }
bool GameObject::lineOfSight(GameObjectPtr, float) { return false; }
float GameObject::relFacingTo(glm::vec3, long) { return 0; }
float GameObject::getExtentRadius() { return 0; }
void GameObject::setExtentRadius(float) {}
bool GameObject::canBeSeen_instr() const { return false; }
void GameObject::extractRenderView(mc2::render_view::RenderView&) {}
void GameObject::renderView(const mc2::render_view::RenderView&) {}
long GameObject::getCaptureBlocker(GameObjectPtr, GameObjectPtr*) { return 0; }
long GameObject::kill() { return 0; }
bool GameObject::onScreen() { return false; }
MechClass GameObject::getMechClass() { return {}; }
bool GameObject::isFriendly(GameObjectPtr) { return false; }
bool GameObject::isEnemy(GameObjectPtr) { return false; }
bool GameObject::isNeutral(GameObjectPtr) { return false; }
void GameObject::Save(PacketFilePtr, long) {}
namespace mc2verify {
bool Fail(const char*, int, const char*, const char*, ...) {
    FAIL("production Part initialization rejected the fixture FIT");
    return false;
}
}

float Terrain::worldUnitsMapSide = 1000;

void Mission::destroy(bool) {
    if (parts) std::free(parts);
    parts = nullptr;
    numParts = 0;
}
void AppearanceTypeList::destroy(void) {}
