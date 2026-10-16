#ifndef PILOTPROFILEPRESERVE_H
#define PILOTPROFILEPRESERVE_H
/***************************************************************
* FILENAME: PilotProfilePreserve.h
* DESCRIPTION: Pure decision kernel for which [WarriorN] Profile string a
*              mission save writes.
*
* When Pilot::initPilots() loads zero pilots
* (pilots.csv zlib-compressed inside the FST, or warriors/*.fit missing),
* every Pilot::load falls back to s_GoodPilots[0] whose fileName is NULL,
* so saving must preserve profiles that existed on disk despite the empty catalog.
*
* Precedence:
*   1. rawProfile   -- the string read from the mission .fit (kept in sync
*                      by Pilot::setName when the user reassigns a pilot),
*                      so on-disk data always round-trips.
*   2. catalogName  -- the pilots.csv catalog match (fresh, never-loaded
*                      units placed in the editor).
*   3. ""           -- last resort; *dataLossOut is set so the caller can
*                      warn loudly instead of failing silent.
*
* Header-only and dependency-free so tests/unit can exercise it offline.
****************************************************************/

inline const char* PilotProfileForSave( const char* rawProfile,
                                        const char* catalogName,
                                        bool* dataLossOut )
{
	if ( dataLossOut )
		*dataLossOut = false;

	if ( rawProfile && rawProfile[0] )
		return rawProfile;

	if ( catalogName && catalogName[0] )
		return catalogName;

	if ( dataLossOut )
		*dataLossOut = true;

	return "";
}

#endif // PILOTPROFILEPRESERVE_H
