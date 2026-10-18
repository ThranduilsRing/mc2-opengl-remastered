//==========================================================================//
// File:    light_manager.h                                                  //
// Contents: mc2::particles::LightManager - point-light effect host          //
//           extracted from gosFX::LightManager (mclib/gosfx/pointlight.hpp) //
//           per integrated plan §5.4 B1 Stage 2' C2.                        //
//                                                                           //
//           API preserved verbatim per plan §5 Q5 so gosFX::PointLight and  //
//           the seven lifecycle sites (code/mechcmd2.cpp:1676/:2053,        //
//           mclib/txmmgr.cpp:395/:396/:501/:502/:541, Viewer/View.cpp:569)  //
//           continue to reference gosFX::LightManager via the using-alias   //
//           shim in mclib/gosfx/pointlight.hpp.                             //
//===========================================================================//

#pragma once

#include "gosfx/gosfx.hpp"
#include <stuff/stuff.hpp>

namespace gosFX {
    class Light;
}

namespace mc2 {
namespace particles {

class LightManager
    #if defined(_ARMOR)
        : public Stuff::Signature
    #endif
{
public:
    static LightManager* Instance;

    virtual gosFX::Light*
        MakePointLight(const char* light_map = NULL);

    struct Info {
        Stuff::RGBColor
            m_color;
        glm::mat4
            m_origin;
        Stuff::Scalar
            m_intensity,
            m_inner,
            m_outer;
        Stuff::Scalar
            m_spread;
    };

    virtual void
        ChangeLight(
            gosFX::Light* light,
            Info* info
        );

    virtual void
        DeleteLight(gosFX::Light* light);

    void
        TestInstance() const
            {}
};

}  // namespace particles
}  // namespace mc2
