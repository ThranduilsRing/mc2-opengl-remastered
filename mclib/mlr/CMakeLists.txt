cmake_minimum_required (VERSION 3.10)
project(mlr)

set(CODEC_SOURCES
    mlrindexedprimitivebase.cpp
    mlrprimitivebase.cpp
    mlrshape.cpp
    mlr_i_c_det_pmesh.cpp
    mlr_i_c_dt_pmesh.cpp
    mlr_i_c_pmesh.cpp
    mlr_i_det_pmesh.cpp
    mlr_i_dt_pmesh.cpp
    mlr_i_l_det_pmesh.cpp
    mlr_i_l_dt_pmesh.cpp
    mlr_i_l_pmesh.cpp
    mlr_i_mt_pmesh.cpp
    mlr_i_pmesh.cpp
    mlr_i_c_det_tmesh.cpp
    mlr_i_c_dt_tmesh.cpp
    mlr_i_c_tmesh.cpp
    mlr_i_det_tmesh.cpp
    mlr_i_dt_tmesh.cpp
    mlr_i_l_det_tmesh.cpp
    mlr_i_l_dt_tmesh.cpp
    mlr_i_l_tmesh.cpp
    mlr_i_tmesh.cpp
    mlr_i_tmesh_cast.cpp
    )

set(SOURCES
    mlrambientlight.cpp
    mlrinfinitelight.cpp
    mlrinfinitelightwithfalloff.cpp
    mlrlight.cpp
    mlrlightmap.cpp
    mlrlookuplight.cpp
    mlrpointlight.cpp
    mlrspotlight.cpp
    mlrsortbyorder.cpp
    mlrsorter.cpp
    mlrstate.cpp
    mlrtexture.cpp
    mlrtexturepool.cpp
    gosimage.cpp
    gosimagepool.cpp
    gosvertex.cpp
    gosvertex2uv.cpp
    mlrclipper.cpp
    mlr_gate.cpp
    mlrclippingstate.cpp
    mlreffect.cpp
    mlrindexedtrianglecloud.cpp
    mlrtrianglecloud.cpp
    mlr.cpp
    mlrheaders.cpp
    )

include_directories(${COMMON_INCLUDE_DIRS} "../../GameOS/include" "../../mclib")

# Retained shape factories, serialized geometry and material accessors.
# Keep the objects in the existing archive while runtime support is retired.
add_library(mlr_codecs OBJECT ${CODEC_SOURCES})
target_link_libraries(mlr_codecs PUBLIC mc2_gate)
add_library(mlr ${SOURCES} $<TARGET_OBJECTS:mlr_codecs>)

# cat MCLib.vcproj | grep ".cpp" | perl -pe 's/.+\"(\w+\.cpp)\".+/\1/g' >> CMakeLists.txt


# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET mlr)
    target_link_libraries(mlr PUBLIC mc2_gate)
endif()
