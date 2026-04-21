#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <cstring>

#include "gameos.hpp"
#include "font3d.hpp"
#include "gos_font.h"

#ifdef LINUX_BUILD
#include <cstdarg>
#endif

#include "platform_stdlib.h"
#include "platform_str.h"

#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/Image.h"
#include "utils/vec.h"
#include "utils/string_utils.h"
#include "utils/timing.h"
#include "gos_render.h"
#include "gos_postprocess.h"
#include "gos_profiler.h"

class gosRenderer;
class gosFont;

static const DWORD INVALID_TEXTURE_ID = 0;

static gosRenderer* g_gos_renderer = NULL;

static bool debugEnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, int count) {
    if (!vertices || count <= 0)
        return false;

    for (int i = 0; i < count; ++i) {
        if ((vertices[i].frgb & 0x000000ffu) != 3u)
            return false;
    }

    return true;
}

gosRenderer* getGosRenderer() {
    return g_gos_renderer;
}

// Public helper for consumers outside this TU (e.g. gos_static_prop_batcher)
// that need to turn a gosTextureHandle into a raw GL texture name. Declared
// here so local code can reference it; defined at end of this file after
// the gosRenderer / gosTexture class bodies are in scope.
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

struct gosTextureInfo {
    int width_;
    int height_;
    gos_TextureFormat format_;
};

////////////////////////////////////////////////////////////////////////////////
class gosBuffer {
	friend class gosRenderer;
public:
	GLuint buffer_;
	int element_size_;
	uint32_t count_;
	gosBUFFER_TYPE type_;
	gosBUFFER_USAGE usage_;
};

GLenum getGLVertexAttribType(gosVERTEX_ATTRIB_TYPE type) {
	GLenum t = -1;
	switch (type)
	{
	case gosVERTEX_ATTRIB_TYPE::BYTE: return GL_BYTE;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
	case gosVERTEX_ATTRIB_TYPE::SHORT: return GL_SHORT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
	case gosVERTEX_ATTRIB_TYPE::INT: return GL_INT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_INT: return GL_UNSIGNED_INT;
	case gosVERTEX_ATTRIB_TYPE::FLOAT: return GL_FLOAT;
	default:
		gosASSERT(0 && "unknows vertex attrib type");
	}

	return t;
};

////////////////////////////////////////////////////////////////////////////////
class gosVertexDeclaration {
	friend class gosRenderer;

	gosVERTEX_FORMAT_RECORD* vf_;
	uint32_t count_;

	gosVertexDeclaration() :vf_(0), count_(0) {}
public:

	static gosVertexDeclaration* create(gosVERTEX_FORMAT_RECORD* vf, int count)
	{
		gosVertexDeclaration* vdecl = new gosVertexDeclaration();
		if (!vdecl)
			return nullptr;

		vdecl->vf_ = new gosVERTEX_FORMAT_RECORD[count];
		memcpy(vdecl->vf_, vf, count * sizeof(gosVERTEX_FORMAT_RECORD));
		vdecl->count_ = count;

		return vdecl;
	}

	static void destroy(gosVertexDeclaration* vdecl)
	{
		delete[] vdecl->vf_;
		vdecl->count_ = -1;
		vdecl->vf_ = nullptr;
		delete vdecl;
	}

	void apply() {

		for (uint32_t i = 0; i < count_; ++i) {

			gosVERTEX_FORMAT_RECORD* rec = vf_ + i;

			GLuint type = getGLVertexAttribType(rec->type);

			glEnableVertexAttribArray(rec->index);
			glVertexAttribPointer(rec->index, rec->num_components, type, rec->normalized ? GL_TRUE : GL_FALSE, rec->stride, BUFFER_OFFSET(rec->offset));
		}
	}

	void end() {

		for (uint32_t i = 0; i < count_; ++i) {
			gosVERTEX_FORMAT_RECORD* rec = vf_ + i;
			glDisableVertexAttribArray(rec->index);
		}
	}

};

class gosMaterialVariationHelper;
class gosMaterialVariation {
        friend class gosMaterialVariationHelper;
        char* defines_;
        char* unique_name_suffix_;

    public:
        gosMaterialVariation():defines_(nullptr), unique_name_suffix_(nullptr) {}
        const char* getDefinesString() const { return defines_; }
        const char* getUniqueSuffix() const { return unique_name_suffix_; }

        ~gosMaterialVariation()
        {
            delete[] defines_;
            delete[] unique_name_suffix_;
        }
};

class gosMaterialVariationHelper {
        std::vector<std::string> defines;
    public:

        void addDefine(const char* define)
        {
            defines.push_back(std::string(define));
        }

        void addDefines(const char** define)
        {
            gosASSERT(define);
            while(*define)
            {
                defines.push_back(std::string(*define));
                define++;
            }
        }

        void addDefines(const std::vector<std::string>& define)
        {
            defines.insert(defines.end(), define.begin(), define.end());
        }

        void getMaterialVariation(gosMaterialVariation& variation)
        {
            std::string defines_str = "#version 420\n";
            std::string unique_suffix_str = "#";
            for(auto d : defines)
            {
                defines_str.append("#define ");
                defines_str.append(d);
                defines_str.append(" = 1\n");

                unique_suffix_str.append(d);
                unique_suffix_str.append("#");
            }
            // Add MRT_ENABLED when normal buffer is attached (AMD requires
            // GBuffer1 to only be declared when the FBO actually has attachment 1)
            {
                gosPostProcess* pp = getGosPostProcess();
                if (pp && pp->getSceneNormalTexture()) {
                    defines_str.append("#define MRT_ENABLED 1\n");
                }
            }
            defines_str.append("\n");

            if(variation.defines_)
                delete[] variation.defines_;

            if(variation.unique_name_suffix_)
                delete[] variation.unique_name_suffix_;

            size_t size = defines_str.size() + 1;
            variation.defines_ = new char[size];
            memcpy(variation.defines_, defines_str.c_str(), size);
            variation.defines_[size-1]='\0';

            size = unique_suffix_str.size() + 1;
            variation.unique_name_suffix_ = new char[size];
            memcpy(variation.unique_name_suffix_, unique_suffix_str.c_str(), size);
            variation.unique_name_suffix_[size-1]='\0';
        }
};

enum class gosGLOBAL_SHADER_FLAGS : unsigned int
{
    ALPHA_TEST = 0,
    IS_OVERLAY = 1
};

#define SHADER_FLAG_INDEX_TO_MASK(x) (1 << ((uint32_t)x))
#define SHADER_FLAG_MASK_TO_INDEX(x) (ffs(x)-1) // use __popcnt etc for windows, and move to platform_*.h

static const char* const g_shader_flags[] = {
    "ALPHA_TEST",
    "IS_OVERLAY"
};


class gosRenderMaterial {

		static const std::string s_mvp;
		static const std::string s_fog_color;
    public:
        static gosRenderMaterial* load(const char* shader, const gosMaterialVariation& mvar) {
            gosASSERT(shader);
            gosRenderMaterial* pmat = new gosRenderMaterial();
            char vs[256];
            char ps[256];
            StringFormat(vs, 255, "shaders/%s.vert", shader);
            StringFormat(ps, 255, "shaders/%s.frag", shader);

            std::string sh_name = shader;
            if (mvar.getUniqueSuffix())
                sh_name.append(mvar.getUniqueSuffix());

            // Terrain gets TCS/TES for tessellation
            if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {
                char tcs[256], tes[256];
                StringFormat(tcs, 255, "shaders/%s.tesc", shader);
                StringFormat(tes, 255, "shaders/%s.tese", shader);
                printf("[TESS] Loading terrain shader with TCS=%s TES=%s\n", tcs, tes);
                pmat->program_ = glsl_program::makeProgram2(sh_name.c_str(),
                    vs, tcs, tes, nullptr, ps, 0, nullptr, mvar.getDefinesString());
            } else {
                pmat->program_ = glsl_program::makeProgram(sh_name.c_str(), vs, ps, mvar.getDefinesString());
            }

            if(!pmat->program_) {
                SPEW(("SHADERS", "Failed to create %s material\n", shader));
                printf("[TESS] FAILED to create shader: %s\n", shader);
                delete pmat;
                return NULL;
            }

            pmat->name_ = new char[strlen(shader) + 1];
            strcpy(pmat->name_, shader);

            pmat->onLoad();

            return pmat;
        }


        void onLoad() {
            gosASSERT(program_);

            pos_loc = program_->getAttribLocation("pos");
            color_loc = program_->getAttribLocation("color");
            spec_color_and_fog_loc = program_->getAttribLocation("fog");
            texcoord_loc = program_->getAttribLocation("texcoord");
        }

        static void destroy(gosRenderMaterial* pmat) {
            gosASSERT(pmat);
            if(pmat->program_) {
                glsl_program::deleteProgram(pmat->name_);
                pmat->program_ = 0;
            }

            delete[] pmat->name_;
            pmat->name_ = 0;
        }

        void checkReload()
        {
            if(program_) {
                if(program_->needsReload()) {
                    if(program_->reload())
                        onLoad();
                }
            }
        }

        void applyVertexDeclaration() {
            
            const int stride = sizeof(gos_VERTEX);
            
            // gos_VERTEX structure
	        //float x,y;
	        //float z;
	        //float rhw;
	        //DWORD argb;
	        //DWORD frgb;
	        //float u,v;	

            gosASSERT(pos_loc >= 0);
            glEnableVertexAttribArray(pos_loc);
            glVertexAttribPointer(pos_loc, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);

            if(color_loc != -1) {
                glEnableVertexAttribArray(color_loc);
                glVertexAttribPointer(color_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, 
                        BUFFER_OFFSET(4*sizeof(float)));
            }

            if(spec_color_and_fog_loc != -1) {
                glEnableVertexAttribArray(spec_color_and_fog_loc);
                glVertexAttribPointer(spec_color_and_fog_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                        BUFFER_OFFSET(4*sizeof(float) + sizeof(uint32_t)));
            }

            if(texcoord_loc != -1) {
                glEnableVertexAttribArray(texcoord_loc);
                glVertexAttribPointer(texcoord_loc, 2, GL_FLOAT, GL_FALSE, stride, 
                        BUFFER_OFFSET(4*sizeof(float) + 2*sizeof(uint32_t)));
            }
        }

        bool setSamplerUnit(const std::string& sampler_name, uint32_t unit) {
            gosASSERT(!sampler_name.empty());
            // TODO: may also check that current program is equal to our program
            if(program_->samplers_.count(sampler_name)) {
                glUniform1i(program_->samplers_[sampler_name]->index_, unit);
                return true;
            }
            return false;
        }

        bool setUniformBlock(const std::string& uniform_block_name, uint32_t unit) {
            gosASSERT(!uniform_block_name.empty());
            if(program_->uniform_blocks_.count(uniform_block_name)) {
				//glBindBufferBase(GL_UNIFORM_BUFFER, , unit);
				glUniformBlockBinding(program_->shp_, program_->uniform_blocks_[uniform_block_name]->index_, unit);
                return true;
            }
            return false;
        }

        bool setTransform(const mat4& m) {
            program_->setMat4(s_mvp, m);
            return true;
        }

		bool setFogColor(const vec4& fog_color) {
            program_->setFloat4(s_fog_color, fog_color);
            return true;
		}

        void apply() {
            gosASSERT(program_);
            program_->apply();
        }

        const char* getName() const { return name_; }

        // TODO: think how to not expose this
        glsl_program* getShader() { return program_; }

		void endVertexDeclaration() {

			glDisableVertexAttribArray(pos_loc);

			if (color_loc != -1) {
				glDisableVertexAttribArray(color_loc);
			}

			if (spec_color_and_fog_loc != -1) {
				glDisableVertexAttribArray(spec_color_and_fog_loc);
			}

			if (texcoord_loc != -1) {
				glDisableVertexAttribArray(texcoord_loc);
			}
		}

		void end() {
            glUseProgram(0);
        }

    private:
        gosRenderMaterial():
            program_(NULL)
            , name_(NULL)
            , pos_loc(-1)
            , color_loc(-1)
            , spec_color_and_fog_loc(-1)
            , texcoord_loc(-1)
        {
        }

        glsl_program* program_;
        char* name_;
        GLint pos_loc;
        GLint color_loc;
        GLint spec_color_and_fog_loc;
        GLint texcoord_loc;
};

const std::string gosRenderMaterial::s_mvp = std::string("mvp");
const std::string gosRenderMaterial::s_fog_color = std::string("fog_color");

class gosMesh {
    public:
        typedef WORD INDEX_TYPE;

        static gosMesh* makeMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity = 0) {
            GLuint vb = makeBuffer(GL_ARRAY_BUFFER, 0, sizeof(gos_VERTEX)*vertex_capacity, GL_DYNAMIC_DRAW);
            if(!vb)
                return NULL;

            GLuint ib = 0;
            if(index_capacity > 0) {
                ib = makeBuffer(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(INDEX_TYPE)*index_capacity, GL_DYNAMIC_DRAW);
                if(!ib)
                    return NULL;
            }

            gosMesh* mesh = new gosMesh(prim_type, vertex_capacity, index_capacity);
            mesh->vb_ = vb;
            mesh->ib_ = ib;
            mesh->pvertex_data_ = new gos_VERTEX[vertex_capacity];
            mesh->pindex_data_ = new INDEX_TYPE[index_capacity];
            return mesh;
        }

        static void destroy(gosMesh* pmesh) {

            gosASSERT(pmesh);

            delete[] pmesh->pvertex_data_;
            delete[] pmesh->pindex_data_;

            GLuint b[] = {pmesh->vb_, pmesh->ib_};
            glDeleteBuffers(sizeof(b)/sizeof(b[0]), b);
        }

        bool addVertices(gos_VERTEX* vertices, int count) {
            if(num_vertices_ + count <= vertex_capacity_) {
                memcpy(pvertex_data_ + num_vertices_, vertices, sizeof(gos_VERTEX)*count);
                num_vertices_ += count;
                return true;
            }
            return false;
        }

        bool addIndices(INDEX_TYPE* indices, int count) {
            if(num_indices_ + count <= index_capacity_) {
                memcpy(pindex_data_ + num_indices_, indices, sizeof(INDEX_TYPE)*count);
                num_indices_ += count;
                return true;
            }
            return false;
        }

        int getVertexCapacity() const { return vertex_capacity_; }
        int getIndexCapacity() const { return index_capacity_; }
        int getNumVertices() const { return num_vertices_; }
        int getNumIndices() const { return num_indices_; }
        const gos_VERTEX* getVertices() const { return pvertex_data_; }
        const WORD* getIndices() const { return pindex_data_; }

        int getIndexSizeBytes() const { return sizeof(INDEX_TYPE); }

        GLuint getVB() const { return vb_; }
        GLuint getIB() const { return ib_; }

        void uploadBuffers() {
            if (num_vertices_ > 0) {
                updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_,
                    num_vertices_ * sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);
            }
            if (num_indices_ > 0) {
                updateBuffer(ib_, GL_ELEMENT_ARRAY_BUFFER, pindex_data_,
                    num_indices_ * sizeof(INDEX_TYPE), GL_DYNAMIC_DRAW);
            }
        }

        void rewind() { num_vertices_ = 0; num_indices_ = 0; }

        void draw(gosRenderMaterial* material) const;
        void drawIndexed(gosRenderMaterial* material) const;

		static void drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, gosRenderMaterial* material);
		static void drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl);

		static const std::string s_tex1;

    private:

        gosMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity)
            : vertex_capacity_(vertex_capacity)
            , index_capacity_(index_capacity)
            , num_vertices_(0)
            , num_indices_(0)
            , pvertex_data_(NULL)    
            , pindex_data_(NULL)    
            , prim_type_(prim_type)
            , vb_(-1)  
            ,ib_(-1) 
         {
         }

        int vertex_capacity_;
        int index_capacity_;
        int num_vertices_;
        int num_indices_;
        gos_VERTEX* pvertex_data_;
        INDEX_TYPE* pindex_data_;
        gosPRIMITIVETYPE prim_type_;

        GLuint vb_;
        GLuint ib_;
};

const std::string gosMesh::s_tex1 = std::string("tex1");

void gosMesh::draw(gosRenderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW_ARB);

    material->apply();

    material->setSamplerUnit(s_tex1, 0);

	glBindBuffer(GL_ARRAY_BUFFER, vb_);
    CHECK_GL_ERROR;

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    glDrawArrays(pt, 0, num_vertices_);

    material->endVertexDeclaration();
    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);

}

void gosMesh::drawIndexed(gosRenderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);
    updateBuffer(ib_, GL_ELEMENT_ARRAY_BUFFER, pindex_data_, num_indices_*sizeof(INDEX_TYPE), GL_DYNAMIC_DRAW);

    material->apply();

    material->setSamplerUnit(s_tex1, 0);

	glBindBuffer(GL_ARRAY_BUFFER, vb_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_);
    CHECK_GL_ERROR;

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    glDrawElements(pt, num_indices_, getIndexSizeBytes()==2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    material->endVertexDeclaration();
    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}

