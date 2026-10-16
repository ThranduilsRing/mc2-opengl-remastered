/***************************************************************
* FILENAME: EditorObjects.h
* DESCRIPTION: Defines Editor object wrappers for buildings, mechs, vehicles, and related map objects.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added Editor Remaster attribution header and null-safe appearance accessors.
****************************************************************/

#ifndef EDITOROBJECTS_H
#define EDITOROBJECTS_H

class FitIniFile;

#include "ObjectAppearance.h"

#include <vector>
#include <cstdint>
#include "AuthoredMissionState.h"
#include <string.h> // strncpy in Pilot::syncRawProfile

#ifndef HEAP_H
#include <heap.h>
#endif

#include <list>

#include "DAprType.h"
inline constexpr int MAX_PILOT = 64;

class EditorObject
{
public:
	
	EditorObject();
	virtual ~EditorObject();
	EditorObject( const EditorObject& );
	EditorObject& operator=( const EditorObject& );
	virtual void CastAndCopy(const EditorObject &master) { (*this) = master; }
	virtual EditorObject *Clone() { return(new EditorObject(*this)); }

	void *operator new (size_t mySize);
	void operator delete (void *us);

	void				select( bool bSelect );
	inline bool			isSelected() const { return appearance() && appearance()->selected ? true : false; }

	void				setAlignment( int align );
	inline int			getAlignment( )const{ return appearInfo->appearance->teamId; } 
	
	unsigned long		getColor() const;
	long				getID() const { return  id; }
	std::uint64_t instanceId() const { return objectInstanceId; }

	void				getCells( long& cellJ, long& cellI ) const;
	
	virtual bool save( FitIniFile* file, int warriorNumber ){ return false; }
	virtual bool load( FitIniFile* file, int warriorNumber ){ return false; }
	virtual int			getType() const { return BLDG_TYPE; }

	int					getSpecialType() const;
	int					getGroup() const;
	int					getIndexInGroup() const;

	void				setDamage( bool bDamage );
	bool				getDamage() const;

	const char*			getDisplayName() const;

	const Stuff::Vector3D&	getPosition() const { return appearance()->position; }
	
	void 				markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
	{
		appearInfo->appearance->markTerrain(pInfo,type, counter);
	}

	void setAppearance( int Group, int indexInGroup );

	// by Methuselas: selection can now reach legacy Editor objects through the
	// embedded GL child window.  Some catalog/preview paths can carry an object
	// before its appearance is fully populated, so callers must be able to test
	// for a missing appearance instead of crashing inside this accessor.
	ObjectAppearance* appearance() { return appearInfo ? appearInfo->appearance : nullptr; }
	const ObjectAppearance* appearance() const { return appearInfo ? appearInfo->appearance : nullptr; }

	long				getForestID() const { return forestId; }
	void				setForestID( int newID ) { forestId = newID; }

	void				setScale( long newScale ) { scale = newScale; }
	long				getScale( ) const { return scale; }

protected:

	struct AppearanceInfo
	{
		ObjectAppearance*   appearance;
		int					refCount; // so we only delete once

		void *operator new (size_t mySize)
		{
			void *result = nullptr;
			result = systemHeap->Malloc(mySize);
			
			return(result);
		
		}

		void operator delete (void *us)
		{
			systemHeap->Free(us);
		}

		AppearanceInfo& operator=( const AppearanceInfo& src );


	};

	long	cellColumn;
	long	cellRow;
	long	id;
	// Instance identity survives undo snapshots; catalog IDs identify types only.
	std::uint64_t objectInstanceId;
	AppearanceInfo* appearInfo;
	long	forestId;
	long	scale; // forest only

	friend class EditorObjectMgr; // the only thing that can move and change these things

};


// THIS CLASS CLEARLY NEEDS FLESHING OUT!
//*************************************************************************************************
class Pilot
{

public:

	Pilot(){info = nullptr; rawProfile[0] = 0;}

	static void initPilots();

	struct PilotInfo
	{
		char*	name;
		char*	fileName;
	};
	
	static	PilotInfo s_GoodPilots[MAX_PILOT];
	static	PilotInfo s_BadPilots[MAX_PILOT];
	static	long goodCount;
	static	long badCount;

