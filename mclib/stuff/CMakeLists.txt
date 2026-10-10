cmake_minimum_required (VERSION 3.10)
project(stuff)

set(SOURCES ${SOURCES}
    style.cpp
    trace.cpp
    affinematrix.cpp
    angle.cpp
    color.cpp
    extentbox.cpp
    line.cpp
    linearmatrix.cpp
    matrix.cpp
    motion.cpp
    normal.cpp
    obb.cpp
    origin.cpp
    plane.cpp
    point3d.cpp
    polar.cpp
    random.cpp
    ray.cpp
    rotation.cpp
    scalar.cpp
    sphere.cpp
    unitvector.cpp
    vector3d.cpp
    vector4d.cpp
    matrixstack.cpp
    memoryblock.cpp
    mstring.cpp
    chain.cpp
    hash.cpp
    iterator.cpp
    link.cpp
    node.cpp
    plug.cpp
    registeredclass.cpp
    safechain.cpp
    safesocket.cpp
    slot.cpp
    socket.cpp
    sortedchain.cpp
    sortedsocket.cpp
    table.cpp
    tree.cpp
    database.cpp
    filestream.cpp
    filestreammanager.cpp
    memorystream.cpp
    namelist.cpp
    notationfile.cpp
    note.cpp
    page.cpp
    stuff.cpp
    stuffheaders.cpp
    )

include_directories(${COMMON_INCLUDE_DIRS})

add_library(stuff ${SOURCES})

# cat MCLib.vcproj | grep ".cpp" | perl -pe 's/.+\"(\w+\.cpp)\".+/\1/g' >> CMakeLists.txt


# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET stuff)
    target_link_libraries(stuff PUBLIC mc2_gate)
endif()
