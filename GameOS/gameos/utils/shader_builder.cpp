#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif
#include <GL/glew.h>
#include <assert.h>
#include <stdio.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <sstream>
#include <string>
#include <algorithm>
#include <map>

#include "diagnostic_trace.h"
#include "gos_validate.h"
#include "utils/stream.h"
#include "utils/logging.h"
#include "utils/gl_utils.h"
#include "utils/shader_builder.h"
#include "utils/timing.h"
#include "utils/file_utils.h"
#include "gos_profiler.h"


std::map<std::string, glsl_shader*> glsl_shader::s_shaders[glsl_shader::NUM_SHADER_TYPES];

std::map<std::string, glsl_program*> glsl_program::s_programs;
UNIFORM_FUNC glsl_program::uniformFuncs[15] = {0};

const int constantSizes[] = {
    sizeof(float),
    sizeof(int),
    2*sizeof(float),
    3*sizeof(float),
    4*sizeof(float),
    sizeof(int) * 2,
    sizeof(int) * 3,
    sizeof(int) * 4,
    sizeof(int),
    sizeof(int) * 2,
    sizeof(int) * 3,
    sizeof(int) * 4,
    4*sizeof(float),
    9*sizeof(float),
    16*sizeof(float),
};

void init_func_ptrs(UNIFORM_FUNC (&uniformFuncs)[15])
{
	// changed fromARB variants, to work with CORE profile as well (because *ARB variants are not initialized in case of CORE profile)
    uniformFuncs[CONSTANT_FLOAT] = (UNIFORM_FUNC) glUniform1fv;
    uniformFuncs[CONSTANT_VEC2]  = (UNIFORM_FUNC) glUniform2fv;
    uniformFuncs[CONSTANT_VEC3]  = (UNIFORM_FUNC) glUniform3fv;
    uniformFuncs[CONSTANT_VEC4]  = (UNIFORM_FUNC) glUniform4fv;
    uniformFuncs[CONSTANT_INT]   = (UNIFORM_FUNC) glUniform1iv;
    uniformFuncs[CONSTANT_IVEC2] = (UNIFORM_FUNC) glUniform2iv;
    uniformFuncs[CONSTANT_IVEC3] = (UNIFORM_FUNC) glUniform3iv;
    uniformFuncs[CONSTANT_IVEC4] = (UNIFORM_FUNC) glUniform4iv;
    uniformFuncs[CONSTANT_BOOL]  = (UNIFORM_FUNC) glUniform1iv;
    uniformFuncs[CONSTANT_BVEC2] = (UNIFORM_FUNC) glUniform2iv;
    uniformFuncs[CONSTANT_BVEC3] = (UNIFORM_FUNC) glUniform3iv;
    uniformFuncs[CONSTANT_BVEC4] = (UNIFORM_FUNC) glUniform4iv;
    uniformFuncs[CONSTANT_MAT2]  = (UNIFORM_FUNC) glUniformMatrix2fv;
    uniformFuncs[CONSTANT_MAT3]  = (UNIFORM_FUNC) glUniformMatrix3fv;
    uniformFuncs[CONSTANT_MAT4]  = (UNIFORM_FUNC) glUniformMatrix4fv;
}

namespace {

std::string diag_json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

static const char* stage_name(glsl_shader::Shader_t stype) {
    switch (stype) {
    case glsl_shader::VERTEX:   return "vertex";
    case glsl_shader::FRAGMENT: return "fragment";
    case glsl_shader::HULL:     return "hull";
    case glsl_shader::DOMAINE:  return "domain";
    case glsl_shader::GEOMERTY: return "geometry";
    default:                    return "unknown";
    }
}

} // namespace

// true - error, false - no error
bool get_shader_error_status(GLuint shader, GLenum status_type, std::string* out_log = nullptr)
{
    int status;
    glGetShaderiv(shader, status_type, &status);

    if(!status)
    {
        char* buf = 0;
        GLsizei len = 0, len2= 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        buf = new char[len];

        glGetShaderInfoLog(shader, len, &len2, buf);
        if(len2!=0) {
            log_error("CompileShader: %s\n", buf);
            printf("[SHADER ERROR] CompileShader: %s\n", buf);
            fflush(stdout);
            validateRecordShaderError(buf);
            if (out_log) *out_log = buf;
        } else {
            printf("[SHADER ERROR] CompileShader: driver reported FAIL with empty info log (GL id=%u)\n", shader);
            fflush(stdout);
        }
		delete[] buf;

        return true;
    }

    return false;
}

bool get_program_error_status(GLuint program, GLenum status_type, std::string* out_log = nullptr)
{
    int status;
    glGetProgramiv(program, status_type, &status);

    if(!status)
    {
        char* buf = 0;
        GLsizei len = 0, len2= 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        buf = new char[len];

        glGetProgramInfoLog(program, len, &len2, buf);
        if(len2!=0) {
            log_error("LinkProgram: %s\n", buf);
            printf("[SHADER ERROR] LinkProgram: %s\n", buf);
            fflush(stdout);
            validateRecordShaderError(buf);
            if (out_log) *out_log = buf;
        }
		delete[] buf;
        return true;
    }

    return false;
}

GLenum get_gl_shader_type(glsl_shader::Shader_t type)
{
	static const  GLenum types[] =  {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER,  GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER, GL_GEOMETRY_SHADER };
	assert(type >=0  && type < sizeof(types)/sizeof(types[0]) );

	return types[type];
}

glsl_shader::Shader_t get_shader_type(GLenum type)
{
	switch(type)
	{
		case GL_VERTEX_SHADER:
			return glsl_shader::VERTEX;
		case GL_FRAGMENT_SHADER:
			return glsl_shader::FRAGMENT;
		case GL_TESS_CONTROL_SHADER:
			return glsl_shader::HULL;
		case GL_TESS_EVALUATION_SHADER:
			return glsl_shader::DOMAINE;
		case GL_GEOMETRY_SHADER:
			return glsl_shader::GEOMERTY;
		default:
			assert(0 && "This shader type is not supported yet!");
			return glsl_shader::NUM_SHADER_TYPES;
	}
}



