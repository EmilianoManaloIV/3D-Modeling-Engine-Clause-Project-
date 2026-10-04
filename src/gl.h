#pragma once
// Tiny OpenGL 3.3 core loader. Only the functions this program uses are
// declared; they live in namespace `gl` (gl::DrawArrays, ...) and are fetched
// at startup through platform::getProcAddress. No system GL header is needed.
#include <cstddef>
#include <string>

#if defined(_WIN32)
#define MODELER_GLAPI __stdcall
#else
#define MODELER_GLAPI
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef std::ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_MULTISAMPLE 0x809D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_R8 0x8229
#define GL_TEXTURE0 0x84C0
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

// X(return type, name without "gl" prefix, parameter list)
#define MODELER_GL_FUNCTIONS(X)                                                                              \
    X(void, Clear, (GLbitfield mask))                                                                        \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                       \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                             \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                              \
    X(void, Enable, (GLenum cap))                                                                            \
    X(void, Disable, (GLenum cap))                                                                           \
    X(void, BlendFunc, (GLenum sfactor, GLenum dfactor))                                                     \
    X(void, DepthFunc, (GLenum func))                                                                        \
    X(void, DepthMask, (GLboolean flag))                                                                     \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                  \
    X(const GLubyte*, GetString, (GLenum name))                                                              \
    X(GLenum, GetError, (void))                                                                              \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                        \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data))   \
    X(void, Finish, (void))                                                                                  \
    X(void, GenTextures, (GLsizei n, GLuint * textures))                                                     \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                             \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                    \
    X(void, ActiveTexture, (GLenum texture))                                                                 \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, \
                         GLenum format, GLenum type, const void* pixels))                                    \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                       \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                           \
    X(void, GenBuffers, (GLsizei n, GLuint * buffers))                                                       \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                               \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                      \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                    \
    X(void, GenVertexArrays, (GLsizei n, GLuint * arrays))                                                   \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                           \
    X(void, BindVertexArray, (GLuint array))                                                                 \
    X(void, EnableVertexAttribArray, (GLuint index))                                                         \
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, \
                                  const void* pointer))                                                      \
    X(GLuint, CreateShader, (GLenum type))                                                                   \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))  \
    X(void, CompileShader, (GLuint shader))                                                                  \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint * params))                                      \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog))          \
    X(void, DeleteShader, (GLuint shader))                                                                   \
    X(GLuint, CreateProgram, (void))                                                                         \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                   \
    X(void, LinkProgram, (GLuint program))                                                                   \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint * params))                                    \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog))        \
    X(void, DeleteProgram, (GLuint program))                                                                 \
    X(void, UseProgram, (GLuint program))                                                                    \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                                       \
    X(void, Uniform1i, (GLint location, GLint v0))                                                           \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                                         \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                             \
    X(void, Uniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                                 \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                     \
    X(void, UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))

namespace gl {
#define MODELER_GL_DECLARE(ret, name, params)              \
    typedef ret(MODELER_GLAPI* PFN_##name) params;         \
    extern PFN_##name name;
MODELER_GL_FUNCTIONS(MODELER_GL_DECLARE)
#undef MODELER_GL_DECLARE

// Loads every function; on failure `missing` lists the ones not found.
bool load(std::string& missing);
}  // namespace gl
