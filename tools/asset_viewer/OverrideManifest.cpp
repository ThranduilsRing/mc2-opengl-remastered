#include "OverrideManifest.h"
#include <cctype>
#include <string>
#include <string_view>
static std::string lower(std::string s){ for(char& c:s) c=(char)std::tolower((unsigned char)c); return s; }
static bool isSafeSource(std::string_view s){
    if (s.empty()||s[0]=='/'||s[0]=='\\') return false;
    if (s.size()>=2 && s[1]==':') return false;
    if (s.find("..")!=std::string::npos) return false;
    std::string l=lower(std::string(s));
    return (l.size()>=4 && l.compare(l.size()-4,4,".glb")==0) || (l.size()>=5 && l.compare(l.size()-5,5,".gltf")==0);
}
std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& r){
    std::vector<Warning> w; auto block=[&](std::string_view c,std::string_view m){ w.push_back({WarnSeverity::Block,std::string(c),std::string(m)}); };
    if (!r.renderOnly)                       block("renderOnly","renderOnly must be true (MVP)");
    if (r.fallback!="stock")                 block("fallback","fallback must be \"stock\" (MVP)");
    if (r.scale!=1.0f)                       block("scale","runtime scale must be exactly 1.0 — bake scale into the GLB");
    { std::string c=lower(r.overrideClass); if (c!="staticprop" && c!="tree" && c!="vehicle") block("class","class must be staticProp, tree or vehicle"); }
    if (r.appearanceName.empty())            block("replaces","no stock appearance bound");
    if (!r.appearanceVerified)               block("appearance-unverified","appearance key not confirmed — verify it matches the engine appearance name");
    if (!isSafeSource(r.sourceRelPath))      block("source","source must be a safe relative .glb/.gltf path");
    int last=0; for (const auto& l: r.lods){
        if (l.lod<=last)                     block("lod-order","LOD indices must strictly ascend (LOD0=source)");
        if (!isSafeSource(l.sourceRelPath))  block("lod-source","LOD source must be a safe relative .glb/.gltf path");
        last=l.lod; }
    return w;
}

static std::string esc(std::string_view s){
    std::string o; o.reserve(s.size()+8);
    for(char c:s){ switch(c){
        case '"': o+="\\\""; break; case '\\': o+="\\\\"; break;
        case '\n': o+="\\n"; break; case '\t': o+="\\t"; break; case '\r': o+="\\r"; break;
        default: o+=c; } }
    return o;
}
std::string ToModelsJson(const std::vector<WorkbenchOverride>& recs){
    std::string o="{\n  \"overrides\": [\n";
    for (size_t i=0;i<recs.size();++i){ const auto& r=recs[i];
        o+="    {\"type\":\"model\",\"class\":\""+esc(r.overrideClass)+"\",";
        o+="\"replaces\":\""+esc(r.overrideClass+":"+r.appearanceName)+"\",";
        o+="\"source\":\""+esc(r.sourceRelPath)+"\",\"renderOnly\":true,\"scale\":1.0,\"fallback\":\"stock\"";
        if(!r.lods.empty()){ o+=",\"lods\":[";
            for(size_t j=0;j<r.lods.size();++j){ const auto& l=r.lods[j];
                o+="{\"lod\":"+std::to_string(l.lod)+",\"source\":\""+esc(l.sourceRelPath)+"\",\"distance\":"+std::to_string(l.distance)+"}";
                if(j+1<r.lods.size()) o+=","; }
            o+="]"; }
        o+="}"; if(i+1<recs.size()) o+=","; o+="\n"; }
    o+="  ]\n}\n"; return o;
}