const char* glsl_load(const char* fname, size_t* out_size = nullptr)
{
    assert(fname);
    stream* pstream = stream::makeFileStream();
    if(0 != pstream->open(fname,"rb"))
    {
        log_error("Can't open %s \n", fname);
        delete pstream;
        return 0;
    }

    pstream->seek(0, stream::S_END);
    size_t size = pstream->tell();
    pstream->seek(0, stream::S_SET);

    char* pdata = new char[size + 1];
    size_t rv = pstream->read(pdata, 1, size);
    assert(rv==size);
    pdata[size] = '\0';

    // Strip UTF-8 BOM (EF BB BF) defensively.
    // BOM is valid UTF-8 but GLSL forbids non-ASCII tokens — driver reports
    // "unexpected token '€'" at line 0 with no file name. Strip + emit so
    // SHADER_COMPILE tag captures it; check_shader_bom.py is the preflight gate.
    if (size >= 3 &&
        static_cast<unsigned char>(pdata[0]) == 0xEF &&
        static_cast<unsigned char>(pdata[1]) == 0xBB &&
        static_cast<unsigned char>(pdata[2]) == 0xBF) {
        memmove(pdata, pdata + 3, size - 3 + 1); // +1 = null terminator
        size -= 3;
        fprintf(stderr, "[shader_builder] WARN: stripped UTF-8 BOM from %s\n", fname);
        char evJson[512];
        snprintf(evJson, sizeof(evJson),
            "{\"event\":\"bom_stripped\",\"path\":\"%s\"}", fname);
        mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, evJson);
    }

    if(out_size)
        *out_size = size;

    pstream->close();
    delete pstream;

    return pdata;
}

bool parse_include(const char* str, const char*& include, size_t& size, const char*& ieol)
{
    const char* begin = strchr(str, '<');
    const char* end = strchr(str, '>');
    const char* eol = strchr(str, '\n');
    ieol = eol+1; // skip \n

    if(!begin || !end || (eol && end > eol) || end - begin <= 1)
        return false;

    begin++;

    while(*begin==' ')
        begin++;

    while(*(end-1)==' ')
        end--;

    include = begin;
    size = end - begin;
    return true;
}

bool load_shader(const char* fname, std::string& shader_source, std::vector<std::string>& includes);

size_t get_num_lines(const char* text)
{
    if(!text)
        return 0;

    size_t count = 1;
    const char* token = text;
    while(token && (token = strchr(token, '\n')))
    {
        token++;
        count++;
    }

    return count;
}

void append_line_directive(std::string& code, size_t line, const char* fname)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "#line %zu // %s\n", line, fname);
    buf[510] = '\n'; // just in case our snprintf'ed line will be more than 512
    code.append(buf);
}

// Find next "#include" occurrence in 'p' that is NOT inside a // line
// comment or /* ... */ block comment or a "..." / '...' string literal.
// Naive strstr() would match the token inside prose like "no #include,"
// and derail the include parser.
static const char* find_next_include_directive(const char* p)
{
    static const char* INCLUDE = "#include";
    while (*p)
    {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') ++p;
        }
        else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) ++p;
            if (*p) p += 2;
        }
        else if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) p += 2;
                else ++p;
            }
            if (*p) ++p;
        }
        else if (p[0] == '#' && strncmp(p, INCLUDE, 8) == 0) {
            return p;
        }
        else {
            ++p;
        }
    }
    return nullptr;
}

bool parse_includes(const char* fname, const char* psource, std::vector<std::string>& include_list, std::string& parsed_source)
{
    size_t current_line = 1;

    std::string base_path = filesystem::get_path(fname);

    static const char* INCLUDE = "#include";
    const char* token = psource;
    const char* start = psource;
    while((token = find_next_include_directive(start)))
    {
        std::string code = std::string(start, token - start);

        append_line_directive(parsed_source, current_line, fname);
        parsed_source.append(code);
        current_line += get_num_lines(code.c_str());

        const char* include;
        size_t size;
        if(!parse_include(token + strlen(INCLUDE), include, size, start))
            return false;

        std::string inc = std::string(include, size);
        std::string include_path = base_path + std::string(filesystem::kPathSeparator) + inc;
        include_list.push_back(include_path);

        // insert include contents to the shader source code
        std::string source;
        if(!load_shader(include_path.c_str(), source, include_list))
            return false;

        parsed_source.append(source);

        if(!start) // include was at last line 
            break;
    }

    if(*start)
    {
        append_line_directive(parsed_source, current_line, fname);
        parsed_source.append(std::string(start));
    }

    return true;
}

bool load_shader(const char* fname, std::string& shader_source, std::vector<std::string>& includes)
{
    const char* psource = glsl_load(fname);
    if(!psource)
        return false;

    if(!parse_includes(fname, psource, includes, shader_source))
    {
		log_error("Shader filename: %s: failed to parse includes\n", fname);
        delete[] psource;
        return false;
    }

    delete[] psource;

    return true;
}

bool compile_shader(GLenum shader, const char** strings, size_t count, std::string* out_log = nullptr)
{
    ZoneScopedN("Shader.Compile");
    // Drain any leftover GL errors from prior calls — an uncleared
    // GL_INVALID_ENUM (e.g. from debug output probes) would otherwise
    // poison this compile's return value even if compile itself succeeds.
    while(glGetError() != GL_NO_ERROR) { /* discard */ }

    glShaderSource(shader, count, strings, 0);
    glCompileShader(shader);

	GLenum err = glGetError();
	if(err != GL_NO_ERROR)
	{
		log_error("OpenGL Error: %s\n", ogl_get_error_code_str(err));
	}

    bool error = get_shader_error_status(shader, GL_COMPILE_STATUS, out_log);
    return !error && err==GL_NO_ERROR;
}


