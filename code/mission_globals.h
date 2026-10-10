// Game-system constants defined in mission.cpp (read from gamesys.fit) and used by the mission, save/load and mover code.
#ifndef MISSION_GLOBALS_H
#define MISSION_GLOBALS_H

extern float maxVisualRange;
extern unsigned long MaxTreeLOSCellBlock;
extern float MaxVisualRadius;
extern float fireVisualRange;
extern float WeaponRange[];
extern float DefaultAttackRange;
extern float baseSensorRange;
extern long visualRangeTable[];
extern unsigned char godMode;
extern unsigned char revealTacMap;
extern unsigned char footPrints;
extern bool CantTouchThis;
extern long tonnageDivisor;
extern long resourcesPerTonDivided;
extern float MineDamage;
extern float MineSplashDamage;
extern float MineSplashRange;
extern long MineExplosion;
extern long MineLayThrottle;
extern long MineSweepThrottle;
extern float StrikeWaitTime;
extern float StrikeTimeToImpact;
extern float MineWaitTime;
extern long totalSmokeSpheres;
extern long totalSmokeShapeSize;
extern long maxFiresBurning;
extern float maxFireBurnTime;
extern bool force64MB;
extern bool force32Mb;
extern float InfluenceRadius;
extern float InfluenceTime;

#endif // MISSION_GLOBALS_H
