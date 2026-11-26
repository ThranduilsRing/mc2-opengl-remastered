// LogisticsData: current-mission accessors, mech modification accept / cancel, and the encrypt / decrypt helpers.
// Declared in the class headers; shared names in logisticsdata_internal.h.
#include "logisticsdata_internal.h"
#include <algorithm>
#include "dropweight_rules.h"


const std::string& LogisticsData::getCurrentMission() const
{
	return missionInfo->getCurrentMission(); 
}

const std::string& LogisticsData::getLastMission() const
{
	return missionInfo->getLastMission(); 
}

std::string LogisticsData::getCurrentABLScript() const
{
	// missionInfo may be null before bg init thread completes -- return empty so
	// the caller's `if (!brainfile.empty())` guard skips tutorial brain load safely.
	return missionInfo ? missionInfo->getCurrentABLScriptName() : std::string();
}

std::optional<long> LogisticsData::getCurrentMissionTune()
{
	return missionInfo->getCurrentLogisticsTuneId();
}

long LogisticsData::getCurrentMissionId()
{
	return missionInfo->getCurrentMissionId();
}

void LogisticsData::clearInventory()
{
	if ( currentlyModifiedMech )
		cancelMechModfications();
	for ( auto iter = inventory.begin(); iter != inventory.end(); ++iter )
	{
		(*iter)->setPilot( nullptr );
	}

	inventory.clear();
}

int	LogisticsData::getPilotCount()
{
	return static_cast<int>(std::ssize(pilots));
}
int	LogisticsData::getPilots( LogisticsPilot** pArray, long& count )
{
	ensureMechBayDataLoaded("getPilots");
	if ( count < std::ssize(pilots) )
	 {
		return NEED_BIGGER_ARRAY;
	 }	 

	 count= 0;

	 for (auto & pilot : instance->pilots)
	 {
		 pArray[count++] = &pilot;
	 }

	 return 0;
}

int LogisticsData::getMaxDropWeight() const
{
	return  missionInfo->getCurrentDropWeight() + rules().dropWeightStep * missionInfo->getDropWeightPurchases();
}

bool LogisticsData::isOverDropLimit() const
{
	return dropweight::overLimit( getCurrentDropWeight(), getMaxDropWeight() );
}

int LogisticsData::getDropWeightPurchases() const
{
	return missionInfo->getDropWeightPurchases();
}

long LogisticsData::getDropWeightPrice() const
{
	return dropweight::priceForNext( missionInfo->getDropWeightPurchases() );
}

bool LogisticsData::buyDropWeight()
{
	const long price = getDropWeightPrice();
	if ( !dropweight::purchaseAvailable( MPlayer != nullptr ) || missionInfo->getCBills() < price )
		return false;
	missionInfo->decrementCBills( static_cast<int>( price ) );
	missionInfo->addDropWeightPurchase();
	return true;
}

