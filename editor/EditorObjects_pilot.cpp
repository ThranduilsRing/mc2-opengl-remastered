// Editor objects: DropZone, Brain and Pilot construction, copy, save / load and pilot initialisation (and the pilot data-loss warning).
// Declared in the class headers; shared names in EditorObjects_internal.h.
#include "EditorObjects_internal.h"


// loud, unconditional data-loss warning (rare;
// fires only when a save would blank a [WarriorN] Profile). Same log file
// as the MC2_EDITOR_TRACE probes, but NOT gated -- data loss must be seen.
static void PilotDataLossWarn( const char* msg )
{
	OutputDebugStringA( msg );
	OutputDebugStringA( "\n" );
	FILE* f = fopen( "editor-startup.log", "a" );
	if ( f )
	{
		fputs( "[PILOT-PROFILE-PRESERVE-1] WARNING: ", f );
		fputs( msg, f );
		fputc( '\n', f );
		fclose( f );
	}
}


bool DropZone::save( FitIniFile* file, int number )
{
	Stuff::Vector3D pos = getPosition();
	
	file->writeIdLong( "NumSlots", 4 );
	if ( bVTol )
	{
		file->writeIdBoolean( "IsVTOL", 1 );
		file->writeIdFloat( "PositionX", pos.x );
		file->writeIdFloat( "PositionY", pos.y );
	}

	else
	{

		file->writeIdFloat( "PositionX", pos.x );
		file->writeIdFloat( "PositionY", pos.y );
	
		file->writeIdFloat( "OffsetX0", 0 );
		file->writeIdFloat( "OffsetY0", 0 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -71 );
		file->writeIdFloat( "OffsetY0", 71 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -71 );
		file->writeIdFloat( "OffsetY0", -71 );
		file->writeIdFloat( "Rotation0", 45);

		file->writeIdFloat( "OffsetX0", -142 );
		file->writeIdFloat( "OffsetY0", 0 );
		file->writeIdFloat( "Rotation0", 45);

	}

	return true;
}

DropZone::DropZone( const Stuff::Vector3D& pos, int alignment, bool bvtol )
{
	bVTol = bvtol;
}

void DropZone::CastAndCopy(const EditorObject &master)
{
	const DropZone *pCastedMaster = dynamic_cast<const DropZone *>(&master);
	if (nullptr != pCastedMaster)
	{
		(*this) = (*pCastedMaster);
	}
	else
	{
		gosASSERT(false);
		(*((EditorObject *)this)) = (master);
	}
}

bool Brain::save( FitIniFile* file, int warriorNumber, bool bPlayer )
{
	if (brainName[0])
	{
		file->writeIdString( "Brain", brainName);
		file->writeIdLong( "NumCells", numCells );
		file->writeIdLong( "NumStaticVars", numStaticVars );
		
		char text[256];
		for (long i=0;i<numCells;i++)
		{
			sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
			file->writeBlock(text);
			file->writeIdLong("Cell",  cellNum[i]);
			file->writeIdLong("MemType", cellType[i]);
			file->writeIdFloat("Value", cellData[i]);
		}

		if (mechAsset && brainName[0])
		{
			char buf[512] = {0};
			strcpy(buf, "Data\\Missions\\Warriors\\");
			strcat(buf, brainName);
			strcat(buf, ".abl");
			mechAsset->AddRelationship("Brain", buf);
		}
	}
	else
	{
		if (!bPlayer)
		{
			file->writeIdString( "Brain", "DredAttack01");
			file->writeIdLong( "NumCells", 3 );
			file->writeIdLong( "NumStaticVars", 0 );
			
			char text[256];
			for (long i=0;i<3;i++)
			{
				sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
				file->writeBlock(text);
				
				switch (i)
				{
					case 0:
						file->writeIdLong("Cell",  2);
						file->writeIdLong("MemType", 1);
						file->writeIdFloat("Value", 150);
					break;
					
					case 1:
						file->writeIdLong("Cell",  9);
						file->writeIdLong("MemType", 0);
						file->writeIdFloat("Value", 0.0f);
					break;
					
					case 2:
						file->writeIdLong("Cell",  28);
						file->writeIdLong("MemType", 0);
						file->writeIdFloat("Value", 0.0f);
					break;
				}
			}
		}
		else
		{
			file->writeIdString( "Brain", "PBrain");
			file->writeIdLong( "NumCells", 0 );
			file->writeIdLong( "NumStaticVars", 0 );
		}
	}

	return true;
}