void gosMesh::drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, gosRenderMaterial* material)
{
	gosASSERT(material);

	int index_size = ib->element_size_;
	gosASSERT(index_size == 2 || index_size == 4);

	if (ib->count_ == 0)
		return;

	material->apply();
	CHECK_GL_ERROR;

	material->setSamplerUnit(s_tex1, 0);
	CHECK_GL_ERROR;

	glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
	CHECK_GL_ERROR;

	vdecl->apply();
	CHECK_GL_ERROR;

	GLenum pt = GL_TRIANGLES;
	glDrawElements(pt, ib->count_, index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

	vdecl->end();
	material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}

void gosMesh::drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
	int index_size = ib->element_size_;
	gosASSERT(index_size == 2 || index_size == 4);

	if (ib->count_ == 0)
		return;

	CHECK_GL_ERROR;

	glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
	CHECK_GL_ERROR;

	vdecl->apply();
	CHECK_GL_ERROR;

	GLenum pt = GL_TRIANGLES;
	glDrawElements(pt, ib->count_, index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

	vdecl->end();

	//material->end();
	glUseProgram(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}



class gosTexture {
    public:
        gosTexture(gos_TextureFormat fmt, const char* fname, DWORD hints, BYTE* pdata, DWORD size, bool from_memory)
        {

	        //if(fmt == gos_Texture_Detect || /*fmt == gos_Texture_Keyed ||*/ fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(fname) {
                filename_ = new char[strlen(fname)+1];
                strcpy(filename_, fname);
            } else {
                filename_ = 0;
            }
            texname_ = NULL;

            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            if(size) {
                size_ = size;
                pcompdata_ = new BYTE[size];
                memcpy(pcompdata_, pdata, size);
            }

            is_locked_ = false;
            is_from_memory_ = from_memory;
        }

        gosTexture(gos_TextureFormat fmt, DWORD hints, DWORD w, DWORD h, const char* texname)
        {
	        //if(fmt == gos_Texture_Detect /*|| fmt == gos_Texture_Keyed*/ || fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(texname) {
                texname_ = new char[strlen(texname)+1];
                strcpy(texname_, texname);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            tex_.w = w;
            tex_.h = h;

            is_locked_ = false;
            is_from_memory_ = true;
        }

        bool createHardwareTexture();

        ~gosTexture() {

            //SPEW(("Destroying texture: %s\n", filename_));

            gosASSERT(is_locked_ == false);

            if(pcompdata_)
                delete[] pcompdata_;
            if(filename_)
                delete[] filename_;
            if(texname_)
                delete[] texname_;

            destroyTexture(&tex_);
        }

        uint32_t getTextureId() const { return tex_.id; }
        TexType getTextureType() const { return tex_.type_; }

        BYTE* Lock(int mipl_level, bool is_read_only, int* pitch) {
            gosASSERT(is_locked_ == false);
            is_locked_ = true;
            // TODO:
            gosASSERT(pitch);
            *pitch = tex_.w;

            gosASSERT(!plocked_area_);
#if 0 
            glBindTexture(GL_TEXTURE_2D, tex_.id);
            GLint pack_row_length;
            GLint pack_alignment;
            glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
            // always return rgba8 formatted data
            lock_type_read_only_ = is_read_only;
            const uint32_t ts = tex_.w*tex_.h * getTexFormatPixelSize(TF_RGBA8);
            plocked_area_ = new BYTE[ts];
            getTextureData(tex_, 0, plocked_area_, TF_RGBA8);
            for(int y=0;y<tex_.h;++y) {
                for(int x=0;x<tex_.w;++x) {
                    DWORD rgba = ((DWORD*)plocked_area_)[tex_.w*y + x];
                    DWORD r = rgba&0xff;
                    DWORD g = (rgba&0xff00)>>8;
                    DWORD b = (rgba&0xff0000)>>16;
                    DWORD a = (rgba&0xff000000)>>24;
                    DWORD bgra = (a<<24) | (r<<16) | (g<<8) | b;
                    ((DWORD*)plocked_area_)[tex_.w*y + x] = bgra;
                }
            }
            return plocked_area_;
        }

        void Unlock() {
            gosASSERT(is_locked_ == true);
        
            if(!lock_type_read_only_) {
                for(int y=0;y<tex_.h;++y) {
                    for(int x=0;x<tex_.w;++x) {
                        DWORD bgra = ((DWORD*)plocked_area_)[tex_.w*y + x];
                        DWORD b = bgra&0xff;
                        DWORD g = (bgra&0xff00)>>8;
                        DWORD r = (bgra&0xff0000)>>16;
                        DWORD a = (bgra&0xff000000)>>24;
                        DWORD argb = (a<<24) | (b<<16) | (g<<8) | r;
                        ((DWORD*)plocked_area_)[tex_.w*y + x] = argb;
                    }
                }
                updateTexture(tex_, plocked_area_, TF_RGBA8);
            }

            delete[] plocked_area_;
            plocked_area_ = NULL;

            is_locked_ = false;
        }

        void getTextureInfo(gosTextureInfo* texinfo) const {
            gosASSERT(texinfo);
            texinfo->width_ = tex_.w;
            texinfo->height_ = tex_.h;
            texinfo->format_ = format_;
        }

    private:
        BYTE* pcompdata_;
        BYTE* plocked_area_;
        DWORD size_;
        Texture tex_;

        gos_TextureFormat format_;
        char* filename_;
        char* texname_;
        DWORD hints_;

        bool is_locked_;
        bool lock_type_read_only_;
        bool is_from_memory_; // not loaded from file
};

struct gosTextAttribs {
    HGOSFONT3D FontHandle;
    DWORD Foreground;
    float Size;
    bool WordWrap;
    bool Proportional;
    bool Bold;
    bool Italic;
    DWORD WrapType;
    bool DisableEmbeddedCodes;
};

static void makeKindaSolid(Image& img) {
    // have to do this, otherwise texutre with zero alpha could be drawn with alpha blend enabled, evel though logically aplha blend should not be enabled!
    // (happens when drawing terrain, see TerrainQuad::draw() case when no detail and no owerlay bu t isCement is true)
    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            pixels[y*img.getWidth() + x] = pix | 0xff000000;
        }
    }
}

static bool doesLookLikeAlpha(const Image& img) {
    gosASSERT(img.getFormat() == FORMAT_RGBA8);

    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            if((0xFF000000 & pix) != 0xFF000000)
                return true;
        }
    }
    return false;
}

static gos_TextureFormat convertIfNecessary(Image& img, gos_TextureFormat gos_format) {

    const bool has_alpha_channel = FORMAT_RGBA8 == img.getFormat();

    if(gos_format == gos_Texture_Detect) {
        bool has_alpha = has_alpha_channel ? doesLookLikeAlpha(img) : false;
        gos_format = has_alpha ? gos_Texture_Alpha : gos_Texture_Solid;
    }

    if(gos_format == gos_Texture_Solid && has_alpha_channel)
        makeKindaSolid(img);

    return gos_format;
}

bool gosTexture::createHardwareTexture() {

    // Opt-in to mipmaps via gosHint_MipmapFilter0. MC2's original convention
    // was "absence of DisableMipmap means mipmaps on," but in this port many
    // HUD/GUI/tacmap loads pass hints=0 without DisableMipmap and must stay
    // non-mipmapped for pixel-perfect sampling. We use MipmapFilter0 as a
    // positive opt-in instead -- no existing code sets this bit, so only
    // explicitly-updated load sites enable mipmaps. DisableMipmap wins if
    // both are set (defensive).
    const bool wantMipmaps = (hints_ & gosHint_MipmapFilter0) != 0
                          && (hints_ & gosHint_DisableMipmap) == 0;

    if(!is_from_memory_) {

        gosASSERT(filename_);

        Image img;
        if(!img.loadFromFile(filename_)) {
            SPEW(("DBG", "failed to load texture from file: %s\n", filename_));
            return false;
        }

        // check for only those formats, because lock.unlock may incorrectly work with different channes size (e.g. 16 or 32bit or floats)
        FORMAT img_fmt = img.getFormat();
        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        return tex_.isValid();

    } else if(pcompdata_ && size_ > 0) {

        // TODO: this is texture from memory, so maybe do not load it from file eh?

        Image img;
        if(!img.loadTGA(pcompdata_, size_)) {
            SPEW(("DBG", "failed to load texture from data, filename: %s, texname: %s\n", filename_? filename_ : "NO FILENAME", texname_?texname_:"NO TEXNAME"));
            return false;
        }

        FORMAT img_fmt = img.getFormat();

        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        return tex_.isValid();
    } else {
        gosASSERT(tex_.w >0 && tex_.h > 0);

        TexFormat tf = TF_RGBA8; // TODO: check format_ and do appropriate stuff
        DWORD* pdata = new DWORD[tex_.w*tex_.h];
        for(int i=0;i<tex_.w*tex_.h;++i)
            pdata[i] = 0xFF00FFFF;
        tex_ = create2DTexture(tex_.w, tex_.h, tf, (const uint8_t*)pdata, wantMipmaps);
        delete[] pdata;
        return tex_.isValid();
    }

}

static gosTexture* lookupBatchTextureOrWarn(const std::vector<gosTexture*>& textureList,
                                            DWORD textureId,
                                            const char* batchName) {
    if (textureId == INVALID_TEXTURE_ID)
        return nullptr;

    if (textureId >= textureList.size() || textureList[textureId] == nullptr) {
        static unsigned int warnCount = 0;
        if (warnCount < 16) {
            printf("%s: dropping invalid texture handle %u (texture count=%zu)\n",
                   batchName, textureId, textureList.size());
            ++warnCount;
        }
        return nullptr;
    }

    return textureList[textureId];
}

////////////////////////////////////////////////////////////////////////////////
class gosFont {
        friend class gosRenderer;
    public:
        static gosFont* load(const char* fontFile);

        int getMaxCharWidth() const { return gi_.max_advance_; }
        int getMaxCharHeight() const { return gi_.font_line_skip_; }
        int getFontAscent() const { return gi_.font_ascent_; }

        int getCharWidth(int c) const;
        void getCharUV(int c, uint32_t* u, uint32_t* v) const;
        int getCharAdvance(int c) const;
        const gosGlyphMetrics& getGlyphMetrics(int c) const;


        DWORD getTextureId() const { return tex_id_; }
        const char* getName() const { return font_name_; }
        const char* getId() const { return font_id_; }

        uint32_t getRefCount() { return ref_count_; }
        uint32_t addRef() { return ++ref_count_; }
        uint32_t decRef() { gosASSERT(ref_count_>0); return --ref_count_; }

    private:
        static uint32_t destroy(gosFont* font);
        gosFont():font_name_(0), font_id_(0), tex_id_(0), ref_count_(1) {};
        ~gosFont();

        char* font_name_;
        char* font_id_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
        uint32_t ref_count_;
};


enum HudDrawKind { kHudQuadBatch, kHudLineBatch, kHudTriBatch, kHudTextQuadBatch };

struct HudDrawCall {
    HudDrawKind              kind;
    std::vector<gos_VERTEX>  vertices;
    uint32_t                 stateSnapshot[gos_MaxState];
    mat4                     projection;
    DWORD                    fontTexId;       // kHudTextQuadBatch only
    DWORD                    foregroundColor; // kHudTextQuadBatch only
};

////////////////////////////////////////////////////////////////////////////////
class gosRenderer {

    friend class gosShapeRenderer;

    typedef uint32_t RenderState[gos_MaxState];
	static const std::string s_Foreground;