// ---------------------------------------------------------------------------
// SPIRV-CONSUMER-PILOT-BUILD-1 — default-OFF runtime SPIR-V consumer.
// Pilot scope: the postprocess composite vertex/fragment pair ONLY. Loads the
// offline-baked .spv (OFFLINE-SHADER-VARIANT-BUILD-1) via glShaderBinary +
// glSpecializeShader, replacing the runtime GLSL compile for that one family.
// Any miss (env OFF / extension absent / hot-reload on / not a pilot shader /
// non-default variant / artifact missing / specialize failure) returns false
// so makeShader falls through to the unchanged GLSL path. Hot reload (reload())
// is GLSL-only by construction and never reaches this code.
// ---------------------------------------------------------------------------
namespace {

bool spirvPilotEnabled()
{
    const char* e = std::getenv("MC2_SHADER_SPIRV");
    if (!e || e[0] != '1') return false;                 // default OFF
    const char* hr = std::getenv("MC2_SHADER_HOT_RELOAD");
    if (hr && hr[0] == '1') return false;                // dev loop stays GLSL
    // GL 4.3 context request does NOT imply SPIR-V support — check at runtime.
    if (!(GLEW_ARB_gl_spirv || GLEW_VERSION_4_6)) return false;
    return true;
}

bool spirvFatal()
{
    const char* e = std::getenv("MC2_SHADER_SPIRV_FATAL");
    return e && e[0] == '1';
}

static bool spirvEndsWith(const char* fname, const char* suffix)
{
    if (!fname) return false;
    std::string f(fname);
    size_t n = std::strlen(suffix);
    return f.size() >= n && f.compare(f.size() - n, n, suffix) == 0;
}

// Pilot stage table: file-suffix -> (base, short stage). The base MUST match the
// offline builder's pilots.json base so the keyed lookup resolves. Note many
// programs share /postprocess.vert (composite + cloud/shoreline/ssao/fog/...);
// it is baked ONCE as base "postprocess" and reused by every such program's vert
// stage. shadow_mech is intentionally absent (stays GLSL).
static const struct { const char* suffix; const char* base; const char* stage; }
kSpirvPilotStages[] = {
    {"/postprocess.vert", "postprocess", "vert"},
    {"/postprocess.frag", "postprocess", "frag"},
    {"/mech.vert",        "mech",        "vert"},
    {"/mech.frag",        "mech",        "frag"},
    // SPIRV-POSTPROCESS-FAMILY-1: frag-only family members (vert = postprocess.vert).
    {"/ssao.frag",        "ssao",        "frag"},
    {"/cloud.frag",       "cloud",       "frag"},
    {"/shoreline.frag",   "shoreline",   "frag"},
    {"/fog_oob.frag",     "fog_oob",     "frag"},
    {"/edge_fog.frag",    "edge_fog",    "frag"},
    {"/hzb_reduce.frag",  "hzb_reduce",  "frag"},
};

// Pilot PROGRAM table: exact (vert-suffix, frag-suffix) pairs that are pilots.
// Matching by BOTH stage filenames keeps it program-atomic — a shared vert being
// SPIR-V in one program and GLSL in a non-pilot program is fine (different
// programs); a program is all-SPIR-V or all-GLSL, never mixed.
static const struct { const char* vsuf; const char* fsuf; } kSpirvPilotPrograms[] = {
    {"/postprocess.vert", "/postprocess.frag"},
    {"/mech.vert",        "/mech.frag"},
    {"/postprocess.vert", "/ssao.frag"},
    {"/postprocess.vert", "/cloud.frag"},
    {"/postprocess.vert", "/shoreline.frag"},
    {"/postprocess.vert", "/fog_oob.frag"},
    {"/postprocess.vert", "/edge_fog.frag"},
    {"/postprocess.vert", "/hzb_reduce.frag"},
};

// Pilot allowlist: fname -> (base, short stage). Returns false if not a pilot.
bool spirvPilotStage(const char* fname, std::string& base, std::string& stageShort)
{
    for (const auto& s : kSpirvPilotStages) {
        if (spirvEndsWith(fname, s.suffix)) { base = s.base; stageShort = s.stage; return true; }
    }
    return false;
}

// Program-atomic pilot decision (called by makeProgram2 with the FULL stage set).
bool spirvCompositePilotProgram(const char* vp, const char* hp, const char* dp,
                                const char* gp, const char* fp, const char* /*prefix*/)
{
    if (!spirvPilotEnabled()) return false;
    if (hp || dp || gp) return false;                            // pilots are vert+frag only
    for (const auto& p : kSpirvPilotPrograms) {
        if (spirvEndsWith(vp, p.vsuf) && spirvEndsWith(fp, p.fsuf)) return true;
    }
    return false;
}

// SPIRV-KEYED-VARIANT-CONSUMER-1: canonical define-set key from the runtime
// prefix. MUST match the offline builder's canonicalization exactly:
//   key = ";".join(sorted("NAME=VALUE" | "NAME" for each #define))
// Only #define lines are considered (#version/#extension are skipped). This is
// the COMPLETE realized define-set — a define present in the GLSL prefix but not
// represented in any baked artifact yields a key with no index match => GLSL
// fallback (no silent "some defines" matching).
std::string spirvDefineKey(const char* prefix)
{
    std::vector<std::string> defs;
    if (prefix) {
        std::istringstream ss(prefix);
        std::string line;
        while (std::getline(ss, line)) {
            size_t p = line.find("#define");
            if (p == std::string::npos) continue;
            std::istringstream ls(line.substr(p + 7)); // after "#define"
            std::string name, val;
            ls >> name;
            if (name.empty()) continue;
            ls >> val;
            defs.push_back(val.empty() ? name : (name + "=" + val));
        }
    }
    std::sort(defs.begin(), defs.end());
    std::string key;
    for (size_t i = 0; i < defs.size(); ++i) { if (i) key += ";"; key += defs[i]; }
    return key;
}

// Extract the quoted string value of `field` at/after `from`; advance `endpos`.
static bool spirvJsonField(const std::string& t, size_t from, const char* field,
                           std::string& out, size_t& endpos)
{
    size_t k = t.find(field, from);
    if (k == std::string::npos) return false;
    size_t c = t.find(':', k);     if (c == std::string::npos) return false;
    size_t q1 = t.find('"', c);    if (q1 == std::string::npos) return false;
    size_t q2 = t.find('"', q1+1); if (q2 == std::string::npos) return false;
    out = t.substr(q1 + 1, q2 - q1 - 1);
    endpos = q2 + 1;
    return true;
}

// Deployed variant index: shaders/spv/spirv_index.json, written by
// build_variants.py. Maps "base|stage|defkey" -> artifact filename. Parsed once
// (records emit "key" before "artifact", so the pairing is order-stable).
const std::map<std::string, std::string>& spirvIndex()
{
    static std::map<std::string, std::string> idx;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream in("shaders/spv/spirv_index.json", std::ios::binary);
        if (in) {
            std::string t((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
            size_t pos = 0;
            std::string key, art;
            size_t e1 = 0, e2 = 0;
            while (spirvJsonField(t, pos, "\"key\"", key, e1) &&
                   spirvJsonField(t, e1, "\"artifact\"", art, e2)) {
                idx[key] = art;
                pos = e2;
            }
        }
    }
    return idx;
}

// Specialize `shader` from the baked .spv. true => caller skips GLSL compile.
// Keyed by the COMPLETE define-set: (base, stage, defkey) -> artifact via the
// deployed index. A miss (unknown program, or a define-set with no baked
// artifact) returns false => GLSL fallback.
bool trySpirvSpecialize(GLuint shader, const char* fname, const char* prefix)
{
    if (!spirvPilotEnabled()) return false;
    std::string base, stageShort;
    if (!spirvPilotStage(fname, base, stageShort)) return false;

    const std::string defkey = spirvDefineKey(prefix);
    const std::string key = base + "|" + stageShort + "|" + defkey;
    const auto& idx = spirvIndex();
    auto it = idx.find(key);
    if (it == idx.end()) {
        log_info("[SPIRV] no artifact for variant key '%s' (%s); GLSL fallback\n",
                 key.c_str(), fname);
        return false;
    }
    const std::string artifact = it->second;
    const std::string spvPath = "shaders/spv/" + artifact;
    std::ifstream in(spvPath, std::ios::binary | std::ios::ate);
    if (!in) {
        log_info("[SPIRV] artifact missing %s; GLSL fallback\n", spvPath.c_str());
        return false;
    }
    std::streamsize sz = in.tellg();
    in.seekg(0);
    std::vector<char> blob(sz > 0 ? (size_t)sz : 0);
    if (sz <= 0 || !in.read(blob.data(), sz)) {
        log_error("[SPIRV] failed to read %s; GLSL fallback\n", spvPath.c_str());
        return false;
    }

    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, blob.data(), (GLsizei)sz);
    if (glGetError() != GL_NO_ERROR) {
        if (spirvFatal()) { log_error("[SPIRV] FATAL glShaderBinary %s\n", spvPath.c_str()); assert(false); }
        log_error("[SPIRV] glShaderBinary rejected %s; GLSL fallback\n", spvPath.c_str());
        return false;
    }
    glSpecializeShader(shader, "main", 0, nullptr, nullptr);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (glGetError() != GL_NO_ERROR || ok != GL_TRUE) {
        if (spirvFatal()) { log_error("[SPIRV] FATAL specialize %s\n", spvPath.c_str()); assert(false); }
        log_error("[SPIRV] glSpecializeShader failed %s; GLSL fallback\n", spvPath.c_str());
        return false;
    }
    log_info("[SPIRV] loaded %s key='%s' (specialized \"main\")\n",
             artifact.c_str(), key.c_str());
    if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
        std::ostringstream data;
        data << "{\"event\":\"spirv_loaded\",\"path\":\""
             << diag_json_escape(std::string(fname)) << "\",\"key\":\""
             << diag_json_escape(key) << "\",\"artifact\":\""
             << diag_json_escape(artifact) << "\",\"result\":\"ok\"}";
        mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
    }
    return true;
}

} // namespace

// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: public canonical define-key, in
// lockstep with the SPIR-V variant consumer (delegates to spirvDefineKey above).
std::string glsl_program::shaderDefineKey(const char* prefix)
{
    return spirvDefineKey(prefix);
}

glsl_shader* glsl_shader::makeShader(Shader_t stype, const char* fname, const char* prefix/* = nullptr*/, bool trySpirv/* = false*/)
{
    ZoneScopedN("Shader.MakeShader");
    std::string shader_source;
    std::vector<std::string> shader_includes;

    {
    ZoneScopedN("Shader.LoadSource");
    if(!load_shader(fname, shader_source, shader_includes))
    {
		log_error("Shader filename: %s, failed to load shader\n", fname);
        return nullptr;
    }
    }

    log_info("Loading shader: %s\n", fname);


    glsl_shader* pshader = new glsl_shader();

    char unique_id[256] = {0};
    snprintf(unique_id, 256, "%s_%p", fname, pshader);
    std::string uid = &unique_id[0];

#define DUMP_SHADER_PREPROCESSED_FILES 1
#if DUMP_SHADER_PREPROCESSED_FILES
    char dump_name[256] = {0};
    snprintf(dump_name, 256, "./dump/%s.glsl", uid.c_str());
    FILE* f = fopen(dump_name, "w");
    if(f) {
        fwrite(shader_source.c_str(), shader_source.size(), 1, f);
		fclose(f);
    }
#endif

	
    GLenum type = get_gl_shader_type(stype);
    GLuint shader = glCreateShader(type);
    if(0 == shader)
    {
        glDeleteShader(shader);
        delete pshader;
        return 0;
    }

    // SPIRV-CONSUMER-PILOT-BUILD-1: when the program-atomic pilot gate enabled
    // it (trySpirv), the baked .spv MUST load — if it fails we return nullptr so
    // makeProgram2 can rebuild the WHOLE program as GLSL (atomic; never a program
    // with one SPIR-V stage + one GLSL stage, which would fail to link).
    if (trySpirv && !trySpirvSpecialize(shader, fname, prefix))
    {
        glDeleteShader(shader);
        delete pshader;
        return nullptr;
    }
    if (!trySpirv)
    {
    const char* strings[] = { prefix == nullptr ? "" : prefix, shader_source.c_str() };
    std::string compileLog;
    if(!compile_shader(shader, strings, sizeof(strings)/sizeof(strings[0]), &compileLog))
    {
        if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
            std::ostringstream data;
            data << "{\"event\":\"compile_fail\",\"stage\":\""
                 << stage_name(stype) << "\",\"path\":\""
                 << diag_json_escape(std::string(fname)) << "\",\"result\":\"fail\",\"info_log\":\""
                 << diag_json_escape(compileLog) << "\"}";
            mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
        }
        glDeleteShader(shader);
        delete pshader;
        return nullptr;
    }
    // Log warnings from successful compile. NVIDIA emits them; AMD log is
    // empty on success (len<=1). Drops silently when there's nothing to say.
    {
        GLsizei warnLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &warnLen);
        if (warnLen > 1) {
            char* warnBuf = new char[warnLen];
            GLsizei actualLen = 0;
            glGetShaderInfoLog(shader, warnLen, &actualLen, warnBuf);
            if (actualLen > 0) {
                printf("[SHADER WARN] compile %s:\n%s\n", fname, warnBuf);
                fflush(stdout);
            }
            delete[] warnBuf;
        }
    }

    if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
        std::ostringstream data;
        data << "{\"event\":\"compile_ok\",\"stage\":\""
             << stage_name(stype) << "\",\"path\":\""
             << diag_json_escape(std::string(fname)) << "\",\"result\":\"ok\"}";
        mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
    }
    } // !usedSpirv
    pshader->fname_ = fname;
    pshader->shader_ = shader;
    pshader->type_ = type;
    pshader->includes_ = shader_includes;

    if(s_shaders[stype].count(uid))
	{
        log_error("Duplicate shader name: %s\n", fname);
        delete pshader;
        return nullptr;

		//glsl_shader* pshader = s_shaders[stype][fname];
		//s_shaders[stype].erase(fname);
        //delete pshader;
	}
    s_shaders[stype].insert( std::make_pair(uid, pshader) );

     
    return pshader;
}

