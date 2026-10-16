#pragma once
#ifndef TACORDR_H
#define TACORDR_H
// tacordr.h stub
// Provides TacticalOrder, enums, LocationNode for brain_special_dispatch.cpp.
// No engine header dependencies.

#include "gameobj.h"
#include <cstring>
#include <cstdint>

// ---- Constants ----
#define MAX_WAYPTS 15

// arrival status codes.
// In the real engine (tacordr.h), TACORDER_SUCCESS is defined as 1.
// Stub provides the same value; status() always returns TACORDER_RUNNING so
// the harness never triggers cursor advance (arrival is smoke-only).
#define TACORDER_SUCCESS  1
#define TACORDER_RUNNING  0

// ---- Enums ----

typedef enum { ORDER_ORIGIN_PLAYER, ORDER_ORIGIN_COMMANDER, ORDER_ORIGIN_SELF } OrderOriginType;

typedef enum {
    TACTICAL_ORDER_NONE = 0,
    TACTICAL_ORDER_WAIT,
    TACTICAL_ORDER_MOVETO_POINT,
    TACTICAL_ORDER_MOVETO_OBJECT,
    TACTICAL_ORDER_JUMPTO_POINT,
    TACTICAL_ORDER_JUMPTO_OBJECT,
    TACTICAL_ORDER_TRAVERSE_PATH,
    TACTICAL_ORDER_PATROL_PATH,
    TACTICAL_ORDER_ESCORT,
    TACTICAL_ORDER_FOLLOW,
    TACTICAL_ORDER_GUARD,
    TACTICAL_ORDER_STOP,
    TACTICAL_ORDER_POWERUP,
    TACTICAL_ORDER_POWERDOWN,
    TACTICAL_ORDER_WAYPOINTS_DONE,
    TACTICAL_ORDER_EJECT,
    TACTICAL_ORDER_ATTACK_OBJECT,
    TACTICAL_ORDER_ATTACK_POINT,
    TACTICAL_ORDER_HOLD_FIRE,
    TACTICAL_ORDER_WITHDRAW,
    TACTICAL_ORDER_SCRAMBLE,
    TACTICAL_ORDER_CAPTURE,
    TACTICAL_ORDER_REFIT,
    TACTICAL_ORDER_GETFIXED,
    TACTICAL_ORDER_LOAD_INTO_CARRIER,
    TACTICAL_ORDER_DEPLOY_ELEMENTALS,
    TACTICAL_ORDER_RECOVER,
    NUM_TACTICAL_ORDERS
} TacticalOrderCode;

typedef enum { TRAVEL_MODE_INVALID = -1, TRAVEL_MODE_SLOW, TRAVEL_MODE_FAST, TRAVEL_MODE_JUMP } TravelModeType;
typedef enum { ATTACK_TO_DESTROY = 0, ATTACK_TO_DISABLE } AttackType;
typedef enum { ATTACKMETHOD_RANGED = 0, ATTACKMETHOD_MELEE } AttackMethod;
typedef enum { FIRERANGE_OPTIMAL = 0, FIRERANGE_SHORT, FIRERANGE_LONG } FireRangeType;
typedef enum { TACTIC_NONE = 0 } TacticType;
typedef enum { MOVE_MODE_NORMAL = 0 } SpecialMoveMode;

// ---- Structs ----

struct WayPath {
    long          numPoints = 0;
    long          curPoint  = 0;
    float         points[3 * MAX_WAYPTS] = {};
    unsigned char mode[MAX_WAYPTS]       = {};
};

struct TacOrderMoveParams {
    WayPath       wayPath;
    bool          faceObject  = false;
    bool          wait        = false;
    SpecialMoveMode modeMove  = MOVE_MODE_NORMAL;
    bool          escapeTile  = false;
    bool          jump        = false;
    long          fromArea    = 0;
    bool          keepMoving  = false;
};

struct TacOrderAttackParams {
    AttackType    type         = ATTACK_TO_DESTROY;
    AttackMethod  method       = ATTACKMETHOD_RANGED;
    FireRangeType range        = FIRERANGE_OPTIMAL;
    TacticType    tactic       = TACTIC_NONE;
    long          aimLocation  = 0;
    bool          pursue       = false;
    bool          obliterate   = false;
    float         targetPointX = 0, targetPointY = 0, targetPointZ = 0;
};

struct LocationNode {
    struct { float x = 0, y = 0, z = 0; } location;
    bool         run  = false;
    LocationNode* next = nullptr;
};
typedef LocationNode* LocationNodePtr;

// ---- TacticalOrder ----

class TacticalOrder {
public:
    long              id             = 0;
    float             time           = 0;
    float             delayedTime    = 0;
    float             lastTime       = 0;
    bool              unitOrder      = false;
    bool              subOrder       = false;
    OrderOriginType   origin         = ORDER_ORIGIN_SELF;
    TacticalOrderCode code           = TACTICAL_ORDER_NONE;
    TacOrderMoveParams   moveParams;
    TacOrderAttackParams attackParams;
    GameObjectWatchID targetWID      = 0;
    long              targetObjectClass = 0;
    long              selectionIndex = 0;
    char              stage          = 0;
    char              statusCode     = 0;
    char              pointLocalMoverId = 0;
    unsigned long     groupFlags     = 0;
    unsigned long     data[2]        = {};

    TacticalOrder() { std::memset(this, 0, sizeof(*this)); }

    void init() { std::memset(this, 0, sizeof(*this)); }

    void init(OrderOriginType _origin, TacticalOrderCode _code, bool _unitOrder = false) {
        init();
        origin    = _origin;
        code      = _code;
        unitOrder = _unitOrder;
    }

    void initWayPath(LocationNodePtr path) {
        // Stub: just record first location
        if (path) {
            moveParams.wayPath.numPoints = 1;
            moveParams.wayPath.points[0] = path->location.x;
            moveParams.wayPath.points[1] = path->location.y;
            moveParams.wayPath.points[2] = path->location.z;
            moveParams.wayPath.mode[0]   = TRAVEL_MODE_FAST;
        }
    }

    void pack(GameObjectPtr, GameObjectPtr) { /* stub — no-op for harness */ }

    void destroy() {}

    // arrival poll.
    // Stub always returns TACORDER_RUNNING — arrival detection is smoke-only.
    // The harness never triggers tickPatrolAdvance cursor advance.
    long status(void* /*warrior*/) const { return TACORDER_RUNNING; }

    void* operator new(size_t sz);
    void  operator delete(void* p);
};

#endif // TACORDR_H