    public:
        gosRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
            width_ = w;
            height_ = h;
            ctx_h_ = ctx_h;
            win_h_ = win_h;
            hudFlushed_ = false;
        }

        uint32_t addTexture(gosTexture* texture) {
            gosASSERT(texture);
            textureList_.push_back(texture);
            return (uint32_t)(textureList_.size()-1);
        }

        uint32_t addFont(gosFont* font) {
            gosASSERT(font);
            fontList_.push_back(font);
            return (uint32_t)(fontList_.size()-1);
        }

		uint32_t addBuffer(gosBuffer* buffer) {
			gosASSERT(buffer);
			bufferList_.push_back(buffer);
			return (uint32_t)(bufferList_.size() - 1);
		}

		uint32_t addVertexDeclaration(gosVertexDeclaration* vdecl) {
			gosASSERT(vdecl);
			vertexDeclarationList_.push_back(vdecl);
			return (uint32_t)(vertexDeclarationList_.size() - 1);
		}

        // TODO: do same as with texture?
        void deleteFont(gosFont* font) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            
            struct equals_to {
                gosFont* fnt_;
                bool operator()(gosFont* fnt) {
                    return fnt == fnt_;
                }
            };

            equals_to eq;
            eq.fnt_ = font;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
            {
                gosFont* font = *it;
                if(0 == gosFont::destroy(font))
                    fontList_.erase(it);
            }
        }

		bool deleteBuffer(gosBuffer* buffer) {
			std::vector<gosBuffer*>::iterator it = std::find(bufferList_.begin(), bufferList_.end(), buffer);
			if (it != bufferList_.end())
			{
				bufferList_.erase(it);
				return true;
			}
			return false;
		}

		bool deleteVertexDeclaration(gosVertexDeclaration* vdecl) {
			std::vector<gosVertexDeclaration*>::iterator it = std::find(vertexDeclarationList_.begin(), vertexDeclarationList_.end(), vdecl);
			if (it != vertexDeclarationList_.end())
			{
				vertexDeclarationList_.erase(it);
				return true;
			}
			return false;
		}

        gosFont* findFont(const char* font_id) {
            
            struct equals_to {
                const char* font_id_;
                bool operator()(const gosFont* fnt) {
                    return strcmp(fnt->getId(), font_id_)==0;
                }
            };

            equals_to eq;
            eq.font_id_ = font_id;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
                return *it;
            return NULL;
        }

        gosTexture* getTexture(DWORD texture_id) {
            // TODO: return default texture
            if(texture_id == INVALID_TEXTURE_ID) {
                gosASSERT(0 && "Should not be requested");
                return NULL;
            }
            gosASSERT(textureList_.size() > texture_id);
            gosASSERT(textureList_[texture_id] != 0);
            return textureList_[texture_id];
        }

        // Bound-check helper for gos_GetGLTextureId. Avoids the hard
        // assert in getTexture() for bogus handle values seen on
        // TG_TinyTexture entries that were never loaded (e.g. 0xFFFFFFFF).
        size_t getTextureListSize() const { return textureList_.size(); }

        void deleteTexture(DWORD texture_id) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            gosASSERT(textureList_.size() > texture_id);
            delete textureList_[texture_id];
            textureList_[texture_id] = 0;
        }

        uint32_t getFlagsFromStates()
        {
            return curStates_[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
        }

        gosRenderMaterial* getRenderMaterial(const char* name) {

            uint32_t flags = getFlagsFromStates();
            return materialDB_[name][flags];
        }

        gosTextAttribs& getTextAttributes() { return curTextAttribs_; }
        void setTextPos(int x, int y) { curTextPosX_ = x; curTextPosY_ = y; }
        void getTextPos(int& x, int& y) { x = curTextPosX_; y = curTextPosY_; }
        void setTextRegion(int Left, int Top, int Right, int Bottom) {
            curTextLeft_ = Left;
            curTextTop_ = Top;
            curTextRight_ = Right;
            curTextBottom_ = Bottom;
        }

        int getTextRegionWidth() { return curTextRight_ - curTextLeft_; }
        int getTextRegionHeight() { return curTextBottom_ - curTextTop_; }

        void setupViewport(bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil = 0, DWORD StencilValue = 0) {

            clearDepth_ = FillZ;
            clearDepthValue_ = ZBuffer;
            clearColor_ = FillBG;
            clearColorValue_ = BGColor;
            clearStencil_ = ClearStencil;
            clearStencilValue_ = StencilValue;
            viewportTop_ = top;
            viewportLeft_ = left;
            viewportBottom_ = bottom;
            viewportRight_ = right;
        }

        void getViewportTransform(float* viewMulX, float* viewMulY, float* viewAddX, float* viewAddY) {
            gosASSERT(viewMulX && viewMulY && viewAddX && viewAddY);
            *viewMulX = (viewportRight_ - viewportLeft_)*width_;
            *viewMulY = (viewportBottom_ - viewportTop_)*height_;
            *viewAddX = viewportLeft_ * width_;
            *viewAddY = viewportTop_ * height_;
        }

		void setRenderViewport(const vec4& vp) { render_viewport_ = vp; }
		vec4 getRenderViewport() { return render_viewport_; }

		const mat4& getProj2Screen() { return projection_; }
		int getWidth()  const { return width_; }
		int getHeight() const { return height_; }
        const vec4& getFogColor() const { return fog_color_; }

        void setRenderState(gos_RenderState RenderState, int Value) {
            renderStates_[RenderState] = Value;
        }

        int getRenderState(gos_RenderState RenderState) const {
            return renderStates_[RenderState];
        }

        void setScreenMode(DWORD width, DWORD height, DWORD bit_depth, bool GotoFullScreen, bool anti_alias) {
            reqWidth = width;
            reqHeight = height;
            reqBitDepth = bit_depth;
            reqAntiAlias = anti_alias;
            reqGotoFullscreen = GotoFullScreen;
            pendingRequest = true;
        }

        void pushRenderStates();
        void popRenderStates();

        void applyRenderStates();

        void drawQuads(gos_VERTEX* vertices, int count);
        void drawLines(gos_VERTEX* vertices, int count);
        void flushHUDBatch();
        void replayTextQuads(const HudDrawCall& call);
        void drawPoints(gos_VERTEX* vertices, int count);
        void drawTris(gos_VERTEX* vertices, int count);
        void drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl);
        void drawText(const char* text);
        void terrainDrawIndexedPatches(gosRenderMaterial* material, gosMesh* mesh);
        void drawGrassPass(gosMesh* mesh);

        void beginFrame();
        void endFrame();

        void init();
        void destroy();
        void flush();

        // debug interface
        void setNumDrawCallsToDraw(uint32_t num) { num_draw_calls_to_draw_ = num; }
        uint32_t getNumDrawCallsToDraw() { return num_draw_calls_to_draw_; }
        void setBreakOnDrawCall(bool b_break) { break_on_draw_call_ = b_break; }
        bool getBreakOnDrawCall() { return break_on_draw_call_; }
        void setBreakDrawCall(uint32_t num) { break_draw_call_num_ = num; }

        graphics::RenderContextHandle getRenderContextHandle() { return ctx_h_; }

        uint32_t getRenderState(gos_RenderState render_state) { return curStates_[render_state]; }

        gosRenderMaterial* selectBasicRenderMaterial(const RenderState& rs) const ;
        gosRenderMaterial* selectLightedRenderMaterial(const RenderState& rs) const ;

		void handleEvents();

        // Terrain tessellation
        void setTerrainTessParams(float level, float near_dist, float far_dist) {
            terrain_tess_level_ = level;
            terrain_tess_dist_near_ = near_dist;
            terrain_tess_dist_far_ = far_dist;
        }
        void setTerrainPhongAlpha(float a) { terrain_phong_alpha_ = a; }
        void setTerrainDisplaceScale(float s) { terrain_displace_scale_ = s; }
        void setTerrainWireframe(bool w) { terrain_wireframe_ = w; }
        void setTerrainDebugMode(float mode) {
            terrain_debug_mode_ = mode;
        }
        void setTerrainMVP(const float* m) {
            memcpy(&terrain_mvp_, m, 16 * sizeof(float));
            terrain_mvp_valid_ = true;
            // Compute inverse VP for post-process depth reconstruction
            mat4 invVP = inverseMat4(terrain_mvp_);
            gosPostProcess* pp = getGosPostProcess();
            if (pp) {
                pp->setInverseViewProj((const float*)&invVP);
                pp->setViewProj(m);
            }
        }
        void setTerrainCameraPos(float x, float y, float z) {
            terrain_camera_pos_ = vec4(x, y, z, 1.0f);
        }
        void setTerrainViewport(float vmx, float vmy, float vax, float vay) {
            terrain_viewport_ = vec4(vmx, vmy, vax, vay);
        }

        void terrainExtraReset() { terrain_extra_count_ = 0; terrain_extra_draw_offset_ = 0; terrain_batch_extras_ = nullptr; terrain_batch_extras_count_ = 0; }
        void terrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
            if (terrain_extra_count_ + count <= terrain_extra_capacity_) {
                memcpy(terrain_extra_data_ + terrain_extra_count_, data, sizeof(gos_TERRAIN_EXTRA) * count);
                terrain_extra_count_ += count;
            }
        }
        void setTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
            terrain_batch_extras_ = extras;
            terrain_batch_extras_count_ = count;
        }
        int getTerrainExtraCount() const { return terrain_extra_count_; }
        float getTerrainTessLevel() const { return terrain_tess_level_; }
        float getTerrainTessDistNear() const { return terrain_tess_dist_near_; }
        float getTerrainTessDistFar() const { return terrain_tess_dist_far_; }
        float getTerrainPhongAlpha() const { return terrain_phong_alpha_; }
        float getTerrainDisplaceScale() const { return terrain_displace_scale_; }
        float getTerrainDetailTiling() const { return terrain_detail_tiling_; }
        bool getTerrainWireframe() const { return terrain_wireframe_; }
        GLuint getTerrainExtraVB() const { return terrain_extra_vb_; }
        gos_TERRAIN_EXTRA* getTerrainExtraData() const { return terrain_extra_data_; }
        bool isTerrainMVPValid() const { return terrain_mvp_valid_; }
        const mat4& getTerrainMVP() const { return terrain_mvp_; }
        const vec4& getTerrainViewport() const { return terrain_viewport_; }
        gosRenderMaterial* getTerrainMaterial() const { return terrain_material_; }
        const vec4& getTerrainCameraPos() const { return terrain_camera_pos_; }

        // Shadow mode
        void setShadowMode(bool enabled) { shadow_mode_ = enabled; }
        bool getShadowMode() const { return shadow_mode_; }
        gosRenderMaterial* getShadowTerrainMaterial() const { return shadow_terrain_material_; }

        // Shadow softness (penumbra radius in texels)
        void setTerrainShadowSoftness(float s) { terrain_shadow_softness_ = s; }
        float getTerrainShadowSoftness() const { return terrain_shadow_softness_; }

        // Terrain draw killswitch
        void setTerrainDrawEnabled(bool e) { terrain_draw_enabled_ = e; }
        bool getTerrainDrawEnabled() const { return terrain_draw_enabled_; }
        float getTerrainDebugMode() const { return terrain_debug_mode_; }

        // Shadow pre-pass (separate pass over all terrain batches before shading)
        void beginShadowPrePass(bool clearDepth = true);
        void drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
            WORD* indices, int numIndices,
            const gos_TERRAIN_EXTRA* extras, int extraCount);
        void drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
            HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4);
        void endShadowPrePass();

        // Dynamic object shadow pass (camera-centered, per-frame)
        void beginDynamicShadowPass();
        void endDynamicShadowPass();

        // Terrain splatting setters
        void setTerrainLightDir(float x, float y, float z) { terrain_light_dir_ = vec4(x, y, z, 0.0f); }
        const vec4& getTerrainLightDir() const { return terrain_light_dir_; }
        void setTerrainDetailParams(float tiling, float strength) { terrain_detail_tiling_ = tiling; terrain_detail_strength_ = strength; }
        void setTerrainMaterialNormal(int idx, GLuint texId) { if (idx >= 0 && idx < 5) terrain_mat_normal_[idx] = texId; }
        void setTerrainCellBombParams(float s, float j, float r) { terrain_cell_scale_ = s; terrain_cell_jitter_ = j; terrain_cell_rotation_ = r; }
        void setTerrainPOMParams(float scale, float, float) { terrain_pom_scale_ = scale; }
        void setTerrainWorldScale(float scale) { terrain_world_scale_ = scale; }

        // World-space overlay batch API (thin public surface — internals are private)
        void pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void drawTerrainOverlays();
        void drawDecals();

    private:

        bool beforeDrawCall();
        void afterDrawCall();

        // render target size
        int width_;
        int height_;
        graphics::RenderContextHandle ctx_h_;
        graphics::RenderWindowHandle win_h_;

        // fits vertices into viewport
        mat4 projection_;

		vec4 fog_color_;

        void initRenderStates();

        std::vector<gosTexture*> textureList_;
        std::vector<gosFont*> fontList_;
        std::vector<gosBuffer*> bufferList_;
        std::vector<gosVertexDeclaration*> vertexDeclarationList_;
        std::vector<gosRenderMaterial*> materialList_;
		typedef std::map<uint32_t, gosRenderMaterial*> MaterialDBValue_t;
		typedef std::map<std::string, MaterialDBValue_t> MaterialDB_t;
        MaterialDB_t materialDB_;

        DWORD reqWidth;
        DWORD reqHeight;
        DWORD reqBitDepth;
        DWORD reqAntiAlias;
        bool reqGotoFullscreen;
        bool pendingRequest;

        // states data
        RenderState curStates_;
        RenderState renderStates_;

        static const int RENDER_STATES_STACK_SIZE = 16;
        int renderStatesStackPointer;
        RenderState statesStack_[RENDER_STATES_STACK_SIZE];
        //

        // text data
        gosTextAttribs curTextAttribs_;

        int curTextPosX_;
        int curTextPosY_;

        int curTextLeft_;
        int curTextTop_;
        int curTextRight_;
        int curTextBottom_;
        //
        
        // viewport config
        bool clearDepth_;
        float clearDepthValue_;
        bool clearColor_;
        DWORD clearColorValue_;
        bool clearStencil_;
        DWORD clearStencilValue_;
        float viewportTop_;
        float viewportLeft_;
        float viewportBottom_;
        float viewportRight_;
        //

		vec4 render_viewport_;
        
        gosMesh* quads_;
        gosMesh* tris_;
        gosMesh* indexed_tris_;
        gosMesh* lines_;
        gosMesh* points_;
        gosMesh* text_;
        gosRenderMaterial* basic_material_;
        gosRenderMaterial* basic_tex_material_;
        gosRenderMaterial* text_material_;

        gosRenderMaterial* basic_lighted_material_;
        gosRenderMaterial* basic_tex_lighted_material_;
        //
        uint32_t num_draw_calls_;
        uint32_t num_draw_calls_to_draw_;
        bool break_on_draw_call_;
        uint32_t break_draw_call_num_;

        // HUD command buffer
        std::vector<HudDrawCall> hudBatch_;
        bool                     hudFlushed_;

        // Tessellation
        float terrain_tess_level_ = 4.0f;          // base inner/outer tessellation factor
        float terrain_tess_dist_near_ = 200.0f;    // full tess below this distance
        float terrain_tess_dist_far_ = 2000.0f;    // tess=1 beyond this distance
        float terrain_phong_alpha_ = 0.5f;         // Phong smoothing strength
        float terrain_displace_scale_ = 2.0f;      // displacement amplitude (dirt-only in TES)
        bool terrain_wireframe_ = false;            // wireframe overlay toggle
        float terrain_debug_mode_ = 0.0f;          // 0=off, 1=normals, 2=worldPos

        // Terrain extra VBO for world pos + normal
        GLuint terrain_extra_vb_ = 0;
        gos_TERRAIN_EXTRA* terrain_extra_data_ = nullptr;
        int terrain_extra_count_ = 0;
        int terrain_extra_capacity_ = 0;
        int terrain_extra_draw_offset_ = 0;  // consumed offset for per-batch alignment (legacy)

        // Per-batch extras from texture manager (replaces global offset)
        const gos_TERRAIN_EXTRA* terrain_batch_extras_ = nullptr;
        int terrain_batch_extras_count_ = 0;

        // Terrain tessellation MVP (world-to-NDC)
        mat4 terrain_mvp_;
        bool terrain_mvp_valid_ = false;
        vec4 terrain_camera_pos_;  // MC2 world space camera position for TCS LOD
        vec4 terrain_viewport_;    // (vmx, vmy, vax, vay) for TES perspective project

        // Terrain tessellation material
        gosRenderMaterial* terrain_material_ = nullptr;

        // Shadow mode
        gosRenderMaterial* shadow_terrain_material_ = nullptr;
        gosRenderMaterial* shadow_object_material_ = nullptr;
        bool shadow_mode_ = false;
        bool shadow_prepass_active_ = false;
        float terrain_shadow_softness_ = 0.9f;  // tuned for gradient-adaptive PCF + 4096² maps
        bool terrain_draw_enabled_ = true;
        GLint shadow_prepass_prev_fbo_ = 0;
        GLint shadow_prepass_prev_viewport_[4] = {0};
        const float* active_light_space_matrix_ = nullptr;  // routes static or dynamic LSM

        // Terrain splatting textures (from main source, needed for material normal maps)
        vec4 terrain_light_dir_;
        float terrain_detail_tiling_ = 1.0f;
        float terrain_detail_strength_ = 4.0f;
        GLuint terrain_mat_normal_[5] = {0, 0, 0, 0, 0};
        float terrain_cell_scale_ = 8.0f;
        float terrain_cell_jitter_ = 0.8f;
        float terrain_cell_rotation_ = 1.0f;
        float terrain_pom_scale_ = 0.02f;
        float terrain_world_scale_ = 15360.0f;

        // Cached uniform locations for terrain shader (avoid per-draw glGetUniformLocation)
        struct TerrainUniformLocs {
            GLint tessLevel = -1, tessDistanceRange = -1, tessDisplace = -1;
            GLint cameraPos = -1, tessDebug = -1, terrainViewport = -1, terrainMVP = -1;
            GLint terrainLightDir = -1, detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1, -1, -1, -1, -1};
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            GLint time = -1;
            GLint mapHalfExtent = -1;
            GLuint program = 0;
        } terrainLocs_;

        // Cached uniform locations for shadow terrain shader
        struct ShadowUniformLocs {
            GLint lightSpaceMatrix = -1, tessLevel = -1, tessDistanceRange = -1;
            GLint tessDisplace = -1, cameraPos = -1, mvp = -1;
            GLint matNormal2 = -1, detailNormalTiling = -1, tex1 = -1;
            GLuint program = 0;
        } shadowLocs_;

        void cacheTerrainUniformLocations(GLuint shp) {
            if (terrainLocs_.program == shp) return;  // already cached
            terrainLocs_.program = shp;
            terrainLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            terrainLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            terrainLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            terrainLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            terrainLocs_.tessDebug = glGetUniformLocation(shp, "tessDebug");
            terrainLocs_.terrainViewport = glGetUniformLocation(shp, "terrainViewport");
            terrainLocs_.terrainMVP = glGetUniformLocation(shp, "terrainMVP");
            terrainLocs_.terrainLightDir = glGetUniformLocation(shp, "terrainLightDir");
            terrainLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            terrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            terrainLocs_.pomParams = glGetUniformLocation(shp, "pomParams");
            terrainLocs_.terrainWorldScale = glGetUniformLocation(shp, "terrainWorldScale");
            terrainLocs_.cellBombParams = glGetUniformLocation(shp, "cellBombParams");
            terrainLocs_.matNormal[0] = glGetUniformLocation(shp, "matNormal0");
            terrainLocs_.matNormal[1] = glGetUniformLocation(shp, "matNormal1");
            terrainLocs_.matNormal[2] = glGetUniformLocation(shp, "matNormal2");
            terrainLocs_.matNormal[3] = glGetUniformLocation(shp, "matNormal3");
            terrainLocs_.matNormal[4] = glGetUniformLocation(shp, "matNormal4");
            terrainLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            terrainLocs_.enableShadows = glGetUniformLocation(shp, "enableShadows");
            terrainLocs_.shadowSoftness = glGetUniformLocation(shp, "shadowSoftness");
            terrainLocs_.shadowMap = glGetUniformLocation(shp, "shadowMap");
            terrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            terrainLocs_.enableDynamicShadows = glGetUniformLocation(shp, "enableDynamicShadows");
            terrainLocs_.dynamicShadowMap = glGetUniformLocation(shp, "dynamicShadowMap");
            terrainLocs_.time = glGetUniformLocation(shp, "time");
            terrainLocs_.mapHalfExtent = glGetUniformLocation(shp, "mapHalfExtent");
        }

        void cacheShadowUniformLocations(GLuint shp) {
            if (shadowLocs_.program == shp) return;
            shadowLocs_.program = shp;
            shadowLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            shadowLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            shadowLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            shadowLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            shadowLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            shadowLocs_.mvp = glGetUniformLocation(shp, "mvp");
            shadowLocs_.matNormal2 = glGetUniformLocation(shp, "matNormal2");
            shadowLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            shadowLocs_.tex1 = glGetUniformLocation(shp, "tex1");
        }

        // ── World-space overlay batches ────────────────────────────────────────
        // Both terrain overlays (cement perimeter) and decals (craters, footprints)
        // use the same vertex layout (WorldOverlayVert, 28 bytes) and VAO setup.
        // Per-draw texture grouping: multiple addTriangle calls within a frame with
        // different texHandle values are batched into separate draw entries.
        struct OverlayBatchEntry_ {
            unsigned int texHandle;
            unsigned int firstVert;    // index into verts vector
            unsigned int vertCount;
        };
        struct OverlayBatch_ {
            GLuint vbo = 0, vao = 0;
            std::vector<WorldOverlayVert> verts;
            std::vector<OverlayBatchEntry_> draws;
        };

        OverlayBatch_ terrainOverlayBatch_;
        OverlayBatch_ decalBatch_;

        glsl_program* overlayProg_ = nullptr;  // terrain_overlay.vert + terrain_overlay.frag
        glsl_program* decalProg_   = nullptr;  // terrain_overlay.vert + decal.frag

        struct OverlayUniformLocs_ {
            GLint terrainMVP     = -1;
            GLint terrainVP      = -1;
            GLint mvp            = -1;
            GLint tex1           = -1;
            GLint fog_color      = -1;
            GLint time           = -1;
            GLint cameraPos      = -1;
            GLint surfaceDebugMode = -1;
            GLint terrainLightDir = -1;
            GLint mapHalfExtent  = -1;
        };
        OverlayUniformLocs_ overlayLocs_;
        OverlayUniformLocs_ decalLocs_;
        uint64_t timeStart_ = 0;          // shared epoch for all time-animated shaders

        // private helpers
        void pushToOverlayBatch_(OverlayBatch_& b, const WorldOverlayVert* v3, unsigned int texHandle);
        void uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed);
        // ── End world-space overlay batch members ──────────────────────────────
};

const std::string gosRenderer::s_Foreground = std::string("Foreground");

static GLuint gVAO = 0;
static float  s_hud_scale = 0.85f;  // default while iterating; RAlt+5 cycles
static bool   s_hud_scale_active = false;  // gated: only shrink during mission