void glsl_shader::deleteShader(glsl_shader* psh)
{
	glsl_shader::Shader_t t = get_shader_type(psh->type_);
	if(s_shaders[t].count(psh->fname_))
    {   
        delete s_shaders[t][psh->fname_];
        s_shaders[t].erase(psh->fname_);
    }
}

glsl_shader::~glsl_shader()
{
    glDeleteShader(shader_);
}

bool glsl_shader::reload(const char* prefix)
{
    std::string shader_source;
    std::vector<std::string> shader_includes;

    if(!load_shader(fname_.c_str(), shader_source, shader_includes))
    {
		log_error("Shader filename: %s, failed to load shader\n", fname_.c_str());
        return false;
    }
	
    const char* strings[] = { prefix == nullptr ? "" : prefix, shader_source.c_str() };
    if(!compile_shader(shader_, strings, sizeof(strings)/sizeof(strings[0])))
    {
        return false;
    }

    includes_ = shader_includes;

    return true;
}

uint64_t glsl_shader::getModTimeMs()
{
    using namespace filesystem;

	uint64_t mt = get_file_mod_time_ms(fname_.c_str());

    for(int i=0;i<includes_.size();++i)
    {
	    uint64_t t = get_file_mod_time_ms(includes_[i].c_str());
        mt = max(mt, t);
    }
    return mt;
}

void parse_uniforms(GLuint pprogram, glsl_program::UniArr_t* puniforms, glsl_program::SamplerArr_t* psamplers)
{
    GLint num_uni, max_name_len;
    glGetProgramiv(pprogram, GL_ACTIVE_UNIFORMS, &num_uni);
    glGetProgramiv(pprogram, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_name_len);
    char* buf = new char[max_name_len+1];
    GLsizei len;
    GLint size;
    GLenum type;
    for(GLint i=0;i<num_uni;++i)
    {
        glGetActiveUniform(pprogram, i, max_name_len+1, &len, &size, &type, buf);
        if(-1 == i) continue; // gl_ variable or does not correspond to an active uniform variable name in program

		// PR2c Stage 2c — additive fix: GL_SAMPLER_2D_ARRAY (0x8DC1) lives
		// outside the contiguous GL_SAMPLER_1D..GL_SAMPLER_2D_SHADOW range,
		// so mine_static.frag's `uniform sampler2DArray mineSpriteArray`
		// previously fell through to the default constant-uniform path and
		// produced a garbage type_ index → typeNames[] OOB read → crash in
		// logmsg. Treat it as a sampler with the synthetic SAMPLER_2D type
		// (engine binds via glActiveTexture+glBindTexture either way; the
		// type_ field is only used for the log line below).
		if(type >=GL_SAMPLER_1D && type<= GL_SAMPLER_2D_SHADOW)
		{
			glsl_sampler* psampler = new glsl_sampler;
			psampler->index_ =  glGetUniformLocation(pprogram, buf);
			psampler->name_ = buf;
			psampler->type_ = (SamplerType)(type - GL_SAMPLER_1D);

			assert(psampler->type_ <= SAMPLER_2D_SHADOW);

			static const char *typeNames[] = {
				"sampler_1d", "sampler_2d", "sampler_3d", "sampler_cube", "sampler_1d_shadow", "sampler_2d_shadow"
			};

			log_info("name: %s type: %s\n", buf, typeNames[psampler->type_]);

			psamplers->insert(std::make_pair(psampler->name_, psampler));

			continue;
		}
		if(type == GL_SAMPLER_2D_ARRAY)
		{
			glsl_sampler* psampler = new glsl_sampler;
			psampler->index_ = glGetUniformLocation(pprogram, buf);
			psampler->name_ = buf;
			psampler->type_ = SAMPLER_2D;  // synthetic; bind path is identical to 2D
			log_info("name: %s type: sampler_2d_array\n", buf);
			psamplers->insert(std::make_pair(psampler->name_, psampler));
			continue;
		}
		// OBJECTIDDEBUG-VIEWMODE-1 additive fix: integer / unsigned-integer
		// samplers (e.g. `usampler2D` for the GL_R32UI object-ID buffer) live at
		// GL_INT_SAMPLER_2D (0x8DCA) / GL_UNSIGNED_INT_SAMPLER_2D (0x8DD2),
		// OUTSIDE the float-sampler range handled above. They previously fell
		// through to the default constant-uniform path -> garbage type_ ->
		// constantSizes[]/typeNames[] OOB read -> crash in log_info (EXACTLY the
		// GL_SAMPLER_2D_ARRAY crash class documented above). Treat them as
		// samplers; the bind path (glActiveTexture+glBindTexture /
		// glProgramUniform1i via setInt) is type-agnostic.
		if(type == GL_INT_SAMPLER_2D || type == GL_UNSIGNED_INT_SAMPLER_2D)
		{
			glsl_sampler* psampler = new glsl_sampler;
			psampler->index_ = glGetUniformLocation(pprogram, buf);
			psampler->name_ = buf;
			psampler->type_ = SAMPLER_2D;  // synthetic; integer-sampler 2D bind path
			log_info("name: %s type: %s\n", buf,
			         (type == GL_INT_SAMPLER_2D) ? "isampler_2d" : "usampler_2d");
			psamplers->insert(std::make_pair(psampler->name_, psampler));
			continue;
		}

        glsl_uniform* puni = new glsl_uniform(); 
        puni->name_ = buf;
        puni->is_dirty_ = true;
        puni->index_ = glGetUniformLocation(pprogram, buf);

        switch(type)
        {
            case GL_FLOAT:
                puni->type_ = CONSTANT_FLOAT;
                puni->num_el_ = 1;
                break;
            case GL_INT:
            case GL_UNSIGNED_INT:  // VFX-FLIPBOOK-ASSET-TABLE-1: uint uniforms (e.g. u_atlasColumns)
                                   // Treated as CONSTANT_INT — same 4-byte constantSizes entry.
                                   // Bridge sets these via glUniform1ui/glUniform1i directly;
                                   // the cached type is not used for uint uniforms.
                puni->type_ = CONSTANT_INT;
                puni->num_el_ = 1;
                break;
            default:
            {
                // Map float-vector and matrix GL types to ConstantType by offset from
                // GL_FLOAT_VEC2. Validate bounds before indexing typeNames[].
                const ConstantType ct =
                    (ConstantType)(CONSTANT_VEC2 + (int)(type - GL_FLOAT_VEC2));
                if (ct < CONSTANT_VEC2 || ct > CONSTANT_MAT4) {
                    // Unknown type (e.g. uint arrays, double, image units).
                    // Bridge sets these via direct glUniform calls; skip caching.
                    log_info("name: %s type: unknown (GL type=0x%x, skipped)\n",
                             buf, (unsigned)type);
                    delete puni;
                    continue;
                }
                puni->type_ = ct;
                puni->num_el_ = size;
                break;
            }
        }


        size_t datasize = constantSizes[ puni->type_ ] * puni->num_el_;
        puni->data_ = new unsigned char[ datasize ];
        memset(puni->data_, 0, datasize);

        static const char *typeNames[] = {
            "float", "int  ", "vec2 ", "vec3 ", "vec4 ", "ivec2", "ivec3", "ivec4",
            "bool ", "bvec2", "bvec3", "bvec4", "mat2 ", "mat3 ", "mat4 "
        };
        log_info("name: %s type: %s  num_el: %d\n", buf, typeNames[puni->type_], puni->num_el_);

        puniforms->insert( std::make_pair(puni->name_, puni) );
    }

    delete[] buf;
}

