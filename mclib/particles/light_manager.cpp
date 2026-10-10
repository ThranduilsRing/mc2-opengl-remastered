//==========================================================================//
// File:    light_manager.cpp                                                //
// Contents: LightManager implementation - body moved verbatim from          //
//           mclib/gosfx/pointlight.cpp per plan §5.4 B1 Stage 2' C2.        //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "light_manager.h"
#include <mlr/mlrpointlight.hpp>

mc2::particles::LightManager*
    mc2::particles::LightManager::Instance = nullptr;

gosFX::Light*
    mc2::particles::LightManager::MakePointLight(const char* light_map)
{
    return reinterpret_cast<gosFX::Light*>(this);
}

void
    mc2::particles::LightManager::ChangeLight(
        gosFX::Light* light,
        Info* info
    )
{
}

void
    mc2::particles::LightManager::DeleteLight(gosFX::Light* light)
{
}