void gosRenderer::init() {
    ZoneScopedN("gosRenderer::init");
    initRenderStates();

    // x = 1/w; x =2*x - 1;
    // y = 1/h; y= 1- y; y =2*y - 1;
    // z = z;
    projection_ = mat4(
            2.0f / (float)width_, 0, 0.0f, -1.0f,
            0, -2.0f / (float)height_, 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);

	graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight);

    // setup viewport
    setupViewport(true, 1.0f, true, 0, 0.0f, 0.0f, 1.0f, 1.0f);

    {
        ZoneScopedN("gosRenderer::init meshes");
        quads_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
        gosASSERT(quads_);
        tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
        gosASSERT(tris_);
        indexed_tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*60, 1024*60);
        gosASSERT(indexed_tris_);
        lines_ = gosMesh::makeMesh(PRIMITIVE_LINELIST, 1024*10);
        gosASSERT(lines_);
        points_= gosMesh::makeMesh(PRIMITIVE_POINTLIST, 1024*10);
        gosASSERT(points_);
        text_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 4024 * 6);
        gosASSERT(text_);
    }


    const char* shader_list[] = {"gos_vertex", "gos_tex_vertex", "gos_text", "gos_vertex_lighted", "gos_tex_vertex_lighted"};
    gosRenderMaterial** shader_ptr_list[] = { &basic_material_, &basic_tex_material_, &text_material_, &basic_lighted_material_, &basic_tex_lighted_material_};

    static_assert(COUNTOF(shader_list) == COUNTOF(shader_ptr_list), "Arrays myst have same size");
    uint32_t combinations[] = {
        0,
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST),
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY),
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) | SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY)
    };

    {
    ZoneScopedN("gosRenderer::init baseMaterials");
    for(size_t i=0; i<COUNTOF(combinations); ++i)
    {
        std::vector<std::string> defines;
        for(size_t bit = 0; bit < 32; ++bit)
        {
            uint32_t bit_mask = 1<<bit;
            if(bit_mask & combinations[i])
            {
                std::string s = g_shader_flags[bit];
                defines.push_back(s);
            }
        }

        gosMaterialVariationHelper helper;
        helper.addDefines(defines);
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);

        //TODO: remove texture / no texture variants and move it to flags
        
        for(uint32_t sh_idx = 0; sh_idx < COUNTOF(shader_list); ++sh_idx)
        {
            gosRenderMaterial* pmat = gosRenderMaterial::load(shader_list[sh_idx], mvar);
            gosASSERT(pmat);
            materialList_.push_back(pmat);
            materialDB_[ shader_list[sh_idx] ].insert(std::make_pair(combinations[i], pmat));

            *shader_ptr_list[sh_idx] = pmat;
        }
    }
    }


    { ZoneScopedN("gosRenderer::init vao"); glGenVertexArrays(1, &gVAO); }

    pendingRequest = false;

    num_draw_calls_ = 0;
    num_draw_calls_to_draw_ = 0;
    break_on_draw_call_ = false;
    break_draw_call_num_ = 0;

    // add fake texture so that no one will get 0 index, as it is invalid in this game
    DWORD tex_id;
    { ZoneScopedN("gosRenderer::init dummyTexture"); tex_id = gos_NewEmptyTexture( gos_Texture_Solid, "DEBUG_this_is_not_a_real_texture_debug_it!", 1); }
    (void)tex_id;
    gosASSERT(tex_id == INVALID_TEXTURE_ID);

	fog_color_ = vec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Terrain tessellation extra VBO
    terrain_extra_capacity_ = 1024 * 120;  // large buffer for extended view distance
    {
        ZoneScopedN("gosRenderer::init terrainExtra");
        terrain_extra_data_ = new gos_TERRAIN_EXTRA[terrain_extra_capacity_];
        terrain_extra_vb_ = makeBuffer(GL_ARRAY_BUFFER, 0,
            sizeof(gos_TERRAIN_EXTRA) * terrain_extra_capacity_, GL_DYNAMIC_DRAW);
    }
    printf("[TESS] Extra VBO created: capacity=%d vb=%u\n", terrain_extra_capacity_, terrain_extra_vb_);

    // Load terrain tessellation material (TCS/TES shaders)
    {
        ZoneScopedN("gosRenderer::init terrainMaterial");
        printf("[TESS] About to load terrain shader...\n"); fflush(stdout);
        gosMaterialVariationHelper terrainHelper;
        gosMaterialVariation mvar;
        terrainHelper.getMaterialVariation(mvar);
        terrain_material_ = gosRenderMaterial::load("gos_terrain", mvar);
        if (terrain_material_) {
            materialList_.push_back(terrain_material_);
            printf("[TESS] Terrain material loaded successfully\n");
        } else {
            printf("[TESS] WARNING: Terrain material failed to load — tessellation disabled\n");
        }
        fflush(stdout);
    }

    // Load shadow terrain material (VS+FS only, no tessellation)
    {
        ZoneScopedN("gosRenderer::init shadowTerrainMaterial");
        gosMaterialVariationHelper helper;
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);
        shadow_terrain_material_ = gosRenderMaterial::load("shadow_terrain", mvar);
        if (shadow_terrain_material_) {
            materialList_.push_back(shadow_terrain_material_);
        }
    }

    // Load shadow object material (VS+FS only, no tessellation)
    {
        ZoneScopedN("gosRenderer::init shadowObjectMaterial");
        gosMaterialVariationHelper helper;
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);
        shadow_object_material_ = gosRenderMaterial::load("shadow_object", mvar);
        if (shadow_object_material_) {
            materialList_.push_back(shadow_object_material_);
        }
    }

    // Load world-space overlay shaders and create VAOs/VBOs.
    // Both batches share terrain_overlay.vert; decal uses a different frag.
    // Use glsl_program::makeProgram directly (not gosRenderMaterial::load) because
    // the material loader always pairs [name].vert + [name].frag from the same name.
    {
        ZoneScopedN("gosRenderer::init overlayPrograms");
        overlayProg_ = glsl_program::makeProgram("terrain_overlay",
            "shaders/terrain_overlay.vert", "shaders/terrain_overlay.frag");
        decalProg_ = glsl_program::makeProgram("decal",
            "shaders/terrain_overlay.vert", "shaders/decal.frag");
    }

    if (!overlayProg_)
        fprintf(stderr, "[OverlayBatch] Failed to compile terrain_overlay shader\n");
    if (!decalProg_)
        fprintf(stderr, "[OverlayBatch] Failed to compile decal shader\n");

    // Cache uniform locations for both shaders
    auto cacheOverlayLocs = [](glsl_program* prog, OverlayUniformLocs_& locs) {
        if (!prog) return;
        GLuint shp = prog->shp_;
        locs.terrainMVP      = glGetUniformLocation(shp, "terrainMVP");
        locs.terrainVP       = glGetUniformLocation(shp, "terrainViewport");
        locs.mvp             = glGetUniformLocation(shp, "mvp");
        locs.tex1            = glGetUniformLocation(shp, "tex1");
        locs.fog_color       = glGetUniformLocation(shp, "fog_color");
        locs.time            = glGetUniformLocation(shp, "time");
        locs.cameraPos       = glGetUniformLocation(shp, "cameraPos");
        locs.surfaceDebugMode = glGetUniformLocation(shp, "surfaceDebugMode");
        locs.terrainLightDir = glGetUniformLocation(shp, "terrainLightDir");
        locs.mapHalfExtent   = glGetUniformLocation(shp, "mapHalfExtent");
    };
    { ZoneScopedN("gosRenderer::init overlayUniforms"); cacheOverlayLocs(overlayProg_, overlayLocs_); cacheOverlayLocs(decalProg_, decalLocs_); }
    timeStart_ = timing::get_wall_time_ms();

    // Create VBO/VAO for each batch.
    // WorldOverlayVert layout (stride 28 bytes):
    //   offset  0: vec3 worldPos    → attrib location 0
    //   offset 12: vec2 texcoord    → attrib location 1
    //   offset 20: float fog        → attrib location 2
    //   offset 24: uint8[4] argb    → attrib location 3, GL_UNSIGNED_BYTE, normalized
    auto makeOverlayVAO = [](OverlayBatch_& batch) {
        constexpr int kStride = 28;  // sizeof(WorldOverlayVert)
        glGenBuffers(1, &batch.vbo);
        glGenVertexArrays(1, &batch.vao);
        glBindVertexArray(batch.vao);
        glBindBuffer(GL_ARRAY_BUFFER, batch.vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kStride, (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kStride, (void*)12);
        glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kStride, (void*)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kStride, (void*)24);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    };
    { ZoneScopedN("gosRenderer::init overlayVAOs"); makeOverlayVAO(terrainOverlayBatch_); makeOverlayVAO(decalBatch_); }
}

void gosRenderer::destroy() {

    gosMesh::destroy(quads_);
    gosMesh::destroy(tris_);
    gosMesh::destroy(indexed_tris_);
    gosMesh::destroy(lines_);
    gosMesh::destroy(points_);
    gosMesh::destroy(text_);

    for(size_t i=0; i<fontList_.size(); i++) {
        gosRenderMaterial::destroy(materialList_[i]);
    }
    materialList_.clear();

    // delete fonts before textures, because they refer them
    for(size_t i=0; i<fontList_.size(); i++) {
        while(gosFont::destroy(fontList_[i])) {};
    }
    fontList_.clear();

    for(size_t i=0; i<textureList_.size(); i++) {
        delete textureList_[i];
    }
    textureList_.clear();

    // Terrain tessellation cleanup
    delete[] terrain_extra_data_;
    terrain_extra_data_ = nullptr;
    if (terrain_extra_vb_) { glDeleteBuffers(1, &terrain_extra_vb_); terrain_extra_vb_ = 0; }
    if (terrain_material_) { gosRenderMaterial::destroy(terrain_material_); terrain_material_ = nullptr; }

    glDeleteVertexArrays(1, &gVAO);

}

void gosRenderer::initRenderStates() {

	renderStates_[gos_State_Texture] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Texture2] = INVALID_TEXTURE_ID;
    renderStates_[gos_State_Texture3] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Filter] = gos_FilterNone;
	renderStates_[gos_State_ZCompare] = 1; 
    renderStates_[gos_State_ZWrite] = 1;
	renderStates_[gos_State_AlphaTest] = 0;
	renderStates_[gos_State_Perspective] = 1;
	renderStates_[gos_State_Specular] = 0;
	renderStates_[gos_State_Dither] = 0;
	renderStates_[gos_State_Clipping] = 0;	
	renderStates_[gos_State_WireframeMode] = 0;
	renderStates_[gos_State_AlphaMode] = gos_Alpha_OneZero;
	renderStates_[gos_State_TextureAddress] = gos_TextureWrap;
	renderStates_[gos_State_ShadeMode] = gos_ShadeGouraud;
	renderStates_[gos_State_TextureMapBlend] = gos_BlendModulateAlpha;
	renderStates_[gos_State_MipMapBias] = 0;
	renderStates_[gos_State_Fog]= 0;
	renderStates_[gos_State_MonoEnable] = 0;
	renderStates_[gos_State_Culling] = gos_Cull_None;
	renderStates_[gos_State_StencilEnable] = 0;
	renderStates_[gos_State_StencilFunc] = gos_Cmp_Never;
	renderStates_[gos_State_StencilRef] = 0;
	renderStates_[gos_State_StencilMask] = 0xffffffff;
	renderStates_[gos_State_StencilZFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilPass] = gos_Stencil_Keep;
	renderStates_[gos_State_Multitexture] = gos_Multitexture_None;
	renderStates_[gos_State_Ambient] = 0xffffff;
	renderStates_[gos_State_Lighting] = 0;
	renderStates_[gos_State_NormalizeNormals] = 0;
	renderStates_[gos_State_VertexBlend] = 0;

    applyRenderStates();
    renderStatesStackPointer = -1;
}

void gosRenderer::pushRenderStates()
{
    gosASSERT(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1);
    if(!(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1)) {
        return;
    }

    renderStatesStackPointer++;
    memcpy(&statesStack_[renderStatesStackPointer], &renderStates_, sizeof(renderStates_));
}

void gosRenderer::popRenderStates()
{
    gosASSERT(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE);
    
    if(!(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE)) {
        return;
    }

    memcpy(&renderStates_, &statesStack_[renderStatesStackPointer], sizeof(renderStates_));
    renderStatesStackPointer--;
}

void gosRenderer::applyRenderStates() {
    ZoneScopedN("ApplyRenderStates");

	////////////////////////////////////////////////////////////////////////////////
	switch (renderStates_[gos_State_Culling]) {
		case gos_Cull_None: glDisable(GL_CULL_FACE); break;
		case gos_Cull_CW:	
		case gos_Cull_CCW:
			glEnable(GL_CULL_FACE);
			// by default in OpenGL front face is CCW (could be changed by glFrontFace)
			glCullFace(renderStates_[gos_State_Culling] == gos_Cull_CW ? GL_BACK : GL_FRONT);
			break;
		default: gosASSERT(0 && "Wrong cull face value");
	}
	curStates_[gos_State_Culling] = renderStates_[gos_State_Culling];

	////////////////////////////////////////////////////////////////////////////////

	////////////////////////////////////////////////////////////////////////////////
	fog_color_ = uint32_to_vec4(renderStates_[gos_State_Fog]);
	curStates_[gos_State_Fog] = renderStates_[gos_State_Fog];

   ////////////////////////////////////////////////////////////////////////////////
   switch(renderStates_[gos_State_ZWrite]) {
       case 0: glDepthMask(GL_FALSE); break;
       case 1: glDepthMask(GL_TRUE); break;
       default: gosASSERT(0 && "Wrong depth write value");
   }
   curStates_[gos_State_ZWrite] = renderStates_[gos_State_ZWrite];

   ////////////////////////////////////////////////////////////////////////////////
   if(0 == renderStates_[gos_State_ZCompare]) {
       glDisable(GL_DEPTH_TEST);
   } else {
       glEnable(GL_DEPTH_TEST);
   }
   switch(renderStates_[gos_State_ZCompare]) {
       case 0: glDepthFunc(GL_ALWAYS); break;
       case 1: glDepthFunc(GL_LEQUAL); break;
       case 2: glDepthFunc(GL_LESS); break;
       default: gosASSERT(0 && "Wrong depth test value");
   }
   curStates_[gos_State_ZCompare] = renderStates_[gos_State_ZCompare];

   ////////////////////////////////////////////////////////////////////////////////
   bool disable_blending = renderStates_[gos_State_AlphaMode] == gos_Alpha_OneZero;
   if(disable_blending) {
       glDisable(GL_BLEND);
   } else {
       glEnable(GL_BLEND);
   }
   switch(renderStates_[gos_State_AlphaMode]) {
       case gos_Alpha_OneZero:          glBlendFunc(GL_ONE, GL_ZERO); break;
       case gos_Alpha_OneOne:           glBlendFunc(GL_ONE, GL_ONE); break;
       case gos_Alpha_AlphaInvAlpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_OneInvAlpha:      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_AlphaOne:         glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
       default: gosASSERT(0 && "Wrong alpha mode value");
   }
   curStates_[gos_State_AlphaMode] = renderStates_[gos_State_AlphaMode];

   ////////////////////////////////////////////////////////////////////////////////
   #if 0 // now in shaders (this is not supported in CORE OpenGL profile
   bool enable_alpha_test = renderStates_[gos_State_AlphaTest] == 1;
   if(enable_alpha_test) {
       glEnable(GL_ALPHA_TEST);
       glAlphaFunc(GL_NOTEQUAL, 0.0f);
   } else {
       glDisable(GL_ALPHA_TEST);
   }
   #endif
   curStates_[gos_State_AlphaTest] = renderStates_[gos_State_AlphaTest];

   ////////////////////////////////////////////////////////////////////////////////
   TexFilterMode filter = TFM_NONE;
   switch(renderStates_[gos_State_Filter]) {
       case gos_FilterNone: filter = TFM_NEAREST; break;
       case gos_FilterBiLinear : filter = TFM_LINEAR; break;
       case gos_FilterTriLinear: filter = TFM_LNEAR_MIPMAP_LINEAR; break;
   }
   // no mips for now, so ensure no invalid filters used
   //gosASSERT(filter == TFM_NEAREST || filter == TFM_LINEAR);
   // i do not know of any mipmaps that we are using
   if(filter == TFM_LNEAR_MIPMAP_LINEAR)
       filter = TFM_LINEAR;

   // in this case does not necessaily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_Filter] = renderStates_[gos_State_Filter];
  
   ////////////////////////////////////////////////////////////////////////////////
   TexAddressMode address_mode = 
       renderStates_[gos_State_TextureAddress] == gos_TextureWrap ? TAM_REPEAT : TAM_CLAMP_TO_EDGE;
   // in this case does not necessarily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_TextureAddress] = renderStates_[gos_State_TextureAddress];

   ////////////////////////////////////////////////////////////////////////////////
   uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
   for(int i=0; i<sizeof(tex_states) / sizeof(tex_states[0]); ++i) {
       DWORD gosTextureHandle = renderStates_[tex_states[i]];

       glActiveTexture(GL_TEXTURE0 + i);

       gosTexture* tex = gosTextureHandle == INVALID_TEXTURE_ID ? 0 : this->getTexture(gosTextureHandle);
       if(tex) {
           glBindTexture(GL_TEXTURE_2D, tex->getTextureId());
           setSamplerParams(tex->getTextureType(), address_mode, filter);

           gosTextureInfo texinfo;
           tex->getTextureInfo(&texinfo);
           if(renderStates_[gos_State_TextureMapBlend] == gos_BlendDecal && texinfo.format_ == gos_Texture_Alpha)
           {
               PAUSE((""));
           }

       } else {
           glBindTexture(GL_TEXTURE_2D, 0);
       }
       curStates_[tex_states[i]] = gosTextureHandle;
   }

   ////////////////////////////////////////////////////////////////////////////////
   // Terrain tessellation flag — copy then auto-reset to prevent bleed to non-terrain draws
   curStates_[gos_State_Terrain] = renderStates_[gos_State_Terrain];
   renderStates_[gos_State_Terrain] = 0;
   curStates_[gos_State_Water] = renderStates_[gos_State_Water];
   renderStates_[gos_State_Water] = 0;
   // Overlay GPU projection — copy, no auto-reset (managed by renderLists loop)
   curStates_[gos_State_Overlay] = renderStates_[gos_State_Overlay];

}

void gosRenderer::beginFrame()
{
    // Frame-boundary hygiene: IsHUD must be cleared by every callsite before the frame ends.
    // If it is still set here, a callsite leaked the bit across the frame boundary.
    if (renderStates_[gos_State_IsHUD] != 0) {
        SPEW(("GRAPHICS", "[HUD] gos_State_IsHUD still set at frame start -- callsite leak\n"));
        renderStates_[gos_State_IsHUD] = 0;
    }
    hudBatch_.clear();
    hudFlushed_ = false;
    glBindVertexArray(gVAO);
    num_draw_calls_ = 0;
}

void gosRenderer::endFrame()
{
    // check for file changes every half second
    static uint64_t last_check_time = timing::get_wall_time_ms();
    if(timing::get_wall_time_ms() - last_check_time > 500)
    {
        for(int i=0; i< materialList_.size(); ++i)
        {
            materialList_[i]->checkReload();
        }
    }
}

void gosRenderer::handleEvents()
{
    if(pendingRequest) {
        ZoneScopedN("gosRenderer::handleEvents pendingRequest");

        width_ = reqWidth;
        height_ = reqHeight;

        // x = 1/w; x =2*x - 1;
        // y = 1/h; y= 1- y; y =2*y - 1;
        // z = z;
        projection_ = mat4(2.0f / (float)width_, 0, 0.0f, -1.0f,
                0, -2.0f / (float)height_, 0.0f, 1.0f,
                0, 0, 1.0f, 0.0f,
                0, 0, 0.0f, 1.0f);

        {
        ZoneScopedN("gosRenderer::handleEvents resizeWindow");
        if(graphics::resize_window(win_h_, width_, height_))
		{
            { ZoneScopedN("gosRenderer::handleEvents fullscreen"); graphics::set_window_fullscreen(win_h_, reqGotoFullscreen); }

            Environment.screenWidth = width_;
            Environment.screenHeight = height_;

			{ ZoneScopedN("gosRenderer::handleEvents drawableSize"); graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight); }

        }
        }
        pendingRequest = false;
    }
}

bool gosRenderer::beforeDrawCall()
{
    num_draw_calls_++;
    if(break_draw_call_num_ == num_draw_calls_ && break_on_draw_call_) {
        PAUSE(("Draw call %d break\n", num_draw_calls_ - 1));
    }

    return (num_draw_calls_ > num_draw_calls_to_draw_) && num_draw_calls_to_draw_ != 0;
}

void gosRenderer::afterDrawCall()
{
}

gosRenderMaterial* gosRenderer::selectBasicRenderMaterial(const RenderState& rs) const
{
	ZoneScopedN("SelectBasicMaterial");
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex")->second :
		materialDB_.find("gos_vertex")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
    if (rs[gos_State_Overlay])
        flags |= SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY);

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "basic", flags));
        return nullptr;
    }
}

gosRenderMaterial* gosRenderer::selectLightedRenderMaterial(const RenderState& rs) const
{
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex_lighted")->second :
		materialDB_.find("gos_vertex_lighted")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "lighted", flags));
        return nullptr;
    }
}

void gosRenderer::drawQuads(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawQuads");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawQuads discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudQuadBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;

    int num_quads = count / 4;
    int num_vertices = num_quads * 6;

    if(quads_->getNumVertices() + num_vertices > quads_->getVertexCapacity()) {
        applyRenderStates();
        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
        mat->setFogColor(fog_color_);
        quads_->draw(mat);
        quads_->rewind();
    } 

    gosASSERT(quads_->getNumVertices() + num_vertices <= quads_->getVertexCapacity());
    for(int i=0; i<count;i+=4) {

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 1, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);
        quads_->addVertices(vertices + 4*i + 3, 1);
    }

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
    gosASSERT(mat);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    quads_->draw(mat);
    quads_->rewind();

    afterDrawCall();
}

void gosRenderer::drawLines(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawLines");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawLines discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudLineBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;

    if(lines_->getNumVertices() + count > lines_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
        basic_material_->setFogColor(fog_color_);
        lines_->draw(basic_material_);
        lines_->rewind();
    }

    gosASSERT(lines_->getNumVertices() + count <= lines_->getVertexCapacity());
    lines_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    basic_material_->setTransform(projection_);
    basic_material_->setFogColor(fog_color_);
    lines_->draw(basic_material_);
    lines_->rewind();

    afterDrawCall();
}

