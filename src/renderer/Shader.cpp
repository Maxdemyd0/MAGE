// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "renderer/Shader.h"
#include "renderer/GL.h"
#include "core/Log.h"

#include <glm/gtc/type_ptr.hpp>
#include <vector>

namespace mage {

using namespace gl;

namespace {
GLuint Compile(const char* name, GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len > 1 ? len : 1));
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        Log::Error("Shader '{}' ({}) failed to compile:\n{}", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

Shader::~Shader() { Destroy(); }

bool Shader::Create(const char* name, const char* vertexSource, const char* fragmentSource) {
    Destroy();
    m_name = name;
    const GLuint vs = Compile(name, GL_VERTEX_SHADER, vertexSource);
    const GLuint fs = Compile(name, GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<size_t>(len > 1 ? len : 1));
        glGetProgramInfoLog(m_program, len, nullptr, log.data());
        Log::Error("Shader '{}' failed to link:\n{}", name, log.data());
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    return true;
}

void Shader::Destroy() {
    if (m_program && glDeleteProgram) glDeleteProgram(m_program);
    m_program = 0;
    m_locations.clear();
}

void Shader::Bind() const { glUseProgram(m_program); }

int Shader::Location(const char* uniform) {
    auto it = m_locations.find(uniform);
    if (it != m_locations.end()) return it->second;
    const int loc = glGetUniformLocation(m_program, uniform);
    m_locations.emplace(uniform, loc);
    return loc;
}

void Shader::SetInt(const char* n, int v) { glUniform1i(Location(n), v); }
void Shader::SetFloat(const char* n, float v) { glUniform1f(Location(n), v); }
void Shader::SetVec2(const char* n, const glm::vec2& v) { glUniform2f(Location(n), v.x, v.y); }
void Shader::SetVec3(const char* n, const glm::vec3& v) { glUniform3fv(Location(n), 1, glm::value_ptr(v)); }
void Shader::SetVec4(const char* n, const glm::vec4& v) { glUniform4fv(Location(n), 1, glm::value_ptr(v)); }
void Shader::SetMat3(const char* n, const glm::mat3& m) { glUniformMatrix3fv(Location(n), 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetMat4(const char* n, const glm::mat4& m) { glUniformMatrix4fv(Location(n), 1, GL_FALSE, glm::value_ptr(m)); }

} // namespace mage
