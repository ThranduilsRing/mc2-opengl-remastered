// LogisticsVariant: the variant stat getters, component placement checks and lookups, save, and the LogisticsVehicle setup.
// Declared in the class headers; shared names in logisticsvariant_internal.h.
#include "logisticsvariant_internal.h"
#include <array>
#include "mc2fmt.h"
#include "mc2str.h"
#include "objective_text.h"
#include "package_chassis.h"


// Next discrete move-speed class up the 5/10/15/19/23/27/31/35 ladder.
static long bumpSpeedClass( long s )
{
	static const long ladder[] = { 5, 10, 15, 19, 23, 27, 31, 35 };
	for ( int i = 0; i < 7; i++ )
		if ( s <= ladder[i] )
			return ladder[i + 1];
	return s;   // already top class
}

int LogisticsVariant::getCost( ) const 
{
	int cost = chassis->baseCost;

	for ( int i = 0; i < componentCount; ++i )
	{
		cost += components[i].component->getCost();
	}

	return cost;
}

int LogisticsVariant::getWeight() const
{
	//Yup.  20% of base weight Plus 7 tons is used by armor and misc components which we do not explicitly track.

	float baseWeight = (chassis->maxWeight * .20) + 7.0;
	// BT2018-FIRST-CLASS P7: a package chassis carries an explicit empty mass (BT structure,
	// engine, armour) so the bay weight and the payload rule in canAddComponent agree.
	if ( chassis->getPackage() )
		baseWeight = chassis->getPackage()->emptyMass;

	for ( int i = 0; i < componentCount; ++i )
	{
		baseWeight += components[i].component->getWeight();
	}

	return baseWeight;
}

const std::string& LogisticsVariant::getMechClass() const
{
	return chassis->mechClass;
}

int			LogisticsVariant::getMaxJumpRange() const
{
	return 5;
}

// BOGUS, this is entirely made up.  There is no real spec for jumpranges yet
int LogisticsVariant::getJumpRange() const
{
	int jumpJetCount = 0;

	for ( int i = 0; i < componentCount; ++i )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_JUMPJET )
			jumpJetCount++;
	}

	if ( jumpJetCount )
		return 5;

	return 0;
}

int LogisticsVariant::getHeat() const
{
	int retVal = 0;
	for ( int i = 0; i < componentCount; ++i )
	{
		retVal += components[i].component->getHeat();
	}

	return retVal;
}

// BOGUS -- range isn't really the right thing to add...
int LogisticsVariant::getArmor( ) const
{
	int retArmor = chassis->baseArmor;

	for ( int i = 0; i < componentCount; ++i )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_BULK )
		{
			// Armor points per BULK unit come from the component's value
			// (compbas Damage column), not a hardcoded 32 — so ferro-fibrous
			// (value 64) gives double the armor of basic Armor Plating (value 32).
			int pts = static_cast<int>(components[i].component->getDamage());
			retArmor += pts > 0 ? pts : 32;
		}
	}

	return retArmor;
}

bool LogisticsVariant::hasXLEngine() const
{
	static const bool s_engineXL = mc2::gate::get<"MC2_ENGINE_XL">();
	if ( !s_engineXL )
		return false;
	for ( int i = 0; i < componentCount; ++i )
		if ( components[i].component->getType() == COMPONENT_FORM_ENGINE )
			return true;
	return false;
}

long LogisticsVariant::effectiveSpeed() const
{
	long s = static_cast<long>(chassis->speed);
	if ( hasXLEngine() )
		s = bumpSpeedClass( s );
	return s;
}

int LogisticsVariant::getSpeed() const
{
	return effectiveSpeed();
}

int LogisticsVariant::getDisplaySpeed() const
{
	return chassis->getDisplaySpeedForSpeed( effectiveSpeed() );
}
int		LogisticsVariant::getMaxHeat() const
{
	int heat = 0;
	for ( int i = 0; i < componentCount; i++ )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_HEATSINK )
		{
			heat += components[i].component->getDamage();
		}
	}

	return heat + chassis->maxHeat;
}
// MC2_JUMPJETS_ALL (default OFF; launcher Engine Option): allow jump jets on any
// chassis (e.g. mount JJ on a Mad Cat). Centralized here so the mech-bay JJ slot,
// its drop target, and the add-component validation all agree.
bool LogisticsChassis::jumpJetsAllowed() const
{
	static const bool s_jumpjetsAll = mc2::gate::get<"MC2_JUMPJETS_ALL">();
	return canHaveJumpJets || s_jumpjetsAll;
}

