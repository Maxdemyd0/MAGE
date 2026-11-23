// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - JPH::DebugRenderer implementation that turns Jolt debug geometry into
// renderer debug lines (wireframes of bodies, constraints and vehicle suspension).

#include <Jolt/Jolt.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>

namespace mage {

class Renderer;

class PhysicsDebugRenderer final : public JPH::DebugRendererSimple {
public:
    void SetTarget(Renderer* renderer) { m_target = renderer; }

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override;
    void DrawTriangle(JPH::RVec3Arg v1, JPH::RVec3Arg v2, JPH::RVec3Arg v3, JPH::ColorArg color,
                      ECastShadow castShadow) override;
    void DrawText3D(JPH::RVec3Arg position, const JPH::string_view& text, JPH::ColorArg color, float height) override;

private:
    Renderer* m_target = nullptr;
};

} // namespace mage
#endif
