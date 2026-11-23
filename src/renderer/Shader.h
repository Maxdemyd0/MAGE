#pragma once
// MAGE v1 - GLSL program wrapper with a uniform-location cache.

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

namespace mage {

class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    bool Create(const char* name, const char* vertexSource, const char* fragmentSource);
    void Destroy();
    void Bind() const;
    bool Valid() const { return m_program != 0; }

    int Location(const char* uniform);
    void SetInt(const char* name, int v);
    void SetFloat(const char* name, float v);
    void SetVec2(const char* name, const glm::vec2& v);
    void SetVec3(const char* name, const glm::vec3& v);
    void SetVec4(const char* name, const glm::vec4& v);
    void SetMat3(const char* name, const glm::mat3& m);
    void SetMat4(const char* name, const glm::mat4& m);

private:
    unsigned int m_program = 0;
    std::string m_name;
    std::unordered_map<std::string, int> m_locations;
};

} // namespace mage