// if you pass in -1's for x and y, we'll figure out where it can go, and return where it went
int LogisticsVariant::canAddComponent( LogisticsComponent* pComponent, long& x, long& y ) const
{
	/*  weight no longer matters
	int weight = getWeight();
	if ( weight + pComponent->getWeight() > chassis->maxWeight )
		return COMPONENT_TOO_HEAVY;*/

	if ( getHeat() + pComponent->getHeat() > getMaxHeat() )
		return COMPONENT_TOO_HOT;

	if ( pComponent->getType() == COMPONENT_FORM_JUMPJET  )
	{
		// jumpJetsAllowed() honors MC2_JUMPJETS_ALL, so the mech-bay JJ slot
		// (mechlabscreen.cpp:356/1576) and this drop check agree.
		if ( !chassis->jumpJetsAllowed() )
			return JUMPJETS_NOT_ALLOWED;
		else if ( hasJumpJets() )
			return ONLY_ONE_JUMPJET_ALLOWED;
		else if ( ( x!= -1 && x != -2)
					|| ( y != -1 && y != -2 ) )
			return COMPONENT_SLOT_FULL;

		return packageCheck( pComponent, -2, -2 );
	}
	else if ( x == -2 && y == -2 ) // trying to put something illegal in jump jet slot
		return COMPONENT_SLOT_FULL;

	// Only one XL engine upgrade per mech.
	if ( pComponent->getType() == COMPONENT_FORM_ENGINE )
	{
		for ( int i = 0; i < componentCount; ++i )
			if ( components[i].component->getType() == COMPONENT_FORM_ENGINE )
				return ADD_COMPONENT_FAILED;
	}
		 

	// BT2018-FIRST-CLASS P7: a package chassis auto-places into a region with a free mount
	// (the legacy scan below would take the first free cell, even one in the wrong location).
	if ( chassis->getPackage() && x == -1 && y == -1 && !packageAutoPlace( pComponent, x, y ) )
	{
		const int why = packageCheck( pComponent, -1, -1 );
		return why ? why : ADD_COMPONENT_FAILED;
	}

	if ( x!= -1 && y != -1 )
	{
		for ( int i = 0; i < pComponent->getComponentWidth(); i++ )
		{
			for ( int j = 0; j < pComponent->getComponentHeight(); j++ )
			{
				if ( getComponentAtLocation( x + i, y  + j)
					|| x + i >= chassis->componentAreaWidth
					|| j + y >= chassis->componentAreaHeight )
					return COMPONENT_SLOT_FULL;
			}  
		}
	}
	else
	{
		for ( int j = 0; j < chassis->componentAreaHeight && x == -1; j++ )
		{
			for ( int i = 0; i < chassis->componentAreaWidth && x == -1; i++ )
			{
				if ( !getComponentAtLocation( i, j ) )
				{
					bool bAdd = true;
					for ( int l = 0; l < pComponent->getComponentHeight(); ++l )
					{
						for ( int k =0; k < pComponent->getComponentWidth(); ++k )
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

		if ( x == -1 || y == -1 )
		{
			return ADD_COMPONENT_FAILED;
		}
	}

	if ( pComponent->getType() == COMPONENT_FORM_BULK )
	{
		int pts = static_cast<int>(pComponent->getDamage());
		if ( getArmor() + ( pts > 0 ? pts : 32 ) > getMaxArmor() )
			return NO_MORE_ARMOR;
	}

	if ( pComponent->getType() == COMPONENT_FORM_SENSOR )
	{
		if ( !chassis->canHaveAdvSensor )
			return SENSORS_NOT_ALLOWED;
		else if ( hasSensor() )
			return ONLY_ONE_SENSOR_ALLOWED;
	}

	if ( pComponent->getType() == COMPONENT_FORM_ECM  )
	{
		if ( !chassis->canHaveECM )
			return ECM_NOT_ALLOWED;
		else if ( hasECM() )
			return ONLY_ONE_ECM_ALLOWED;
	}

	// BT2018-FIRST-CLASS P7: whole-region placement, a free BT mount, the mass budget.
	if ( chassis->getPackage() )
		return packageCheck( pComponent, x, y );

	

	return 0;

}

const LogisticsChassis::ComponentInfo* LogisticsVariant::getComponentAtLocation( long x, long y ) const
{
	if ( x == -1 && y == -1 )
		return nullptr;

	for ( int i = 0; i < componentCount; ++i )
	{
		LogisticsComponent* pComponent = components[i].component;

		for ( int j = 0; j < pComponent->getComponentHeight(); ++j )
		{
			for ( int k = 0; k < pComponent->getComponentWidth(); ++k )
			{
				if ( components[i].xCoord + k == x
					 && components[i].yCoord + j == y )
				{
					return &components[i];
				}
			}
		}
	}

	return nullptr;
}

bool LogisticsVariant::hasJumpJets() const
{
	for ( int i = 0; i < componentCount; i++ )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_JUMPJET )
			return true;
	}

	return false;
}
bool LogisticsVariant::hasECM() const
{
	for ( int i = 0; i < componentCount; i++ )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_ECM )
			return true;
	}

	return false;

}
bool LogisticsVariant::hasSensor() const
{
	for ( int i = 0; i < componentCount; i++ )
	{
		if ( components[i].component->getType() == COMPONENT_FORM_SENSOR )
			return true;
	}

	return false;

}


