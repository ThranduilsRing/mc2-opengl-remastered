// LogisticsData: current-mission accessors, mech modification accept / cancel, and the encrypt / decrypt helpers.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"


const EString& LogisticsData::getCurrentMission() const
{
	return missionInfo->getCurrentMission(); 
}

const EString& LogisticsData::getLastMission() const
{
	return missionInfo->getLastMission(); 
}

const char * LogisticsData::getCurrentABLScript() const
{
	// missionInfo may be null before bg init thread completes -- return null so
	// the caller's `if (brainfile)` guard skips tutorial brain load safely.
	return missionInfo ? missionInfo->getCurrentABLScriptName() : nullptr;
}

long LogisticsData::getCurrentMissionTune()
{
	return missionInfo->getCurrentLogisticsTuneId();
}

long LogisticsData::getCurrentMissionId()
{
	return missionInfo->getCurrentMissionId();
}

void LogisticsData::clearInventory()
{
	for (MECH_LIST::EIterator iter = inventory.Begin(); !iter.IsDone(); iter++ )
	{
		(*iter)->setPilot( nullptr );
		delete *iter;
	}

	inventory.Clear();
}

int	LogisticsData::getPilotCount()
{
	return pilots.Count();
}
int	LogisticsData::getPilots( LogisticsPilot** pArray, long& count )
{
	ensureMechBayDataLoaded("getPilots");
	if ( count < pilots.Count() )
	 {
		return NEED_BIGGER_ARRAY;
	 }	 

	 count= 0;

	 for ( PILOT_LIST::EIterator iter = instance->pilots.Begin(); !iter.IsDone(); iter++  )
	 {
		 pArray[count++] = &(*iter);
	 }

	 return 0;
}

int LogisticsData::getMaxDropWeight() const
{
	return  missionInfo->getCurrentDropWeight();
}

int LogisticsData::getCurrentDropWeight() const
{
	long retVal = 0;
	for ( MECH_LIST::EIterator iter = instance->inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getForceGroup() )
		{
			retVal += (*iter)->getMaxWeight();
		}

	}

	return retVal;

}

bool	LogisticsData::canAddMechToForceGroup( LogisticsMech* pMech )
{
	if ( !pMech )
		return 0;

	int maxUnits = 12;

#ifndef VIEWER
	if ( MPlayer )
	{
		long playerCount;
		MPlayer->getPlayers( playerCount );
		maxUnits = MAX_MULTIPLAYER_MECHS_IN_LOGISTICS/playerCount;

		if ( maxUnits > 12 )
			maxUnits = 12;
	}
#endif

	int fgCount = 0;
	
	for ( MECH_LIST::EIterator iter = instance->inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getForceGroup() )
			{
				fgCount ++;
			}


	}

	if ( fgCount >= maxUnits )
		return 0;


	return (pMech->getMaxWeight() + getCurrentDropWeight() <= getMaxDropWeight() ) ? 1 : 0;
}


int LogisticsData::getVariantsInInventory( LogisticsVariant* pVar, bool bIncludeForceGroup )
{
	long retVal = 0;
	for ( MECH_LIST::EIterator iter = instance->inventory.Begin(); !iter.IsDone(); iter++ )
	{
		if ( (*iter)->getVariant() == pVar )
		{
			if ( !(*iter)->getForceGroup() || bIncludeForceGroup )
			{
				retVal ++;
			}
		}

	}

	return retVal;

}

int		LogisticsData::getChassisVariants( const LogisticsChassis* pChassis, 
										  LogisticsVariant** pVar, 
										  int& maxCount )
{
	int retVal = 0;
	
	int i = 0;
	for ( VARIANT_LIST::EIterator iter = variants.Begin(); 
		!iter.IsDone(); iter++ )
		{
			if ( (*iter)->getChassis() == pChassis  )
			{
				if ( i < maxCount )
					pVar[i]	= (*iter);	
				else 
					retVal = NEED_BIGGER_ARRAY;
				++i;

				
			} 
		}

	maxCount = i;

	return retVal; 
}


int LogisticsData::setMechToModify( LogisticsMech* pMech )
{
	if ( !pMech )
		return -1;
	ensureMechBayDataLoaded("setMechToModify");
	currentlyModifiedMech = pMech;
	oldVariant = pMech->getVariant();

	LogisticsVariant* pVar = new LogisticsVariant( *oldVariant );
	pMech->setVariant( pVar );

	return 0;
}

