// ObjectManager global stub definition.
// brain_special_dispatch.cpp references ObjectManager as a global pointer.
// The harness main sets this to a FakeObjectManager instance.
#include "objmgr.h"

GameObjectManagerPtr ObjectManager = nullptr;
