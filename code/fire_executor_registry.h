#ifndef FIRE_EXECUTOR_REGISTRY_H
#define FIRE_EXECUTOR_REGISTRY_H
// FIRE-EXECUTOR-REGISTRY-1 (equipment arc, engine slice E7).
//
// The FIRING analogue of the unit arc's ActionExecutorRegistry
// (action_executor_registry.h). That registry replaced "is this a BattleMech?"
// with "is a compatible executor bound for this action?". This one replaces
// "if (masterId==PPC || class==LASER)" with "resolve the weapon's fireExecutor
// name to a REGISTERED executor". A WeaponFacet NAMES its fire executor
// (`beam_direct`, `projectile_ballistic`, ...) via WeaponFireExecutorId
// (weapon_definition.h); this registry binds each stable name to a small
// DESCRIPTOR and resolves the field to it.
//
// ★★ SCHEMA / DISPATCH ONLY. This registry establishes the LOOKUP and nothing
// else. It carries NO firing code, NO ammo/damage/FX behavior. The legacy
// MasterComponent / weaponFire path stays fully authoritative; nothing here
// fires a weapon, and (E7) nothing calls resolve() to fire. E-source-flip is
// where the def's fireExecutor becomes authoritative and a consumer appears.
//
// PURITY: header-only, GL/IO/engine-FREE. Depends only on weapon_definition.h
// (pure schema: WeaponFireExecutorId / WeaponFacet). Plain enums / fixed data /
// std::map keyed on the 32-bit hashed id -> compiles into the offline
// `mc2_tests` target and drags nothing, exactly like action_executor_registry.h.
//
// ID VOCABULARY MUST MATCH the offline converter's assigned fireExecutor names
// (tools/unit_converter/equipment_extract.py `_fire_executor` + equipment_model.py
// FX_* constants): beam_direct / projectile_ballistic / missile_salvo /
// missile_streak / artillery_indirect / lbx_spread. The stock set additionally
// binds flamer_area and tag_designator (named in the schema docstring) so the
// engine registers the full designed vocabulary even though the current
// converter does not yet emit those two.

#include "weapon_definition.h"   // WeaponFireExecutorId, WeaponFacet
#include <map>

// -------------------------------------------------------------------------
// FireExecutorKind -- coarse family of a fire executor. DESCRIPTIVE metadata
// only; it selects nothing and fires nothing. Mirrors the converter's mapping
// of weapon FORM/tag -> executor name.
// -------------------------------------------------------------------------
enum FireExecutorKind {
    FEK_BEAM      = 0,  // continuous energy beam (beam_direct)
    FEK_PROJECTILE,     // direct-fire ballistic round (projectile_ballistic, lbx_spread)
    FEK_MISSILE,        // guided/salvo missiles (missile_salvo, missile_streak)
    FEK_ARTILLERY,      // indirect-fire arc (artillery_indirect)
    FEK_FLAMER,         // area/close flame (flamer_area)
    FEK_TAG,            // designator, deals no damage (tag_designator)
    FEK__COUNT
};

// -------------------------------------------------------------------------
// FireExecutorEntry -- the registered descriptor for one executor id. Pure
// metadata: a kind + a few boolean traits a validator / future dispatcher can
// read WITHOUT any firing behavior living here.
// -------------------------------------------------------------------------
struct FireExecutorEntry {
    WeaponFireExecutorId id;         // the stable-name id this entry is bound to
    FireExecutorKind     kind;       // coarse family (descriptive)
    bool                 needsAmmo;  // true for ballistic/missile (self-powered energy = false)
    bool                 isBeam;     // continuous-beam presentation (energy)
    bool                 isIndirect; // arcing / indirect fire (artillery)

    FireExecutorEntry()
        : kind(FEK_PROJECTILE), needsAmmo(false), isBeam(false), isIndirect(false) {}
    FireExecutorEntry(const WeaponFireExecutorId& i, FireExecutorKind k,
                      bool ammo, bool beam, bool indirect)
        : id(i), kind(k), needsAmmo(ammo), isBeam(beam), isIndirect(indirect) {}
};

// -------------------------------------------------------------------------
// FireExecutorRegistry -- binds WeaponFireExecutorId -> FireExecutorEntry.
// Pure lookup; mirrors ActionExecutorRegistry's bind/isBound/*For shape. OWNS
// no gameplay state, resolves names to descriptors, fires nothing.
//   bind(entry)            -- register (or overwrite) an executor by its id.
//   isBound(id)            -- is anything registered for this fireExecutor id?
//   entryFor(id)           -- the bound entry, or nullptr.
//   resolve(WeaponFacet)   -- entryFor(facet.fireExecutor), or nullptr when the
//                             weapon's fireExecutor is unset or unbound.
// An UNSET id (id==0) is never bound and always resolves to nullptr, which a
// validator reports as EQV_WEAPON_NO_FIRE_EXECUTOR; a SET-but-unbound id is the
// reportable EQV_FIRE_EXECUTOR_UNBOUND condition (see helpers below).
// -------------------------------------------------------------------------
class FireExecutorRegistry {
    std::map<unsigned int, FireExecutorEntry> bindings_;  // keyed by id.id (hash)

