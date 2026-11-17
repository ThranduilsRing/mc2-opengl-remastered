#include "doctest.h"
#include "pak_zip.h"
#include "vfs.h"
#include "file.h"
#include "ffile.h"
#include "fastfile.h"
#include "fst_hash.h"
#include "mc2fmt.h"
#include "platform_windows.h"
#include "Gates/gate_name.h"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdlib>
#include <cstring>
namespace fs = std::filesystem;
namespace {
using Bytes = std::vector<uint8_t>;
const fs::path fixtures = fs::absolute(fs::path(__FILE__).parent_path().parent_path() / "fixtures/pak");
void le(Bytes& b, uint64_t v, unsigned n) { for (unsigned i=0; i<n; ++i) b.push_back(static_cast<uint8_t>(v >> (8*i))); }
uint64_t get(const Bytes& b, size_t p, unsigned n) {
    uint64_t v=0; for (unsigned i=0; i<n; ++i) v |= uint64_t(b.at(p+i)) << (8*i); return v;
}
void set(Bytes& b, size_t p, uint64_t v, unsigned n) { for (unsigned i=0; i<n; ++i) b.at(p+i)=static_cast<uint8_t>(v >> (8*i)); }
Bytes load(const fs::path& p) { std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), {}); }
void save(const fs::path& p, const Bytes& b) {
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary); REQUIRE(f.good());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
const std::vector<std::pair<std::string,std::string>> payloads = {
    {"data/pak/store.bin", "abc"}, {"data/pak/zstd.txt", [] { std::string s; for(int i=0;i<100;++i) s += "hello Zstandard\n"; return s; }()},
    {"data/pak/128/strip.bin", "xyz"}
};
// Same tiny FST authored by T1's make_install and packed by the committed CLI.
void author(const fs::path& p, bool changed=false) {
    Bytes toc, body; le(toc, 0xCADDECAF,4); le(toc,payloads.size(),4);
    for (auto [name, raw] : payloads) {
        if (changed) raw += "changed";
        uLongf size=compressBound(static_cast<uLong>(raw.size())); Bytes compressed(size);
        REQUIRE(compress2(compressed.data(), &size, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION)==Z_OK);
        compressed.resize(size); le(toc,8+payloads.size()*266+body.size(),4); le(toc,size,4); le(toc,raw.size(),4); le(toc,0,4);
        toc.insert(toc.end(),name.begin(),name.end()); toc.resize(toc.size()+250-name.size(),0);
        body.insert(body.end(),compressed.begin(),compressed.end());
    }
    toc.insert(toc.end(),body.begin(),body.end()); save(p,toc);
}
struct Scratch {
    fs::path old=fs::current_path(), root=fs::temp_directory_path()/"mc2pak-unit";
    Scratch() {
        REQUIRE(!fs::exists(root)); fs::create_directories(root/"paks"); fs::current_path(root);
        maxFastFiles=16; numFastFiles=0; fastFiles=static_cast<FastFile**>(std::calloc(16,sizeof(FastFile*)));
        author("first.fst"); author("second.fst");
        fs::copy_file(fixtures/"stock.mc2pak","paks/stock.mc2pak");
    }
    void mount() { REQUIRE(FastFileInit("first.fst")); REQUIRE(FastFileInit("second.fst")); mc2::vfs::rebuild(); }
    ~Scratch() { FastFileFini(); fs::current_path(old); mc2::vfs::rebuild(); fs::remove_all(root); }
};
std::string read(const char* k) {
    File f; REQUIRE(f.open(k)==NO_ERR); std::string out(f.fileSize(),'\0');
    CHECK(f.read(reinterpret_cast<MemoryPtr>(out.data()),static_cast<long>(out.size()))==static_cast<long>(out.size())); return out;
}
size_t central(const Bytes& b, const std::string& name) {
    for(size_t p=0;p+46<b.size();++p) if(get(b,p,4)==0x02014b50 &&
        std::string(reinterpret_cast<const char*>(b.data()+p+46),static_cast<size_t>(get(b,p+28,2)))==name) return p;
    REQUIRE(false); return 0;
}
size_t dataOffset(const Bytes& b,size_t c) {
    auto l=static_cast<size_t>(get(b,c+42,4)); return l+30+static_cast<size_t>(get(b,l+26,2)+get(b,l+28,2));
}
}
TEST_CASE("pak/identity authority digest and private RAM writes") {
    Scratch s; s.mount();
    for(const auto& [k,raw]:payloads) {
        auto chain=mc2::vfs::candidates(k); REQUIRE(chain.size()==3);
        CHECK(chain[0].provider==mc2::vfs::Provider::Pak);
        CHECK(chain[1].provider==mc2::vfs::Provider::Fst);
        for(size_t i=0;i<chain.size();++i) CHECK(chain[i].source.authorityRank==i);
        CHECK(read(k.c_str())==raw); CHECK(fileExists(k.c_str())==2);
        const auto digest=mc2::vfs::contentDigest(k);
        File f; REQUIRE(f.open(k.c_str())==NO_ERR); unsigned char x='X'; CHECK(f.write(&x,1)==1); f.close();
        CHECK(read(k.c_str())==raw);
        fs::rename("paks/stock.mc2pak","paks/stock.mc2pak.off"); mc2::vfs::rebuild();
        CHECK(read(k.c_str())==raw); CHECK(mc2::vfs::contentDigest(k)==digest);
        fs::rename("paks/stock.mc2pak.off","paks/stock.mc2pak"); mc2::vfs::rebuild();
    }
    save("data/pak/store.bin",Bytes{'l','o','o','s','e'}); mc2::vfs::rebuild();
    CHECK(read("data/pak/store.bin")=="loose");
    CHECK(mc2::vfs::candidates("data/pak/128/store.bin")[0].viaStripAlias);
}
TEST_CASE("pak/provenance and earlier archive authority") {
    Scratch s;
    SUBCASE("changed source ineligible") {
        author("first.fst",true); s.mount();
        CHECK(mc2::vfs::candidates(payloads[0].first)[0].provider==mc2::vfs::Provider::Fst);
        CHECK(read(payloads[0].first.c_str())=="abcchanged");
    }
    SUBCASE("earlier archive adds key over matching later source") {
        // The packed winner came from first.fst; a newly registered earlier archive adds the key.
        author("earlier.fst",true); REQUIRE(FastFileInit("earlier.fst")); s.mount();
        auto c=mc2::vfs::candidates(payloads[0].first); REQUIRE(c.size()==4);
        CHECK(c[0].provider==mc2::vfs::Provider::Fst); CHECK(c[1].provider==mc2::vfs::Provider::Pak);
        CHECK(read(payloads[0].first.c_str())=="abcchanged");
    }
    SUBCASE("absent sources remain available") {
        mc2::vfs::rebuild(); CHECK(read(payloads[0].first.c_str())=="abc");
    }
}
TEST_CASE("pak/mod loose strip pak fst ranks") {
    Scratch s; s.mount();
    const char* active=mc2::gate::name<"MC2_ACTIVE_MOD">();
    struct Mods { const char* env; ~Mods(){ _putenv_s(env, ""); InitModSearchPaths("mods"); } } mods{active};
    const char* k="data/pak/128/strip.bin";
    save("data/pak/store.bin",Bytes{'s'});
    save("data/pak/strip.bin",Bytes{'s'});
    save(k,Bytes{'l'});
    save("mods/pakmod/data/pak/128/strip.bin",Bytes{'m'});
    save("mods/pakmod/data/pak/store.bin",Bytes{'m'});
    _putenv_s(active,"pakmod"); InitModSearchPaths("mods"); mc2::vfs::rebuild();
    auto c=mc2::vfs::candidates(payloads[0].first); REQUIRE(c.size()==5);
    CHECK(c[0].provider==mc2::vfs::Provider::Mod); CHECK(c[1].provider==mc2::vfs::Provider::Loose);
    CHECK(c[2].provider==mc2::vfs::Provider::Pak); CHECK(c[3].provider==mc2::vfs::Provider::Fst);
    for(size_t i=0;i<c.size();++i) CHECK(c[i].source.authorityRank==i);
    auto alias=mc2::vfs::candidates(k); REQUIRE(alias.size()==6);
    CHECK(alias[0].provider==mc2::vfs::Provider::Mod); CHECK(alias[1].provider==mc2::vfs::Provider::Loose);
    CHECK(alias[2].viaStripAlias); CHECK(alias[3].provider==mc2::vfs::Provider::Pak);
    CHECK(alias[4].provider==mc2::vfs::Provider::Fst); CHECK(read(k)=="m");
    for(size_t i=0;i<alias.size();++i) CHECK(alias[i].source.authorityRank==i);
}
TEST_CASE("pak/corruption structural bounds and unsupported fallback") {
    Scratch s; auto bytes=load("paks/stock.mc2pak"); const auto k=payloads[0].first;
    const auto c=central(bytes,k), l=static_cast<size_t>(get(bytes,c+42,4)); bool reject=false, zstd=false;
    SUBCASE("CRC entry eviction") { bytes[dataOffset(bytes,c)]^=1; }
    SUBCASE("central directory signature") { bytes[c]^=1; reject=true; }
    SUBCASE("truncated EOCD") { bytes.resize(bytes.size()-10); reject=true; }
    SUBCASE("oversized allocation claim") { set(bytes,c+24,(1ull<<30)+1,4); set(bytes,l+22,(1ull<<30)+1,4); reject=true; }
    SUBCASE("offset overflow") { set(bytes,c+42,UINT32_MAX-1,4); reject=true; }
    SUBCASE("encrypted") { set(bytes,c+8,1,2); set(bytes,l+6,1,2); reject=true; }
    SUBCASE("descriptor") { set(bytes,c+8,8,2); set(bytes,l+6,8,2); reject=true; }
    SUBCASE("unsupported method") { set(bytes,c+10,14,2); set(bytes,l+8,14,2); }
    SUBCASE("zstd frame size differs") {
        zstd=true;
        auto z=central(bytes,payloads[1].first); auto zl=static_cast<size_t>(get(bytes,z+42,4));
        set(bytes,z+24,payloads[1].second.size()+1,4); set(bytes,zl+22,payloads[1].second.size()+1,4);
    }
    save("paks/stock.mc2pak",bytes); s.mount();
    for(const auto& [name,raw]:payloads) { CHECK(read(name.c_str())==raw); CHECK(read(name.c_str())==raw); }
    if(reject) for(const auto& e:mc2::vfs::candidates(k)) CHECK(e.provider!=mc2::vfs::Provider::Pak);
    else CHECK(mc2::vfs::candidates(zstd ? payloads[1].first : k)[0].provider!=mc2::vfs::Provider::Pak);
}
TEST_CASE("pak/Store Deflate Zstandard odd offsets and ZIP64 records") {
    Scratch s;
    for(const char* name:{"stock.mc2pak","deflate.mc2pak","local64.mc2pak","zip64.mc2pak"}) {
        mc2::vfs::PakZip zip((fixtures/name).string()); REQUIRE(zip.valid());
        for(const auto& [k,raw]:payloads) {
            auto e=zip.find(k); REQUIRE(e); auto b=zip.read(*e); REQUIRE(b);
            CHECK(std::string(b->begin(),b->end())==raw);
        }
    }
}
TEST_CASE("pak/stock_identity") {
    const char* install=std::getenv("MC2_PAK_IDENTITY_INSTALL");
    if(!install) return;
    REQUIRE(fs::is_directory(install));
    const auto old=fs::current_path();
    struct Restore { fs::path old; ~Restore(){ FastFileFini(); fs::current_path(old); mc2::vfs::rebuild(); } } restore{old};
    fs::current_path(install);
    mc2::vfs::PakZip zip("paks/stock.mc2pak"); REQUIRE(zip.valid());
    maxFastFiles=16; numFastFiles=0; fastFiles=static_cast<FastFile**>(std::calloc(16,sizeof(FastFile*)));
    const char* names[]={"mission","tgl","art","textures","misc","camera","effect","insignia"};
    for(auto n:names) {
        auto path=std::string(n)+".fst"; if(!fs::exists(path)) path="data/assets/"+path;
        REQUIRE(FastFileInit(path.c_str()));
    }
    std::set<std::string> seen; uint64_t fstBytes=0,pakBytes=0,duplicates=0,mismatches=0;
    for(long a=0;a<numFastFiles;++a) {
        auto* ff=fastFiles[a]; auto* info=ff->getFilesInfo();
        for(long i=0;i<ff->getNumFiles();++i) {
            const auto k=mc2::vfs::key(info[i].pfe->name);
            if(!seen.insert(k).second) { ++duplicates; continue; }
            auto pe=zip.find(k); REQUIRE(pe); auto packed=zip.read(*pe); REQUIRE(packed);
            auto member=ff->openFast(elfHash(k.c_str()),k.c_str()); REQUIRE(member>=0);
            Bytes raw(ff->sizeFast(member));
            CHECK(ff->readFast(member,raw.data(),static_cast<DWORD>(raw.size()))==static_cast<long>(raw.size()));
            ff->closeFast(member); fstBytes+=raw.size(); pakBytes+=packed->size();
            if(raw!=*packed) { ++mismatches; mc2::fmt::print_out("[PAK_IDENTITY_MISMATCH] key={}\n",k); }
        }
    }
    mc2::fmt::print_out("[PAK_IDENTITY] keys={} mismatches={} fst_bytes={} pak_bytes={} shadowed={}\n",seen.size(),
        static_cast<unsigned long long>(mismatches),static_cast<unsigned long long>(fstBytes),
        static_cast<unsigned long long>(pakBytes),static_cast<unsigned long long>(duplicates));
    CHECK(mismatches==0);
}
