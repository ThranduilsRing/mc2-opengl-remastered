#pragma once

#include <string>
#include <vector>
#include <map>
#include "utils/graphics.h"

typedef GLvoid (APIENTRY *UNIFORM_FUNC)(GLint location, GLsizei count, const void *value);
typedef GLvoid (APIENTRY *UNIFORM_MAT_FUNC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

// Recursive #include resolver used by glsl_shader::makeShader (the makeProgram
// path). Reads `fname`, splices `#include <...>` bodies and injects #line
// directives into `shader_source`; appends every resolved file (incl. nested)
// to `includes`. Returns false on read/parse failure. Exposed here so the
// GPU-driven compute-program builder can resolve includes the same way the
// rest of the engine does, instead of duplicating the substitution logic.
bool load_shader(const char* fname, std::string& shader_source, std::vector<std::string>& includes);

enum ConstantType {
    CONSTANT_FLOAT,
    CONSTANT_INT,

    CONSTANT_VEC2,
    CONSTANT_VEC3,
    CONSTANT_VEC4,
    CONSTANT_IVEC2,
    CONSTANT_IVEC3,
    CONSTANT_IVEC4,
    CONSTANT_BOOL,
    CONSTANT_BVEC2,
    CONSTANT_BVEC3,
    CONSTANT_BVEC4,
    CONSTANT_MAT2,
    CONSTANT_MAT3,
    CONSTANT_MAT4,
};

enum SamplerType {
	SAMPLER_1D,
	SAMPLER_2D,
	SAMPLER_3D,
	SAMPLER_CUBE,
	SAMPLER_1D_SHADOW,
	SAMPLER_2D_SHADOW
};

struct glsl_uniform
{
    ConstantType type_;
    int num_el_;
    std::string name_;
    GLint index_;
    unsigned char* data_;
    bool is_dirty_;
};

struct glsl_uniform_block
{
    std::string name_;
	GLuint index_;
    int num_uniforms_;
    GLint binding_;
	GLint data_size_;
    bool is_dirty_;
};

struct glsl_sampler
{
	SamplerType type_;
	std::string name_;
	GLint index_;
};

struct glsl_shader
{    

    enum Shader_t { VERTEX = 0, FRAGMENT = 1, HULL = 2, DOMAINE = 3, GEOMERTY = 4, NUM_SHADER_TYPES }; // E on purpose

    GLenum type_;
    std::string fname_;
    GLuint shader_;
    std::vector<std::string> includes_;

    static std::map<std::string, glsl_shader*> s_shaders[glsl_shader::NUM_SHADER_TYPES];

    bool reload(const char* prefix);
    // trySpirv (SPIRV-CONSUMER-PILOT-BUILD-1): when true AND the pilot gate
    // passes, load the offline-baked .spv for this stage instead of compiling
    // GLSL. Program-atomic: only makeProgram2 sets it true, and only for a
    // fully-baked pilot program, so a program never mixes SPIR-V + GLSL stages.
    // Default false => unchanged GLSL path for every other caller.
    static glsl_shader* makeShader(Shader_t type, const char* fname, const char* prefix = nullptr, bool trySpirv = false);
    static void deleteShader(glsl_shader* psh);

    // returns time of least-recently modified file on which this shader depends
    uint64_t getModTimeMs();

private:
    glsl_shader():type_(0), fname_(""), shader_(0), includes_()  {}
    ~glsl_shader();
};

struct glsl_program {

    GLuint shp_;
    glsl_shader* vsh_;
    glsl_shader* fsh_;
	glsl_shader* hsh_;
	glsl_shader* dsh_;
	glsl_shader* gsh_;
    char* prefix_;

    typedef std::map<std::string, glsl_uniform*> UniArr_t;
	typedef std::map<std::string, glsl_sampler*> SamplerArr_t;
    typedef std::map<std::string, glsl_uniform_block*> UniBlockArr_t;
    UniArr_t uniforms_;
	UniBlockArr_t uniform_blocks_;
	SamplerArr_t samplers_;

    bool reload();
    void apply();

    bool setFloat(const char* name, const float v);
    bool setFloat2(const char* name, const float v[2]);
    bool setFloat3(const char* name, const float v[3]);
    bool setFloat4(const char* name, const float v[4]);
	bool setFloat4(const std::string& name, const float v[4]);
	bool setInt(const char* name, const int v);
    bool setInt2(const char* name, const int v[2]);
    bool setInt3(const char* name, const int v[3]);
    bool setInt4(const char* name, const int v[4]);
    bool setMat2(const char* name, const float v[4]);
    bool setMat3(const char* name, const float v[9]);
    bool setMat4(const char* name, const float v[16]);
    bool setMat4(const std::string& name , const float v[16]);
   
    bool is_valid();

    static glsl_program* makeProgram(const char* name, const char* vsh, const char* fsh, const char* prefix = nullptr);
	static glsl_program* makeProgram2(const char* name, const char* vp, const char* hp, const char* dp, const char* gp, const char* fp, int count = 0, const char** xfb_variables = 0, const char* prefix = nullptr);
	// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: canonical define-set key from a
	// program prefix — ";"-joined sorted "NAME=VALUE" #defines. This is the SAME
	// canonicalization the SPIR-V variant consumer uses (spirvDefineKey), so the
	// logical pipeline-variant identity a caller records is path-independent
	// (GLSL vs SPIR-V both build from this prefix). Used by the pipeline-key path.
	static std::string shaderDefineKey(const char* prefix);
	static void deleteProgram(const char* name);
	GLint getAttribLocation(const char* pattrib);

    static std::map<std::string, glsl_program*> s_programs;

    uint64_t last_load_time_;
    // returns time of least-recently modified file on which this shader depends
    uint64_t getModTimeMs();
    bool needsReload();
    // DEV-SHELL-1: full compile/link error text of the last failed reload()
    // ("" after a successful one). The dev shell returns this to the client
    // so a failed hot-reload is never silent.
    std::string reload_log_;
	
private:
    glsl_program():shp_(0), vsh_(0), fsh_(0), hsh_(0), dsh_(0), gsh_(0), is_valid_(false) {};
    ~glsl_program();

    static UNIFORM_FUNC uniformFuncs[15];

    bool is_valid_;
    std::string name_;  // program name as passed to makeProgram2 — used by reload() for diagnostics
};

