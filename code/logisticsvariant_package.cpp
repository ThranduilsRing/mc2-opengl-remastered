// BT2018-FIRST-CLASS P7: LogisticsVariant rules for package chassis. Every rule itself is the
// pure mc2pkg::checkAdd / assignMounts in code/package_chassis.h; this file only adapts
// LogisticsComponent placements to it. Stock chassis (getPackage() == nullptr) never reach here.
#include "logisticsvariant_internal.h"
#include "package_chassis.h"
#include "mechlab_loc_layout.h"

namespace {

mc2pkg::Mount categoryOf(const LogisticsComponent* c) {
    switch (c->getType()) {
        case COMPONENT_FORM_WEAPON_ENERGY:    return mc2pkg::Mount::Energy;
        case COMPONENT_FORM_WEAPON_BALLISTIC: return mc2pkg::Mount::Ballistic;
        case COMPONENT_FORM_WEAPON_MISSILE:   return mc2pkg::Mount::Missile;
        case COMPONENT_FORM_WEAPON:           return mc2pkg::Mount::Energy;   // untyped legacy weapon: energy, as the equipment schema defaults
        default:                              return mc2pkg::Mount::None;
    }
}

mc2pkg::Placed placedOf(const LogisticsComponent* c, long x, long y) {
    mc2pkg::Placed p;
    p.x = static_cast<int>(x);
    p.y = static_cast<int>(y);
    p.w = c->getComponentWidth();
    p.h = c->getComponentHeight();
    p.category = categoryOf(c);
    p.ordinal = c->getID();
    p.weight = c->getWeight();
    return p;
}

} // namespace

int LogisticsVariant::packageCheck( LogisticsComponent* pComponent, long x, long y ) const
{
    const mc2pkg::PackageChassis* pc = chassis->getPackage();
    if ( !pc )
        return 0;
    std::vector<mc2pkg::Placed> existing;
    existing.reserve( componentCount );
    for ( int i = 0; i < componentCount; ++i )
        existing.push_back( placedOf( components[i].component, components[i].xCoord, components[i].yCoord ) );
    // Jump jets and head electronics live in their own legacy slots (-2/-3): mass only.
    const bool slotItem = ( x < 0 && y < 0 && ( x == -2 || x == -3 ) );
    mc2pkg::Placed cand = placedOf( pComponent, slotItem ? -1 : x, slotItem ? -1 : y );
    if ( slotItem )
        cand.category = mc2pkg::Mount::None;
    // Verdict order: Legal, OutsideRegion, NoFreeMount, OverPayload (package_chassis.h).
    static constexpr int kErrorFor[] = { 0, INVALID_LOCATION, NO_COMPATIBLE_MOUNT, COMPONENT_TOO_HEAVY };
    return kErrorFor[ static_cast<int>( mc2pkg::checkAdd( *pc, existing, cand ) ) ];
}

// Auto-place (x == y == -1): the first free grid spot that lies wholly in one region AND
// passes the package rules, scanning regions in their declared order.
bool LogisticsVariant::packageAutoPlace( LogisticsComponent* pComponent, long& x, long& y ) const
{
    const mc2pkg::PackageChassis* pc = chassis->getPackage();
    if ( !pc )
        return false;
    const int w = pComponent->getComponentWidth();
    const int h = pComponent->getComponentHeight();
    for ( const mc2pkg::Region& r : pc->regions )
    {
        for ( int j = r.y; j + h <= r.y + r.h; ++j )
        {
            for ( int i = r.x; i + w <= r.x + r.w; ++i )
            {
                bool free = true;
                for ( int l = 0; l < h && free; ++l )
                    for ( int k = 0; k < w && free; ++k )
                        if ( getComponentAtLocation( i + k, j + l ) )
                            free = false;
                if ( free && packageCheck( pComponent, i, j ) == 0 )
                {
                    x = i;
                    y = j;
                    return true;
                }
            }
        }
    }
    return false;
}

int LogisticsVariant::getPackageMounts( long& count, unsigned int* keys ) const
{
    const mc2pkg::PackageChassis* pc = chassis->getPackage();
    if ( !pc || count < componentCount )
    {
        count = componentCount;
        return pc ? NEED_BIGGER_ARRAY : -1;
    }
    std::vector<mc2pkg::Placed> items;
    items.reserve( componentCount );
    for ( int i = 0; i < componentCount; ++i )
        items.push_back( placedOf( components[i].component, components[i].xCoord, components[i].yCoord ) );
    const std::vector<int> mounts = mc2pkg::assignMounts( *pc, items );
    for ( int i = 0; i < componentCount; ++i )
        keys[i] = mounts[i] >= 0 ? pc->hardpoints[mounts[i]].key : 0u;
    count = componentCount;
    return 0;
}

