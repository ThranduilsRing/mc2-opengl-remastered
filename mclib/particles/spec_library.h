//==========================================================================//
// File:    spec_library.h                                                   //
// Contents: mc2::particles::SpecLibrary - the particle-effect specification //
//           catalog (parser + lookup) extracted from gosFX::EffectLibrary.  //
//                                                                           //
//           Per integrated plan §5.4 B1 Stage 2' C1. Owns the m_effects     //
//           array (formerly owned by gosFX::EffectLibrary). gosFX::         //
//           EffectLibrary becomes a forwarding adapter during the migration //
//           window so existing producer call sites compile unchanged.       //
//                                                                           //
//           Preserved typedef: Find() returns gosFX::Effect::Specification* //
//           (per plan §5 Q5) so MakeEffect / m_effectID semantics survive.  //
//===========================================================================//

#pragma once

#include "mc2_czstring.h"
#include "gosfx/effect.hpp"
#include <stuff/stuff.hpp>

namespace mc2 {
namespace particles {

class SpecLibrary
{
public:
    static SpecLibrary* Instance();
    static void Shutdown();

    void Load(Stuff::MemoryStream* stream);
    void Save(Stuff::MemoryStream* stream);

    gosFX::Effect::Specification* Find(mc2::czstring name);

    // Index-based access used by gosFX::EffectLibrary::MakeEffect forwarder.
    gosFX::Effect::Specification* At(unsigned index);
    unsigned Count() const;

private:
    SpecLibrary();
    ~SpecLibrary();

    SpecLibrary(const SpecLibrary&) = delete;
    SpecLibrary& operator=(const SpecLibrary&) = delete;

    Stuff::DynamicArrayOf<gosFX::Effect::Specification*> m_effects;
};

}  // namespace particles
}  // namespace mc2
