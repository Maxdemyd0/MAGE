// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - minimal, self-contained OpenGL 4.x core function loader.
// Only the entry points the renderer actually uses are loaded (via glfwGetProcAddress).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define MAGE_GLAPI __stdcall
#else
#define MAGE_GLAPI
#endif

namespace mage::gl {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLDEBUGPROC = void(MAGE_GLAPI*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                      const GLchar* message, const void* userParam);

// ---- constants ----------------------------------------------------------------
inline constexpr GLenum GL_FALSE = 0;
inline constexpr GLenum GL_TRUE = 1;
inline constexpr GLenum GL_NONE = 0;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE_BORDER_COLOR = 0x1004;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;
inline constexpr GLenum GL_CLAMP_TO_BORDER = 0x812D;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
inline constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
inline constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
inline constexpr GLenum GL_COMPARE_REF_TO_TEXTURE = 0x884E;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
inline constexpr GLenum GL_DEBUG_SEVERITY_HIGH = 0x9146;
inline constexpr GLenum GL_DEBUG_SEVERITY_MEDIUM = 0x9147;
inline constexpr GLenum GL_DEBUG_SEVERITY_LOW = 0x9148;
inline constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
inline constexpr GLenum GL_DEBUG_SEVERITY_NOTIFICATION = 0x826B;

// ---- entry points (X-macro: return type, name without "gl", parameter list) ----
#define MAGE_GL_REQUIRED_FUNCTIONS(X)                                                                         \
    X(void, Clear, (GLbitfield mask))                                                                         \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                         \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                               \
    X(void, Enable, (GLenum cap))                                                                             \
    X(void, Disable, (GLenum cap))                                                                            \
    X(void, DepthFunc, (GLenum func))                                                                         \
    X(void, DepthMask, (GLboolean flag))                                                                      \
    X(void, CullFace, (GLenum mode))                                                                          \
    X(void, BlendFunc, (GLenum s, GLenum d))                                                                  \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                   \
    X(void, LineWidth, (GLfloat width))                                                                       \
    X(void, DrawBuffer, (GLenum buf))                                                                         \
    X(void, ReadBuffer, (GLenum buf))                                                                         \
    X(const GLubyte*, GetString, (GLenum name))                                                               \
    X(void, GetIntegerv, (GLenum pname, GLint* data))                                                         \
    X(void, ActiveTexture, (GLenum texture))                                                                  \
    X(GLuint, CreateShader, (GLenum type))                                                                    \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))   \
    X(void, CompileShader, (GLuint shader))                                                                   \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint* params))                                        \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei maxLength, GLsizei* length, GLchar* log))               \
    X(void, DeleteShader, (GLuint shader))                                                                    \
    X(GLuint, CreateProgram, (void))                                                                          \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                    \
    X(void, LinkProgram, (GLuint program))                                                                    \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint* params))                                      \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei maxLength, GLsizei* length, GLchar* log))             \
    X(void, DeleteProgram, (GLuint program))                                                                  \
    X(void, UseProgram, (GLuint program))                                                                     \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                                        \
    X(void, Uniform1i, (GLint loc, GLint v))                                                                  \
    X(void, Uniform1f, (GLint loc, GLfloat v))                                                                \
    X(void, Uniform2f, (GLint loc, GLfloat x, GLfloat y))                                                     \
    X(void, Uniform3fv, (GLint loc, GLsizei count, const GLfloat* v))                                         \
    X(void, Uniform4fv, (GLint loc, GLsizei count, const GLfloat* v))                                         \
    X(void, UniformMatrix3fv, (GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v))              \
    X(void, UniformMatrix4fv, (GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v))              \
    X(void, GenVertexArrays, (GLsizei n, GLuint* arrays))                                                     \
    X(void, BindVertexArray, (GLuint array))                                                                  \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                            \
    X(void, GenBuffers, (GLsizei n, GLuint* buffers))                                                         \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                       \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                     \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))               \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                                \
    X(void, EnableVertexAttribArray, (GLuint index))                                                          \
    X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr)) \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))                     \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                            \
    X(void, GenFramebuffers, (GLsizei n, GLuint* ids))                                                        \
    X(void, BindFramebuffer, (GLenum target, GLuint fbo))                                                     \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level)) \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                        \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* ids))                                               \
    X(void, GenTextures, (GLsizei n, GLuint* textures))                                                       \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                     \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* data)) \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* data)) \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                        \
    X(void, TexParameterfv, (GLenum target, GLenum pname, const GLfloat* params))                             \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                              \
    X(void, GenRenderbuffers, (GLsizei n, GLuint* ids))                                                       \
    X(void, BindRenderbuffer, (GLenum target, GLuint rb))                                                     \
    X(void, RenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h))                        \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbTarget, GLuint rb))          \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* ids))                                                  X(void, PixelStorei, (GLenum pname, GLint param))                                                             X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data))

#define MAGE_GL_OPTIONAL_FUNCTIONS(X) \
    X(void, DebugMessageCallback, (GLDEBUGPROC callback, const void* userParam))

#define MAGE_GL_DECLARE(ret, name, args)        \
    using PFN_gl##name = ret(MAGE_GLAPI*) args; \
    extern PFN_gl##name gl##name;
MAGE_GL_REQUIRED_FUNCTIONS(MAGE_GL_DECLARE)
MAGE_GL_OPTIONAL_FUNCTIONS(MAGE_GL_DECLARE)
#undef MAGE_GL_DECLARE

using GLProcLoader = void* (*)(const char* name);

// Loads every entry point. Returns false if a required function is missing.
bool LoadFunctions(GLProcLoader loader);

} // namespace mage::gl