	const char* 		getName() const { return info->name; }
	void	setName( const char* );

	// mirror the current catalog match into
	// rawProfile (used after a deliberate reassignment via setName).
	void	syncRawProfile()
	{
		const char* fn = ( info && info->fileName ) ? info->fileName : "";
		strncpy( rawProfile, fn, sizeof( rawProfile ) - 1 );
		rawProfile[sizeof( rawProfile ) - 1] = 0;
	}

	void	save( FitIniFile* file, int bGoodGuy );
	void	load( FitIniFile* file, int bGoodGuy );

	PilotInfo* info;
	/*note: The value of info should always be NULL or a pointer to static data. So the default
	assignment/copy operator (shallow copy) is valid. */

	// the raw Profile
	// string as read from the mission .fit. Pilot::save prefers this over the
	// pilots.csv catalog match so a broken catalog environment (initPilots
	// loads zero pilots -> info->fileName == NULL) can no longer silently
	// blank every [WarriorN] Profile on re-save. Kept in sync by setName.
	// Array member -> the shallow copy above stays valid.
	char rawProfile[64];
	
};


class Brain
{
	long numCells;
	long *cellNum;
	long *cellType;
	float *cellData;
	
	long numStaticVars;
	
	char brainName[256];
	
public:
	Brain (void)
	{
		numStaticVars = numCells = 0;
		cellNum = cellType = nullptr;
		cellData = nullptr;
		
		brainName[0] = 0;
	}

	~Brain (void)
	{
		free(cellNum);
		free(cellType);
		free(cellData);
		
		numStaticVars = numCells = 0;
		cellNum = cellType = nullptr;
		cellData = nullptr;
		
		brainName[0] = 0;
	}

	Brain( const Brain&  );
	Brain& operator=( const Brain& );
	
	bool save( FitIniFile* file, int warriorNumber, bool usePBrain = false );
	bool load( FitIniFile* file, int warriorNumber );

	// Read-only accessors for the editor AI/Brain inspector panel (UnitBrainPanel).
	const char* getBrainName() const { return brainName; }
	long        getNumCells() const { return numCells; }
	long        getNumStaticVars() const { return numStaticVars; }
};



class Unit : public EditorObject
{
public:

	Unit( int alignment );
	Unit( const Unit& src );
	Unit& operator=( const Unit& src );
	~Unit() override;
	void CastAndCopy(const EditorObject &master) override;
	EditorObject *Clone() override { return(new Unit(*this)); }

	void setLanceInfo( int newLance, int index ){ lance = newLance; lanceIndex = index; }
	void getLanceInfo( int &newLance, int &index ){ newLance = lance; index = lanceIndex; }
	int			getType() const override { return GV_TYPE; }	

	unsigned long getSquad() const { return squad; }
	void setSquad(unsigned long newSquad);

	bool save( FitIniFile* file, int warriorNumber ) override;
	bool load( FitIniFile* file, int warriorNumber ) override;
	const std::optional<PartDefinition>& partDefinition() const { return authoredPart; }
	void restorePartDefinition(std::optional<PartDefinition> definition) { authoredPart = std::move(definition); }
	void refreshPartFromCatalog();

	void getColors( unsigned long& base, unsigned long& color1, unsigned long& color2 ) const;
	void setColors( unsigned long base, unsigned long color1, unsigned long color2 );

	bool getSelfRepairBehaviorEnabled() const { return selfRepairBehaviorEnabled; }
	void setSelfRepairBehaviorEnabled(bool val) { selfRepairBehaviorEnabled = val; }

	inline Pilot*	getPilot() { return &pilot; }

	// Read-only brain access for the editor AI/Brain inspector panel (UnitBrainPanel).
	const Brain&	getBrain() const { return brain; }

	void setVariant( unsigned long newVar ){ variant = newVar; }
	inline int getVariant() const { return variant; }

