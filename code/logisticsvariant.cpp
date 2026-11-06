#define LOGISTICSVARIANT_CPP
/*************************************************************************************************\
LogisticsVariant.cpp			: Implementation of the LogisticsVariant component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "logisticsvariant_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "package_chassis.h"
#include <filesystem>



int LogisticsChassis::weightClasses[5] = { 0, 35, 55, 75, 100};
/*[0- 34	Light
35-55		Medium 
56-75		Heavy
76-100		Assualt
*/


LogisticsChassis::LogisticsChassis()
{
	appearance = nullptr;
	chassisNameID = 0;
	baseCost = 0;
	maxWeight = 0;
	refCount = 0;
	bIsVehicle = false;   // VEHICLE-AUTO-PILOT-1
	canHaveActiveProbe =
	canHaveAdvSensor = 
	canHaveExtendedSensor = 
	canHaveECM = 
	canHaveGlobalSensor = 
	canHaveOptics =
	canHaveJumpJets = 0;

	// UI-PROFILE-1 (Phase-2 slice 6): no override until init() resolves one.
	uiNameIDOverride = -1;
	uiEncyclopediaIDOverride = -1;
}

int LogisticsChassis::init( CSVFile* file, int chassisID )
{
	ID = chassisID;
	char buffer[256];
	
	fileName = mc2::str::ascii_lower(mc2::str::from_c(file->getFilename()));

	// BT2018-FIRST-CLASS P7: package chassis carry their region/mount/mass record.
	package = packageChassisByStem( std::filesystem::path( fileName ).stem().string() );

	long result = file->readLong( 11, 5, baseCost );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 5, 2, chassisNameID );
	gosASSERT( result == NO_ERR );
	
	result = file->readFloat(  3, 5, maxWeight );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 12, 5, maxArmor );
	gosASSERT( result == NO_ERR );
	
	result = file->readLong( 8,2, helpID );
	gosASSERT( result == NO_ERR );
	
	result = file->readLong( 6, 2,  encyclopediaID );
	gosASSERT( result == NO_ERR );

	file->readLong( 12, 2, iconPictureIndex );
	gosASSERT( result == NO_ERR );

	
	houseID;
	result = file->readLong( 4, 2, houseID );
	ID |= ((houseID << 8) & 0x0000ff00);
	gosASSERT( result == NO_ERR );

	for ( int row = 9; row < 12; ++row )
	{
		result = file->readString( row, 2, buffer, 256 );
		gosASSERT( result == NO_ERR );
		iconFileNames[row - 9] = buffer;
	}

	result = file->readBoolean(  3, 8, canHaveAdvSensor );
	gosASSERT( result == NO_ERR );
	
	result = file->readBoolean(4, 8, canHaveOptics );
	gosASSERT( result == NO_ERR );
	
	result = file->readBoolean( 5, 8, canHaveGlobalSensor );
	gosASSERT( result == NO_ERR );
	
	result = file->readBoolean( 6, 8, canHaveECM );
	gosASSERT( result == NO_ERR );
	
	result = file->readBoolean( 7, 8, canHaveActiveProbe );
	gosASSERT( result == NO_ERR );

	result = file->readBoolean( 8,8, canHaveJumpJets );
	 gosASSERT( result == NO_ERR );

 	result = file->readBoolean( 9,8, canHaveExtendedSensor );
	 gosASSERT( result == NO_ERR );


	result = file->readFloat(  7, 5, speed );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 4, 5, maxHeat );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 6, 5, baseArmor );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 13, 2, componentAreaWidth );
	gosASSERT( result == NO_ERR );

	result = file->readLong( 14, 2, componentAreaHeight );
	gosASSERT( result == NO_ERR );

	int ID = IDS_VERY_LIGHT;
	for ( int i = 4; i > -1; --i )
	{
		if ( maxWeight > weightClasses[i] )
		{
			ID = IDS_VERY_LIGHT + i + 1;
			break;
		}		
	}

	mechClass = cLoadString(ID, 256).c_str();

	// VEHICLE-AUTO-PILOT-1: logistics-safe vehicle detection. objTypeManager is null at the
	// logistics/pilot-ready screen, so we cannot query the real object class. Use two
	// CSV-visible signals (computed once here, where the chassis CSV is open):
	//  (1) ground vehicles list a "Vehicle ..." component (e.g. "Vehicle Sensors/Engine") in
	//      the Item* component rows; mechs never do (verified across stock data/objects).
	//  (2) rotor/air units (scoutcopter etc.) reuse the mech chassis layout verbatim, so the
	//      only logistics-visible tell is the chassis filename token "copter".
	{
		bool isVeh = false;
		char comp[256];
		for ( int r = 26; r <= 48 && !isVeh; ++r )            // Item0..Item22 component rows, name in col 5
		{
			if ( NO_ERR == file->readString( r, 5, comp, 256 ) )
			{
				S_strlwr( comp );
				if ( 0 == S_strncmp( comp, "vehicle", 7 ) )
					isVeh = true;
			}
		}
		if ( !isVeh && fileName.find( "copter" ) != std::string::npos )
			isVeh = true;
		bIsVehicle = isVeh;
	}

	// UI-PROFILE-1 (Phase-2 slice 6): resolve optional data-owned UI overrides,
	// keyed by this chassis' filename. Gate OFF / no registry entry -> null ->
	// overrides stay unset -> the accessors keep their legacy iconFileNames[]/IDS_
	// reads (stock parity). Report-only: a declared-but-missing icon file is logged
	// (UPV_UI_ASSET_MISSING) and still falls through to the legacy slot, never
	// blocking load. NO icon-atlas blit-math touch — only which asset/id is chosen.
	// SINGLE-SOURCE-UNIT-DEFINITION-1 (slice 7): resolve the UI facet through the
	// SAME pure merge the sim/mover side now uses (mergeUIProfile), so the two
	// readers can no longer diverge. The logistics reader has no inline `.fit`
	// [UIProfile] block (it parses the chassis CSV), so its base is null and the
	// additive-merge registry is the sole `over` source — identical result to the
	// prior hand-inlined copy, but via ONE shared resolution point. Legacy accessor
	// shape (uiIconOverride[]/id + iconFileNames[] fallback) is untouched: no atlas
	// blit-math change (mechicon.cpp CANARY).
	if (const UIProfile* reg = uiProfileForChassis(fileName.c_str())) {
		const UIProfile up = mergeUIProfile(/*base=*/nullptr, /*over=*/reg);
		if (!up.icon.empty())       uiIconOverride[0] = up.icon.c_str();
		if (!up.silhouette.empty()) uiIconOverride[1] = up.silhouette.c_str();
		if (!up.portrait.empty())   uiIconOverride[2] = up.portrait.c_str();
		if (up.nameStringId != UI_STRING_ID_UNSET)         uiNameIDOverride = up.nameStringId;
		if (up.encyclopediaStringId != UI_STRING_ID_UNSET) uiEncyclopediaIDOverride = up.encyclopediaStringId;
		// REPORT-ONLY asset-resolve check (UPV_UI_ASSET_MISSING): a declared icon
		// file that does not exist would silently fall back to the legacy slot.
		for (auto & uiIconOverrideElem : uiIconOverride) {
			if (!uiIconOverrideElem.empty() && !fileExists(uiIconOverrideElem.c_str()))
				mc2::fmt::print_out("[UIProfile:VALIDATE] {}: declared UI asset '{}' not found (reason={:d})\n", fileName, uiIconOverrideElem, mc2::fmt::as_i32(static_cast<int>(UPV_UI_ASSET_MISSING)));
		}
	}

	return 0;


}

