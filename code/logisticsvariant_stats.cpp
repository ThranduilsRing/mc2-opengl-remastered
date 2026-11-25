// LogisticsVariant: the variant stat getters, component placement checks and lookups, save, and the LogisticsVehicle setup.
// Declared in the class headers; shared names in logisticsvariant_internal.h.
#include "logisticsvariant_internal.h"
#include <array>
#include "component_rules.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "objective_text.h"
#include "package_chassis.h"
#include "logistics_save_version.h"
#include "purchase_catalog.h"


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
	for ( int i = 0; i < componentCount; ++i )
		if ( components[i].component->getType() == COMPONENT_FORM_ENGINE )
			return true;
	return false;
}

long LogisticsVariant::effectiveSpeed() const
{
	long s = static_cast<long>(chassis->speed);
	if ( hasXLEngine() )
		s = mc2::components::nextSpeedClass( s );
	return s;
}

int LogisticsVariant::getSpeed() const
{
	return effectiveSpeed();
}

std::optional<int> LogisticsVariant::getDisplaySpeed() const
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

std::optional<std::reference_wrapper<const LogisticsChassis::ComponentInfo>> LogisticsVariant::getComponentAtLocation( long x, long y ) const
{
	if ( x == -1 && y == -1 )
		return std::nullopt;

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
					return std::cref(components[i]);
				}
			}
		}
	}

	return std::nullopt;
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

std::expected<void, int> LogisticsVariant::removeComponent( long xCoord, long yCoord )
{
	auto found = getComponentAtLocation( xCoord, yCoord );

	if ( !found )
		return std::unexpected(-1);
	const auto* info = &found->get();

	if ( info->component && info->component->getType() == COMPONENT_FORM_HEATSINK )
	{
		if ( getMaxHeat() - info->component->getDamage() < getHeat() )
			return std::unexpected(INSUFFICIENT_HEAT);
	}

	for ( int i = 0; i < componentCount; i++ )
	{
		if ( &components[i] == info )
		{
			memmove( &components[i], &components[i + 1], (componentCount - ( i + 1 )) * sizeof( LogisticsChassis::ComponentInfo ) );
			break;
		}
	}

	componentCount --;
	if (hasExplicitArmor()) layoutState.armor = armorAllocation();
	layoutState.version = 1;

	return {};
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
	const auto record = locationRecord();
	char tmp[256];
	mc2::fmt::to_buf(tmp, "Variant{:d}", mc2::fmt::as_i32(counter));
	file.writeBlock( tmp );
	
	// A layout-revised package chassis names itself under LayoutChassis, so an older reader (which asks for Chassis) rejects the
	// block instead of restoring its coordinates on a layout it does not know (code/logistics_save_version.h).
	const int layoutRev = chassis->getPackage() ? chassis->getPackage()->layoutRev : 1;
	file.writeIdString( mc2::logistics_save::variantChassisKey( layoutRev ), chassis->fileName.c_str() );
	if ( mc2::logistics_save::layoutRevised( layoutRev ) )
		file.writeIdLong( "LayoutRev", layoutRev );
	file.writeIdString("PurchasePack", mc2purchase::chassisSource(chassis->fileName).c_str());
	file.writeIdString( "VariantName", variantName.c_str() );
	return mc2pkg::writeLocationVariant(file, record) ? 0 : -1;
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

std::optional<std::reference_wrapper<LogisticsComponent>> LogisticsVariant::getCompAtLocation( int i, int j, long& realI, long& realJ )
{
	realI = realJ = -1;
	auto pInfo = getComponentAtLocation( i, j );

	if ( pInfo )
	{
		realI = pInfo->get().xCoord;
		realJ = pInfo->get().yCoord;
		if ( pInfo->get().component )
			return *pInfo->get().component;
	}

	return std::nullopt;
}

int	LogisticsVariant::getComponentLocation( LogisticsComponent* pComp, long& x, long& y )
{
	auto pInfo = getComponentAtLocation( x, y );
	x = y = -1;

	if ( pInfo )
	{
		x = pInfo->get().xCoord;
		y = pInfo->get().yCoord;
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

LogisticsVariant::LogisticsVariant(const LogisticsVehicle& vehicle, int chassisId)
	: LogisticsVariant(std::make_unique<LogisticsChassis>(vehicle).release(), true)
{
	chassis->ID = chassisId;
	ID = chassisId;
	variantName = chassis->getDisplayName();
	componentCount = vehicle.componentCount;
	std::copy_n(vehicle.components, componentCount, components);
	for (long i = 0; i < componentCount; ++i) components[i].location = CHEST;
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
	float rating = 25;
	file.readIdFloat("BattleRating", rating);
	baseCost = static_cast<long>(rating * 100);
	if (baseCost <= 0) {
		FitIniFile prices;
		if (prices.open("data/defs/purchase/vehicle_prices.fit") == NO_ERR && prices.seekBlock("VehiclePrices") == NO_ERR) {
			long perTon = 0, minimum = 0;
			prices.readIdLong("PerTon", perTon);
			prices.readIdLong("Minimum", minimum);
			long roleRate = 0;
			if (prices.readIdLong(fileName.c_str(), roleRate) == NO_ERR) perTon = roleRate;
			baseCost = mc2purchase::vehiclePrice(baseCost, maxWeight, perTon, minimum);
		}
	}
	maxHeat = 0;
	helpID = encyclopediaID = IDS_VEHICLE_DESCRIPTION_0;
	componentAreaWidth = 6;
	componentAreaHeight = 9;
	iconPictureIndex = 0;
	iconFileNames[0] = "MCL_GN_MECHICONRAVEN";
	iconFileNames[1] = iconFileNames[2] = "MCL_MC_MECHPAYLOADRAVEN";

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

	unsigned char pts = 0;
	for (auto & part : parts)
	{
		file.seekBlock( part );
		file.readIdUChar( "MaxArmorPoints", pts );
		baseArmor += pts;
		file.readIdUChar( "CurInternalStructure", pts );
		baseArmor += pts;

	}

	maxArmor = baseArmor;
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

			LogisticsComponent* pComponent = LogisticsData::instance->getComponent( fitID ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
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