void parse_uniform_blocks(GLuint pprogram, glsl_program::UniBlockArr_t* puniforms)
{
	GLint num_uni_blocks, max_name_len;
	glGetProgramiv(pprogram, GL_ACTIVE_UNIFORM_BLOCKS, &num_uni_blocks);
	glGetProgramiv(pprogram, GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH, &max_name_len);
	char* buf = new char[max_name_len + 1];
	GLsizei len;

	GLint binding;
	GLint data_size;
	GLint num_uniforms;
	for (GLint i = 0; i<num_uni_blocks; ++i)
	{
		glGetActiveUniformBlockName(pprogram, i, max_name_len + 1, &len, buf);
		glGetActiveUniformBlockiv(pprogram, i, GL_UNIFORM_BLOCK_BINDING, &binding);
		glGetActiveUniformBlockiv(pprogram, i, GL_UNIFORM_BLOCK_DATA_SIZE, &data_size);
		glGetActiveUniformBlockiv(pprogram, i, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &num_uniforms);

		// should be equal to i ?
		GLuint index = glGetUniformBlockIndex(pprogram, buf);
		
		// ???
		if (-1 == i) continue; // gl_ variable or does not correspond to an active uniform variable name in program

		glsl_uniform_block* uni_block = new glsl_uniform_block();
		uni_block->index_ = index;
		uni_block->binding_ = binding;
		uni_block->data_size_ = data_size;
		uni_block->is_dirty_ = true;
		uni_block->name_ = buf;
		uni_block->num_uniforms_ = num_uniforms;

		puniforms->insert(std::make_pair(buf, uni_block));
	}

	delete[] buf;
}

glsl_program* glsl_program::makeProgram2(const char* name, const char* vp, const char* hp, const char* dp, const char* gp, const char* fp, int count/* = 0*/, const char** xfb_variables/* = 0*/, const char* prefix/*=nullptr*/)
{
	ZoneScopedN("Shader.MakeProgram");
	if(!uniformFuncs[0])
        init_func_ptrs(uniformFuncs);

    assert(name);

	assert((count && xfb_variables) || 0==count);

    if(s_programs.count(name))
    {
        log_error("Program with this name (%s) already exists\n", name);
        return 0;
    }

    // SPIRV-CONSUMER-PILOT-BUILD-1: program-atomic pilot gate — either ALL
    // stages of the composite pilot load SPIR-V or none do (no mixed links).
    const bool spirvPilot = spirvCompositePilotProgram(vp, hp, dp, gp, fp, prefix);

    glsl_shader* vsh = 0, *hsh = 0, *dsh = 0, *gsh = 0, *fsh = 0;
    { ZoneScopedN("Shader.Stage.Vertex"); vsh = glsl_shader::makeShader(glsl_shader::VERTEX, vp, prefix, spirvPilot); }
    if(spirvPilot && vsh && fp)
    {
        ZoneScopedN("Shader.Stage.Fragment"); fsh = glsl_shader::makeShader(glsl_shader::FRAGMENT, fp, prefix, true);
    }
    // Atomic pilot fallback: if ANY pilot stage SPIR-V load failed, discard the
    // partial program and rebuild EVERY stage as GLSL (never a mixed SPIR-V/GLSL
    // program -> would fail to link). Composite pilot has only vert+frag.
    if(spirvPilot && (!vsh || (fp && !fsh)))
    {
        log_error("[SPIRV] pilot program %s: a stage failed SPIR-V; rebuilding all stages GLSL\n", name);
        if(vsh) glsl_shader::deleteShader(vsh);
        if(fsh) glsl_shader::deleteShader(fsh);
        vsh = fsh = 0;
    }
    if(!vsh)
    {
        { ZoneScopedN("Shader.Stage.Vertex"); vsh = glsl_shader::makeShader(glsl_shader::VERTEX, vp, prefix); }
        if(!vsh) return 0;
    }
    if(fp && !fsh)
    {
        { ZoneScopedN("Shader.Stage.Fragment"); fsh = glsl_shader::makeShader(glsl_shader::FRAGMENT, fp, prefix); }
        if(!fsh) return 0;
    }
    	
	if(hp)
	{
		{ ZoneScopedN("Shader.Stage.Hull"); hsh = glsl_shader::makeShader(glsl_shader::HULL, hp, prefix); }
		if(!hsh)
			return 0;
	}

	if(dp)
	{
		{ ZoneScopedN("Shader.Stage.Domain"); dsh = glsl_shader::makeShader(glsl_shader::DOMAINE, dp, prefix); }
		if(!dsh)
			return 0;
	}

	if(gp)
	{
		{ ZoneScopedN("Shader.Stage.Geometry"); gsh = glsl_shader::makeShader(glsl_shader::GEOMERTY, gp, prefix); }
		if(!gsh)
			return 0;
	}
			
	glsl_shader* pipeline[] = { vsh, hsh, dsh, gsh, fsh };

    GLuint shp = glCreateProgram();

	GLuint last_not_null = 0;
	{
	ZoneScopedN("Shader.AttachStages");
	for(size_t i=0; i< sizeof(pipeline)/sizeof(pipeline[0]); ++i)
	{
		if(!pipeline[i]) continue;
		last_not_null = pipeline[i]->shader_;

		glAttachShader(shp, pipeline[i]->shader_);
		if( GL_NO_ERROR != glGetError())
		{
	        glDeleteProgram(shp);
			log_error("glAttachShader: error during attaching %s\n", pipeline[i]->fname_.c_str());
			return 0;
		}
	}
	}

	GLenum err = glGetError();
	if(err != GL_NO_ERROR)
	{
		log_error("Shader name: %s\n", name);
		log_error("OpenGL Error: %s\n", ogl_get_error_code_str(err));
	}

	if(count)
	{
		glTransformFeedbackVaryings(last_not_null, count, xfb_variables, GL_INTERLEAVED_ATTRIBS);
	}

	err = glGetError();
	if(err != GL_NO_ERROR)
	{
		log_error("Shader name: %s\n", name);
		log_error("OpenGL Error: %s\n", ogl_get_error_code_str(err));
	}

    { ZoneScopedN("Shader.LinkProgram"); glLinkProgram(shp); }

	err = glGetError();
	if(err != GL_NO_ERROR)
	{
		log_error("Shader name: %s\n", name);
		log_error("OpenGL Error: %s\n", ogl_get_error_code_str(err));
	}

	CHECK_GL_ERROR
    std::string linkLog;
	if(get_program_error_status(shp, GL_LINK_STATUS, &linkLog))
    {
        if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
            std::ostringstream data;
            data << "{\"event\":\"link_fail\",\"program\":\""
                 << diag_json_escape(std::string(name)) << "\",\"stage\":\"link\",\"result\":\"fail\",\"info_log\":\""
                 << diag_json_escape(linkLog) << "\"}";
            mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
        }
        glDeleteProgram(shp);
        return 0;
    }
    // Log warnings from successful link (NVIDIA may emit them; AMD log empty).
    {
        GLsizei warnLen = 0;
        glGetProgramiv(shp, GL_INFO_LOG_LENGTH, &warnLen);
        if (warnLen > 1) {
            char* warnBuf = new char[warnLen];
            GLsizei actualLen = 0;
            glGetProgramInfoLog(shp, warnLen, &actualLen, warnBuf);
            if (actualLen > 0) {
                printf("[SHADER WARN] link '%s':\n%s\n", name, warnBuf);
                fflush(stdout);
            }
            delete[] warnBuf;
        }
    }

    if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
        std::ostringstream data;
        data << "{\"event\":\"link_ok\",\"program\":\""
             << diag_json_escape(std::string(name)) << "\",\"stage\":\"link\",\"result\":\"ok\"}";
        mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
    }

    glsl_program* pprogram = new glsl_program();
    pprogram->name_ = name ? name : "";
    pprogram->shp_ = shp;
    pprogram->vsh_ = vsh;
    pprogram->fsh_ = fsh;
    pprogram->hsh_ = hsh;
    pprogram->dsh_ = dsh;
    pprogram->gsh_ = gsh;
    if(prefix) {
        size_t size = strlen(prefix) + 1;
        pprogram->prefix_ = new char[size];
        memcpy(pprogram->prefix_, prefix, size);
    } else {
        pprogram->prefix_ = nullptr;
    }
    pprogram->is_valid_ = true;

	for(size_t i=0; i< sizeof(pipeline)/sizeof(pipeline[0]); ++i)
	{
		if(!pipeline[i]) continue;
		glDetachShader(shp, pipeline[i]->shader_);
	}

    { ZoneScopedN("Shader.ParseUniforms"); parse_uniforms(shp, &pprogram->uniforms_, &pprogram->samplers_); parse_uniform_blocks(shp, &pprogram->uniform_blocks_); }

    pprogram->last_load_time_ = timing::get_wall_time_ms();

    s_programs.insert(std::make_pair(name, pprogram) );
    return pprogram;

}

