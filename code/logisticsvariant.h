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

#include"estring.h"

#pragma warning (disable:4514)


enum LOGISTICS_COMPONENT_LOCATION
{
	HEAD = 0,
	CHEST = 1,
	LEGS = 2
};


class LogisticsChassis
{
public:

	LogisticsChassis( );
	int				init( CSVFile* fileName, int chassisID );
	void			setFitID( int id ){ fitID = id; }
	void			setNameID( long id ){ chassisNameID = id; }
	// UI-PROFILE-1: prefer a resolved UIProfile string-id override, else legacy.
	int				getNameID() const { return (int)( uiNameIDOverride != -1 ? uiNameIDOverride : chassisNameID ); }
	int				getEncyclopediaID() const { return (int)( uiEncyclopediaIDOverride != -1 ? uiEncyclopediaIDOverride : encyclopediaID ); }
	const EString& getFileName() const { return fileName; }
	int				getHouseID() const{ return houseID; }
	float			getMaxWeight() const{ return maxWeight; }
	const EString&	getMechClass() const  { return mechClass; }
	long			getArmor() const { return baseArmor; }
	int				getArmorClass() const;
	float			getSpeed() const { return speed; }
	int				getDisplaySpeed() const;
	int				getDisplaySpeedForSpeed( long s ) const;
	void			setScale( float newScale ) { scale = newScale; }
	float			getScale() const { return scale; }
	long			getMaxArmor() const { return maxArmor; }
	int				getSensorID() const;
	int				getECM() const;
	bool			jumpJetsAllowed() const;   // honors MC2_JUMPJETS_ALL gate
	// VEHICLE-AUTO-PILOT-1: true for a LogisticsVehicle chassis (set in
	// LogisticsVehicle::init). Logistics-safe vehicle discriminator — does NOT need the
	// mission-only objTypeManager that loadObjectType derefs (which is null at the
	// logistics/pilot-ready screen).
	bool			isVehicle() const { return bIsVehicle; }

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
	EString				fileName;
	EString				mechClass;
	EString				iconFileNames[3];

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
	// filename). Empty EString / -1 == "no override" == the accessor keeps its
	// legacy iconFileNames[]/IDS_ value. Stock chassis have no registry entry, so
	// all three EStrings stay empty and both ids stay -1 -> byte-identical stock UI.
	// Applied at getSmall/Medium/LargeIconFileName + getNameID/getEncyclopediaID.
	EString				uiIconOverride[3];   // [0]=small(icon) [1]=medium(silhouette) [2]=large(portrait)
	long				uiNameIDOverride;         // -1 == unset
	long				uiEncyclopediaIDOverride; // -1 == unset

	MechAppearance*		appearance; 

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
		const EString&		getName() const { return variantName; }
		long			getEncyclopediaID() const { return chassis->getEncyclopediaID(); }  // UI-PROFILE-1: via override-aware chassis accessor
		long			getHelpID() const { return chassis->helpID; }
		long			getBaseCost() const { return chassis->baseCost; }
		long			getComponentCount() const { return componentCount; }
		int				canAddComponent( LogisticsComponent* pComponent, long& x, long& y ) const;
		int				getCost() const;
		int				getWeight() const;
		const EString&		getMechClass() const;
		int				getChassisID() const { return chassis->ID; }
		int				getArmor() const;
		int				getArmorClass() { return chassis->getArmorClass(); }
		int				getJumpRange() const;
		int				getMaxJumpRange() const;
		int				getSpeed() const;
		int				getDisplaySpeed() const;
		bool			hasXLEngine() const;
		long			effectiveSpeed() const;

		int				getVariantID() const { return ID & 0x00ffffff; }
		// UI-PROFILE-1: prefer a resolved UIProfile icon-file override, else legacy.
		const EString&		getSmallIconFileName() const { return chassis->uiIconOverride[0].Length() ? chassis->uiIconOverride[0] : chassis->iconFileNames[0]; }
		const EString&	getMediumIconFileName() const { return chassis->uiIconOverride[1].Length() ? chassis->uiIconOverride[1] : chassis->iconFileNames[1]; }
		const EString&		getLargeIconFileName() const { return chassis->uiIconOverride[2].Length() ? chassis->uiIconOverride[2] : chassis->iconFileNames[2]; }
		int				getComponentsWithLocation( long& count, long* IDArray, long* xLocationArray, long* yLocationArray );
		int				removeComponent( long xCoord, long yCoord );
		int				addComponent( LogisticsComponent*, long& xCoord, long& yCoord );
		inline			bool canDelete( ) const { return !isDesignerMech(); }
		inline			bool isDesignerMech() const { return bDesignerMech; }
		int				getComponents( long& count, long* array );
		int				getComponents( long& count, LogisticsComponent** array );

		int				getHeat() const;
		int				getMaxHeat() const;
		LogisticsVariant& operator=( const LogisticsVariant&  );

		bool			operator==( const LogisticsVariant& src );
		const EString&		getFileName(){ return chassis->fileName; }

		long			save( FitIniFile& file, long counter );

		const LogisticsChassis* getChassis() const { return chassis; }
		bool			addComponent( int idFromFitFile, long& x, long& y );
		void			setName( const char* name ); // will allocate for you

		void		setAvailable( bool available );

		int			getIconIndex() const { return chassis->iconPictureIndex; }

		int			getComponentAreaWidth() const{ return chassis->componentAreaWidth; } 
		int			getComponentAreaHeight() const { return chassis->componentAreaHeight; } 

		LogisticsComponent*	getCompAtLocation( int i, int j, long& realI, long& realJ );
		int			getComponentLocation( LogisticsComponent* pComp, long& i, long& j );

		int			getOptimalRangeString( uint32_t& color ) const;
		long		getHouseID(){ return chassis->houseID; }
		long		getMaxArmor() const { return chassis->maxArmor; }
		int			getSensorID() const { return chassis->getSensorID(); }
		int			getECM() const { return chassis->getECM(); }

		bool		allComponentsAvailable() const;

		int			getFileID() const { return fileID; }



		
	protected:

	
		const LogisticsChassis::ComponentInfo* getComponentAtLocation(long x, long y) const;

		
		unsigned long		ID; // bottom 8 bits = mech chassis, next 8 = number within
		int					fileID; // in csv file
		
		LogisticsChassis*	chassis;
		EString				variantName;	// in file
		
		long				componentCount;
		LogisticsChassis::ComponentInfo		components[54]; // I count a max of 54 components, might want to check though

		bool				availableToUser;
		bool				bDesignerMech;
		bool				bHidden;


		// HELPERS
		bool				hasJumpJets() const;
		bool				hasECM() const;
		bool				hasSensor() const;

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