// MECHLAB-LOC-1: location-view placement. Same cells and the same mc2pkg::checkAdd as the grid.
LogisticsVariant::LocationPlace LogisticsVariant::packageLocationPlace( LogisticsComponent* pComponent, int regionIndex ) const
{
    const mc2pkg::PackageChassis* pc = chassis->getPackage();
    LocationPlace out;
    if ( !pc )
        return out;
    std::vector<mc2pkg::Placed> existing;
    existing.reserve( componentCount );
    for ( int i = 0; i < componentCount; ++i )
        existing.push_back( placedOf( components[i].component, components[i].xCoord, components[i].yCoord ) );
    mc2::mechlab_loc::Candidate cand;
    cand.w = pComponent->getComponentWidth();
    cand.h = pComponent->getComponentHeight();
    cand.category = categoryOf( pComponent );
    cand.ordinal = pComponent->getID();
    cand.weight = pComponent->getWeight();
    const mc2::mechlab_loc::Explained e = mc2::mechlab_loc::explain( *pc, existing, cand, regionIndex );
    const mc2pkg::Region* r = ( regionIndex >= 0 && regionIndex < static_cast<int>( pc->regions.size() ) ) ? &pc->regions[regionIndex] : nullptr;
    out.reason = mc2::mechlab_loc::reasonText( e, cand.category, r, cand );
    if ( e.reason != mc2::mechlab_loc::Reason::Legal )
        return out;
    out.legal = true;
    out.x = e.x;
    out.y = e.y;
    return out;
}

// Saved component list -> variant. Stock chassis: each item at its saved cell, exactly as before. Package chassis:
// the whole loadout is solved against the current regions/mounts (mc2::mechlab_loc::migrateLoadout); the kept items
// are then added in INPUT order with the solved cells (mount assignment and deployment are order-dependent).
void LogisticsVariant::setLoadout( const std::vector<SavedComponent>& items, LoadoutReport* report )
{
    const mc2pkg::PackageChassis* pc = chassis->getPackage();
    if ( !pc )
    {
        for ( const SavedComponent& it : items )
        {
            long x = it.x;
            long y = it.y;
            addComponent( it.id, x, y );
        }
        return;
    }
    std::vector<LogisticsComponent*> comps;
    std::vector<mc2::mechlab_loc::LoadoutItem> in;
    for ( const SavedComponent& it : items )
    {
        LogisticsComponent* pComp = LogisticsData::instance->getComponent( it.id ).transform( []( auto ref ) { return &ref.get(); } ).value_or( nullptr );
        if ( !pComp )
            continue;   // an unknown component id never loaded in the stock path either
        mc2::mechlab_loc::LoadoutItem li;
        li.slot = pComp->getType() == COMPONENT_FORM_JUMPJET || pComp->getType() == COMPONENT_FORM_ECM || pComp->getType() == COMPONENT_FORM_SENSOR;
        li.p = placedOf( pComp, li.slot ? -1 : it.x, li.slot ? -1 : it.y );
        if ( li.slot )
            li.p.category = mc2pkg::Mount::None;
        comps.push_back( pComp );
        in.push_back( li );
    }
    const mc2::mechlab_loc::LoadoutResult r = mc2::mechlab_loc::migrateLoadout( *pc, in );
    std::vector<mc2pkg::Placed> placed;
    for ( size_t i = 0; i < in.size(); ++i )
    {
        if ( !r.kept[i] )
            continue;
        mc2pkg::Placed p = in[i].p;
        p.x = r.cells[i].x;
        p.y = r.cells[i].y;
        placed.push_back( p );
        long x = r.cells[i].x;
        long y = r.cells[i].y;
        addComponent( comps[i]->getID(), x, y );
    }
    if ( !report )
        return;
    report->moved = r.moved;
    report->kept = static_cast<int>( placed.size() );
    for ( size_t i = 0; i < in.size(); ++i )
    {
        if ( r.kept[i] )
            continue;
        mc2pkg::Placed probe = in[i].p;
        probe.x = probe.y = -1;
        const mc2pkg::Verdict v = mc2pkg::checkAdd( *pc, placed, probe );
        report->dropped.push_back( { comps[i]->getName(), comps[i]->getID(), comps[i]->getCost(),
                                     v == mc2pkg::Verdict::Legal ? "no-free-cells" : mc2pkg::verdictName( v ) } );
    }
}
