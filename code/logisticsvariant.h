#ifndef LOGISTICSVARIANT_H
#define LOGISTICSVARIANT_H
/*************************************************************************************************\
LogisticsVariant.h			: Interface for the LogisticsVariant component.  This thing
					is a simplified mech/vehicle object without AI.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

// forward declarations
class MechAppearance;
class LogisticsComponent;
class CSVFile;
class FitIniFile;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <expected>
#include "chassis_layout.h"
#include "armor_alloc.h"

#pragma warning (disable:4514)


enum LOGISTICS_COMPONENT_LOCATION
{
	HEAD = 0,
	CHEST = 1,
	LEGS = 2
};


namespace mc2pkg { struct PackageChassis; }
class LogisticsVariant;

class LogisticsChassis
{
public:

	LogisticsChassis( );
	int				init( CSVFile* fileName, int chassisID );
	void			setFitID( int id ){ fitID = id; }
	void			setNameID( long id ){ chassisNameID = id; }
	// UI-PROFILE-1: prefer a resolved UIProfile string-id override, else legacy.
	int				getNameID() const { return static_cast<int>(( uiNameIDOverride != -1 ? uiNameIDOverride : chassisNameID )); }
	int				getEncyclopediaID() const { return static_cast<int>(( uiEncyclopediaIDOverride != -1 ? uiEncyclopediaIDOverride : encyclopediaID )); }
	const std::string& getFileName() const { return fileName; }
	// The name lists, titles and headers show: string table, then the authored free-text name, then the variant, then the file stem.
	std::string		getDisplayName( const LogisticsVariant* variant = nullptr ) const;
	void			setAuthoredName( std::string name ) { authoredName = std::move( name ); }
	const std::string&	getAuthoredName() const { return authoredName; }
	int				getHouseID() const{ return houseID; }
	float			getMaxWeight() const{ return maxWeight; }
	const std::string&	getMechClass() const  { return mechClass; }
	long			getArmor() const { return baseArmor; }
	int				getArmorClass() const;
	float			getSpeed() const { return speed; }
	std::optional<int> getDisplaySpeed() const;
	std::optional<int> getDisplaySpeedForSpeed( long s ) const;
	void			setScale( float newScale ) { scale = newScale; }
	float			getScale() const { return scale; }
	long			getMaxArmor() const { return maxArmor; }
	std::optional<int> getSensorID() const;
	std::optional<int> getECM() const;
	bool			jumpJetsAllowed() const;   // honors MC2_JUMPJETS_ALL gate
	// VEHICLE-AUTO-PILOT-1: true for a LogisticsVehicle chassis (set in
	// LogisticsVehicle::init). Logistics-safe vehicle discriminator — does NOT need the
	// mission-only objTypeManager that loadObjectType derefs (which is null at the
	// logistics/pilot-ready screen).
	bool			isVehicle() const { return bIsVehicle; }
	// BT2018-FIRST-CLASS P7: the package chassis record (regions, mounts, mass budget) when this
	// chassis comes from a package; nullptr for every stock chassis.
	const mc2pkg::PackageChassis* getPackage() const { return package; }
	mc2::armor::Allocation baseArmorLoc{};
	std::array<int, 8> structure{};
	bool armorEditable = false;

protected:

	int					refCount;
	
	float				maxWeight;
	long				chassisNameID;	// resource ID
	long				baseCost;
	long				maxHeat;
	long				baseArmor;
	long				maxArmor;

	float					speed;
	float				scale;

	long				iconPictureIndex;
	long				ID;
	std::string			fileName;
	std::string			authoredName;	// the MechName cell, or free text in the buildings.csv NameID cell
	std::string			mechClass;
	std::string			iconFileNames[3];

	long				componentAreaWidth;
	long				componentAreaHeight;

	bool canHaveAdvSensor;//can Mech Have Advanced Sensor installed.
	bool canHaveOptics;//can Mech Have Optics Installed.
	bool canHaveGlobalSensor;//can Mech have Global Sensor installed.
	bool canHaveECM;//can Mech have ECM Installed.
	bool canHaveActiveProbe;//can Mech have Active Probe Installed.
	bool canHaveJumpJets;
	bool canHaveExtendedSensor;
	bool bIsVehicle;   // VEHICLE-AUTO-PILOT-1: set true by LogisticsVehicle::init

	long				encyclopediaID;
	long				helpID;
	long				houseID;

	long				fitID;

	// UI-PROFILE-1 (Phase-2 slice 6): optional data-owned UI overrides, resolved
	// ONCE in init() from the additive-merge UIProfile registry (keyed by chassis
	// filename). Empty string / -1 == "no override" == the accessor keeps its
	// legacy iconFileNames[]/IDS_ value. Stock chassis have no registry entry, so
	// all three strings stay empty and both ids stay -1 -> byte-identical stock UI.
	// Applied at getSmall/Medium/LargeIconFileName + getNameID/getEncyclopediaID.
	std::string			uiIconOverride[3];   // [0]=small(icon) [1]=medium(silhouette) [2]=large(portrait)
	long				uiNameIDOverride;         // -1 == unset
	long				uiEncyclopediaIDOverride; // -1 == unset

	MechAppearance*		appearance; 

	const mc2pkg::PackageChassis* package = nullptr;
	// Parsed locations.fit / package layout, loaded once per chassis object (a content reload recreates the chassis).
	mutable std::optional<mc2pkg::ChassisLayout> layoutCache;
	mutable bool layoutCacheLoaded = false;

	friend class		LogisticsVariant;
	friend class		LogisticsMech;

	static int			weightClasses[]; // mech weights

	friend class LogisticsData;

	struct ComponentInfo
	{
		long										xCoord;
		long										yCoord;
		LOGISTICS_COMPONENT_LOCATION location;
		LogisticsComponent*				component;
		int bodyLocation = -1, bodySlot = -1;
		unsigned int layoutHardpoint = 0;
	};
	


};

class LogisticsVariant
{
protected:

	public:

		LogisticsVariant();
		LogisticsVariant( const LogisticsChassis* pChassis, bool isDesignerMech );
		LogisticsVariant( const LogisticsVariant&  ); // going to need copy c'tor
		virtual ~LogisticsVariant();
		
		int init( CSVFile* fileName, LogisticsChassis*, int blockID );

		int		compareWeight( LogisticsVariant* );
		
		inline bool			isAvailable() const { return (availableToUser && !bHidden); }
		unsigned long	getID() const { return ID; }
		float			getMaxWeight() const { return chassis->maxWeight; }
		long			getChassisName() const { return chassis->chassisNameID; }
		std::string		getChassisDisplayName() const { return chassis->getDisplayName( this ); }
		const std::string&	getName() const { return variantName; }
		long			getEncyclopediaID() const { return chassis->getEncyclopediaID(); }  // UI-PROFILE-1: via override-aware chassis accessor
		long			getHelpID() const { return chassis->helpID; }
		long			getBaseCost() const { return chassis->baseCost; }
		long			getComponentCount() const { return componentCount; }
		int				canAddComponent( LogisticsComponent* pComponent, long& x, long& y ) const;
		int				getCost() const;
		int				getWeight() const;
		const std::string&	getMechClass() const;
		int				getChassisID() const { return chassis->ID; }
		int				getArmor() const;
		int				getArmorClass() { return chassis->getArmorClass(); }
		int				getJumpRange() const;
		int				getMaxJumpRange() const;
		int				getSpeed() const;
		std::optional<int> getDisplaySpeed() const;
		bool			hasXLEngine() const;
		long			effectiveSpeed() const;

		int				getVariantID() const { return ID & 0x00ffffff; }
		// UI-PROFILE-1: prefer a resolved UIProfile icon-file override, else legacy.
		const std::string&	getSmallIconFileName() const { return chassis->uiIconOverride[0].empty() ? chassis->iconFileNames[0] : chassis->uiIconOverride[0]; }
		const std::string&	getMediumIconFileName() const { return chassis->uiIconOverride[1].empty() ? chassis->iconFileNames[1] : chassis->uiIconOverride[1]; }
		const std::string&	getLargeIconFileName() const { return chassis->uiIconOverride[2].empty() ? chassis->iconFileNames[2] : chassis->uiIconOverride[2]; }
		int				getComponentsWithLocation( long& count, long* IDArray, long* xLocationArray, long* yLocationArray );
		std::expected<void, int> removeComponent( long xCoord, long yCoord );
		int				addComponent( LogisticsComponent*, long& xCoord, long& yCoord );
		inline			bool canDelete( ) const { return !isDesignerMech(); }
		inline			bool isDesignerMech() const { return bDesignerMech; }
		int				getComponents( long& count, long* array );
		int				getComponents( long& count, LogisticsComponent** array );

		int				getHeat() const;
		int				getMaxHeat() const;
		LogisticsVariant& operator=( const LogisticsVariant&  );

		bool			operator==( const LogisticsVariant& src ) const;
		const std::string&	getFileName() const { return chassis->fileName; }

		long			save( FitIniFile& file, long counter );
		mc2pkg::LocationVariant locationRecord() const;
		mc2::armor::Allocation armorAllocation(bool legacy = false) const;
		mc2::armor::Allocation armorCapacity() const;
		bool hasExplicitArmor() const { return chassis->armorEditable && layoutState.armorExplicit; }
		bool stepArmor(int location, int delta);
		bool restoreLocationRecord(mc2pkg::LocationVariant record);
		bool isLayoutGrandfathered() const { return locationRecord().grandfathered; }

		const LogisticsChassis* getChassis() const { return chassis; }
		bool			addComponent( int idFromFitFile, long& x, long& y );
		void			setName( std::string name );

		void		setAvailable( bool available );

		int			getIconIndex() const { return chassis->iconPictureIndex; }

		int			getComponentAreaWidth() const{ return chassis->componentAreaWidth; } 
		int			getComponentAreaHeight() const { return chassis->componentAreaHeight; } 

		std::optional<std::reference_wrapper<LogisticsComponent>> getCompAtLocation( int i, int j, long& realI, long& realJ );
		int			getComponentLocation( LogisticsComponent* pComp, long& i, long& j );

		int			getOptimalRangeString( uint32_t& color ) const;
		long		getHouseID(){ return chassis->houseID; }
		long		getMaxArmor() const { return chassis->maxArmor; }
		std::optional<int> getSensorID() const { return chassis->getSensorID(); }
		std::optional<int> getECM() const { return chassis->getECM(); }

		bool		allComponentsAvailable() const;

		// BT2018-FIRST-CLASS P7: package chassis rules (code/package_chassis.h). Weapon hardpoint
		// keys in getComponents() order (0 for non-weapons) -- what deployment hands BattleMech.
		int			getPackageMounts( long& count, unsigned int* keys ) const;
		// MECHLAB-LOC-1: first legal cell for pComponent inside package region `regionIndex`; `reason` is the short
		// verdict text either way. Cells + mc2pkg::checkAdd only.
		struct LocationPlace { bool legal = false; long x = -1; long y = -1; std::string reason; };
		LocationPlace	packageLocationPlace( LogisticsComponent* pComponent, int regionIndex ) const;
		// Loader path: install a saved component list. Stock chassis add each item at its saved cell (unchanged).
		// A package chassis solves the WHOLE loadout against the current regions and mounts (mc2::mechlab_loc::
		// migrateLoadout): legal loadouts load as saved; otherwise items keep or move cells and only what has no
		// legal place is dropped, named in `report` with its cost.
		struct SavedComponent { int id = 0; long x = -1; long y = -1; };
		struct DroppedComponent { std::string name; int id = 0; long cost = 0; std::string reason; };
		struct LoadoutReport { int kept = 0, moved = 0; std::vector<DroppedComponent> dropped; };
		void		setLoadout( const std::vector<SavedComponent>& items, LoadoutReport* report = nullptr );

		int			getFileID() const { return fileID; }



		
	protected:

	
		std::optional<std::reference_wrapper<const LogisticsChassis::ComponentInfo>> getComponentAtLocation(long x, long y) const;

		
		unsigned long		ID; // bottom 8 bits = mech chassis, next 8 = number within
		int					fileID; // in csv file
		
		LogisticsChassis*	chassis;
		std::string			variantName;	// in file
		
		long				componentCount;
		LogisticsChassis::ComponentInfo		components[54]; // I count a max of 54 components, might want to check though
		mc2pkg::LocationVariant layoutState;

		bool				availableToUser;
		bool				bDesignerMech;
		bool				bHidden;


		// HELPERS
		bool				hasJumpJets() const;
		bool				hasECM() const;
		bool				hasSensor() const;
		int					packageCheck( LogisticsComponent* pComponent, long x, long y ) const;
		bool				packageAutoPlace( LogisticsComponent* pComponent, long& x, long& y ) const;

		// SUPPRESSED
		
		friend class LogisticsMech;


};

class LogisticsVehicle : public LogisticsChassis
{

public:
	void init( FitIniFile& file );
	int  getComponents( long& count, LogisticsComponent** array );

protected:
	long				componentCount;
	ComponentInfo		components[54]; // I count a max of 54 components, might want to check though

};


//*************************************************************************************************
#endif  // end of file ( LogisticsVariant.h )