glsl_program* glsl_program::makeProgram(const char* name, const char* vp, const char* fp, const char* prefix /*= nullptr*/)
{
	return makeProgram2(name, vp, 0, 0, 0, fp, 0, nullptr, prefix);
}

void glsl_program::deleteProgram(const char* name)
{
    if(s_programs.count(name))
    {
        glsl_program* pprogram = s_programs[name];
        s_programs.erase(name);
        delete pprogram;
    }
}

glsl_program::~glsl_program()
{
    if(shp_)
    {
        // Shaders were detached after link in makeProgram2 / reload()
        // (standard GL pattern so shader objects can outlive the program).
        // Re-detaching here returns GL_INVALID_OPERATION and spams the
        // debug callback. glDeleteProgram handles cleanup of any shaders
        // still attached automatically.
        glDeleteProgram(shp_);
    }
}

void glsl_program::apply()
{
    ZoneScopedN("Shader.Apply");
    glUseProgram(shp_);

    UniArr_t::iterator it = uniforms_.begin(); 
    UniArr_t::iterator end = uniforms_.end(); 
    for(;it!=end;++it)
    {
        glsl_uniform* puni = it->second;
        if(puni->is_dirty_)
        {
            if (puni->type_ >= CONSTANT_MAT2){
                ((UNIFORM_MAT_FUNC) uniformFuncs[puni->type_])(puni->index_, puni->num_el_, GL_TRUE, (float *) puni->data_);
            } else {
                uniformFuncs[puni->type_](puni->index_, puni->num_el_, (float *) puni->data_);
                if(GL_INVALID_OPERATION == glGetError())
                    log_error("Error setting variable\n");
            }
            puni->is_dirty_ = false;
        }
    }
}