void gosRenderer::drawPoints(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawPoints");
    gosASSERT(vertices);

    if(beforeDrawCall()) return;

    if(points_->getNumVertices() + count > points_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
		basic_material_->setFogColor(fog_color_);
        points_->draw(basic_material_);
        points_->rewind();
    } 

    gosASSERT(points_->getNumVertices() + count <= points_->getVertexCapacity());
    points_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    points_->draw(basic_material_);
    points_->rewind();

    afterDrawCall();
}

void gosRenderer::drawTris(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawTris");
    gosASSERT(vertices);

    gosASSERT((count % 3) == 0);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawTris discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudTriBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;


    if(tris_->getNumVertices() + count > tris_->getVertexCapacity()) {
        applyRenderStates();

        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
		mat->setFogColor(fog_color_);
        tris_->draw(mat);
        tris_->rewind();
    } 

    gosASSERT(tris_->getNumVertices() + count <= tris_->getVertexCapacity());
    tris_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
    gosASSERT(mat);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    tris_->draw(mat);
    tris_->rewind();

    afterDrawCall();
}

// --- Shadow pre-pass: renders ALL terrain batches to shadow map before any shading ---
// This eliminates per-batch seams where early batches couldn't see later batches' shadows.

void gosRenderer::beginShadowPrePass(bool clearDepth) {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_) return;

    ZoneScopedN("Shadow.StaticPrePass");
    TracyGpuZone("Shadow.StaticPrePass");

    // Save current FBO and viewport
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &shadow_prepass_prev_fbo_);
    glGetIntegerv(GL_VIEWPORT, shadow_prepass_prev_viewport_);

    // Unbind shadow texture from unit 9 (prevent feedback loop — AMD requirement)
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    // Disable comparison mode for writing (AMD requirement)
    glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Bind shadow FBO, optionally clear depth, and configure state
    glBindFramebuffer(GL_FRAMEBUFFER, pp->getShadowFBO());
    int smSize = pp->getShadowMapSize();
    glViewport(0, 0, smSize, smSize);
    glDepthMask(GL_TRUE);
    if (clearDepth) glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    // Bind shadow shader and upload lightSpaceMatrix
    shadow_terrain_material_->apply();
    GLint lsmLoc = glGetUniformLocation(
        shadow_terrain_material_->getShader()->shp_, "lightSpaceMatrix");
    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, pp->getLightSpaceMatrix());

    active_light_space_matrix_ = pp->getLightSpaceMatrix();
    shadow_prepass_active_ = true;
}

void gosRenderer::drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount)
{
    if (!shadow_prepass_active_ || numVerts <= 0 || extraCount <= 0) return;

    ZoneScopedN("Shadow.TessBatch");
    TracyGpuZone("Shadow.TessBatch");

    // Upload vertices + indices to indexed_tris_ (same mesh used by normal terrain draw)
    indexed_tris_->rewind();
    indexed_tris_->addVertices(vertices, numVerts);
    indexed_tris_->addIndices(indices, numIndices);
    indexed_tris_->uploadBuffers();

    // Re-activate shadow shader (end() from previous batch deactivates program)
    shadow_terrain_material_->apply();
    GLuint shp = shadow_terrain_material_->getShader()->shp_;
    cacheShadowUniformLocations(shp);
    const auto& sl = shadowLocs_;

    // Upload lightSpaceMatrix (must re-upload per-batch since apply() resets state)
    {
        const float* lsm = active_light_space_matrix_;
        if (!lsm) {
            gosPostProcess* pp = getGosPostProcess();
            if (pp) lsm = pp->getLightSpaceMatrix();
        }
        if (lsm && sl.lightSpaceMatrix >= 0)
            glUniformMatrix4fv(sl.lightSpaceMatrix, 1, GL_FALSE, lsm);
    }

    // Upload tessellation uniforms via direct GL (same pattern as terrainDrawIndexedPatches)
    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4] = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4] = { 0.0f, terrain_displace_scale_, 0.0f, 0.0f };  // no Phong in shadow

    if (sl.tessLevel >= 0) glUniform4fv(sl.tessLevel, 1, tessParams);
    if (sl.tessDistanceRange >= 0) glUniform4fv(sl.tessDistanceRange, 1, tessDist);
    if (sl.tessDisplace >= 0) glUniform4fv(sl.tessDisplace, 1, tessDisp);
    if (sl.cameraPos >= 0) glUniform4fv(sl.cameraPos, 1, (const float*)&terrain_camera_pos_);

    // projection_ needed by shadow vert shader (TES overrides gl_Position, but VS needs it for gl_Position passthrough)
    if (sl.mvp >= 0) glUniformMatrix4fv(sl.mvp, 1, GL_TRUE, (const float*)&projection_);

    // Displacement textures: dirt normal on unit 7
    if (sl.matNormal2 >= 0) {
        glUniform1i(sl.matNormal2, 7);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[2]);  // dirt normal map
        glActiveTexture(GL_TEXTURE0);
    }

    float tiling[4] = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    if (sl.detailNormalTiling >= 0) glUniform4fv(sl.detailNormalTiling, 1, tiling);

    // tex1 (colormap) sampler — bound to unit 0 by the gos_SetRenderState texture call
    if (sl.tex1 >= 0) glUniform1i(sl.tex1, 0);

    // Bind main VBO (pos, color, fog, texcoord at locations 0-3)
    glBindBuffer(GL_ARRAY_BUFFER, indexed_tris_->getVB());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexed_tris_->getIB());
    shadow_terrain_material_->applyVertexDeclaration();

    // Bind extras VBO for worldPos (location 4) and worldNorm (location 5)
    updateBuffer(terrain_extra_vb_, GL_ARRAY_BUFFER,
        extras, extraCount * sizeof(gos_TERRAIN_EXTRA), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);

    glEnableVertexAttribArray(4);  // worldPos
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)0);
    glEnableVertexAttribArray(5);  // worldNorm
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float)));

    // Draw tessellated patches
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawElements(GL_PATCHES, numIndices,
        indexed_tris_->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    // Cleanup
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    shadow_terrain_material_->endVertexDeclaration();
    shadow_terrain_material_->end();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void gosRenderer::drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4)
{
    if (!shadow_prepass_active_ || !vb || !ib || ib->count_ == 0) return;
    if (!shadow_object_material_ || !shadow_object_material_->getShader()) return;

    ZoneScopedN("Shadow.DynObjectDirect");
    TracyGpuZone("Shadow.DynObjectDirect");

    // Direct GPU draw: bypass material system, use shadow_object shader directly.
    // This avoids the glGetBufferSubData readback that was 26% of frame time.
    GLuint shp = shadow_object_material_->getShader()->shp_;
    glUseProgram(shp);

    // Upload uniforms
    GLint lsmLoc = glGetUniformLocation(shp, "lightSpaceMatrix");
    GLint wmLoc = glGetUniformLocation(shp, "worldMatrix");
    GLint loLoc = glGetUniformLocation(shp, "lightOffset");

    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, active_light_space_matrix_);
    if (wmLoc >= 0)
        glUniformMatrix4fv(wmLoc, 1, GL_TRUE, worldMatrix4x4);  // row-major Stuff matrix
    if (loLoc >= 0) {
        float lo[3] = {
            terrain_light_dir_.x * 30.0f,
            terrain_light_dir_.y * 30.0f,
            terrain_light_dir_.z * 30.0f
        };
        glUniform3fv(loLoc, 1, lo);
    }

    // Force depth write ON — this is the key fix. applyRenderStates would disable it.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Bind VB with position attribute at location 0 (TG_HWTypeVertex: pos at offset 0, stride 36)
    glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);

    // Bind IB and draw
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
    GLenum indexType = (ib->element_size_ == 2) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    glDrawElements(GL_TRIANGLES, ib->count_, indexType, (void*)0);

    // Cleanup — disable the attribute we enabled
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void gosRenderer::endShadowPrePass() {
    if (!shadow_prepass_active_) return;

    ZoneScopedN("Shadow.PrePassEnd");

    gosPostProcess* pp = getGosPostProcess();

    // Restore comparison mode for sampling
    glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore previous FBO and viewport
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_prepass_prev_fbo_);
    glViewport(shadow_prepass_prev_viewport_[0], shadow_prepass_prev_viewport_[1],
               shadow_prepass_prev_viewport_[2], shadow_prepass_prev_viewport_[3]);

    active_light_space_matrix_ = nullptr;
    shadow_prepass_active_ = false;
}

void gosRenderer::beginDynamicShadowPass() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_ || !pp->getDynamicShadowFBO()) return;

    ZoneScopedN("Shadow.DynBegin");

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &shadow_prepass_prev_fbo_);
    glGetIntegerv(GL_VIEWPORT, shadow_prepass_prev_viewport_);

    // Unbind dynamic shadow texture from unit 10 (AMD feedback loop prevention)
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    // Disable comparison mode for writing
    glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, pp->getDynamicShadowFBO());
    glViewport(0, 0, pp->getDynamicShadowMapSize(), pp->getDynamicShadowMapSize());
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    shadow_terrain_material_->apply();
    GLint lsmLoc = glGetUniformLocation(
        shadow_terrain_material_->getShader()->shp_, "lightSpaceMatrix");
    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    active_light_space_matrix_ = pp->getDynamicLightSpaceMatrix();
    shadow_prepass_active_ = true;
}

void gosRenderer::endDynamicShadowPass() {
    if (!shadow_prepass_active_) return;

    ZoneScopedN("Shadow.DynEnd");

    gosPostProcess* pp = getGosPostProcess();

    // Restore comparison mode on dynamic shadow texture
    glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, shadow_prepass_prev_fbo_);
    glViewport(shadow_prepass_prev_viewport_[0], shadow_prepass_prev_viewport_[1],
               shadow_prepass_prev_viewport_[2], shadow_prepass_prev_viewport_[3]);

    active_light_space_matrix_ = nullptr;
    shadow_prepass_active_ = false;
}

void gosRenderer::terrainDrawIndexedPatches(gosRenderMaterial* material, gosMesh* mesh) {
    ZoneScopedN("Terrain.DrawPatches");
    TracyGpuZone("Terrain.DrawPatches");
    int nv = mesh->getNumVertices();
    int ni = mesh->getNumIndices();
    if (nv == 0) return;

    // Upload main VBO + IBO
    mesh->uploadBuffers();

    // Apply shader first (glUseProgram + upload cached uniforms)
    material->apply();
    material->setSamplerUnit(gosMesh::s_tex1, 0);

    // Set tessellation uniforms via direct GL calls (bypass uniform cache)
    GLuint shp = material->getShader()->shp_;
    cacheTerrainUniformLocations(shp);
    const auto& tl = terrainLocs_;

    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4] = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4] = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };

    if (tl.tessLevel >= 0) glUniform4fv(tl.tessLevel, 1, tessParams);
    if (tl.tessDistanceRange >= 0) glUniform4fv(tl.tessDistanceRange, 1, tessDist);
    if (tl.tessDisplace >= 0) glUniform4fv(tl.tessDisplace, 1, tessDisp);
    if (tl.cameraPos >= 0) glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    float tessDebugVec[4] = { terrain_debug_mode_, 0.0f, 0.0f, 0.0f };
    if (tl.tessDebug >= 0) glUniform4fv(tl.tessDebug, 1, tessDebugVec);

    // Map half-extent for off-map edge haze (fades meta-ring terrain to sky).
    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);
    }

    // Upload viewport params for TES perspective projection
    if (tl.terrainViewport >= 0) glUniform4fv(tl.terrainViewport, 1, (const float*)&terrain_viewport_);

    // Upload terrainMVP (axisSwap*worldToClip) via direct GL
    if (terrain_mvp_valid_) {
        if (tl.terrainMVP >= 0)
            glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    }

    // Bind terrain splatting uniforms (light, tiling, POM, cell bomb)
    if (tl.terrainLightDir >= 0) glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    float tiling[4] = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    if (tl.detailNormalTiling >= 0) glUniform4fv(tl.detailNormalTiling, 1, tiling);
    float strength[4] = { terrain_detail_strength_, 0.0f, 0.0f, 0.0f };
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    float pomP[4] = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    if (tl.pomParams >= 0) glUniform4fv(tl.pomParams, 1, pomP);
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    if (tl.terrainWorldScale >= 0) glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    float cellP[4] = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.cellBombParams >= 0) glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.time >= 0) {
        float elapsed = (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }

    // Bind per-material normal maps (units 5-8)
    for (int i = 0; i < 5; i++) {
        if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
            glUniform1i(tl.matNormal[i], 5 + i);
            glActiveTexture(GL_TEXTURE5 + i);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    // Shadow map binding (unit 9 = static, unit 10 = dynamic)
    {
        gosPostProcess* pp = getGosPostProcess();
        if (pp && pp->shadowsEnabled_) {
            if (tl.lightSpaceMatrix >= 0) glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
            if (tl.enableShadows >= 0) glUniform1i(tl.enableShadows, 1);
            if (tl.shadowSoftness >= 0) glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
            if (tl.shadowMap >= 0) {
                glUniform1i(tl.shadowMap, 9);
                glActiveTexture(GL_TEXTURE9);
                glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
            // Dynamic object shadow map (unit 10)
            if (pp->getDynamicShadowFBO()) {
                if (tl.dynamicLightSpaceMatrix >= 0)
                    glUniformMatrix4fv(tl.dynamicLightSpaceMatrix, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
                if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
                if (tl.dynamicShadowMap >= 0) {
                    glUniform1i(tl.dynamicShadowMap, 10);
                    glActiveTexture(GL_TEXTURE10);
                    glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                    glActiveTexture(GL_TEXTURE0);
                }
            } else {
                if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
            }
        } else {
            if (tl.enableShadows >= 0) glUniform1i(tl.enableShadows, 0);
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    }

    // Bind main VBO and set standard vertex attribs
    glBindBuffer(GL_ARRAY_BUFFER, mesh->getVB());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->getIB());
    material->applyVertexDeclaration();

    // Bind extra VBO for world pos + normal (locations 4-5)
    // Use per-batch extras from texture manager (aligned with main VBO by construction)
    const gos_TERRAIN_EXTRA* batchExtras = terrain_batch_extras_;
    int batchExtrasCount = terrain_batch_extras_count_;
    if (batchExtras && batchExtrasCount > 0) {
        updateBuffer(terrain_extra_vb_, GL_ARRAY_BUFFER,
            batchExtras, batchExtrasCount * sizeof(gos_TERRAIN_EXTRA), GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);
    GLint worldPosLoc = glGetAttribLocation(material->getShader()->shp_, "worldPos");
    GLint worldNormLoc = glGetAttribLocation(material->getShader()->shp_, "worldNorm");
    if (worldPosLoc >= 0) {
        glEnableVertexAttribArray(worldPosLoc);
        glVertexAttribPointer(worldPosLoc, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA), (void*)0);
    }
    if (worldNormLoc >= 0) {
        glEnableVertexAttribArray(worldNormLoc);
        glVertexAttribPointer(worldNormLoc, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float)));
    }

    // Shadow depth is now written in a separate pre-pass (beginShadowPrePass/drawShadowBatch/endShadowPrePass)
    // called from renderLists() before the shading loop — no per-batch shadow work here.

    // Wireframe overlay
    if (terrain_wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    // Set patch vertices and draw
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    {
        GLenum err = glGetError(); // clear any pending
        glDrawElements(GL_PATCHES, ni,
            mesh->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);
        err = glGetError();
        static int gl_err_count = 0;
        if (err != GL_NO_ERROR && gl_err_count++ < 5) {
            printf("[TESS] GL ERROR after glDrawElements(GL_PATCHES): 0x%x ni=%d nv=%d\n", err, ni, nv);
            fflush(stdout);
        }
    }

    // Cleanup
    if (worldPosLoc >= 0) glDisableVertexAttribArray(worldPosLoc);
    if (worldNormLoc >= 0) glDisableVertexAttribArray(worldNormLoc);

    material->endVertexDeclaration();
    material->end();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    if (terrain_wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

void gosRenderer::drawGrassPass(gosMesh* mesh) {
    ZoneScopedN("Grass.Draw");
    TracyGpuZone("Grass.Draw");
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->grassEnabled_) return;

    glsl_program* prog = pp->getGrassProgram();
    if (!prog || !prog->is_valid()) return;

    int ni = mesh->getNumIndices();
    if (ni == 0 || !terrain_mvp_valid_) return;

    // Alpha blending, double-sided
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    GLuint shp = prog->shp_;
    glUseProgram(shp);

    // Helper for uniform lookup
    auto L = [&](const char* name) -> GLint { return glGetUniformLocation(shp, name); };
    GLint loc;

    // Tessellation + transform uniforms (same values as terrain pass)
    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4]   = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4]   = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };
    float tiling[4]     = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };

    loc = L("tessLevel");         if (loc >= 0) glUniform4fv(loc, 1, tessParams);
    loc = L("tessDistanceRange"); if (loc >= 0) glUniform4fv(loc, 1, tessDist);
    loc = L("tessDisplace");      if (loc >= 0) glUniform4fv(loc, 1, tessDisp);
    loc = L("cameraPos");         if (loc >= 0) glUniform4fv(loc, 1, (const float*)&terrain_camera_pos_);
    loc = L("terrainViewport");   if (loc >= 0) glUniform4fv(loc, 1, (const float*)&terrain_viewport_);
    loc = L("terrainLightDir");   if (loc >= 0) glUniform4fv(loc, 1, (const float*)&terrain_light_dir_);
    loc = L("detailNormalTiling");if (loc >= 0) glUniform4fv(loc, 1, tiling);
    loc = L("terrainMVP");        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, (const float*)&terrain_mvp_);
    loc = L("mvp");               if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, (const float*)&projection_);

    // Time for wind animation
    static uint64_t grass_start = timing::get_wall_time_ms();
    float elapsed = (float)(timing::get_wall_time_ms() - grass_start) / 1000.0f;
    loc = L("time"); if (loc >= 0) glUniform1f(loc, elapsed);

    // Colormap (unit 0) — still bound from terrain pass
    loc = L("tex1"); if (loc >= 0) glUniform1i(loc, 0);

    // Material normals (units 5-8) for TES displacement
    for (int i = 0; i < 4; i++) {
        char name[16]; snprintf(name, sizeof(name), "matNormal%d", i);
        loc = L(name);
        if (loc >= 0 && terrain_mat_normal_[i] != 0) {
            glUniform1i(loc, 5 + i);
            glActiveTexture(GL_TEXTURE5 + i);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
        }
    }

    // Shadow maps
    if (pp->shadowsEnabled_) {
        loc = L("enableShadows");  if (loc >= 0) glUniform1i(loc, 1);
        loc = L("shadowSoftness"); if (loc >= 0) glUniform1f(loc, terrain_shadow_softness_);
        loc = L("lightSpaceMatrix"); if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getLightSpaceMatrix());
        loc = L("shadowMap"); if (loc >= 0) { glUniform1i(loc, 9); glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture()); }
        if (pp->getDynamicShadowFBO()) {
            loc = L("enableDynamicShadows"); if (loc >= 0) glUniform1i(loc, 1);
            loc = L("dynamicLightSpaceMatrix"); if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
            loc = L("dynamicShadowMap"); if (loc >= 0) { glUniform1i(loc, 10); glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture()); }
        } else {
            loc = L("enableDynamicShadows"); if (loc >= 0) glUniform1i(loc, 0);
        }
    } else {
        loc = L("enableShadows"); if (loc >= 0) glUniform1i(loc, 0);
        loc = L("enableDynamicShadows"); if (loc >= 0) glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE0);

    // Rebind terrain mesh and draw
    glBindBuffer(GL_ARRAY_BUFFER, mesh->getVB());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->getIB());
    terrain_material_->applyVertexDeclaration();

    if (terrain_batch_extras_ && terrain_batch_extras_count_ > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);
        GLint worldPosLoc  = glGetAttribLocation(shp, "worldPos");
        GLint worldNormLoc = glGetAttribLocation(shp, "worldNorm");
        if (worldPosLoc >= 0) { glEnableVertexAttribArray(worldPosLoc); glVertexAttribPointer(worldPosLoc, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)0); }
        if (worldNormLoc >= 0) { glEnableVertexAttribArray(worldNormLoc); glVertexAttribPointer(worldNormLoc, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float))); }

        glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(GL_PATCHES, ni, mesh->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

        if (worldPosLoc >= 0) glDisableVertexAttribArray(worldPosLoc);
        if (worldNormLoc >= 0) glDisableVertexAttribArray(worldNormLoc);
    }

    terrain_material_->endVertexDeclaration();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // Restore state
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