bool Brain::load( FitIniFile* file, int warriorNumber )
{
	file->readIdLong( "NumCells", numCells );
	file->readIdLong( "NumStaticVars", numStaticVars );
	file->readIdString( "Brain", brainName, 255);

	// if not explicitly set, then redo
	if ( strcmp( brainName, "PBrain" ) == 0 )
		brainName[0] = 0;
	else if ( stricmp( brainName, "DredAttack01" ) == 0 )
		brainName[0] = 0;

	if (0 < numCells)
	{
		cellNum = (long *)malloc(sizeof(long) * numCells);
		cellType = (long *)malloc(sizeof(long) * numCells); 
		cellData = (float *)malloc(sizeof(float) * numCells);
	}
	else
	{
		cellNum = (long *)nullptr;
		cellType = (long *)nullptr; 
		cellData = (float *)nullptr;
	}

	char text[256];
	for (long i=0;i<numCells;i++)
	{
		sprintf( text, "Warrior%ldCell%d", warriorNumber, i );
		
		file->seekBlock(text);
		file->readIdLong("Cell",  cellNum[i]);
		file->readIdLong("MemType", cellType[i]);
		file->readIdFloat("Value", cellData[i]);
	}
	
	return true;
}

Brain::Brain( const Brain& src )
{
	numCells = src.numCells;
	numStaticVars = src.numStaticVars;
	strcpy( brainName, src.brainName );

	cellNum = (long *)malloc(sizeof(long) * numCells);
	cellType = (long *)malloc(sizeof(long) * numCells); 
	cellData = (float *)malloc(sizeof(float) * numCells);


	for (long i=0;i<numCells;i++)
	{
		cellNum[i] = src.cellNum[i];
		cellType[i] = src.cellType[i];
		cellData[i] = src.cellData[i];
	}
	
}
Brain& Brain::operator=( const Brain& src )
{
	if ( &src != this )
	{
		numCells = src.numCells;
		numStaticVars = src.numStaticVars;
		strcpy( brainName, src.brainName );

		if (0 < numCells)
		{
			cellNum = (long *)malloc(sizeof(long) * numCells);
			cellType = (long *)malloc(sizeof(long) * numCells); 
			cellData = (float *)malloc(sizeof(float) * numCells);

			for (long i=0;i<numCells;i++)
			{
				cellNum[i] = src.cellNum[i];
				cellType[i] = src.cellType[i];
				cellData[i] = src.cellData[i];
			}
		}
		else
		{
			cellNum = (long *)nullptr;
			cellType = (long *)nullptr; 
			cellData = (float *)nullptr;
		}
	}

	return *this;
	
}

void Pilot::save( FitIniFile* file, int bGoodGuy )
{
	// never silently
	// blank a Profile that existed on disk. When initPilots() loads zero
	// pilots (pilots.csv zlib-compressed inside the FST / warriors/*.fit
	// missing), every Pilot::load falls back to s_GoodPilots[0] whose
	// fileName is NULL and an empty string would be written for EVERY
	// warrior. rawProfile carries the string read from the mission .fit
	// and wins; the catalog match covers freshly placed units; "" remains
	// the crash-safe last resort, but with a loud warning.
	bool dataLoss = false;
	const char* profileName =
		PilotProfileForSave( rawProfile, info ? info->fileName : nullptr, &dataLoss );
	if ( dataLoss )
		PilotDataLossWarn(
			"Pilot::save: writing empty Profile (no raw string from load, no "
			"pilots.csv catalog match) -- possible editor data loss" );
	file->writeIdString( "Profile", profileName );

	if (mechAsset && info && info->fileName && info->fileName[0])
	{
		char buf[512] = {0};
		strcpy(buf, "Data\\Missions\\Warriors\\");
		strcat(buf, info->fileName);
		strcat(buf, ".fit");
		mechAsset->AddRelationship("Profile", buf);
	}
}

