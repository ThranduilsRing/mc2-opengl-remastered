#include "doctest.h"
#include <limits>
#include "GameAdapters/vegetation_population_cache.h"
using namespace mc2::vegetation;
TEST_CASE("veg population identity refactor") {
    const std::vector<GosVegetation::Instance> input{
        {256,0,3,0.2f,5,1,0.3f}, {1,1,7,0.7f,6,2,0.4f},
        {2,2,-1,0.9f,8,3,0.5f}, {-1,1,2,1.2f,9,4,0.6f}};
    const auto p = buildPopulation(input, 1);
    REQUIRE(p.instances.size() == 4);
    REQUIRE(p.regions.size() == 3);
    CHECK(p.instances[1].x == 1);
    CHECK(p.instances[2].x == 2);
    CHECK(p.regions[0].first == 0);
    CHECK(p.regions[0].last == 1);
    CHECK(p.regions[1].first == 1);
    CHECK(p.regions[1].last == 3);
    CHECK(p.regions[2].last == 4);
    auto expected = packInstance(1,1,7,0.7f,6,2,0.4f);
    CHECK(std::memcmp(&p.instances[1], &expected, sizeof(expected)) == 0);
    const auto q = buildPopulation(input, 2);
    CHECK(q.instances.size() == 2);
    CHECK(q.instances[0].x == -1);
    CHECK(q.instances[1].x == 2);
}


