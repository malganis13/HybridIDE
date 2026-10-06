// =============================================================================
//  GLLoader.hpp — минимальный загрузчик OpenGL 3.3 Core.
//  Загружает только используемые функции через переданный getProcAddress
//  (glfwGetProcAddress), поэтому не требует glad/glew и не тянет заголовки
//  оконной системы в ядро IDE.
// =============================================================================
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(__MINGW32__)
#  define IDE_GLAPI __stdcall
#elif defined(_WIN32)
#  define IDE_GLAPI __attribute__((__stdcall__))
#else
#  define IDE_GLAPI
#endif

namespace ide::gl {

using GLenum = unsigned int;   using GLuint = unsigned int;   using GLint = int;
using GLsizei = int;           using GLfloat = float;         using GLboolean = unsigned char;
using GLchar = char;           using GLbitfield = unsigned int;
using GLsizeiptr = std::ptrdiff_t;

// Константы (подмножество GL 3.3 Core)
constexpr GLenum FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82;
constexpr GLenum INFO_LOG_LENGTH = 0x8B84, ARRAY_BUFFER = 0x8892, STATIC_DRAW = 0x88E4, DYNAMIC_DRAW = 0x88E8, STREAM_DRAW = 0x88E0;
constexpr GLenum FLOAT = 0x1406, UNSIGNED_BYTE = 0x1401, TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005;
constexpr GLenum FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5, FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE1 = 0x84C1, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, LINEAR = 0x2601, NEAREST = 0x2600, CLAMP_TO_EDGE = 0x812F, REPEAT = 0x2901;
constexpr GLenum RGBA = 0x1908, RGBA8 = 0x8058, RED = 0x1903, R32F = 0x822E, R16F = 0x822D;
constexpr GLenum COLOR_BUFFER_BIT = 0x4000, BLEND = 0x0BE2, SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, ONE = 1;
constexpr GLenum DEPTH_TEST = 0x0B71, SCISSOR_TEST = 0x0C11, CULL_FACE = 0x0B44, UNPACK_ALIGNMENT = 0x0CF5, VIEWPORT = 0x0BA2;

#define IDE_GL_FUNCS(X) \
    X(GLuint, CreateShader, GLenum) \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*) \
    X(void, CompileShader, GLuint) \
    X(void, GetShaderiv, GLuint, GLenum, GLint*) \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
    X(GLuint, CreateProgram) \
    X(void, AttachShader, GLuint, GLuint) \
    X(void, LinkProgram, GLuint) \
    X(void, GetProgramiv, GLuint, GLenum, GLint*) \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*) \
    X(void, DeleteShader, GLuint) \
    X(void, DeleteProgram, GLuint) \
    X(void, UseProgram, GLuint) \
    X(GLint, GetUniformLocation, GLuint, const GLchar*) \
    X(void, Uniform1f, GLint, GLfloat) \
    X(void, Uniform2f, GLint, GLfloat, GLfloat) \
    X(void, Uniform3f, GLint, GLfloat, GLfloat, GLfloat) \
    X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, Uniform1i, GLint, GLint) \
    X(void, Uniform4fv, GLint, GLsizei, const GLfloat*) \
    X(void, GenVertexArrays, GLsizei, GLuint*) \
    X(void, BindVertexArray, GLuint) \
    X(void, DeleteVertexArrays, GLsizei, const GLuint*) \
    X(void, GenBuffers, GLsizei, GLuint*) \
    X(void, BindBuffer, GLenum, GLuint) \
    X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum) \
    X(void, DeleteBuffers, GLsizei, const GLuint*) \
    X(void, EnableVertexAttribArray, GLuint) \
    X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) \
    X(void, DrawArrays, GLenum, GLint, GLsizei) \
    X(void, GenFramebuffers, GLsizei, GLuint*) \
    X(void, BindFramebuffer, GLenum, GLuint) \
    X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    X(GLenum, CheckFramebufferStatus, GLenum) \
    X(void, DeleteFramebuffers, GLsizei, const GLuint*) \
    X(void, GenTextures, GLsizei, GLuint*) \
    X(void, BindTexture, GLenum, GLuint) \
    X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) \
    X(void, TexParameteri, GLenum, GLenum, GLint) \
    X(void, DeleteTextures, GLsizei, const GLuint*) \
    X(void, ActiveTexture, GLenum) \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei) \
    X(void, Clear, GLbitfield) \
    X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat) \
    X(void, Enable, GLenum) \
    X(void, Disable, GLenum) \
    X(void, BlendFunc, GLenum, GLenum) \
    X(void, PixelStorei, GLenum, GLint) \
    X(void, GetIntegerv, GLenum, GLint*)

#define IDE_GL_DECLARE(ret, name, ...) using PFN_##name = ret(IDE_GLAPI*)(__VA_ARGS__); extern PFN_##name name;
IDE_GL_FUNCS(IDE_GL_DECLARE)
#undef IDE_GL_DECLARE

using GetProcFn = void* (*)(const char*);
// Загружает все функции; возвращает false и имя первой отсутствующей
bool load(GetProcFn getProc, const char** missing = nullptr);
bool loaded();

} // namespace ide::gl