void gosRenderer::drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices) {
    ZoneScopedN("DrawIndexedTris.Basic");
    gosASSERT(vertices && indices);

    gosASSERT((num_indices % 3) == 0);

    if(beforeDrawCall()) return;

    bool not_enough_vertices = indexed_tris_->getNumVertices() + num_vertices > indexed_tris_->getVertexCapacity();
    bool not_enough_indices = indexed_tris_->getNumIndices() + num_indices > indexed_tris_->getIndexCapacity();
    if(not_enough_vertices || not_enough_indices){
        applyRenderStates();

        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
		mat->setFogColor(fog_color_);
        indexed_tris_->drawIndexed(mat);
        indexed_tris_->rewind();
    }

    gosASSERT(indexed_tris_->getNumVertices() + num_vertices <= indexed_tris_->getVertexCapacity());
    gosASSERT(indexed_tris_->getNumIndices() + num_indices <= indexed_tris_->getIndexCapacity());
    indexed_tris_->addVertices(vertices, num_vertices);
    indexed_tris_->addIndices(indices, num_indices);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    // Terrain tessellation path
    if (curStates_[gos_State_Terrain] && !curStates_[gos_State_Overlay] && terrain_material_ && terrain_batch_extras_count_ > 0 && terrain_draw_enabled_) {
        ZoneScopedN("Terrain.TessDraw");
        TracyGpuZone("Terrain.TessDraw");
        gosRenderMaterial* tmat = terrain_material_;
        tmat->setTransform(projection_);
        tmat->setFogColor(fog_color_);
        // terrainMVP uploaded via direct GL in terrainDrawIndexedPatches
        terrainDrawIndexedPatches(tmat, indexed_tris_);
        // Mark terrain drawn so post-process effects know to run (god rays, shorelines)
        { gosPostProcess* pp = getGosPostProcess(); if (pp) pp->markTerrainDrawn(); }
        indexed_tris_->rewind();
    } else {
        // When tessellation is active, skip SOLID fallback terrain draws (tessellation
        // already rendered base terrain). Overlay/detail draws don't set gos_State_Terrain
        // (it auto-resets after each draw), so they fall through to the basic renderer.
        if (curStates_[gos_State_Terrain] && terrain_material_) {
            indexed_tris_->rewind();
        } else {
            gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
            gosASSERT(mat);

            mat->setTransform(projection_);
            mat->setFogColor(fog_color_);

            // Water uniforms: set via deferred system before apply() flushes them
            {
                glsl_program* prog = mat->getShader();
                if (curStates_[gos_State_Water]) {
                    prog->setInt("isWater", 1);
                    static uint64_t water_start = timing::get_wall_time_ms();
                    float elapsed = (float)(timing::get_wall_time_ms() - water_start) / 1000.0f;
                    prog->setFloat("time", elapsed);
                } else {
                    prog->setInt("isWater", 0);
                }
            }

            // Overlay.SplitDraw removed — alpha cement and decals now go through
            // gos_DrawTerrainOverlays() / gos_DrawDecals() typed world-space batches.
            {
                ZoneScopedN("BasicDraw.Indexed");
                indexed_tris_->drawIndexed(mat);
            }
            indexed_tris_->rewind();
        }
    }

    afterDrawCall();
}

void gosRenderer::drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    ZoneScopedN("DrawIndexedTris.Lighted");
    TracyGpuZone("DrawIndexedTris.Lighted");
    gosASSERT(ib && vb && mvp);
    gosASSERT((ib->count_ % 3) == 0);

    if(beforeDrawCall()) return;

    applyRenderStates();

    gosRenderMaterial* mat = selectLightedRenderMaterial(curStates_);
    gosASSERT(mat);

	mat4 transform(	mvp[0], mvp[1], mvp[2], mvp[3], 
					mvp[4], mvp[5], mvp[6], mvp[7],
					mvp[8], mvp[9], mvp[10], mvp[11],
					mvp[12], mvp[13], mvp[14], mvp[15]);

	vec4 vp = g_gos_renderer->getRenderViewport();

	mat->getShader()->setFloat4("vp", vp);
	mat->getShader()->setMat4("projection_", projection_);

    mat->setTransform(transform);
    //mat->setFogColor(fog_color_);

	gosMesh::drawIndexed(ib, vb, vdecl, mat);

    afterDrawCall();
}

void gosRenderer::drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    ZoneScopedN("DrawIndexedTris.Unlighted");
    TracyGpuZone("DrawIndexedTris.Unlighted");
    gosASSERT(ib && vb);
    gosASSERT((ib->count_ % 3) == 0);

    if(beforeDrawCall()) return;

    applyRenderStates();

	// maybe getCurMaterial->set.... to not set it from outer code?
	//vec4 vp = g_gos_renderer->getRenderViewport();
	//mat->getShader()->setFloat4("vp", vp);
	//mat->getShader()->setMat4("projection_", projection_);

	gosMesh::drawIndexed(ib, vb, vdecl);

    afterDrawCall();
}

static int get_next_break(const char* text) {
    const char* start = text;
    do {
        char c = *text;
        if(c==' ' || c=='\n')
            return (int32_t)(text - start);
    } while(*text++);

    return (int32_t)(text - start - 1);
}

int findTextBreak(const char* text, const int count, const gosFont* font, const int region_width, int* out_str_width) {

    int width = 0;
    int pos = 0;

    int space_adv = font->getCharAdvance(' ');

    while(text[pos]) {

        int break_pos = get_next_break(text + pos);

        int cur_width = 0;
        for(int j=0;j<break_pos;++j) {
            cur_width += font->getCharAdvance(text[pos + j]);
        }

        // if next possible break will not fit, then return now
        if(width + cur_width >= region_width) {

            if(pos == 0) { // handle case when only one word in line and it does not fit it, just return whole line
                width = cur_width;
                pos = break_pos;
            }
            break;
        } else {
            width += cur_width;
            pos += break_pos;

            if(text[pos] == '\n') {
                pos++;
                break;
            }

            if(text[pos] == ' ') {
                width += space_adv;
                pos++;
            }
        }
    }

    if(out_str_width)
        *out_str_width = width;
    return pos;
}
// returnes num lines in text which should be wrapped in region_width
int calcTextHeight(const char* text, const int count, const gosFont* font, int region_width)
{
    int pos = 0;
    int num_lines = 0;
    while(pos < count) {

        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, NULL);
        pos += num_chars;
        num_lines++;
    }
    return num_lines;
}

void addCharacter(gosMesh* text_, const float u, const float v, const float u2, const float v2, const float x, const float y, const float x2, const float y2) {

    gos_VERTEX tr, tl, br, bl;

    tl.x = x;
    tl.y = y;
    tl.z = 0;
    tl.u = u;
    tl.v = v;
    tl.argb = 0xffffffff;
    tl.frgb = 0xff000000;

    tr.x = x2;
    tr.y = y;
    tr.z = 0;
    tr.u = u2;
    tr.v = v;
    tr.argb = 0xffffffff;
    tr.frgb = 0xff000000;

    bl.x = x;
    bl.y = y2;
    bl.z = 0;
    bl.u = u;
    bl.v = v2;
    bl.argb = 0xffffffff;
    bl.frgb = 0xff000000;

    br.x = x2;
    br.y = y2;
    br.z = 0;
    br.u = u2;
    br.v = v2;
    br.argb = 0xffffffff;
    br.frgb = 0xff000000;

    text_->addVertices(&tl, 1);
    text_->addVertices(&tr, 1);
    text_->addVertices(&bl, 1);

    text_->addVertices(&tr, 1);
    text_->addVertices(&br, 1);
    text_->addVertices(&bl, 1);

}

void gosRenderer::drawText(const char* text) {
    ZoneScopedN("DrawText");
    gosASSERT(text);

    if(beforeDrawCall()) return;

    const int count = (int)strlen(text);  
/*
    if(text_->getNumVertices() + count > text_->getCapacity()) {
        applyRenderStates();
        gosRenderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
        text_->draw(mat);
        text_->rewind();
    } 
*/

    // TODO: take text region into account!!!!
    
    gosASSERT(text_->getNumVertices() + 6 * count <= text_->getVertexCapacity());

    int ix, iy;
    getTextPos(ix, iy);
	float x = (float)ix, y = (float)iy;
    const float start_x = x;

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;

    const DWORD tex_id = font->getTextureId();
    const gosTexture* tex = getTexture(tex_id);
    gosTextureInfo ti;
    tex->getTextureInfo(&ti);
    const float oo_tex_width = 1.0f / (float)ti.width_;
    const float oo_tex_height = 1.0f / (float)ti.height_;
    
    const int font_height = font->getMaxCharHeight();
    const int font_ascent = font->getFontAscent();

    const int region_width = getTextRegionWidth();
    const int region_height = getTextRegionHeight();

    const int num_lines = calcTextHeight(text, count, font, region_width);
    if(ta.WrapType == 3) { // center in Y direction as well
        y += (region_height - num_lines * font_height) / 2;
    }
   
    int pos = 0;
    int str_width = 0;
    while(pos < count) {

        x = start_x;    
        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, &str_width);

        // WrapType		- 0=Left aligned, 1=Right aligned, 2=Centered, 3=Centered in region (X and Y)
        switch(ta.WrapType) {
            case 0: break;
            case 1: x += region_width - str_width; break;
            case 2: x += (region_width - str_width) / 2; break;
            case 3: // see vertical centering above
                    x += (region_width - str_width) / 2;
                    break;
        }

        for(int i=0; i<num_chars; ++i) {

            const char c = text[i + pos];

            const gosGlyphMetrics& gm = font->getGlyphMetrics(c);
            int char_off_x = gm.minx;
            int char_off_y = font_ascent - gm.maxy;
            int char_w = gm.maxx - gm.minx;
            int char_h = gm.maxy - gm.miny;

            uint32_t iu0 = gm.u + char_off_x;
            uint32_t iv0 = gm.v + char_off_y;
            uint32_t iu1 = iu0 + char_w;
            uint32_t iv1 = iv0 + char_h;

            float u0 = (float)iu0 * oo_tex_width;
            float v0 = (float)iv0 * oo_tex_height;
            float u1 = (float)iu1 * oo_tex_width;
            float v1 = (float)iv1 * oo_tex_height;

            addCharacter(text_, u0, v0, u1, v1, (float)(x + char_off_x), (float)(y + char_off_y), (float)(x + char_off_x + char_w), (float)(y + char_off_y + char_h));

            x += font->getCharAdvance(c);
        }
        y += font_height;
        pos += num_chars;
    }
    // T2: if HUD buffering is active, capture pre-expanded glyph geometry and defer
    if (renderStates_[gos_State_IsHUD]) {
        const int n = text_->getNumVertices();
        if (n > 0 && !hudFlushed_) {
            HudDrawCall call;
            call.kind = kHudTextQuadBatch;
            call.vertices.assign(text_->getVertices(), text_->getVertices() + n);
            memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
            call.projection = projection_;
            call.fontTexId = tex_id;
            call.foregroundColor = ta.Foreground;
            hudBatch_.push_back(std::move(call));
        } else if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawText discarded (after flushHUDBatch)\n"));
        }
        text_->rewind();
        return;
    }

    // FIXME: save states before messing with it, because user code can set its ow and does not know that something was changed by us

    int prev_texture = getRenderState(gos_State_Texture);
    
    // All states are set by client code
    // so we only set font texture
    setRenderState(gos_State_Texture, tex_id);
    setRenderState(gos_State_Filter, gos_FilterNone);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    gosRenderMaterial* mat = text_material_;

    //ta.Foreground
    vec4 fg;
    fg.x = (float)((ta.Foreground & 0xFF0000) >> 16);
    fg.y = (float)((ta.Foreground & 0xFF00) >> 8);
    fg.z = (float)(ta.Foreground & 0xFF);
    fg.w = 255.0f;//(ta.Foreground & 0xFF000000) >> 24;
    fg = fg / 255.0f;
    mat->getShader()->setFloat4(s_Foreground, fg);
    //ta.Size 
    //ta.WordWrap 
    //ta.Proportional
    //ta.Bold
    //ta.Italic
    //ta.WrapType
    //ta.DisableEmbeddedCodes

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    text_->draw(mat);
    text_->rewind();

    setRenderState(gos_State_Texture, prev_texture);

    afterDrawCall();
}

void gosRenderer::replayTextQuads(const HudDrawCall& call)
{
    if (call.vertices.empty()) return;

    text_->addVertices(const_cast<gos_VERTEX*>(call.vertices.data()),
                       (int)call.vertices.size());

    int prev_texture = getRenderState(gos_State_Texture);
    setRenderState(gos_State_Texture, call.fontTexId);
    setRenderState(gos_State_Filter, gos_FilterNone);

    applyRenderStates();
    gosRenderMaterial* mat = text_material_;

    vec4 fg;
    fg.x = (float)((call.foregroundColor & 0xFF0000) >> 16) / 255.0f;
    fg.y = (float)((call.foregroundColor & 0xFF00)   >>  8) / 255.0f;
    fg.z = (float)( call.foregroundColor & 0xFF)            / 255.0f;
    fg.w = 1.0f;
    mat->getShader()->setFloat4(s_Foreground, fg);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    text_->draw(mat);
    text_->rewind();

    setRenderState(gos_State_Texture, prev_texture);
}

void gosRenderer::flushHUDBatch()
{
    if (hudBatch_.empty()) {
        hudFlushed_ = true;
        return;
    }

    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // to true by mission->start() and false by mission->destroy()). Menus and
    // modal dialogs run through the same HUD buffer but stay at 100%; we skip
    // any call whose centroid is above 60% of the screen height.
    //
    // Anchor is SINGLE — bottom-center (sw/2, sh). Every bottom-band primitive
    // shrinks toward that one point. Previously we used 9-slice L/C/R anchoring
    // which pulled the tacmap/command clusters apart rather than compressing
    // the HUD strip as a coherent whole. Single-anchor keeps the strip
    // contiguous: tacmap drifts slightly inward-right, force-group stays put,
    // command panel drifts slightly inward-left, all without visible gaps.
    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // true by Mission::start and false by Mission::destroy). Menus and modal
    // dialogs run through the same HUD buffer but stay at 100%.
    //
    // Single bottom-center anchor (sw/2, sh). Every qualifying primitive
    // shrinks toward that one point. Centroid-based gate at 60% of screen
    // height — draws whose centroid is above that threshold are left alone
    // (catches dialogs, menus). This was the variant that looked good in
    // iteration; later attempts at max-Y gating / shrink-in-place broke more
    // than they fixed (touched scene/overlay draws, pulled tall panels out
    // of their corners, etc.).
    const float scale = s_hud_scale;
    if (s_hud_scale_active && scale < 0.999f) {
        const float sw = (float)width_;
        const float sh = (float)height_;
        const float bottomBand = sh * 0.60f;
        const float ax = sw * 0.5f;
        const float ay = sh;
        for (HudDrawCall& call : hudBatch_) {
            if (call.vertices.empty()) continue;
            float cy = 0.0f;
            for (const gos_VERTEX& v : call.vertices) cy += v.y;
            cy /= (float)call.vertices.size();
            if (cy < bottomBand) continue;
            for (gos_VERTEX& v : call.vertices) {
                v.x = ax + (v.x - ax) * scale;
                v.y = ay + (v.y - ay) * scale;
            }
        }
    }

    // pp->endScene() binds FB 0 and sets the full-screen viewport for us,
    // but leaves VAO 0 bound — rebind our VAO so glVertexAttribPointer works.
    glBindVertexArray(gVAO);

    // Save pre-flush render state and projection
    uint32_t priorState[gos_MaxState];
    memcpy(priorState, renderStates_, sizeof(priorState));
    mat4 priorProjection = projection_;

    for (const HudDrawCall& call : hudBatch_) {
        memcpy(renderStates_, call.stateSnapshot, sizeof(renderStates_));
        renderStates_[gos_State_IsHUD] = 0;   // clear to prevent re-buffering on replay
        projection_ = call.projection;

        switch (call.kind) {
            case kHudQuadBatch:
                drawQuads(const_cast<gos_VERTEX*>(call.vertices.data()),
                          (int)call.vertices.size());
                break;
            case kHudLineBatch:
                drawLines(const_cast<gos_VERTEX*>(call.vertices.data()),
                          (int)call.vertices.size());
                break;
            case kHudTriBatch:
                drawTris(const_cast<gos_VERTEX*>(call.vertices.data()),
                         (int)call.vertices.size());
                break;
            case kHudTextQuadBatch:
                replayTextQuads(call);
                break;
        }
    }

    // Restore pre-flush render state and projection
    memcpy(renderStates_, priorState, sizeof(priorState));
    projection_ = priorProjection;
    hudFlushed_ = true;
}