TEST_CASE("veg population codec identity and stale blobs") {
    PopulationRecord r;
    r.population=buildPopulation(std::vector<GosVegetation::Instance>{{256,0,3,0.2f,5,1,0.3f},{1,1,7,0.7f,6,2,0.4f},{2,2,-1,0.9f,8,3,0.5f},{-1,1,2,1.2f,9,4,0.6f}},1);
    r.interval={10,100}; Digest d{};
    auto b=encode(r,d); auto hit=decode(b,d,10); REQUIRE(hit);
    CHECK(populationHash(hit->population.instances)==populationHash(r.population.instances));
    CHECK(populationHash(hit->population.regions)==populationHash(r.population.regions));
    CHECK(std::memcmp(hit->population.instances.data(),r.population.instances.data(),r.population.instances.size()*16)==0);
    CHECK(std::memcmp(hit->population.regions.data(),r.population.regions.data(),r.population.regions.size()*32)==0);
    auto wrong=d; wrong[31]=1; CHECK_FALSE(decode(b,wrong,10));
    CHECK_FALSE(decode(b,d,9)); CHECK(decode(b,d,99)); CHECK_FALSE(decode(b,d,100));
    for(size_t offset : {size_t(8),size_t(12),size_t(48),size_t(56),size_t(72),kBlobHeader}) {
        auto bad=b; bad[offset]^=255; CHECK_FALSE(decode(bad,d,10));
    }
    for(size_t size : {size_t(0),kBlobHeader-1,b.size()-1}) CHECK_FALSE(decode(std::span(b).first(size),d,10));
    // Re-encode invalid region tables so payload-hash success cannot hide missing structural validation.
    for(int mode=0;mode<4;++mode) {
        auto bad=r;
        if(mode==0) bad.population.regions[1].first=0;
        if(mode==1) bad.population.regions[0].last=99;
        if(mode==2) bad.population.regions[0].last=0;
        if(mode==3) bad.population.regions[0].x=std::numeric_limits<float>::infinity();
        CHECK_FALSE(decode(encode(bad,d),d,10));
    }
}
TEST_CASE("veg population capacity interval and admission") {
    auto r=capacityInterval(10,5,2,3,30); REQUIRE(r);
    CHECK(r->lo==30); CHECK(r->hi==55); CHECK_FALSE(r->contains(29)); CHECK(r->contains(30)); CHECK(r->contains(54)); CHECK_FALSE(r->contains(55));
    r=capacityInterval(10,5,3,3,55); REQUIRE(r); CHECK(r->lo==55); CHECK(r->hi==UINT64_MAX);
    r=capacityInterval(10,5,0,3,11); REQUIRE(r); CHECK(r->lo==11); CHECK(r->hi==15);
    r=capacityInterval(10,0,0,0,11); REQUIRE(r); CHECK(r->lo==11); CHECK(r->hi==UINT64_MAX);
    CHECK_FALSE(capacityInterval(10,5,0,3,10)); CHECK_FALSE(capacityInterval(10,UINT64_MAX,2,3,30));
    CHECK(cacheAdmission(true,false,true)); CHECK_FALSE(cacheAdmission(false,false,true));
    CHECK_FALSE(cacheAdmission(true,true,true)); CHECK_FALSE(cacheAdmission(true,false,false)); CHECK_FALSE(cacheAdmission(true,false,true,2));
}
TEST_CASE("veg population source identity covers every input") {
    std::vector<uint32_t> types{1,2}; std::vector<float> heights{1,2},visible{3,4}; std::vector<uint8_t> masks{0,63};
    SourceInput in; in.mapName="mc2_01"; in.types=types; in.elevations=heights; in.visible=visible; in.masks=masks; in.field=FieldRecipe{};
    Digest exe{}; auto hash=[&](const SourceInput& x) { return sourceDigest(serializeSource(x),exe); };
    const auto base=hash(in); CHECK(hash(in)==base);
    auto other=in; other.mapName="mc2_10"; CHECK(hash(other)!=base);
    other=in; ++other.seed; CHECK(hash(other)!=base);
    for(size_t i=0;i<in.frame.size();++i) { other=in; other.frame[i]=1; CHECK(hash(other)!=base); }
    for(size_t i=0;i<in.grid.size();++i) { other=in; other.grid[i]=1; CHECK(hash(other)!=base); }
    types[0]++; CHECK(hash(in)!=base); types[0]--;
    heights[0]++; CHECK(hash(in)!=base); heights[0]--;
    visible[0]++; CHECK(hash(in)!=base); visible[0]--;
    for(int bit=0;bit<6;++bit) { masks[0]=uint8_t(1<<bit); CHECK(hash(in)!=base); } masks[0]=0;
    for(size_t i=0;i<4;++i) for(int field=0;field<4;++field) {
        other=in; auto& r=other.recipes[i];
        if(field==0) r.radius++; if(field==1) r.treeChance+=0.1f; if(field==2) r.moistureBias+=0.1f; if(field==3) r.grass++;
        CHECK(hash(other)!=base);
    }
    other=in; other.field.reset(); CHECK(hash(other)!=base);
    other=in; other.field->spacing++; CHECK(hash(other)!=base); other=in; other.field->height++; CHECK(hash(other)!=base);
    std::vector<int32_t> blocks{-1,0}; std::vector<uint8_t> texels{0,1};
    const auto hs=hardSurfaceDigest(2,1,128,blocks,texels);
    blocks[0]=-2; CHECK(hardSurfaceDigest(2,1,128,blocks,texels)!=hs); blocks[0]=-1;
    texels[0]=1; CHECK(hardSurfaceDigest(2,1,128,blocks,texels)!=hs); texels[0]=0;
    other=in; other.hardSurface=hs; CHECK(hash(other)!=base);
    auto typesCopy=types; auto heightsCopy=heights; auto visibleCopy=visible; auto masksCopy=masks;
    other=in; other.types=typesCopy; other.elevations=heightsCopy; other.visible=visibleCopy; other.masks=masksCopy; CHECK(hash(other)==base);
    CHECK(sourceDigest(serializeSource(in),exe,kProducer+1)!=base); CHECK(sourceDigest(serializeSource(in),exe,kProducer,kFormat+1)!=base);
    exe[0]=1; CHECK(hash(in)!=base);
    // No generation counter enters the serializers; a changed history with identical content hashes identically.
    CHECK(hardSurfaceDigest(2,1,128,blocks,texels)==hs);
}

TEST_CASE("veg population producer tripwire") {
    SourceBytes s;
    const auto centre=clumpCandidate(7,9,42); s.scalar(centre.x); s.scalar(centre.y); s.word(centre.priority);
    const auto clump=makeClump(7,9,42,ClumpRecipe{},0.5f,3,false); s.word(clump.count);
    for(int i=0;i<clump.count;++i) { const auto& m=clump.members[i]; s.scalar(m.x); s.scalar(m.y); s.scalar(m.scale); s.word(m.frame); s.word(m.woody); }
    const auto blade=fieldBlade(7,9,42,3,7,33,FieldRecipe{});
    s.scalar(blade.x); s.scalar(blade.y); s.scalar(blade.scale); s.scalar(blade.yaw); s.scalar(blade.seed); s.word(blade.frame);
    const auto packed=packInstance(1,2,3,0.5f,4,7,0.3f); s.word(packed.heightScale); s.word(packed.appearance);
    const auto p=buildPopulation(std::vector<GosVegetation::Instance>{{1,2,3,0.5f,4,7,0.3f},{-1,2,3,0.5f,4,7,0.3f}},1);
    s.word(populationHash(p.instances)); s.word(populationHash(p.regions));
    CHECK_MESSAGE(mc2::content::fastHash(s.bytes)==15669633845614351006ull, "placement output changed: bump kProducer");
}
