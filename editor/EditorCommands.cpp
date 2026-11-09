// Editor command facade (see EditorCommands.h).
#include "stdafx.h"
#include "EditorCommands.h"
#include "EditorRouteGeometry.h"
#include "EditorObjects.h"
#include "EditorInterface.h"
#include "Action.h"
#include <cmath>
#include <memory>

namespace
{
// Several per-unit order actions as one undo step (N-unit route assign).
class UnitOrderGroupAction : public Action
{
public:
	UnitOrderGroupAction() : Action( "Assign Unit Routes" ) {}
	~UnitOrderGroupAction() override { for ( Action* a : m_actions ) delete a; }
	bool redo() override { bool ok = true; for ( Action* a : m_actions ) ok = a->redo() && ok; return ok; }
	bool undo() override
	{
		bool ok = true;
		for ( auto it = m_actions.rbegin(); it != m_actions.rend(); ++it )
			ok = ( *it )->undo() && ok;
		return ok;
	}
	std::vector<Action*> m_actions;
};

bool sameRoute( const Unit* unit, int orderType, const std::vector<glm::vec3>& wps )
{
	return unit->isOrderAuthored() && unit->getOrderType() == orderType && unit->getWaypoints() == wps;
}

// Apply the route to the unit with before/after snapshots; caller owns the action.
ModifyUnitOrderAction* applyRoute( Unit* unit, int orderType, const std::vector<glm::vec3>& wps )
{
	auto* act = new ModifyUnitOrderAction();
	act->capture( unit );
	unit->setOrderType( orderType );
	unit->clearWaypoints();
	for ( const glm::vec3& wp : wps )
		unit->addWaypoint( wp );
	unit->setOrderAuthored( true );
	act->commit( unit );
	return act;
}

void addAction( Action* act )
{
	if ( ActionUndoMgr::instance )
		ActionUndoMgr::instance->AddAction( act );
	else
		delete act;
}
} // namespace

bool EditorCommands::setRoute( Unit* unit, int orderType, const std::vector<glm::vec3>& waypoints )
{
	if ( !unit || orderType < Unit::ORDER_NONE || orderType > Unit::ORDER_PATROL )
		return false;
	if ( sameRoute( unit, orderType, waypoints ) )
		return false;
	addAction( applyRoute( unit, orderType, waypoints ) );
	return true;
}

int EditorCommands::assignRoute( const Unit* source, const std::vector<Unit*>& targets )
{
	if ( !source )
		return 0;
	const int type = source->getOrderType();
	const std::vector<glm::vec3> wps = source->getWaypoints();
	auto group = std::make_unique<UnitOrderGroupAction>();
	for ( Unit* t : targets )
		if ( t && t != source && !sameRoute( t, type, wps ) )
			group->m_actions.push_back( applyRoute( t, type, wps ) );
	const int changed = (int)group->m_actions.size();
	if ( changed )
		addAction( group.release() );
	return changed;
}

bool EditorCommands::setTeam( EditorObject* obj, int team )
{
	EditorInterface* ei = EditorInterface::instance();
	return ei && ei->applyObjectAlignment( obj, team );
}

int EditorCommands::nearestWaypoint( const std::vector<glm::vec3>& wps, float x, float y, float radius )
{
	return EditorRouteGeometry::nearestWaypoint( wps, x, y, radius );
}

int EditorCommands::insertIndexFor( const std::vector<glm::vec3>& wps, float x, float y, bool loop )
{
	return EditorRouteGeometry::insertIndexFor( wps, x, y, loop );
}
