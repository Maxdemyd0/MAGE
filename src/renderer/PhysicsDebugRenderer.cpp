#include "renderer/PhysicsDebugRenderer.h"

#ifdef JPH_DEBUG_RENDERER
#include "renderer/Renderer.h"

namespace mage {

namespace {
glm::vec3 ToGlm(JPH::RVec3Arg v) {
    return glm::vec3(static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ()));
}
glm::vec3 ToGlm(JPH::ColorArg c) {
    return glm::vec3(c.r, c.g, c.b) / 255.0f;
}
} // namespace

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) {
    if (m_target) m_target->DrawLine(ToGlm(from), ToGlm(to), ToGlm(color));
}

void PhysicsDebugRenderer::DrawTriangle(JPH::RVec3Arg v1, JPH::RVec3Arg v2, JPH::RVec3Arg v3, JPH::ColorArg color,
                                        ECastShadow) {
    if (!m_target) return;
    const glm::vec3 a = ToGlm(v1), b = ToGlm(v2), c = ToGlm(v3), col = ToGlm(color);
    m_target->DrawLine(a, b, col);
    m_target->DrawLine(b, c, col);
    m_target->DrawLine(c, a, col);
}

void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) {
    // Text is not rendered in-world in v1 (ImGui overlays show the same data).
}

} // namespace mage
#endif
