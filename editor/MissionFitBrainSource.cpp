/***************************************************************
* FILENAME: MissionFitBrainSource.cpp
* DESCRIPTION: shared mission.fit declarative Brain{} cache. See header.
* DATE: 2026-07-16
****************************************************************/
#include "stdafx.h"

#include "MissionFitBrainSource.h"

#include "EditorData.h"   // EditorData::getMapName (current mission.fit name)
#include "File.h"         // File (raw mission.fit read)

#include <cstring>
#include <cstdlib>

namespace MissionFitBrainSource
{

namespace {
	MissionFitBrain s_brains[128];
	int             s_brainCount = 0;
	char            s_lastPath[512] = { 0 };
}

bool Refresh()
{
	const char* mapName = EditorData::getMapName();
	if ( !mapName || !mapName[0] )
	{
		s_brainCount = 0;
		s_lastPath[0] = 0;
		return false;
	}

	// getMapName() returns the FULL loaded .pak path (e.g. "...\mc2_24\mc2_24.pak"); the mission
	// .fit sits next to it. Swap the trailing ".pak" -> ".fit" (mirrors EditorData camFileName
	// Replace). Do NOT prepend missionPath — mapName is already an absolute path.
	char pathBuf[512];
	strncpy( pathBuf, mapName, sizeof( pathBuf ) - 1 );
	pathBuf[sizeof( pathBuf ) - 1] = 0;
	size_t plen = strlen( pathBuf );
	if ( plen >= 4 && 0 == _stricmp( pathBuf + plen - 4, ".pak" ) )
		strcpy( pathBuf + plen - 4, ".fit" );             // mc2_24.pak -> mc2_24.fit
	else if ( plen + 4 < sizeof( pathBuf ) )
		strcpy( pathBuf + plen, ".fit" );                 // no .pak ext: just append
	const char* path = pathBuf;

	if ( 0 == strcmp( s_lastPath, path ) )
		return true;   // already parsed this fit

	strncpy( s_lastPath, path, sizeof( s_lastPath ) - 1 );
	s_lastPath[sizeof( s_lastPath ) - 1] = 0;
	s_brainCount = 0;

	File f;
	if ( NO_ERR != f.open( path ) )
		return true;   // no fit on disk yet (count stays 0)

	long sz = f.getLength();
	if ( sz > 0 )
	{
		char* buf = (char*)malloc( sz + 1 );
		if ( buf )
		{
			f.read( (MemoryPtr)buf, sz );
			buf[sz] = 0;
			s_brainCount = parseMissionFitBrains( buf, s_brains, 128 );
			free( buf );
		}
	}
	return true;
}

int BrainCount() { return s_brainCount; }

const MissionFitBrain* BrainAt( int i )
{
	if ( i < 0 || i >= s_brainCount )
		return nullptr;
	return &s_brains[i];
}

const char* LastFitPath() { return s_lastPath; }

const MissionFitBrain* MatchForPos( float ux, float uy )
{
	return matchBrainForPos( s_brains, s_brainCount, ux, uy );   // default tol (50wu), brain_missionfit_oporbd.h
}

} // namespace MissionFitBrainSource