int LogisticsChassis::getArmorClass() const
{
	if (baseArmor < 96)
		return IDS_ARMOR0;
	else if (baseArmor < 113)
		return IDS_ARMOR1;
	else if (baseArmor < 195)
		return IDS_ARMOR2;
	else if (baseArmor < 304)
		return IDS_ARMOR3;
	else
		return IDS_ARMOR4;
}


int LogisticsChassis::getDisplaySpeed() const
{
	return getDisplaySpeedForSpeed( static_cast<long>(speed) );
}

int LogisticsChassis::getDisplaySpeedForSpeed( long speed ) const
{
	switch( speed )
	{
	case 5:
		return 17;
		break;
	case 10:
		return 34;
		break;
	case 15:
		return 54;
		break;
	case 19:
		return 65;
		break;
	case 23:
		return 86;
		break;
	case 27:
		return 97;
		break;
	case 31:
		return 119;
		break;
	case 35:
		return 130;
		break;
	}
	return -1;
}

int	LogisticsChassis::getSensorID() const
{
	if ( canHaveAdvSensor )
		return 15;
	else if ( canHaveExtendedSensor )
		return 14;
	else if ( canHaveGlobalSensor )
		return 17;
	else 
		return -1;

	return -1;
}

int LogisticsChassis::getECM() const
{
	if ( canHaveECM )
		return 38;

	return -1;
}