void encryptFile (const char *inputFile, const char* outputFile)
{
    PAUSE((""));

	//Now we encrypt this by zlib Compressing the file passed in.
	// Then LZ Compressing the resulting zlib data.
	// Since our LZ compression is pretty much non-standard, that should be enough.
	MemoryPtr rawData = nullptr;
	MemoryPtr zlibData = nullptr;
	MemoryPtr LZData = nullptr;

	File dataFile;
	dataFile.open(inputFile);
	DWORD fileSize = dataFile.fileSize();
	rawData = (MemoryPtr)malloc(fileSize);
	zlibData = (MemoryPtr)malloc(fileSize*2);
	LZData = (MemoryPtr)malloc(fileSize*2);

	dataFile.read(rawData,fileSize);

	unsigned long int zlibSize = fileSize * 2;
	compress2(zlibData,&zlibSize,rawData,fileSize,0);
	size_t lzSize = LZCompress (LZData, zlibData, zlibSize);

	dataFile.close();

	File binFile;
	binFile.create(outputFile);
	binFile.writeInt(lzSize);
	binFile.writeInt(zlibSize);
	binFile.writeInt(fileSize);
	binFile.write(LZData,lzSize);
	binFile.close();

	free(rawData);
	free(zlibData);
	free(LZData);
}

void decryptFile (char *inputFile, char *outputFile)
{
    PAUSE((""));

	//Now we decrypt this by lz deCompressing the zlib file created.
	// Then zlib deCompressing the resulting zlib data into the raw File again.
	// Since our LZ compression is pretty much non-standard, that should be enough.
	MemoryPtr rawData = nullptr;
	MemoryPtr zlibData = nullptr;
	MemoryPtr LZData = nullptr;

	File dataFile;
	long result = dataFile.open(inputFile);
	if (result == NO_ERR) 
	{
		long lzSize = dataFile.readInt();
		long zlibSize = dataFile.readInt();
		long fileSize = dataFile.readInt();
	
		rawData = (MemoryPtr)malloc(fileSize);
		zlibData = (MemoryPtr)malloc(zlibSize);
		LZData = (MemoryPtr)malloc(lzSize);
	
		dataFile.read(LZData,lzSize);
	
		unsigned long testSize = fileSize;
		size_t test2Size = LZDecomp(zlibData, LZData, lzSize, zlibSize);
		if (test2Size != zlibSize) 
			STOP(("Didn't Decompress to same size as started with!!"));
	
		uncompress(rawData,&testSize,zlibData,zlibSize);
		if (testSize != fileSize) 
			STOP(("Didn't Decompress to correct format"));
	
		dataFile.close();
	
		File binFile;
		binFile.create(outputFile);
		binFile.write(rawData,fileSize);
		binFile.close();
	
		free(rawData);
		free(zlibData);
		free(LZData);
	}
}

