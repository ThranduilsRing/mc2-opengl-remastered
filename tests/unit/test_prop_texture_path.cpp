#include "doctest.h"
#include "../../RenderCore/PropTexturePath.h"
TEST_CASE("prop_texture_path/stock_layout") {
    CHECK(mc2prop::tieredKtxPath("data/tgl/128/building.tga",512)=="data/tgl/512/building.ktx2");
    CHECK(mc2prop::tieredKtxPath("data\\tgl\\128\\building.tga",512)=="data/tgl/512/building.ktx2");
    CHECK(mc2prop::tieredKtxPath("data/tgl/building.tga",512)=="data/tgl/512/building.ktx2");
    CHECK(mc2prop::tieredKtxPath("mods/own/data/tgl/64/building.txm",256)=="mods/own/data/tgl/256/building.ktx2");
}