void gosRenderer::flush()
{
}

void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
    ZoneScopedN("gos_CreateRenderer");

    g_gos_renderer = new gosRenderer(ctx_h, win_h, w, h);
    g_gos_renderer->init();

    gosPostProcess* pp = new gosPostProcess();
    pp->init(w, h);
}

void gos_DestroyRenderer() {

    gosPostProcess* pp = getGosPostProcess();
    if (pp) { pp->destroy(); delete pp; }

    g_gos_renderer->destroy();
    delete g_gos_renderer;
}

void gos_RendererBeginFrame() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->beginFrame();
}

void gos_RendererEndFrame() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->endFrame();
}

void gos_RendererFlushHUDBatch() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->flushHUDBatch();
}

void gos_RendererHandleEvents() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->handleEvents();
}


gosFont::~gosFont()
{
    if(tex_id_ != INVALID_TEXTURE_ID)
        getGosRenderer()->deleteTexture(tex_id_);

    delete[] gi_.glyphs_;
    delete[] font_name_;
    delete[] font_id_;
}

////////////////////////////////////////////////////////////////////////////////
gosFont* gosFont::load(const char* fontFile) {

    char fname[256];
    char dir[256];
    _splitpath(fontFile, NULL, dir, fname, NULL);
    const char* tex_ext = ".bmp";
    const char* glyph_ext = ".glyph";
    
	const size_t textureNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(tex_ext) + 1;
    char* textureName = new char[textureNameSize];
	memset(textureName, 0, textureNameSize);

	const size_t glyphNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(glyph_ext) + 1;
    char* glyphName = new char[glyphNameSize];
	memset(glyphName, 0, glyphNameSize);

    uint32_t formatted_len = S_snprintf(textureName, textureNameSize, "%s/%s%s", dir, fname, tex_ext);
	gosASSERT(formatted_len <= textureNameSize - 1);

    formatted_len = S_snprintf(glyphName, glyphNameSize, "%s/%s%s", dir, fname, glyph_ext);
	gosASSERT(formatted_len <= glyphNameSize - 1);

    gosTexture* ptex = new gosTexture(gos_Texture_Alpha, textureName, 0, NULL, 0, false);
    if(!ptex || !ptex->createHardwareTexture()) {
        STOP(("Failed to create font texture: %s\n", textureName));
    }

    DWORD tex_id = getGosRenderer()->addTexture(ptex);

    gosFont* font = new gosFont();
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        delete font;
        STOP(("Failed to load font glyphs: %s\n", glyphName));
        return NULL;
    }

    font->font_name_ = new char[strlen(fname) + 1];
    strcpy(font->font_name_, fname);

    font->font_id_ = new char[strlen(fontFile) + 1];
    strcpy(font->font_id_, fontFile);

    font->tex_id_ = tex_id;

    delete[] textureName;
    delete[] glyphName;

    return font;

}

uint32_t gosFont::destroy(gosFont* font) {
    uint32_t rc = font->decRef();
    if(0 == rc) {
        delete font;
    }

    return rc;
}

void gosFont::getCharUV(int c, uint32_t* u, uint32_t* v) const {

    gosASSERT(u && v);

    int32_t pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        *u = *v = 0;
        return;
    }

    *u = gi_.glyphs_[pos].u;
    *v = gi_.glyphs_[pos].v;
}

int gosFont::getCharAdvance(int c) const
{
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        return getMaxCharWidth();
    }

    return gi_.glyphs_[pos].advance;
}

const gosGlyphMetrics& gosFont::getGlyphMetrics(int c) const {
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_)
        pos = 0;

    return gi_.glyphs_[pos];
}




////////////////////////////////////////////////////////////////////////////////
// graphics
//
void _stdcall gos_DrawLines(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawLines(Vertices, NumVertices);
}
void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawPoints(Vertices, NumVertices);
}

bool g_disable_quads = true;
void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(Vertices, NumVertices);
}
void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawTris(Vertices, NumVertices);
}

void __stdcall gos_GetViewport( float* pViewportMulX, float* pViewportMulY, float* pViewportAddX, float* pViewportAddY )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->getViewportTransform(pViewportMulX, pViewportMulY, pViewportAddX, pViewportAddY);
}

HGOSFONT3D __stdcall gos_LoadFont( const char* FontFile, DWORD StartLine/* = 0*/, int CharCount/* = 256*/, DWORD TextureHandle/*=0*/)
{

    gosFont* font = getGosRenderer()->findFont(FontFile);
    if(!font) {
        font = gosFont::load(FontFile);
        getGosRenderer()->addFont(font);
    } else {
        font->addRef();
    }

    return font;
}

void __stdcall gos_DeleteFont( HGOSFONT3D FontHandle )
{
    gosASSERT(FontHandle);
    gosFont* font = FontHandle;
    getGosRenderer()->deleteFont(font);
}

DWORD __stdcall gos_NewEmptyTexture( gos_TextureFormat Format, const char* Name, DWORD HeightWidth, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    int w = HeightWidth;
    int h = HeightWidth;
    if(HeightWidth&0xffff0000)
    {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    gosTexture* ptex = new gosTexture(Format, Hints, w, h, Name);

    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}
DWORD __stdcall gos_NewTextureFromMemory( gos_TextureFormat Format, const char* FileName, BYTE* pBitmap, DWORD Size, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosASSERT(pFunc == 0);

    gosTexture* ptex = new gosTexture(Format, FileName, Hints, pBitmap, Size, true);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}

DWORD __stdcall gos_NewTextureFromFile( gos_TextureFormat Format, const char* FileName, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosTexture* ptex = new gosTexture(Format, FileName, Hints, NULL, 0, false);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }
    return g_gos_renderer->addTexture(ptex);
}
void __stdcall gos_DestroyTexture( DWORD Handle )
{
    g_gos_renderer->deleteTexture(Handle);
}

void __stdcall gos_LockTexture( DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo )
{
    // TODO: does not really locks texture
    
    // not implemented yet
    gosASSERT(MipMapSize == 0);
    int mip_level = 0; //func(MipMapSize);

    gosTextureInfo info;
    int pitch = 0;
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->getTextureInfo(&info);
    BYTE* pdata = ptex->Lock(mip_level, ReadOnly, &pitch);

    TextureInfo->pTexture = (DWORD*)pdata;
    TextureInfo->Width = info.width_;
    TextureInfo->Height = info.height_;
    TextureInfo->Pitch = pitch;
    TextureInfo->Type = info.format_;

    //gosASSERT(0 && "Not implemented");
}

void __stdcall gos_UnLockTexture( DWORD Handle )
{
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->Unlock();

    //gosASSERT(0 && "Not implemented");
}

void __stdcall gos_PushRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->pushRenderStates();
} 

void __stdcall gos_PopRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->popRenderStates();
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(pVertexArray, NumberVertices, lpwIndices, NumberIndices);
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX_2UV* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
   gosASSERT(0 && "not implemented");
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl, mvp);
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl);
}

void __stdcall gos_SetRenderState( gos_RenderState RenderState, int Value )
{
    gosASSERT(g_gos_renderer);
    // gos_BlendDecal mode is not suported (currently texture color always modulated with vertex color)
    //gosASSERT(RenderState!=gos_State_TextureMapBlend || (Value == gos_BlendDecal));
    g_gos_renderer->setRenderState(RenderState, Value);
}

void __stdcall gos_SetScreenMode( DWORD Width, DWORD Height, DWORD bitDepth/*=16*/, DWORD Device/*=0*/, bool disableZBuffer/*=0*/, bool AntiAlias/*=0*/, bool RenderToVram/*=0*/, bool GotoFullScreen/*=0*/, int DirtyRectangle/*=0*/, bool GotoWindowMode/*=0*/, bool EnableStencil/*=0*/, DWORD Renderer/*=0*/)
{
    ZoneScopedN("gos_SetScreenMode");
    gosASSERT(g_gos_renderer);
    gosASSERT((GotoFullScreen && !GotoWindowMode) || (!GotoFullScreen&&GotoWindowMode) || (!GotoFullScreen&&!GotoWindowMode));

    g_gos_renderer->setScreenMode(Width, Height, bitDepth, GotoFullScreen, AntiAlias);
}

void __stdcall gos_SetupViewport( bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil/*=0*/, DWORD StencilValue/*=0*/)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setupViewport(FillZ, ZBuffer, FillBG, BGColor, top, left, bottom, right, ClearStencil, StencilValue);
}


void __stdcall gos_SetRenderViewport(float x, float y, float w, float h)
{
    gosASSERT(g_gos_renderer);
	//glViewport(x, y, w, h);
	g_gos_renderer->setRenderViewport(vec4(x, y, w, h));
}

void __stdcall gos_GetRenderViewport(float* x, float* y, float* w, float* h)
{
    gosASSERT(x && y && w && h);
    gosASSERT(g_gos_renderer);
	vec4 vp = g_gos_renderer->getRenderViewport();
	*x = vp.x;
	*y = vp.y;
	*w = vp.z;
	*h = vp.w;
}


void __stdcall gos_TextDraw( const char *Message, ... )
{

	if (!Message || !strlen(Message)) {
        SPEW(("GRAPHICS", "Trying to draw zero legth string\n"));
        return;
    }

	va_list	ap;
    va_start(ap, Message);

    static const int MAX_TEXT_LEN = 4096;
	char text[MAX_TEXT_LEN] = {0};

	vsnprintf(text, MAX_TEXT_LEN - 1, Message, ap);

	size_t len = strlen(text);
	text[len] = '\0';

    va_end(ap);

    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawText(text);
}

void __stdcall gos_TextDrawBackground( int Left, int Top, int Right, int Bottom, DWORD Color )
{
    // TODO: Is it correctly Implemented?
    gosASSERT(g_gos_renderer);

    //PAUSE((""));

    gos_VERTEX v[4];
    v[0].x = (float)Left;
    v[0].y = (float)Top;
    v[0].z = 0;
	v[0].argb = Color;
	v[0].frgb = 0;
	v[0].u = 0;	
	v[0].v = 0;	
    memcpy(&v[1], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[2], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[3], &v[0], sizeof(gos_VERTEX));
    v[1].x = (float)Right;
    v[1].u = 1.0f;

    v[2].x = (float)Right;
    v[2].y = (float)Bottom;
    v[2].u = 1.0f;
    v[2].v = 0.0f;

    v[1].y = (float)Bottom;
    v[1].v = 1.0f;

    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(v, 4);
}

void __stdcall gos_TextSetAttributes( HGOSFONT3D FontHandle, DWORD Foreground, float Size, bool WordWrap, bool Proportional, bool Bold, bool Italic, DWORD WrapType/*=0*/, bool DisableEmbeddedCodes/*=0*/)
{
    gosASSERT(g_gos_renderer);

    gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    ta.FontHandle = FontHandle;
    ta.Foreground = Foreground;
    ta.Size = Size;
    ta.WordWrap = WordWrap;
    ta.Proportional = Proportional;
    ta.Bold = Bold;
    ta.Italic = Italic;
    ta.WrapType = WrapType;
    ta.DisableEmbeddedCodes = DisableEmbeddedCodes;
}

void __stdcall gos_TextSetPosition( int XPosition, int YPosition )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextPos(XPosition, YPosition);
}

void __stdcall gos_TextSetRegion( int Left, int Top, int Right, int Bottom )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextRegion(Left, Top, Right, Bottom);
}

void __stdcall gos_TextStringLength( DWORD* Width, DWORD* Height, const char *fmt, ... )
{
    gosASSERT(Width && Height);

    if(!fmt) {
        SPEW(("GRAPHICS", "No text to calculate length!"));
        *Width = 1;
        *Height = 1;
        return;
    }

    const int   MAX_TEXT_LEN = 4096;
	char        text[MAX_TEXT_LEN] = {0};
	va_list	    ap;

    va_start(ap, fmt);
	vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);

	size_t len = strlen(text);
    text[len] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    gosASSERT(font);

    int num_newlines = 0;
    int max_width = 0;
    int cur_width = 0;
    const char* txtptr = text;

    while(*txtptr) {
        if(*txtptr == '\n') {
            num_newlines++;
            max_width = max_width > cur_width ? max_width : cur_width;
            cur_width = 0;
        } else {
            const int cw = font->getCharAdvance(*txtptr);
            cur_width += cw;
        }
        txtptr++;
    }
    max_width = max_width > cur_width ? max_width : cur_width;

    *Width = max_width;
    *Height = (num_newlines + 1) * font->getMaxCharHeight();
}

////////////////////////////////////////////////////////////////////////////////
size_t __stdcall gos_GetMachineInformation( MachineInfo mi, int Param1/*=0*/, int Param2/*=0*/, int Param3/*=0*/, int Param4/*=0*/)
{
    // TODO:
    if(mi == gos_Info_GetDeviceLocalMemory)
        return 1024*1024*1024;
    if(mi == gos_Info_GetDeviceAGPMemory)
        return 512*1024*1024; 
    if (mi == gos_Info_CanMultitextureDetail)
        return true;
    if(mi == gos_Info_NumberDevices)
        return 1;
    if(mi == gos_Info_GetDeviceName)
        return (size_t)glGetString(GL_RENDERER);
    if(mi == gos_Info_ValidMode) {
        int xres = Param2;
        int yres = Param3;
        int bpp = Param4;
        return graphics::is_mode_supported(xres, yres, bpp) ? 1 : 0;
    }
    if(mi == gos_Info_GetIMECaretStatus)
        return 1;

    return 0;
}

int gos_GetWindowDisplayIndex()
{   
    gosASSERT(g_gos_renderer);
    
    return graphics::get_window_display_index(g_gos_renderer->getRenderContextHandle());
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}


////////////////////////////////////////////////////////////////////////////////
// GPU Buffers management code
////////////////////////////////////////////////////////////////////////////////

GLenum getGLBufferType(gosBUFFER_TYPE type)
{
	GLenum t = -1;
	switch (type)
	{
		case gosBUFFER_TYPE::VERTEX: t = GL_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::INDEX: t = GL_ELEMENT_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::UNIFORM: t = GL_UNIFORM_BUFFER; break;

		default:
			gosASSERT(0 && "unknows buffer type");
	}
	return t;
}

GLenum getGLBufferUsage(gosBUFFER_USAGE usage)
{
	GLenum u = -1;
	switch (usage)
	{
		case gosBUFFER_USAGE::STREAM_DRAW: u = GL_STREAM_DRAW; break;
		case gosBUFFER_USAGE::STATIC_DRAW: u = GL_STATIC_DRAW; break;
		case gosBUFFER_USAGE::DYNAMIC_DRAW: u = GL_DYNAMIC_DRAW; break;

		default:
			gosASSERT(0 && "unknows buffer usage");
	}
	return u;
}



gosBuffer* __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage, int element_size, uint32_t count, void* buffer_data)
{
	GLenum gl_target = getGLBufferType(type);
	GLenum gl_usage = getGLBufferUsage(usage);

	size_t buffer_size = element_size * count;
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(gl_target, buffer);
	glBufferData(gl_target, buffer_size, buffer_data, gl_usage);
	glBindBuffer(gl_target, 0);

	gosBuffer* pbuffer = new gosBuffer();
	pbuffer->buffer_ = buffer;
	pbuffer->element_size_ = element_size;
	pbuffer->count_ = count;
	pbuffer->type_ = type;
	pbuffer->usage_ = usage;

    gosASSERT(g_gos_renderer);
	g_gos_renderer->addBuffer(pbuffer);

	return pbuffer;
}

uint32_t gos_GetBufferSizeBytes(HGOSBUFFER buffer)
{
	gosASSERT(buffer);
    return buffer->element_size_ * buffer->count_;
}

void __stdcall gos_DestroyBuffer(gosBuffer* buffer)
{
	gosASSERT(buffer);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteBuffer(buffer);
    (void)rv;
	delete buffer;
}

void __stdcall gos_BindBufferBase(gosBuffer* buffer, uint32_t slot)
{
	gosASSERT(buffer);

	GLenum gl_target = getGLBufferType(buffer->type_);
	glBindBufferBase(gl_target, slot, buffer->buffer_);
}

void __stdcall gos_UpdateBuffer(HGOSBUFFER buffer, void* data, size_t offset, size_t num_bytes)
{
	gosASSERT(buffer);
    gosASSERT(buffer->element_size_ * buffer->count_ >= num_bytes);
	GLenum gl_target = getGLBufferType(buffer->type_);
    glBindBuffer(gl_target, buffer->buffer_);
	glBufferData(gl_target, num_bytes, data, GL_DYNAMIC_DRAW);
    glBindBuffer(gl_target, 0);
}

HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD* records, int count)
{
	gosASSERT(records && count > 0);
    gosASSERT(g_gos_renderer);
	gosVertexDeclaration* vdecl = gosVertexDeclaration::create(records, count);
	g_gos_renderer->addVertexDeclaration(vdecl);
	return vdecl;
}

void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION vdecl)
{
	gosASSERT(vdecl);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteVertexDeclaration(vdecl);
    (void)rv;
	gosVertexDeclaration::destroy(vdecl);

}

HGOSRENDERMATERIAL __stdcall gos_getRenderMaterial(const char* material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);
	return g_gos_renderer->getRenderMaterial(material);
}

void __stdcall gos_ApplyRenderMaterial(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);

	//setup commoin stuff
	gos_SetCommonMaterialParameters(material);

	material->apply();
	material->setSamplerUnit(gosMesh::s_tex1, 0);
	material->setUniformBlock("lights_data", 0);
}

void __stdcall gos_SetRenderMaterialParameterFloat4(HGOSRENDERMATERIAL material, const char* name, const float* v)
{
	gosASSERT(material);
	gosASSERT(v);
	material->getShader()->setFloat4(name, v);
}

void __stdcall gos_SetRenderMaterialParameterMat4(HGOSRENDERMATERIAL material, const char* name, const float* m)
{
	gosASSERT(material);
	gosASSERT(m);
	material->getShader()->setMat4(name, m);
}

void __stdcall gos_SetRenderMaterialParameterInt(HGOSRENDERMATERIAL material, const char* name, int v)
{
	gosASSERT(material);
	material->getShader()->setInt(name, v);
}

void __stdcall gos_SetRenderMaterialUniformBlockBindingPoint(HGOSRENDERMATERIAL material, const char* name, uint32_t slot)
{
	gosASSERT(material && name);
	material->setUniformBlock(name, slot);
}

