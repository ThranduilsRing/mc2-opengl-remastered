#ifndef __GL_UTILS_H__
#define __GL_UTILS_H__

#include <cassert>
#include <cstdio>
#include "utils/vec.h"
#include "utils/shader_builder.h"
#include "utils/render_constants.h"

#define BUFFER_OFFSET(bytes) ((GLubyte*) nullptr + (bytes))

uint32_t vec4_to_uint32(const vec4& v);
vec4 uint32_to_vec4(uint32_t v);

struct Texture {
	Texture():id(0), w(0), h(0), depth(1), fmt_(TF_NONE), has_mipmaps(false) {}
    bool isValid() { return id > 0; }

	GLuint id;
	GLenum format;
	int w, h, depth;
    TexFormat fmt_;
    TexType type_;
    bool has_mipmaps;

};

uint32_t getTexFormatPixelSize(TexFormat fmt);

#define CHECK_GL_ERROR \
{ \
   	GLenum err = glGetError();\
	while(err != GL_NO_ERROR) \
	{ \
		printf("OpenGL Error: %s\n", ogl_get_error_code_str(err)); \
		printf("Location : %s : %d\n", __FILE__ , __LINE__); \
   	    err = glGetError();\
	}\
}

template<typename T>
const char* ogl_get_error_code_str(T input)
{
    int errorCode = (int)input;
    switch(errorCode)
    {
	case GL_NO_ERROR:
            return "GL_NO_ERROR";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";               
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";           
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";      
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";                    
        case GL_STACK_OVERFLOW:
            return "GL_STACK_OVERFLOW";                 
        case GL_STACK_UNDERFLOW:
            return "GL_STACK_UNDERFLOW";        
        default:
            return "unknown error code";
    }
}


template<typename T>
static int ogl_check_val(T input, T reference, const char* message)
{
    if(input==reference)
    {
        return true;
    }
    else
    {
	printf("OpenGL Error: %s Error code: %s\n", message, ogl_get_error_code_str(input));
        return false;
    }
}

Texture create2DTexture(int w, int h, TexFormat fmt, const uint8_t* texdata, bool wantMipmaps = false);
Texture createDynamicTexture(int w, int h, TexFormat fmt);
Texture create3DTextureF(int w, int h, int depth);

// texture has to be binded before calling this
void setSamplerParams(TexType tt, TexAddressMode address_mode, TexFilterMode filter);

// pdata_format - specifies format of data provided in pdata
void updateTexture(const Texture& t, void* pdata, TexFormat pdata_format = TF_COUNT);
void destroyTexture(Texture* tex);
// fmt - desired format of returned data
void getTextureData(const Texture& t, int lod, unsigned char* poutdata, TexFormat fmt = TF_COUNT);
unsigned int getPixelSize(const TexFormat fmt);
Texture createPBO(int w, int h, GLenum fmt, int el_size);
void draw_quad(float x0, float y0, float x1, float y1);

struct glsl_program;
void applyTexture(glsl_program* program, int unit, const char* name, GLuint texid);
void applyPBO(glsl_program* program, int unit, const char* name, const Texture pbo, const Texture tex);


using render_func_t = void (*)(int w, int h, void* puserdata);

void draw_in_2d(int w, int h, render_func_t prenderfunc, void* puserdata);

GLuint makeBuffer(GLenum target, const GLvoid* buffer_data, GLsizei buffer_size, GLenum type = GL_STATIC_DRAW);
void updateBuffer(GLuint buf, GLenum target, const GLvoid* buffer_data, GLsizei buffer_size);
void updateBuffer(GLuint buf, GLenum target, const GLvoid* buffer_data, GLsizei buffer_size, GLenum type);

// BUFFERS ETC.
template <typename VERTEX> 
struct glMesh {
	GLuint	    vb_;
	GLuint	    ib_;
	GLuint		instance_vb_;
	VERTEX*	    pvertices_;
	GLsizei     num_vertices_;
	int*	    pindices_;
	int	    num_indices_;

	uint8_t*	pinstance_data_;
	int		instance_count_;
	int		instance_stride_;

	GLenum	    prim_type_;

				    glMesh();
				    ~glMesh();

	static glMesh<VERTEX>*	    makeMesh(int num_vertices, int num_indices, GLenum prim_type, int instance_array_size = 0, int instance_stride = 0);
	static void				    destroyMesh(glMesh<VERTEX>* pmesh);

	int			    gen_hw(GLenum type = GL_STREAM_DRAW);
	void			    update_hw(GLvoid* vertex_data, GLsizeiptr vsize, GLvoid* index_data, GLsizeiptr isize, GLvoid* instance_data = 0 , GLsizeiptr inst_size = 0);
	void			    update_hw(GLsizeiptr num_cvertices, GLsizeiptr num_indices, GLsizeiptr num_instances);

	using myType = glMesh<VERTEX>;

};

template <typename VERTEX>
glMesh<VERTEX>::glMesh():
vb_(0),ib_(0),instance_vb_(0), pvertices_(0),num_vertices_(0), pindices_(0), num_indices_(0), 
pinstance_data_(0), instance_count_(0), instance_stride_(0), prim_type_(0)
{
}