int	LogisticsVariant::getComponentsWithLocation( long& count, long* IDArray, long* xLocationArray, long* yLocationArray )
{
	if ( count < componentCount ) 
		return NEED_BIGGER_ARRAY;

	for ( int i = 0; i < componentCount; ++i )
	{
		IDArray[i] = components[i].component->getID();
		xLocationArray[i] = components[i].xCoord;
		yLocationArray[i] = components[i].yCoord;
	}

	count = componentCount;

	return 0;
}

int	LogisticsVariant::getComponents( long& count, long* array )
{
 	for ( int i = 0; i < componentCount; ++i )
	{
		array[i] = components[i].component->getID();

	}

	count = componentCount;

	return count;

}

int		LogisticsVariant::removeComponent( long xCoord, long yCoord )
{
	LogisticsChassis::ComponentInfo* info = const_cast<LogisticsChassis::ComponentInfo*>(getComponentAtLocation( xCoord, yCoord ));

	if ( !info )
		return -1;

	if ( info->component && info->component->getType() == COMPONENT_FORM_HEATSINK )
	{
		if ( getMaxHeat() - info->component->getDamage() < getHeat() )
			return INSUFFICIENT_HEAT;
	}

	
	if ( !info )
		return INVALID_LOCATION;

	for ( int i = 0; i < componentCount; i++ )
	{
		if ( &components[i] == info )
		{
			memmove( &components[i], &components[i + 1], (componentCount - ( i + 1 )) * sizeof( LogisticsChassis::ComponentInfo ) );
			break;
		}
	}

	componentCount --;

	return 0;
}

// if you pass in -1's for x and y, we'll figure out where it can go, and return where it went
int		LogisticsVariant::addComponent( LogisticsComponent* pComponent, long& xCoord, long& yCoord )
{
	int retVal = canAddComponent( pComponent, xCoord, yCoord );

	if ( retVal == 0 )
	{
		if ( addComponent( pComponent->getID(), xCoord, yCoord ) )
			return 0;
		else
			return ADD_COMPONENT_FAILED;
	}

	return retVal;
}

long LogisticsVariant::save( FitIniFile& file, long counter )
{
	char tmp[256];
	char tmp2[256];
	mc2::fmt::to_buf(tmp, "Variant{:d}", mc2::fmt::as_i32(counter));
	file.writeBlock( tmp );
	
	file.writeIdString( "Chassis", chassis->fileName.c_str() );
	file.writeIdString( "VariantName", variantName.c_str() );
	file.writeIdLong( "ComponentCount", componentCount );

	for ( int i = 0; i < componentCount; i++ )
	{
		mc2::fmt::to_buf(tmp, "Component{:d}", mc2::fmt::as_i32(i));
		file.writeIdLong(tmp, components[i].component->getID() );
		
		strcpy( tmp2, tmp );
		strcat( tmp2, "x" );
		file.writeIdLong( tmp2, components[i].xCoord );

		strcpy( tmp2, tmp );
		strcat( tmp2, "y" );
		file.writeIdLong( tmp2, components[i].yCoord );

		strcpy( tmp2, tmp );
		strcat( tmp2, "Location" );
		file.writeIdLong( tmp2, components[i].location );
	}

	return 0;
}

void LogisticsVariant::setName( std::string name )
{
	variantName = std::move( name );
}

int	LogisticsVariant::getComponents( long& count, LogisticsComponent** array )
{
	if ( count < componentCount ) 
		return NEED_BIGGER_ARRAY;

	for ( int i = 0; i < componentCount; ++i )
	{
		array[i] = components[i].component;
	}

	count = componentCount;

	return 0;

}

LogisticsComponent*	LogisticsVariant::getCompAtLocation( int i, int j, long& realI, long& realJ )
{
	realI = realJ = -1;
	const LogisticsChassis::ComponentInfo* pInfo = getComponentAtLocation( i, j );

	if ( pInfo )
	{
		realI = pInfo->xCoord;
		realJ = pInfo->yCoord;
		return pInfo->component;
	}

	return nullptr;
}