	// --- Patrol / Move order authoring (editor-side) -----------------------
	// MOVE = traverse the waypoints once (one-way); PATROL = cycle them as a
	// closed loop (a move order is the open-path subset of a patrol). Stance
	// mirrors AttitudeType (code/tacordr.h) by index. All persisted additively
	// in the mission .fit; runtime delivery is wired later via the brain dispatch.
	enum UnitOrderType { ORDER_NONE = 0, ORDER_MOVE = 1, ORDER_PATROL = 2 };

	int  getOrderType() const { return orderType; }
	void setOrderType( int t ) { orderType = t; }
	int  getStance() const { return stance; }
	void setStance( int s ) { stance = s; }
	const std::vector<Stuff::Vector3D>& getWaypoints() const { return waypoints; }
	unsigned long getWaypointCount() const { return (unsigned long)waypoints.size(); }
	void addWaypoint( const Stuff::Vector3D& wp ) { waypoints.push_back( wp ); }
	void removeLastWaypoint() { if ( !waypoints.empty() ) waypoints.pop_back(); }
	void clearWaypoints() { waypoints.clear(); }

	// orderAuthored: the user edited orders in-editor, so the new .fit fields are
	// written on save. Pure display imports (existing-patrol .abl) leave it false,
	// so stock missions are not rewritten just by viewing them.
	bool isOrderAuthored() const { return orderAuthored; }
	void setOrderAuthored( bool v ) { orderAuthored = v; }

	// Lazily analyze this unit's brain .abl (one attempt): import an existing
	// patrol (startPatrolPath literals) for display when no editor-authored order
	// exists, and extract the fsm name + a behavior tag for the panel.
	void importPatrolFromBrainIfNeeded();

	// Brain summary (filled by importPatrolFromBrainIfNeeded). Behavior is a coarse
	// classification of the brain .abl; fsm is the script's fsm name.
	enum BrainBehavior { BRAIN_UNKNOWN = 0, BRAIN_PATROL, BRAIN_GUARD, BRAIN_ATTACK, BRAIN_IDLE };
	const char* getBrainFsm() const { return brainFsm; }
	int getBrainBehavior() const { return brainBehavior; }
	const char* getBrainName() const { return brain.getBrainName(); }

	std::list<Unit> alternatives;
	unsigned long tmpNumAlternativeInstances;
	unsigned long tmpAlternativeStartIndex;

protected:

	std::optional<PartDefinition> authoredPart;

	Brain brain;
	bool selfRepairBehaviorEnabled;
	int lance;		// which lance
	int lanceIndex; // number within lance 1 to 12
	unsigned long squad;
	Pilot	pilot;

	// Patrol/Move order authoring (see public accessors above).
	int orderType;   // UnitOrderType (default ORDER_NONE)
	int stance;      // AttitudeType index (default 2 = Normal)
	std::vector<Stuff::Vector3D> waypoints;
	bool orderAuthored;   // transient (NOT saved): user edited orders in-editor
	bool importChecked;   // transient (NOT saved): brain-.abl patrol import attempted
	char brainFsm[64];    // transient: fsm name parsed from the brain .abl
	int  brainBehavior;   // transient: BrainBehavior tag parsed from the brain .abl

	unsigned long baseColor;
	unsigned long highlightColor;
	unsigned long highlightColor2;

	unsigned long						variant;
};



class DropZone : public EditorObject
{
public:

	DropZone( const Stuff::Vector3D& position, int alignment, bool bVTol );
	DropZone& operator=( const DropZone& src ) { bVTol = src.bVTol; EditorObject::operator=( src ); return *this; }
	void CastAndCopy(const EditorObject &master) override;
	EditorObject *Clone() override { return(new DropZone(*this)); }
	bool save( FitIniFile* file, int number ) override;
	
	bool isVTol() { return bVTol; }
	
private:

	bool bVTol;
};

class NavMarker : public EditorObject
{
	public:

		NavMarker();
		EditorObject *Clone() override { return(new NavMarker(*this)); }
		bool save( FitIniFile* file, int number ) override;
		bool readDefinition(FitIniFile& file);
		void CastAndCopy(const EditorObject& master) override;
	private:
		AuthoredTargetPosition authoredPosition;
		float radius = 256.0f;

};




#endif  // end of file ( EditorObjects.h )