  public:
    // Register the descriptor for `entry.id`. Overwrites on rebind (map::[]).
    // An entry with an unset id (id==0) is ignored -- 0 is the "unset" sentinel
    // and must never be a lookup hit.
    void bind(const FireExecutorEntry& entry) {
        if (!entry.id.valid()) return;
        bindings_[entry.id.id] = entry;
    }

    // Convenience: build + bind an entry from a name + traits.
    void bind(const char* name, FireExecutorKind kind,
              bool needsAmmo, bool isBeam, bool isIndirect) {
        bind(FireExecutorEntry(makeFireExecutorId(name), kind,
                               needsAmmo, isBeam, isIndirect));
    }

    bool isBound(const WeaponFireExecutorId& id) const {
        if (!id.valid()) return false;
        return bindings_.find(id.id) != bindings_.end();
    }

    const FireExecutorEntry* entryFor(const WeaponFireExecutorId& id) const {
        if (!id.valid()) return nullptr;
        auto it = bindings_.find(id.id);
        return it == bindings_.end() ? nullptr : &it->second;
    }

    // Resolve a weapon's declared fireExecutor to its registered entry (or
    // nullptr: unset OR unbound). Pure -- reads the facet's field only, fires
    // nothing. THIS is the E7 seam; no caller uses it to fire in this slice.
    const FireExecutorEntry* resolve(const WeaponFacet& w) const {
        return entryFor(w.fireExecutor);
    }

    size_t count() const { return bindings_.size(); }
};

// -------------------------------------------------------------------------
// Resolution outcome for a weapon's fireExecutor -- feeds the validator without
// coupling this header to it. Pure classification.
//   FE_RES_OK       -- a bound executor resolved.
//   FE_RES_UNSET    -- weapon carries no fireExecutor (id==0)  [EQV_WEAPON_NO_FIRE_EXECUTOR]
//   FE_RES_UNBOUND  -- weapon NAMES an executor that is not registered
//                                                              [EQV_FIRE_EXECUTOR_UNBOUND]
// -------------------------------------------------------------------------
enum FireExecutorResolution {
    FE_RES_OK = 0,
    FE_RES_UNSET,
    FE_RES_UNBOUND
};

inline FireExecutorResolution classifyFireExecutor(const FireExecutorRegistry& reg,
                                                   const WeaponFacet& w) {
    if (!w.fireExecutor.valid()) return FE_RES_UNSET;
    return reg.isBound(w.fireExecutor) ? FE_RES_OK : FE_RES_UNBOUND;
}

// -------------------------------------------------------------------------
// theFireExecutorRegistry() -- the ONE engine-side registry instance, populated
// once with the stock executor set on first access (function-local static, no
// GL/IO; init order irrelevant -- same shape as theActionExecutorRegistry()).
// DECLARATION only here so the pure offline `mc2_tests` target that includes
// this header drags nothing (it never links fire_executor_registry.cpp). The
// stock population is asserted by the offline doctest against its OWN local
// registry so the vocabulary stays testable without linking the engine glue.
// -------------------------------------------------------------------------
FireExecutorRegistry& theFireExecutorRegistry();

// Populate `reg` with the stock executor vocabulary. Pure + engine-free so BOTH
// the engine singleton AND the offline doctest bind an identical set. Traits:
//   beam_direct         BEAM       ammo=NO  beam=YES indirect=NO
//   projectile_ballistic PROJECTILE ammo=YES beam=NO  indirect=NO
//   lbx_spread          PROJECTILE ammo=YES beam=NO  indirect=NO
//   missile_salvo       MISSILE    ammo=YES beam=NO  indirect=NO
//   missile_streak      MISSILE    ammo=YES beam=NO  indirect=NO
//   artillery_indirect  ARTILLERY  ammo=YES beam=NO  indirect=YES
//   flamer_area         FLAMER     ammo=NO  beam=NO  indirect=NO
//   tag_designator      TAG        ammo=NO  beam=NO  indirect=NO
inline void populateStockFireExecutors(FireExecutorRegistry& reg) {
    reg.bind("beam_direct",          FEK_BEAM,       /*ammo*/false, /*beam*/true,  /*indirect*/false);
    reg.bind("projectile_ballistic", FEK_PROJECTILE, true,          false,         false);
    reg.bind("lbx_spread",           FEK_PROJECTILE, true,          false,         false);
    reg.bind("missile_salvo",        FEK_MISSILE,    true,          false,         false);
    reg.bind("missile_streak",       FEK_MISSILE,    true,          false,         false);
    reg.bind("artillery_indirect",   FEK_ARTILLERY,  true,          false,         true);
    reg.bind("flamer_area",          FEK_FLAMER,     false,         false,         false);
    reg.bind("tag_designator",       FEK_TAG,        false,         false,         false);
}

// Number of executors populateStockFireExecutors binds (doctest locks this).
enum { NUM_STOCK_FIRE_EXECUTORS = 8 };

#endif // FIRE_EXECUTOR_REGISTRY_H