LogisticsVariant::LogisticsVariant()
{
	componentCount = 0;
	ID = -1;
	chassis = nullptr;
	availableToUser = 0;

	memset( components, 0, sizeof( components ) );
	bDesignerMech = 0;
	bHidden = 0;

	fileID = 0;
}

LogisticsVariant::LogisticsVariant( const LogisticsChassis* pChassis , bool Designer)
{
	chassis = const_cast<LogisticsChassis*>(pChassis);
	availableToUser = 1;
	componentCount = 0;
	ID = (pChassis->refCount << 16) | pChassis->ID;
	chassis->refCount++;
	bDesignerMech = Designer;
	bHidden = 0;
	fileID = 0;


}
LogisticsVariant::~LogisticsVariant()
{
	chassis->refCount--;

	if ( chassis->refCount == 0 )
		delete chassis;

	chassis = nullptr;
}

LogisticsVariant::LogisticsVariant( const LogisticsVariant& src )
{
	chassis = src.chassis;
	availableToUser = src.availableToUser;
	componentCount = src.componentCount;
	
	for ( int i = 0; i < componentCount; ++i )
	{
		components[i] = src.components[i];
	}	

	variantName = src.variantName;
	
	ID = (chassis->refCount << 16) | chassis->ID;
	chassis->refCount++;

	bDesignerMech = 0;
	bHidden= 0;
	fileID = src.fileID;
}

LogisticsVariant& LogisticsVariant::operator=( const LogisticsVariant& src )
{
	if ( &src != this )
	{
		chassis = src.chassis;
		availableToUser = src.availableToUser;
		componentCount = src.componentCount;
		
		for ( int i = 0; i < componentCount; ++i )
		{
			components[i] = src.components[i];
		}	

		variantName = src.variantName;
		
		ID = (chassis->refCount << 16) | chassis->ID;
		chassis->refCount++;

		bDesignerMech = 0;

		fileID = src.fileID;
	}

	return *this;
}

bool LogisticsVariant::operator==( const LogisticsVariant& src ) const
{
		if ( chassis->chassisNameID != src.chassis->chassisNameID )
			return 0;
		
		if ( availableToUser != src.availableToUser )
			return 0;

		if ( componentCount != src.componentCount )
			return 0;
		
		for ( int i = 0; i < componentCount; ++i )
		{
			if ( components[i].component->getID() != src.components[i].component->getID() )
			{
				return 0;
			}
		}	

//		if ( variantName != src.variantName )
//			return 0;
		
		return 1;

}