void Pilot::load( FitIniFile* file, int bGoodGuy )
{
	long result = 0;
	char buffer[256];
	result = file->readIdString( "Profile", buffer, 256 );

	// stash the raw on-disk string so save can
	// round-trip it even when the catalog match below fails (see
	// Pilot::save). Cleared on read failure so a fresh unit stays empty.
	if ( NO_ERR == result )
	{
		strncpy( rawProfile, buffer, sizeof( rawProfile ) - 1 );
		rawProfile[sizeof( rawProfile ) - 1] = 0;
	}
	else
		rawProfile[0] = 0;

	bool bFound = 0;
	if (NO_ERR == result)
	{
		for ( int i = 0; i < goodCount; i++ )
		{
			if ( stricmp( buffer, s_GoodPilots[i].fileName ) == 0 )
			{
				info = &s_GoodPilots[i];
				bFound = 1;
				break;
			}
		}

		if ( !bFound )
		{
			for ( int i = 0; i < badCount; i++ )
			{
				if ( stricmp( buffer, s_BadPilots[i].fileName ) == 0 )
				{
					info = &s_BadPilots[i];
					bFound = 1;
					break;
				}
			}
		}
	}

	if ( !bFound )
	{
		info = &s_GoodPilots[0];
	}

	if ( !info )
	{
		Assert( 0, 0, "reassigning invalid pilot" );
		info = bGoodGuy ? &s_GoodPilots[0] : &s_BadPilots[0];
	}
	
 	
}

void Pilot::initPilots()
{
	CSVFile file;

	char path[256];
	strcpy( path, objectPath );
	strcat( path, "pilots.csv" );

	if ( NO_ERR != file.open( path ) )
	{
		STOP(( "couldn't find pilots.csv file" ));
		return;
	}

	char pilotFileName[256];
	strcpy(pilotFileName, "");

	PilotInfo* infos = s_GoodPilots;
	long* counter = &goodCount;

	for ( int i = 0; i < 2; i++ )
	{
		while( true )
		{
			int bytesRead = file.readLine( (BYTE*)pilotFileName, 256 );

			if ( bytesRead < 2 )
				break;

			// Guard: if the FST reader ever leaks compressed bytes (78 9c
			// deflate leak), a non-printable byte in the name means the
			// data is corrupt/binary — stop loading.
			// File::readLine counts its trailing NUL; the terminator is not part
			// of the name and must be excluded from printable-character validation.
			{
				bool validName = true;
				for ( int ci = 0; ci < bytesRead && pilotFileName[ci] != 0; ++ci )
				{
					unsigned char c = (unsigned char)pilotFileName[ci];
					if ( c < 0x20 || c > 0x7e ) { validName = false; break; }
				}
				if ( !validName )
				{
					// Report binary pilot names because an empty catalog leaves the picker
					// unusable and forces raw-profile preservation on save.
					FILE* f = fopen( "editor-startup.log", "a" );
					if ( f )
					{
						fprintf( f, "[PILOT-CATALOG] corrupt line (pass %d), first bytes %02x %02x %02x %02x -- aborting csv read\n",
							i, (unsigned char)pilotFileName[0],
							bytesRead > 1 ? (unsigned char)pilotFileName[1] : 0,
							bytesRead > 2 ? (unsigned char)pilotFileName[2] : 0,
							bytesRead > 3 ? (unsigned char)pilotFileName[3] : 0 );
						fclose( f );
					}
					break;
				}
			}

			CString postFix;
			if (0 == i)
			{
				if ((strlen(pilotFileName) > strlen("pmw"))
					&& (0 == strnicmp(pilotFileName, "pmw", strlen("pmw"))))
				{
					/*Good pilots that start with "pmw" are single-player pilots.*/
					CString tmpStr;
					tmpStr.LoadString(IDS_PILOT_SINGLE_PLAYER_VERSION);
					postFix = tmpStr.GetBuffer(0);
				}
				else if ((strlen(pilotFileName) > strlen("pmp_"))
					&& (0 == strnicmp(pilotFileName, "pmp_", strlen("pmp_"))))
				{
					/*Good pilots that start with "pmp_" are multi-player pilots.*/
					CString tmpStr;
					tmpStr.LoadString(IDS_PILOT_MULTIPLAYER_VERSION);
					postFix = tmpStr.GetBuffer(0);
				}
			}

			FitIniFile pilotFile;
			FullPathFileName tmpPath;
			tmpPath.init(warriorPath,pilotFileName,".fit");

			if ( NO_ERR != pilotFile.open( tmpPath ) )
			{
				// Pilot .fit missing — skip rather than crash.  The editor's pilot
				// assignment dialog will just have fewer options.
				continue;
			}

			infos[*counter].fileName = new char[strlen( pilotFileName ) + 1];
			strcpy( infos[*counter].fileName, pilotFileName );

			int result = pilotFile.seekBlock( "General" );
			gosASSERT( result == 0 );

			long tmp;
			result = pilotFile.readIdLong( "descIndex", tmp );
			gosASSERT( result == NO_ERR );

			EditorSafeLoadString( tmp, pilotFileName, 64);
			strcat(pilotFileName, "  ");
			strncat(pilotFileName, postFix.GetBuffer(0), 64);

			infos[*counter].name = new char[strlen( pilotFileName  ) +1];
			strcpy( infos[*counter].name, pilotFileName );
			
			(*counter)++;

			if ( goodCount > MAX_PILOT )
				return;
		}

		file.close();
		FullPathFileName path;
		path.init(objectPath,"BadPilots",".csv");

		if ( NO_ERR != file.open( path ) )
		{
			STOP(( "couldn't find BadPilots.csv file" ));
			return;
		}

		infos = s_BadPilots;
		counter = &badCount;

	}

	// Log startup counts. good must be > 0 for the
	// pilot picker / Pilot::load catalog matching to work at all.
	{
		FILE* f = fopen( "editor-startup.log", "a" );
		if ( f )
		{
			fprintf( f, "[PILOT-CATALOG] good=%ld bad=%ld\n", goodCount, badCount );
			fclose( f );
		}
	}
}