bool glsl_program::reload()
{
    ZoneScopedN("Shader.Reload");
    // DO NOT clear is_valid_ here.
    // The old shp_ remains live and usable until a new program fully compiles
    // and links. A failed reload must not mutate live render state — it should
    // leave terrain rendering the old shader rather than going blank.

    glsl_shader* const pipeline[] = { vsh_, hsh_, dsh_, gsh_, fsh_ };

    // Pass 1: compile each stage into a fresh temporary GL shader object.
    // We never touch the existing pipeline[i]->shader_ until the new program
    // successfully links (so glUseProgram(shp_) stays valid throughout).
    GLuint newShaders[5] = {};
    std::vector<std::string> newIncludes[5];
    bool compileOk = true;
    reload_log_.clear();
    for (size_t i = 0; i < 5; ++i) {
        if (!pipeline[i]) continue;
        std::string src;
        if (!load_shader(pipeline[i]->fname_.c_str(), src, newIncludes[i])) {
            log_error("Shader reload: failed to load %s\n", pipeline[i]->fname_.c_str());
            reload_log_ = "failed to load " + pipeline[i]->fname_;
            compileOk = false;
            break;
        }
        newShaders[i] = glCreateShader(pipeline[i]->type_);
        const char* strings[] = { prefix_ ? prefix_ : "", src.c_str() };
        std::string stageLog;
        if (!compile_shader(newShaders[i], strings, 2, &stageLog)) {
            reload_log_ = pipeline[i]->fname_ + ": "
                + (stageLog.empty() ? "compile failed (empty info log)" : stageLog);
            compileOk = false;
            break;
        }
    }
    if (!compileOk) {
        if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
            std::ostringstream data;
            data << "{\"event\":\"reload_compile_fail\",\"program\":\""
                 << diag_json_escape(name_) << "\",\"stage\":\"reload_compile\",\"result\":\"fail\"}";
            mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
        }
        for (int i = 0; i < 5; ++i) if (newShaders[i]) glDeleteShader(newShaders[i]);
        printf("[SHADER] reload failed (compile); keeping previous program\n");
        return false;
    }

    // Pass 2: create a new candidate program, attach, link.
    GLuint newProg = glCreateProgram();
    for (size_t i = 0; i < 5; ++i)
        if (newShaders[i]) glAttachShader(newProg, newShaders[i]);
    glLinkProgram(newProg);
    for (size_t i = 0; i < 5; ++i) {
        if (!newShaders[i]) continue;
        glDetachShader(newProg, newShaders[i]);
        glDeleteShader(newShaders[i]);
    }
    std::string reloadLinkLog;
    if (get_program_error_status(newProg, GL_LINK_STATUS, &reloadLinkLog)) {
        if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
            std::ostringstream data;
            data << "{\"event\":\"reload_link_fail\",\"program\":\""
                 << diag_json_escape(name_) << "\",\"stage\":\"reload_link\",\"result\":\"fail\",\"info_log\":\""
                 << diag_json_escape(reloadLinkLog) << "\"}";
            mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
        }
        glDeleteProgram(newProg);
        reload_log_ = "link: " + (reloadLinkLog.empty() ? std::string("failed (empty info log)") : reloadLinkLog);
        printf("[SHADER] reload failed (link); keeping previous program\n");
        return false;
    }

    // New program is fully valid. Swap in atomically.
    GLuint oldProg = shp_;
    shp_ = newProg;
    if (oldProg) glDeleteProgram(oldProg);

    // Update per-stage includes so needsReload() correctly tracks timestamps.
    for (size_t i = 0; i < 5; ++i)
        if (pipeline[i]) pipeline[i]->includes_ = newIncludes[i];

    // Rebuild the uniform cache for the new program.
    for (auto& kv : uniforms_) delete kv.second;
    uniforms_.clear();
    samplers_.clear();
    uniform_blocks_.clear();
    parse_uniforms(shp_, &uniforms_, &samplers_);
    parse_uniform_blocks(shp_, &uniform_blocks_);

    last_load_time_ = timing::get_wall_time_ms();
    is_valid_ = true;
    if (mc2_diag::tagEnabled("SHADER_COMPILE")) {
        std::ostringstream data;
        data << "{\"event\":\"reload_ok\",\"program\":\""
             << diag_json_escape(name_) << "\",\"stage\":\"reload\",\"result\":\"ok\"}";
        mc2_diag::writeEvent("SHADER_COMPILE", 1, 0, data.str().c_str());
    }
    return true;
}

bool glsl_program::is_valid()
{
    return is_valid_;
}

//=====================================================================================================================================
// Helper: uniforms that are declared in the shader but unused get optimized
// out by the GLSL compiler and never show up in the uniforms_ table. Callers
// often don't know which uniforms survived optimization, so a missing name
// is NOT a bug — only a found-but-wrong-type is worth logging.
bool glsl_program::setFloat(const char* name, const float v)
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false; // optimized out, silent
    if(it->second->type_ != CONSTANT_FLOAT)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, &v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setFloat2(const char* name, const float v[2])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_VEC2)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setFloat3(const char* name, const float v[3])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_VEC3)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}

bool glsl_program::setFloat4(const std::string& name, const float v[4])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_VEC4)
    {
        log_error("Type mismatch: %s\n", name.c_str());
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}

// TODO: need to change interface so that float* instead of float[4] is passed
bool glsl_program::setFloat4(const char* name, const float v[4])
{
	return setFloat4(std::string(name), v);
}

bool glsl_program::setInt(const char* name, const int v)
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it != uniforms_.end())
    {
        if(it->second->type_ != CONSTANT_INT)
        {
            log_error("Type mismatch: %s\n", name);
            return false;
        }
        memcpy(it->second->data_, &v, constantSizes[it->second->type_] * it->second->num_el_);
        it->second->is_dirty_ = true;
        return true;
    }
    // Samplers live in a separate table. Binding them is just an int
    // (texture unit), so setInt("samplerName", unit) is the natural caller API.
    // Use glProgramUniform1i (GL 4.1+) so we don't require this program to be
    // the active one — callers set samplers at init time before any draw.
    SamplerArr_t::iterator sit = samplers_.find(name);
    if(sit != samplers_.end())
    {
        glProgramUniform1i(shp_, sit->second->index_, v);
        return true;
    }
    // Not found in either table — uniform was optimized out; silent.
    return false;
}
bool glsl_program::setInt2(const char* name, const int v[2])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_IVEC2)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setInt3(const char* name, const int v[3])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_IVEC3)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setInt4(const char* name, const int v[4])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_IVEC4)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}

bool glsl_program::setMat2(const char* name, const float v[4])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_MAT2)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setMat3(const char* name, const float v[9])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_MAT3)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}
bool glsl_program::setMat4(const char* name, const float v[16])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_MAT4)
    {
        log_error("Type mismatch: %s\n", name);
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}

bool glsl_program::setMat4(const std::string& name , const float v[16])
{
    UniArr_t::iterator it = uniforms_.find(name);
    if(it == uniforms_.end())
        return false;
    if(it->second->type_ != CONSTANT_MAT4)
    {
        log_error("Type mismatch: %s\n", name.c_str());
        return false;
    }
    memcpy(it->second->data_, v, constantSizes[it->second->type_] * it->second->num_el_);
    it->second->is_dirty_ = true;
    return true;
}

GLint glsl_program::getAttribLocation(const char* pattrib)
{
	assert(pattrib);
	if(this->shp_)
		return glGetAttribLocation(this->shp_, pattrib);
	return -1;
}

uint64_t glsl_program::getModTimeMs()
{
    uint64_t least_recent_mt = 0;
    if(shp_)
    {
		glsl_shader* pipeline[] = { vsh_, hsh_, dsh_, gsh_, fsh_ };
		for(uint32_t i=0; i< sizeof(pipeline)/sizeof(pipeline[0]); ++i)
		{
			if(!pipeline[i]) continue;

			uint64_t mt = pipeline[i]->getModTimeMs();
            least_recent_mt = max(mt, least_recent_mt);
		}
	}

    return least_recent_mt;
}

bool glsl_program::needsReload()
{
    return last_load_time_ < getModTimeMs();
}