int	LogisticsVariant::getComponentLocation( LogisticsComponent* pComp, long& x, long& y )
{
	const LogisticsChassis::ComponentInfo* pInfo = getComponentAtLocation( x, y );
	x = y = -1;

	if ( pInfo )
	{
		x = pInfo->xCoord;
		y = pInfo->yCoord;
	}
	else
	{
		for ( int i = 0; i < componentCount; i++ )
		{
			if ( (components[i].component) == pComp )
			{
				x = components[i].xCoord;
				y = components[i].yCoord;
				return 0;
			}
		}
		return COMPONENT_NOT_FOUND;
	}
	
	return 0;

}

int			LogisticsVariant::getOptimalRangeString( uint32_t& color ) const
{
	float rangeDamage[3];

	std::array<uint32_t, 3> rangeColors = {0xff6e7c00, 0xff005392,0xffa21600  };


	memset( rangeDamage, 0, sizeof ( float ) * 3 );

	float maxDamage = -1.f;

	for ( int i = 0; i < componentCount; i++ )
	{
		if ( components[i].component->isWeapon() )
		{
			float damageTimeRating = components[i].component->getDamage();
			if ( components[i].component->getRecycleTime() )
				damageTimeRating = components[i].component->getDamage() / components[i].component->getRecycleTime();	
			rangeDamage[components[i].component->getRangeType()] += damageTimeRating;

			if ( rangeDamage[components[i].component->getRangeType()] > maxDamage )
				maxDamage = rangeDamage[components[i].component->getRangeType()];
	
		}
	}

	for (int i = 0; i < 3; i++ )
	{
		if ( rangeDamage[i] == maxDamage )
		{
			color = rangeColors[i];
			return IDS_HOTKEY1 + i;
		}
	}

	return IDS_HOTKEY1;

	
}

bool		LogisticsVariant::allComponentsAvailable() const
{

	// 06/05/01 hack, we wan't to hide this atlas for the whole game.
	if ( mc2::str::iequals( variantName, "Renard's Atlas" ) )
		return 0;

	for ( int i = 0; i < componentCount; i++ )
	{
		if ( !components[i].component->isAvailable() )
			return false;
	}

	return true;

}

void		LogisticsVariant::setAvailable( bool available ) 
{ 
	// 06/05/01 hack, we wan't to hide this atlas for the whole game.
	if ( mc2::str::iequals( variantName, "Renard's Atlas" ) )
		availableToUser = 0;

	else
		availableToUser = available; 
}

void LogisticsVehicle::init( FitIniFile& file )
{
	bIsVehicle = true;   // VEHICLE-AUTO-PILOT-1: this chassis is a vehicle/support unit
	componentCount = 0;
	
	file.seekBlock( "ObjectType" );
	// A vehicle file without an AppearanceName has an empty chassis file name; the old read left an uninitialised buffer in it.
	fileName = objective_text::readPlain( file, "AppearanceName" ).value_or( std::string() );

	file.seekBlock( "General" );
	file.readIdLong( "DescIndex", chassisNameID );
	file.readIdFloat( "CurTonnage", maxWeight );

	mechClass = cLoadString(IDS_VEHICLE_CLASS, 256).c_str();

	if ( NO_ERR !=file.readIdLong( "HouseID", houseID ) )
	{
		houseID = -1;
	}
	if ( NO_ERR != file.readIdLong( "EncyclopediaID", encyclopediaID ) )
	{
		encyclopediaID = IDS_VEHICLE_DESCRIPTION_0;
	}

	file.seekBlock( "VehicleDynamics" );
	file.readIdFloat( "MaxVelocity", speed );

	const char* parts[5] = { "Front", "Left", "Right", "Rear", "Turret" };

	baseArmor = 0;

	unsigned char pts;
	for (auto & part : parts)
	{
		file.seekBlock( part );
		file.readIdUChar( "MaxArmorPoints", pts );
		baseArmor += pts;
		file.readIdUChar( "CurInternalStructure", pts );
		baseArmor += pts;

	}

	file.seekBlock( "InventoryInfo" );
	file.readIdUChar( "NumWeapons", pts );

	char blockName[256];
	for (int i = 4; i < 4 + pts; i++ )
	{
		mc2::fmt::to_buf(blockName, "Item:{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			
			unsigned char fitID;
			file.readIdUChar( "MasterID", fitID );

			LogisticsComponent* pComponent = LogisticsData::instance->getComponent( fitID );
			if ( pComponent )
			{
			
				components[componentCount].component = pComponent;
				components[componentCount].xCoord = 0;
				components[componentCount].yCoord = 0;
				componentCount++;
			}
		
		}
	}
}

int LogisticsVehicle::getComponents( long& count, LogisticsComponent** array )
{
	gosASSERT( componentCount <= count );
	
	for ( int i = 0; i < componentCount; ++i )
	{
		array[i] = components[i].component;

	}

	count = componentCount;

	return count;
}