int LogisticsData::acceptMechModifications( const char* name )
{
	if ( !currentlyModifiedMech )
		return -1;

	bool bFound = 0;
	if ( oldVariant )
	{
		missionInfo->incrementCBills( oldVariant->getCost() );
	}
	if ( oldVariant && oldVariant->isDesignerMech() )
	{
		bFound = 1;
	}
	else
	{
		for ( MECH_LIST::EIterator iter = inventory.Begin();
			!iter.IsDone(); iter++ )
		{
			if ( (*iter)->getVariant() == oldVariant && (*iter) != currentlyModifiedMech  )
			{
				bFound = 1;
			}
		}
	}

	if ( !bFound )
	{
		VARIANT_LIST::EIterator vIter = variants.Find( oldVariant );
		if ( vIter != VARIANT_LIST::INVALID_ITERATOR
			&& mc2::str::iequals( oldVariant->getName(), mc2::str::from_c( name ) ) )
		{
			variants.Delete( vIter );
			delete oldVariant;
		}
	}

	// now need to get rid of variants with this name....

	//Code added by Frank on  May 3, 2001 @ 9:54pm.
	// If you run this with the compare set to vIter.IsDone, it crashes in Profile
	// based on Sean Bug number 4359.  We traverse past end of list and crash.
	// Doing it by count does not crash and has the added advantage of being easy to debug!
	// I suspect ESI going south again.  Probably a compiler option...

	// 05/04 HKG, actually, if you increment vIter after deleteing it, it still won't work

	// Good Point.  As you can see, it was pretty late when I "fixed" this!
	long numVariants = variants.Count();
	long i=0;
	const std::string wanted = mc2::str::from_c( name );
	for ( VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone();  )
	{
		if ( mc2::str::iequals( (*vIter)->getName(), wanted ) )
		{
			VARIANT_LIST::EIterator tmpIter = vIter;
			vIter++;
			delete (*tmpIter);
			variants.Delete( tmpIter );
		}
		else
			vIter++;
	}

	currentlyModifiedMech->getVariant()->setName( wanted );
	variants.Append( currentlyModifiedMech->getVariant() );
	missionInfo->decrementCBills( currentlyModifiedMech->getVariant()->getCost() );

	currentlyModifiedMech = nullptr;
	oldVariant = nullptr;


	// temporary, looking for dangling pointers
	for ( MECH_LIST::EIterator iter = inventory.Begin();
		!iter.IsDone(); iter++ )
	{
		if ( (*iter)->getVariant()->getCost() )
		{
			bFound = 1;
		}
	}
 
#ifndef VIEWER

	if ( MPlayer )
	{
		// save the player created variants
		for ( VARIANT_LIST::EIterator vIter = variants.Begin(); !vIter.IsDone();  vIter++ )
		{
			if ( !(*vIter)->isDesignerMech() )
			{
				FullPathFileName mechFile;
				mechFile.init("data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR ,(*vIter)->getName().c_str(),".var");

				FitIniFile file;
				file.create(mechFile);
				(*vIter)->save( file, 0 );
				file.close();

                const char* filename = (const char*)mechFile;
				encryptFile(filename, filename);
			}
		}
	}

#endif

	return 0;
}
int LogisticsData::acceptMechModificationsUseOldVariant( const char* name )
{
	if ( !currentlyModifiedMech )
		return -1;

	if ( oldVariant )
	{
		missionInfo->incrementCBills( oldVariant->getCost() );
	}

	LogisticsVariant* pVar = getVariant( name );

	if ( !pVar )
		Assert( 0, 0, "couldn't find the old variant\n" );

	LogisticsVariant* pOldVar = currentlyModifiedMech->getVariant();
	delete pOldVar;

	currentlyModifiedMech->setVariant( pVar );
	missionInfo->decrementCBills( pVar->getCost() );
	
	currentlyModifiedMech = nullptr;
	oldVariant = nullptr;	

	return 0;
}

bool LogisticsData::canReplaceVariant( const char* name )
{
	int nameCount = 0;
	const std::string wanted = mc2::str::from_c( name );
	for ( MECH_LIST::EIterator iter = inventory.Begin();
		!iter.IsDone(); iter++ )
		{
			if ( mc2::str::iequals( (*iter)->getName(), wanted ) )
			{
				nameCount++;
				if ( (*iter)->getVariant() != oldVariant && (*iter) != currentlyModifiedMech )
				{
					return 0;
				}
			}
		}

		if ( nameCount > 1 )
			return 0;

		for ( VARIANT_LIST::EIterator vIter = variants.Begin();
		!vIter.IsDone(); vIter++ )
		{
			if ( (*vIter)->isDesignerMech() && mc2::str::iequals( (*vIter)->getName(), wanted ) )
				return 0;
		}

		return true;
}

bool	LogisticsData::canDeleteVariant( const char* name )
{
	LogisticsVariant* pVariant = getVariant( name );
	if ( !pVariant )
		return 0;
		
	if ( !canReplaceVariant(name) )
		return 0;

	const std::string wanted = mc2::str::from_c( name );
	if ( mc2::str::iequals( currentlyModifiedMech->getName(), wanted ) || mc2::str::iequals( oldVariant->getName(), wanted ) )
		return 0;

	return 1;

}

int LogisticsData::cancelMechModfications()
{
	if ( !currentlyModifiedMech )
		return -1;

	LogisticsVariant* pCancel = currentlyModifiedMech->getVariant();

	delete pCancel;

	currentlyModifiedMech->setVariant( oldVariant );

	oldVariant = nullptr;
	currentlyModifiedMech = nullptr;

	return 0;
}

const char*			LogisticsData::getCurrentOperationFileName()
{
	return missionInfo->getCurrentOperationFile();
}
const char*			LogisticsData::getCurrentVideoFileName()
{
	return missionInfo->getCurrentVideo();
}

const char*			LogisticsData::getCurrentMissionDescription()
{
	return missionInfo->getCurrentMissionDescription();
}


const char*				LogisticsData::getCurrentMissionFriendlyName( )
{
	return missionInfo->getCurrentMissionFriendlyName();
}

const char*				LogisticsData::getMissionFriendlyName( const char* missionName )
{
	return missionInfo->getMissionFriendlyName( missionName );
}