void Pilot::setName( const char* newName )
{
	// a user reassignment must beat the stashed
	// on-disk string in Pilot::save, so sync rawProfile with the new match.
	int i = 0;
	for ( i = 0; i < goodCount; i++ )
	{
		if ( stricmp( newName, s_GoodPilots[i].name ) == 0 )
		{
			info = &s_GoodPilots[i];
			syncRawProfile();
			return;
		}
	}

	for ( i = 0; i < badCount; i++ )
	{
		if ( stricmp( newName, s_BadPilots[i].name ) == 0 )
		{
			info = &s_BadPilots[i];
			syncRawProfile();
			return;
		}
	}

	gosASSERT( 0 );

}

void NavMarker::CastAndCopy(const EditorObject& master)
{
	if (const auto* marker = dynamic_cast<const NavMarker*>(&master)) *this = *marker;
}

bool NavMarker::readDefinition(FitIniFile& file)
{
	float x = 0, y = 0, loadedRadius = 0;
	if (!EditorFit::floatValue(file, "xPos", x) || !EditorFit::floatValue(file, "yPos", y)
		|| !EditorFit::floatValue(file, "radius", loadedRadius) || loadedRadius < 0)
		return EditorFit::reject("NavMarker coordinates/radius");
	authoredPosition.read(x, y, getPosition().x, getPosition().y);
	radius = loadedRadius;
	return true;
}

bool NavMarker::save( FitIniFile* file, int warriorNumber )
{ 
	char text[32];
	sprintf( text, "NavMarker%ld", warriorNumber );
	file->writeBlock( text );

	file->writeIdFloat( "xPos", authoredPosition.writeX(appearance()->position.x) );
	file->writeIdFloat( "yPos", authoredPosition.writeY(appearance()->position.y) );
	file->writeIdFloat( "radius", radius );
	return true; 
}
