# MC2R Assimp Minimal Vendor Manifest

Source: user-provided assimp-master.zip

Kept importer support:
- ASE
- OBJ
- FBX
- glTF / glTF2

Kept required Assimp directories:
- include/
- code/ core runtime
- code/AssetLib/ASE
- code/AssetLib/FBX
- code/AssetLib/Obj
- code/AssetLib/glTF
- code/AssetLib/glTF2
- code/AssetLib/glTFCommon
- code/AssetLib/STEPParser (required by Assimp core CMake)
- cmake-modules/
- port/

Kept required contrib:
- zlib
- unzip
- utf8cpp
- rapidjson
- stb
- pugixml
- openddlparser
- poly2tri
- clipper
- earcut-hpp
- Open3DGC

Removed:
- test/
- tools/
- samples/
- fuzz/
- packaging/
- most doc/scripts payload
- unused AssetLib importer directories
- large unused contrib payloads such as googletest, draco, zip, android-cmake, tinyusdz/meshlab patch dirs (3MF export / USD / meshlab patches unused; zip was only referenced by the 3MF exporter, which is disabled)

Verified in sandbox:
- CMake configure succeeds on Linux with:
  - BUILD_SHARED_LIBS=OFF
  - ASSIMP_NO_EXPORT=ON
  - ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF
  - ASE/OBJ/FBX/GLTF importers enabled
  - tests/tools/samples/install disabled

Note:
- Full compile was not completed inside the sandbox due execution timeout, but the generated source tree configures cleanly and only enables requested importers.