int LogisticsVariant::init( CSVFile* file, LogisticsChassis* pChassis, int Variant )
{
	bDesignerMech = true;
	
	int offset = 97 * Variant;

	fileID = Variant;
	
	gosASSERT( pChassis );
	chassis = pChassis;
	
	ID = (pChassis->refCount << 16) | pChassis->ID;\
	pChassis->refCount++;

	file->readBoolean( 21 + offset, 4, bHidden );

	char buffer[256];
	if ( NO_ERR == file->readString( 23 + offset, 2, buffer, 256 ) )
	{
		variantName = buffer;
	}

	LogisticsComponent*  pComps[128];
	long xLocs[128];
	long yLocs[128];
	memset( pComps, 0, sizeof( LogisticsComponent* ) * 128 );
	long componentCount = 0;
	
	for ( int i =26; i < 97; i++ )
	{
		long componentID;
		if ( NO_ERR == file->readLong( offset + i, 5, componentID ) && componentID != 0xff )
		{
			LogisticsComponent* pComp = LogisticsData::instance->getComponent(componentID ).transform(std::addressof<LogisticsComponent>).value_or(nullptr);
			if ( pComp )
			{
				file->readLong( offset + i, 4, yLocs[componentCount] );
				file->readLong( offset + i, 3, xLocs[componentCount] );
				pComps[componentCount++] = pComp;
			}
		}
	}

	for ( int j = 4; j > 0; j-- )
	{
		for ( int k = 3; k >0; k-- )
		{
			for (int i = 0; i < componentCount; i++ )
			{
				LogisticsComponent*
				pComponent = pComps[i];

				if ( pComponent&& pComponent->getComponentWidth() == k 
					&& pComponent->getComponentHeight() == j )
				{
					if ( !addComponent( pComps[i]->getID(), xLocs[i], yLocs[i] ) )
					{
						char errorString[256];
						mc2::fmt::to_buf(errorString, "Couldn't add component with id {:d}", mc2::fmt::as_i32(pComponent->getID()));
					}
				}
			}
		}

	}

	
	return 0;


}

bool LogisticsVariant::addComponent( int idFromFitFile, long& x, long& y )
{
	LogisticsComponent*
		pComponent = LogisticsData::instance->getComponent( idFromFitFile ).transform(std::addressof<LogisticsComponent>).value_or(nullptr);


	if ( !pComponent )
	{
	//	Assert( 0, idFromFitFile, "couldn't find the component in the fit file\n" );
		return false;
	}

	if ( pComponent->getType() == COMPONENT_FORM_JUMPJET )
	{
		components[componentCount].location = LEGS;
		x = y = -2;
	}

	else if ( pComponent->getType() == COMPONENT_FORM_ECM || pComponent->getType() == COMPONENT_FORM_SENSOR )
	{
		components[componentCount].location = HEAD;
		x = y = -3;
	}

	else
	{


		// need to see if this thing will fit
		long componentWidth = pComponent->getComponentWidth();
		long componentHeight = pComponent->getComponentHeight();

		if ( x == -1 && y == -1 )
		{
			for ( int j = 0; j < chassis->componentAreaHeight && x == -1; j++ )
			{
				for ( int i = 0; i < chassis->componentAreaWidth && x == -1; i++ )
				{
					bool bAdd = true;
					for ( int l = 0; l < componentHeight; ++l )
					{
						for ( int k =0; k < componentWidth; ++k )
						{

 							if ( getComponentAtLocation( i +k, j + l ) 
								|| ( i + k >= chassis->componentAreaWidth )
								|| ( j +l >= chassis->componentAreaHeight ) )
							{
								bAdd = false;
								break;
								break;
							}
						}
					}

					if ( bAdd )
					{
						x = i;
						y = j;
			
					}
				}
			}
		}

		if ( x == -1 && y == -1 )
			return 0;

		if ( x > -1 )
		{
			for ( int i =0; i < componentWidth; ++i )
			{
				for ( int j = 0; j < componentHeight; ++j )
				{
 					if ( getComponentAtLocation( x +i, y + j ) )
					{
						char errorString[1024];
						mc2::fmt::to_buf(errorString, "couldn't add component {} to variant {} because another object was in the specified location", mc2::fmt::safe(pComponent->getName()), this->getName());
		//				Assert( 0, 0, errorString );
						return false; 
					}
				}
			}
		}

		components[componentCount].location = CHEST;
	}

	components[componentCount].component = pComponent;
	components[componentCount].xCoord = x;
	components[componentCount].yCoord = y;
	componentCount++;
	return true;
}