template <typename VERTEX>
glMesh<VERTEX>* glMesh<VERTEX>::makeMesh(int num_vertices, int num_indices, GLenum prim_type, int instance_count/* = 0*/, int instance_stride/* = 0*/)
{
	glMesh* mesh = new glMesh();
	mesh->num_vertices_ = num_vertices;
	mesh->num_indices_ = num_indices;
	if(num_vertices>0)
	    mesh->pvertices_ = new VERTEX[mesh->num_vertices_];
	if(num_indices>0)
	    mesh->pindices_ = new int[mesh->num_indices_];
	mesh->prim_type_ = prim_type;

	assert(instance_count >= 0);
	if(instance_count > 0)
	{
		assert(instance_stride!=0);

		mesh->instance_count_ = instance_count;
		mesh->instance_stride_ = instance_stride;
		mesh->pinstance_data_ = new unsigned char [mesh->instance_count_ * mesh->instance_stride_];
	}

	return mesh;
}

template <typename VERTEX>
void glMesh<VERTEX>::destroyMesh(glMesh<VERTEX>* pmesh)
{
	assert(pmesh);
	delete pmesh;
}

template <typename VERTEX>
int glMesh<VERTEX>::gen_hw(GLenum type /*= GL_STATIC_DRAW*/)
{
	if(this->pvertices_)
	{
		if(this->vb_ > 0)
			glDeleteBuffers(1, &this->vb_);
		this->vb_ = makeBuffer(GL_ARRAY_BUFFER, this->pvertices_, sizeof(VERTEX)*this->num_vertices_, type);
	}
	if(this->pindices_)
	{
		if(this->ib_ > 0)
			glDeleteBuffers(1, &this->ib_);
		this->ib_ = makeBuffer(GL_ELEMENT_ARRAY_BUFFER, this->pindices_, sizeof(int)*this->num_indices_, type);
	}

	if(this->pinstance_data_)
	{
		if(this->instance_vb_ > 0)
			glDeleteBuffers(1, &this->instance_vb_);
		this->instance_vb_ = makeBuffer(GL_ARRAY_BUFFER, this->pinstance_data_, this->instance_count_ * this->instance_stride_, type);
	}

	return 1;
};

// strange function, ..heh, ..why do I need this?
template <typename VERTEX>
void glMesh<VERTEX>::update_hw(GLvoid* vertex_data, GLsizeiptr vsize, GLvoid* index_data, GLsizeiptr isize, GLvoid* instance_data/* = 0*/, GLsizeiptr inst_count/* = 0*/)
{
	if(this->vb_!=0 && vertex_data && vsize>0)
		updateBuffer(this->vb_, GL_ARRAY_BUFFER, vertex_data, vsize, GL_DYNAMIC_DRAW);

	if(this->ib_!=0 && index_data && isize>0)
		updateBuffer(this->ib_, GL_ELEMENT_ARRAY_BUFFER, index_data, isize, GL_DYNAMIC_DRAW);

	if(this->instance_vb_!=0 && instance_data && isize>0)
	{
		assert(inst_count >= 0);
		updateBuffer(this->instance_vb_, GL_ARRAY_BUFFER, instance_data, inst_count * this->instance_stride_, GL_DYNAMIC_DRAW);
	}
}

template <typename VERTEX>
void glMesh<VERTEX>::update_hw(GLsizeiptr num_vertices, GLsizeiptr num_indices, GLsizeiptr num_instances)
{
	if(this->vb_!=0 && num_vertices>=0)
	{
		GLsizeiptr vsize = (num_vertices!=0 ? num_vertices : num_vertices_) * sizeof(VERTEX); 
		updateBuffer(this->vb_, GL_ARRAY_BUFFER, pvertices_, vsize, GL_DYNAMIC_DRAW);
	}
	if(this->ib_!=0 && num_indices>=0)
	{
		GLsizeiptr isize = (num_indices!=0 ?  num_vertices : num_indices_) * sizeof(int);
		updateBuffer(this->ib_, GL_ELEMENT_ARRAY_BUFFER, pindices_, isize, GL_DYNAMIC_DRAW);
	}
	if(this->instance_vb_!=0 && num_instances>=0)
	{
		GLsizeiptr instsize = (num_instances!=0 ?  num_instances : instance_count_) * instance_stride_;
		updateBuffer(this->instance_vb_, GL_ARRAY_BUFFER, pinstance_data_, instsize, GL_DYNAMIC_DRAW);
	}
}

template <typename VERTEX>
glMesh<VERTEX>::~glMesh()
{
	delete[] pvertices_;
	delete[] pindices_;
	delete[] pinstance_data_;
    	if(this->vb_ > 0)
		glDeleteBuffers(1, &this->vb_);
	if(this->ib_ > 0)
		glDeleteBuffers(1, &this->ib_);
}

void draw_textured_cube(GLuint textureId);

Texture load_texture_from_file(const char* texName);

#endif // __GL_UTILS_H__
