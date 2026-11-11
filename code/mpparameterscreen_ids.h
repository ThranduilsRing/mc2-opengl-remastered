// Private ids shared by the mpparameterscreen*.cpp translation units (split from mpparameterscreen.cpp, no behaviour change).
#pragma once

constexpr int MP_PLAYER_PREFS = 120;
constexpr int MAP_INFO = 121;


constexpr auto CHECK_BUTTON = 200;
constexpr int MP_INCREMENT_DROPWEIGHT = 198;
constexpr int MP_DECREMENT_DROPWEIGHT = 199;
constexpr int MP_INCREMENT_CBILLS = 200;
constexpr int MP_DECREMENT_CBILLS = 201;
constexpr int MP_INCREMENT_TIME = 202;
constexpr int MP_DECREMENT_TIME = 203;
constexpr int MP_INCREMENT_RP = 204;
constexpr int MP_DECREMENT_RP = 205;



constexpr int MP_AIRSTRIKES = 206;
constexpr int MP_SENSOR_PROBE = 207;
constexpr int MP_MINELAYER = 208;
constexpr int MP_REPAIR = 209;
constexpr int MP_LOCKGAME = 210;
constexpr int MP_BOOTPLAYER = 211;
constexpr int MP_SCOUTS = 212;
constexpr int MP_RECOVERY = 213;
constexpr int MP_GUARDTOWER = 214;
constexpr int MP_RESOURCE_BLDG = 215;
constexpr int MP_UNLIMITED_AMMO = 216;
constexpr int MP_VARIANTS = 217;
constexpr int MP_ALLTECH = 218;
constexpr int MP_QUICKSTART = 219;
constexpr int MP_RP_FORMMECHS = 220;

constexpr int MP_INCREMENT_PLAYER_CBILLS = 250;
constexpr int MP_DECREMENT_PLAYER_CBILLS = 251;


static const int FIRST_BUTTON_ID = 1000010;
static const int OK_BUTTON_ID = 1000001;
static const int CANCEL_BUTTON_ID = 1000002;

extern GUID NO_VERSION_GUID;
extern const char* pPurchaseFiles[5];
int __cdecl sortPlayers( const void* p1, const void* p2 );
