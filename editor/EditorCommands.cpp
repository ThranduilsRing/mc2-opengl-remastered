// Editor command facade (see EditorCommands.h).
#include "stdafx.h"
#include "EditorCommands.h"
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
	int best = -1;
	float bestD2 = radius * radius;
	for ( size_t i = 0; i < wps.size(); ++i )
	{
		const float dx = wps[i].x - x, dy = wps[i].y - y, d2 = dx * dx + dy * dy;
		if ( d2 <= bestD2 )
		{
			bestD2 = d2;
			best = (int)i;
		}
	}
	return best;
}

int EditorCommands::insertIndexFor( const std::vector<glm::vec3>& wps, float x, float y, bool loop )
{
	if ( wps.size() < 2 )
		return (int)wps.size();
	const size_t segs = loop ? wps.size() : wps.size() - 1;
	int best = (int)wps.size();
	float bestD2 = 1e30f;
	for ( size_t i = 0; i < segs; ++i )
	{
		const glm::vec3& a = wps[i];
		const glm::vec3& b = wps[( i + 1 ) % wps.size()];
		const float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
		float t = len2 > 1e-6f ? ( ( x - a.x ) * ex + ( y - a.y ) * ey ) / len2 : 0.0f;
		t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
		const float dx = a.x + t * ex - x, dy = a.y + t * ey - y, d2 = dx * dx + dy * dy;
		if ( d2 < bestD2 )
		{
			bestD2 = d2;
			best = (int)i + 1;
		}
	}
	return best;
}