int LogisticsData::getCurrentDropWeight() const
{
	long retVal = 0;
	for (auto & iter : instance->inventory)
	{
		if ( iter->getForceGroup() )
		{
			retVal += iter->getMaxWeight();
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

		maxUnits = std::min(maxUnits, 12);
	}
#endif

	int fgCount = 0;
	
	for (auto & iter : instance->inventory)
	{
		if ( iter->getForceGroup() )
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
	for (auto & iter : instance->inventory)
	{
		if ( iter->getVariant() == pVar )
		{
			if ( !iter->getForceGroup() || bIncludeForceGroup )
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
	for (auto & variant : variants)
		{
			if ( variant->getChassis() == pChassis  )
			{
				if ( i < maxCount )
					pVar[i]	= variant.get();
				else 
					retVal = NEED_BIGGER_ARRAY;
				++i;

				
			} 
		}

	maxCount = i;

	return retVal; 
}


std::expected<void, int> LogisticsData::setMechToModify( LogisticsMech* pMech )
{
	if ( !pMech )
		return std::unexpected(-1);
	if ( currentlyModifiedMech == pMech )
		return {};
	if ( currentlyModifiedMech )
	{
		if ( auto result = cancelMechModfications(); !result )
			return result;
	}
	ensureMechBayDataLoaded("setMechToModify");
	currentlyModifiedMech = pMech;
	oldVariant = pMech->getVariant();

	modifiedVariant = std::make_unique<LogisticsVariant>( *oldVariant );
	pMech->setVariant( modifiedVariant.get() );

	return {};
}

void encryptFile (mc2::czstring inputFile, mc2::czstring outputFile)
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
	rawData = static_cast<MemoryPtr>(malloc(fileSize));
	zlibData = static_cast<MemoryPtr>(malloc(fileSize*2));
	LZData = static_cast<MemoryPtr>(malloc(fileSize*2));

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

void decryptFile (mc2::czstring inputFile, mc2::czstring outputFile)
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
	
		rawData = static_cast<MemoryPtr>(malloc(fileSize));
		zlibData = static_cast<MemoryPtr>(malloc(zlibSize));
		LZData = static_cast<MemoryPtr>(malloc(lzSize));
	
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

std::expected<void, int> LogisticsData::acceptMechModifications( mc2::czstring name )
{
	if ( !currentlyModifiedMech )
		return std::unexpected(-1);
	// Reject before changing money, names or inventory. Campaign persistence
	// still keeps grandfathered equipment; reusing its old variant is allowed.
	if (!modifiedVariant || !mc2pkg::canSaveLocationVariant(modifiedVariant->locationRecord()))
		return std::unexpected(INVALID_VARIANT);

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
		for (auto & iter : inventory)
		{
			if ( iter->getVariant() == oldVariant && iter.get() != currentlyModifiedMech  )
			{
				bFound = 1;
			}
		}
	}

	if ( !bFound )
	{
		const auto vIter = std::find_if( variants.begin(), variants.end(),
			[this](const auto& variant) { return variant.get() == oldVariant; } );
		if ( vIter != variants.end()
			&& mc2::str::iequals( oldVariant->getName(), mc2::str::from_c( name ) ) )
		{
			variants.erase( vIter );
		}
	}

	// Every variant that already carries the new name is deleted, in list order, before the modified variant takes the name.
	const std::string wanted = mc2::str::from_c( name );
	for ( auto vIter = variants.begin(); vIter != variants.end(); )
	{
		if ( mc2::str::iequals( (*vIter)->getName(), wanted ) )
		{
			vIter = variants.erase( vIter );
		}
		else
			++vIter;
	}

	currentlyModifiedMech->getVariant()->setName( wanted );
	variants.push_back( std::move(modifiedVariant) );
	missionInfo->decrementCBills( currentlyModifiedMech->getVariant()->getCost() );

	currentlyModifiedMech = nullptr;
	oldVariant = nullptr;


	// temporary, looking for dangling pointers
	for (auto & iter : inventory)
	{
		if ( iter->getVariant()->getCost() )
		{
			bFound = 1;
		}
	}
 
#ifndef VIEWER

	if ( MPlayer )
	{
		// save the player created variants
		for (auto & variant : variants)
		{
			if ( !variant->isDesignerMech() )
			{
				FullPathFileName mechFile;
				mechFile.init("data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR ,variant->getName().c_str(),".var");

				FitIniFile file;
				file.create(mechFile);
				variant->save( file, 0 );
				file.close();

                const char* filename = static_cast<const char*>(mechFile);
				encryptFile(filename, filename);
			}
		}
	}

#endif

	return {};
}
std::expected<void, int> LogisticsData::acceptMechModificationsUseOldVariant( mc2::czstring name )
{
	if ( !currentlyModifiedMech )
		return std::unexpected(-1);

	if ( oldVariant )
	{
		missionInfo->incrementCBills( oldVariant->getCost() );
	}

	LogisticsVariant* pVar = getVariant( name ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);

	if ( !pVar )
		Assert( 0, 0, "couldn't find the old variant\n" );

	modifiedVariant.reset();

	currentlyModifiedMech->setVariant( pVar );
	missionInfo->decrementCBills( pVar->getCost() );
	
	currentlyModifiedMech = nullptr;
	oldVariant = nullptr;	

	return {};
}

bool LogisticsData::canReplaceVariant( mc2::czstring name )
{
	int nameCount = 0;
	const std::string wanted = mc2::str::from_c( name );
	for (auto & iter : inventory)
		{
			if ( mc2::str::iequals( iter->getName(), wanted ) )
			{
				nameCount++;
				if ( iter->getVariant() != oldVariant && iter.get() != currentlyModifiedMech )
				{
					return 0;
				}
			}
		}

		if ( nameCount > 1 )
			return 0;

		for (auto & variant : variants)
		{
			if ( variant->isDesignerMech() && mc2::str::iequals( variant->getName(), wanted ) )
				return 0;
		}

		return true;
}

bool	LogisticsData::canDeleteVariant( mc2::czstring name )
{
	auto pVariant = getVariant( name );
	if ( !pVariant )
		return 0;
		
	if ( !canReplaceVariant(name) )
		return 0;

	const std::string wanted = mc2::str::from_c( name );
	if ( (currentlyModifiedMech && mc2::str::iequals( currentlyModifiedMech->getName(), wanted )) ||
		 (oldVariant && mc2::str::iequals( oldVariant->getName(), wanted )) )
		return 0;

	return 1;

}

std::expected<void, int> LogisticsData::cancelMechModfications()
{
	if ( !currentlyModifiedMech )
		return std::unexpected(-1);

	modifiedVariant.reset();

	currentlyModifiedMech->setVariant( oldVariant );

	oldVariant = nullptr;
	currentlyModifiedMech = nullptr;

	return {};
}

std::string			LogisticsData::getCurrentOperationFileName()
{
	return missionInfo->getCurrentOperationFile();
}
std::string			LogisticsData::getCurrentVideoFileName()
{
	return missionInfo->getCurrentVideo();
}

std::string			LogisticsData::getCurrentMissionDescription()
{
	return missionInfo->getCurrentMissionDescription();
}


std::string				LogisticsData::getCurrentMissionFriendlyName( )
{
	return missionInfo->getCurrentMissionFriendlyName();
}

std::string				LogisticsData::getMissionFriendlyName( mc2::czstring missionName )
{
	return missionInfo->getMissionFriendlyName( missionName );
}