void __stdcall gos_SetupObjectShadows(HGOSRENDERMATERIAL material)
{
	ZoneScopedN("SetupObjectShadows");
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	gosPostProcess* pp = getGosPostProcess();
	if (!pp) return;

	GLuint shp = material->getShader()->shp_;

	// Upload terrainMVP for GPU projection (MC2 world → clip space)
	if (g_gos_renderer->isTerrainMVPValid()) {
		GLint mvpLoc = glGetUniformLocation(shp, "terrainMVP");
		if (mvpLoc >= 0) {
			const mat4& mvp = g_gos_renderer->getTerrainMVP();
			glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, (const float*)&mvp);
		}
	}

	// Terrain light direction (needed for shadow bias calculations)
	GLint loc = glGetUniformLocation(shp, "terrainLightDir");
	if (loc >= 0)
		glUniform4fv(loc, 1, (const float*)&g_gos_renderer->getTerrainLightDir());

	if (!pp->shadowsEnabled_) {
		loc = glGetUniformLocation(shp, "enableShadows");
		if (loc >= 0) glUniform1i(loc, 0);
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 0);
		return;
	}

	// Static shadow map (texture unit 9)
	loc = glGetUniformLocation(shp, "lightSpaceMatrix");
	if (loc >= 0)
		glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getLightSpaceMatrix());

	loc = glGetUniformLocation(shp, "enableShadows");
	if (loc >= 0) glUniform1i(loc, 1);

	loc = glGetUniformLocation(shp, "shadowSoftness");
	if (loc >= 0) glUniform1f(loc, g_gos_renderer->getTerrainShadowSoftness());

	loc = glGetUniformLocation(shp, "shadowMap");
	if (loc >= 0) {
		glUniform1i(loc, 9);
		glActiveTexture(GL_TEXTURE9);
		glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
	}

	// Dynamic shadow map (texture unit 10)
	if (pp->getDynamicShadowFBO()) {
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 1);

		loc = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
		if (loc >= 0)
			glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

		loc = glGetUniformLocation(shp, "dynamicShadowMap");
		if (loc >= 0) {
			glUniform1i(loc, 10);
			glActiveTexture(GL_TEXTURE10);
			glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
		}
	} else {
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
}

void __stdcall gos_SetCommonMaterialParameters(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	const mat4& projection = getGosRenderer()->getProj2Screen();
	const vec4& vp = getGosRenderer()->getRenderViewport();

	// TODO: make typed parameters !!!!!!!!!!!!!!! not just float* pointers, helps track errors
	gos_SetRenderMaterialParameterMat4(material, "projection_", projection);
	gos_SetRenderMaterialParameterFloat4(material, "vp", vp);
}


void __stdcall gos_ForceApplyRenderStates() {
    // Force-apply by directly setting GL state to match the requested render states.
    // applyRenderStates() may skip if curStates_ == renderStates_, so bypass it.
    if (!g_gos_renderer) return;
    // Just set GL directly for the critical states that renderLists() dirties
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    // Then sync applyRenderStates tracking
    g_gos_renderer->applyRenderStates();
}

// Terrain tessellation API
void __stdcall gos_SetTerrainTessParams(float level, float near_dist, float far_dist) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTessParams(level, near_dist, far_dist);
}
void __stdcall gos_SetTerrainPhongAlpha(float a) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPhongAlpha(a);
}
void __stdcall gos_SetTerrainDisplaceScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDisplaceScale(s);
}
float gos_GetTerrainPhongAlpha() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPhongAlpha() : 0.0f;
}
float gos_GetTerrainDisplaceScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDisplaceScale() : 0.0f;
}
float gos_GetTerrainDetailTiling() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailTiling() : 1.0f;
}
void __stdcall gos_SetTerrainWireframe(bool w) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWireframe(w);
}
void __stdcall gos_SetTerrainDebugMode(float mode) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDebugMode(mode);
}
float __stdcall gos_GetTerrainDebugMode() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDebugMode() : 0.0f;
}
void __stdcall gos_TerrainExtraReset() {
    if (g_gos_renderer) g_gos_renderer->terrainExtraReset();
}
void __stdcall gos_TerrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
    if (g_gos_renderer) g_gos_renderer->terrainExtraAdd(data, count);
}
void __stdcall gos_SetTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
    if (g_gos_renderer) g_gos_renderer->setTerrainBatchExtras(extras, count);
}
bool __stdcall gos_IsTerrainTessellationActive() {
    return g_gos_renderer && g_gos_renderer->getTerrainMaterial() != nullptr;
}

void gos_SetShadowMode(bool enable) {
    if (g_gos_renderer) g_gos_renderer->setShadowMode(enable);
}

void gos_GetTerrainCameraPos(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& cp = g_gos_renderer->getTerrainCameraPos();
        if (x) *x = cp.x;
        if (y) *y = cp.y;
        if (z) *z = cp.z;
    }
}
void __stdcall gos_SetTerrainMVP(const float* matrix16) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMVP(matrix16);
}
void __stdcall gos_SetTerrainViewport(float vmx, float vmy, float vax, float vay) {
    if (g_gos_renderer) g_gos_renderer->setTerrainViewport(vmx, vmy, vax, vay);
}
void __stdcall gos_SetTerrainCameraPos(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCameraPos(x, y, z);
}
// Static shadow API — world-fixed shadow map rendered once at map load
void gos_SetMapHalfExtent(float halfExtent) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setMapHalfExtent(halfExtent);
}
bool gos_StaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->staticLightMatrixBuilt();
}
void gos_BuildStaticLightMatrix() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_) return;
    float lx = 0, ly = 0, lz = 0;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    // Negate: lightDir points scene→sun, but matrix needs light→scene
    pp->buildStaticLightMatrix(-lx, -ly, -lz, pp->getMapHalfExtent());
}
void gos_MarkStaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->markStaticLightMatrixBuilt();
}
void gos_BeginShadowPrePass(bool clearDepth) {
    if (g_gos_renderer) g_gos_renderer->beginShadowPrePass(clearDepth);
}
void gos_EndShadowPrePass() {
    if (g_gos_renderer) g_gos_renderer->endShadowPrePass();
}
// Phase 4a: file-static one-shot flag. Set when external code (or the
// txmmgr.cpp first-terrain-frame latch) wants to force the static shadow
// pass to run regardless of the camera-motion cache threshold.
static bool s_shadowRebuildPending = false;
void gos_RequestFullShadowRebuild() { s_shadowRebuildPending = true; }
bool gos_ShadowRebuildPending() { return s_shadowRebuildPending; }
void gos_ClearShadowRebuildPending() { s_shadowRebuildPending = false; }
void gos_DrawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount) {
    if (g_gos_renderer) g_gos_renderer->drawShadowBatchTessellated(
        vertices, numVerts, indices, numIndices, extras, extraCount);
}
void gos_DrawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4) {
    if (g_gos_renderer) g_gos_renderer->drawShadowObjectBatch(vb, ib, vdecl, worldMatrix4x4);
}

// Dynamic object shadow pass API
void gos_BeginDynamicShadowPass() {
    if (g_gos_renderer) g_gos_renderer->beginDynamicShadowPass();
}
void gos_EndDynamicShadowPass() {
    if (g_gos_renderer) g_gos_renderer->endDynamicShadowPass();
}
void gos_BuildDynamicLightMatrix(float sx, float sy, float sz,
                                  float cx, float cy, float cz) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->buildDynamicLightMatrix(sx, sy, sz, cx, cy, cz);
}

void gos_GetTerrainLightDir(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& ld = g_gos_renderer->getTerrainLightDir();
        if (x) *x = ld.x;
        if (y) *y = ld.y;
        if (z) *z = ld.z;
    }
}
void gos_SetTerrainLightDir(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightDir(x, y, z);
}
void gos_SetTerrainDetailParams(float tiling, float strength) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDetailParams(tiling, strength);
}
void gos_SetTerrainMaterialNormal(int index, unsigned int glTexId) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMaterialNormal(index, glTexId);
}
void gos_SetTerrainWorldScale(float scale) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWorldScale(scale);
}
void gos_SetTerrainCellBombParams(float scale, float jitter, float rotation) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCellBombParams(scale, jitter, rotation);
}
void gos_SetTerrainPOMParams(float scale, float minLayers, float maxLayers) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPOMParams(scale, minLayers, maxLayers);
}
void gos_SetTerrainDetailNormalTexture(unsigned int glTexId) {
    // Legacy single-texture path — not used with per-material splatting, but terrtxm2 still calls it
}
void gos_SetTerrainDisplacementTexture(unsigned int glTexId) {
    // Legacy single displacement — not used with per-material splatting
}
void gos_SetTerrainViewDir(float x, float y, float z) {
    // View direction for POM — stored but not critical for tessellation path
}
unsigned int gos_CreateTerrainNormalTexture(const unsigned char* rgbaData, int width) {
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, width, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("[TESS] Created terrain normal texture: id=%u size=%d\n", texId, width);
    return texId;
}

void gos_SetTerrainShadowSoftness(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainShadowSoftness(s);
}
float gos_GetTerrainShadowSoftness() {
    return g_gos_renderer ? g_gos_renderer->getTerrainShadowSoftness() : 2.5f;
}

void gos_SetTerrainDrawEnabled(bool e) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDrawEnabled(e);
}
bool gos_GetTerrainDrawEnabled() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDrawEnabled() : true;
}

// HUD scale — clamped to [0.5, 1.0]. 1.0 disables the transform entirely.
// s_hud_scale itself is defined near the top of this file so flushHUDBatch()
// can reference it directly without a forward declaration dance.
void gos_SetHudScale(float s) {
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    s_hud_scale = s;
}
float gos_GetHudScale() { return s_hud_scale; }

void gos_SetHudScaleActive(bool on) { s_hud_scale_active = on; }
bool gos_GetHudScaleActive()        { return s_hud_scale_active; }

void gos_HudInverseMousePoint(float& x, float& y) {
    // Inverse of the single-anchor bottom-center HUD transform. Must stay in
    // sync with gosRenderer::flushHUDBatch() above.
    const float scale = s_hud_scale;
    if (!s_hud_scale_active || scale > 0.999f || !g_gos_renderer) return;
    const float sw = (float)g_gos_renderer->getWidth();
    const float sh = (float)g_gos_renderer->getHeight();
    const float bottomBand = sh * 0.60f;
    const float renderedBandTop = sh + (bottomBand - sh) * scale;
    if (y < renderedBandTop) return;
    const float ax = sw * 0.5f;
    const float ay = sh;
    x = ax + (x - ax) / scale;
    y = ay + (y - ay) / scale;
}

// ── World-space overlay batch API ────────────────────────────────────────────

// Private helper: push one triangle into a batch, grouping consecutive same-texture
// calls into a single draw entry.  Batches are cleared at the END of the draw
// functions (called exactly once per frame from renderLists), not on push.
void gosRenderer::pushToOverlayBatch_(OverlayBatch_& b,
                                      const WorldOverlayVert* v3,
                                      unsigned int texHandle)
{
    if (!b.draws.empty() && b.draws.back().texHandle == texHandle) {
        b.draws.back().vertCount += 3;
    } else {
        OverlayBatchEntry_ entry;
        entry.texHandle = texHandle;
        entry.firstVert = (unsigned int)b.verts.size();
        entry.vertCount = 3;
        b.draws.push_back(entry);
    }
    b.verts.push_back(v3[0]);
    b.verts.push_back(v3[1]);
    b.verts.push_back(v3[2]);
}

void gosRenderer::pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(terrainOverlayBatch_, verts3, texHandle);
}

void gosRenderer::pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(decalBatch_, verts3, texHandle);
}

// Upload shadow uniforms for a standalone shader program.
// Kept as a static helper because it only touches public getters — no private types.
static void setupOverlayShadowsForShp(GLuint shp)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    GLint loc;
    loc = glGetUniformLocation(shp, "terrainLightDir");
    if (loc >= 0) glUniform4fv(loc, 1, (const float*)&g_gos_renderer->getTerrainLightDir());

    if (!pp->shadowsEnabled_) {
        loc = glGetUniformLocation(shp, "enableShadows");
        if (loc >= 0) glUniform1i(loc, 0);
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 0);
        return;
    }

    loc = glGetUniformLocation(shp, "lightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getLightSpaceMatrix());
    loc = glGetUniformLocation(shp, "enableShadows");
    if (loc >= 0) glUniform1i(loc, 1);
    loc = glGetUniformLocation(shp, "shadowSoftness");
    if (loc >= 0) glUniform1f(loc, g_gos_renderer->getTerrainShadowSoftness());
    loc = glGetUniformLocation(shp, "shadowMap");
    if (loc >= 0) {
        glUniform1i(loc, 9);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    }

    if (pp->getDynamicShadowFBO()) {
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 1);
        loc = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
        loc = glGetUniformLocation(shp, "dynamicShadowMap");
        if (loc >= 0) {
            glUniform1i(loc, 10);
            glActiveTexture(GL_TEXTURE10);
            glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
        }
    } else {
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE0);
}

// Private member: common uniform upload for both draw paths.
void gosRenderer::uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed)
{
    if (L.terrainMVP >= 0)
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE, (const float*)&getTerrainMVP());
    if (L.terrainVP >= 0)
        glUniform4fv(L.terrainVP, 1, (const float*)&getTerrainViewport());
    // projection_: row-major Stuff matrix — upload GL_TRUE (column-major interpretation)
    if (L.mvp >= 0)
        glUniformMatrix4fv(L.mvp, 1, GL_TRUE, (const float*)&getProj2Screen());
    if (L.fog_color >= 0)
        glUniform4fv(L.fog_color, 1, (const float*)&getFogColor());
    if (L.time >= 0)
        glUniform1f(L.time, elapsed);
    if (L.cameraPos >= 0)
        glUniform4fv(L.cameraPos, 1, (const float*)&getTerrainCameraPos());
    if (L.surfaceDebugMode >= 0)
        glUniform1i(L.surfaceDebugMode, (GLint)terrain_debug_mode_);
    if (L.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(L.mapHalfExtent, halfExt);
    }

    setupOverlayShadowsForShp(shp);
}

// Draw the terrain overlay batch (alpha cement perimeter tiles).
// Render state: opaque, depth-write ON, depth-test LEQUAL (same as solid terrain).
// MRT: terrain_overlay.frag writes GBuffer1.alpha=1 → shadow_screen skips these pixels.
// Batch cleared after draw.
void gosRenderer::drawTerrainOverlays()
{
    if (terrainOverlayBatch_.draws.empty() || !overlayProg_) {
        terrainOverlayBatch_.verts.clear();
        terrainOverlayBatch_.draws.clear();
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, terrainOverlayBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(terrainOverlayBatch_.verts.size() * sizeof(WorldOverlayVert)),
        terrainOverlayBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glUseProgram(overlayProg_->shp_);
    float elapsed = (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed);

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glBindVertexArray(terrainOverlayBatch_.vao);
    for (const auto& entry : terrainOverlayBatch_.draws) {
        if (overlayLocs_.tex1 >= 0)
            glUniform1i(overlayLocs_.tex1, 0);
        glActiveTexture(GL_TEXTURE0);
        gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "terrainOverlayBatch");
        glBindTexture(GL_TEXTURE_2D, t ? t->getTextureId() : 0);
        glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
    }
    glBindVertexArray((GLuint)prevVao);

    glDepthFunc(GL_LESS);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    terrainOverlayBatch_.verts.clear();
    terrainOverlayBatch_.draws.clear();
}

// Draw the decal batch (bomb craters + mech footprints).
// Render state: alpha blend, depth-write OFF, depth-test LEQUAL.
// Batch cleared after draw.
void gosRenderer::drawDecals()
{
    if (decalBatch_.draws.empty() || !decalProg_) {
        decalBatch_.verts.clear();
        decalBatch_.draws.clear();
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, decalBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(decalBatch_.verts.size() * sizeof(WorldOverlayVert)),
        decalBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glUseProgram(decalProg_->shp_);
    float elapsed = (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    uploadOverlayUniforms_(decalProg_->shp_, decalLocs_, elapsed);

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glBindVertexArray(decalBatch_.vao);
    for (const auto& entry : decalBatch_.draws) {
        if (decalLocs_.tex1 >= 0)
            glUniform1i(decalLocs_.tex1, 0);
        glActiveTexture(GL_TEXTURE0);
        gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "decalBatch");
        glBindTexture(GL_TEXTURE_2D, t ? t->getTextureId() : 0);
        glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
    }
    glBindVertexArray((GLuint)prevVao);

    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    decalBatch_.verts.clear();
    decalBatch_.draws.clear();
}

// ── Thin exported wrappers ────────────────────────────────────────────────────

void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts3, unsigned int texHandle) {
    if (g_gos_renderer && verts3) g_gos_renderer->pushTerrainOverlayTri(verts3, texHandle);
}

void __stdcall gos_PushDecal(const WorldOverlayVert* verts3, unsigned int texHandle) {
    if (g_gos_renderer && verts3) g_gos_renderer->pushDecalTri(verts3, texHandle);
}

void __stdcall gos_DrawTerrainOverlays() {
    if (g_gos_renderer) g_gos_renderer->drawTerrainOverlays();
}

void __stdcall gos_DrawDecals() {
    if (g_gos_renderer) g_gos_renderer->drawDecals();
}

// ── End world-space overlay batch API ─────────────────────────────────────────

// Resolve a gosTextureHandle to the underlying raw GL texture name for
// consumers outside this TU (gos_static_prop_batcher). Returns 0 on
// invalid handle — caller should treat 0 as "unbind / default white".
uint32_t gos_GetGLTextureId(uint32_t gosHandle) {
    if (gosHandle == INVALID_TEXTURE_ID || !g_gos_renderer) return 0;
    // Reject uninitialized / sentinel handles (seen in the wild on
    // TG_TinyTexture entries that were never loaded: 0xFFFFFFFF).
    // Bound by the current texture list size to avoid the hard assert
    // inside gosRenderer::getTexture(texture_id >= list size).
    if (gosHandle >= g_gos_renderer->getTextureListSize()) return 0;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    return tex ? tex->getTextureId() : 0;
}

const float* gos_GetTerrainViewportVec4() {
    if (!g_gos_renderer) return nullptr;
    return (const float*)&g_gos_renderer->getTerrainViewport();
}

const float* gos_GetProj2ScreenMat4() {
    if (!g_gos_renderer) return nullptr;
    return (const float*)&g_gos_renderer->getProj2Screen();
}

const float* gos_GetTerrainMVPMat4() {
    if (!g_gos_renderer || !g_gos_renderer->isTerrainMVPValid()) return nullptr;
    return (const float*)&g_gos_renderer->getTerrainMVP();
}

#include "gameos_graphics_debug.cpp"
