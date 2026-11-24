// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// Part of PhysicsWorld.cpp: walking characters on JPH::CharacterVirtual.

namespace {

constexpr float kFitMargin = 0.05f;

// Capsule whose origin is at the feet.
JPH::RefConst<JPH::Shape> FeetCapsule(float radius, float height) {
    const float r = std::max(radius, 0.05f);
    const float half = std::max(height * 0.5f - r, 0.01f);
    return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, half + r, 0), JPH::Quat::sIdentity(), new JPH::CapsuleShape(half, r)).Create().Get();
}

// Characters collide with everything except the query bodies of characters (handled as character pairs).
class CharacterLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit CharacterLayerFilter(bool vehicles) : m_vehicles(vehicles) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer != ToLayer(ObjectLayer::Character) && (m_vehicles || layer != ToLayer(ObjectLayer::Vehicle));
    }

private:
    bool m_vehicles;
};

} // namespace

CharacterHandle PhysicsWorld::CreateCharacter(const CharacterDesc& desc, uint64_t userData) {
    CharacterSlot slot;
    slot.desc = desc;
    JPH::CharacterVirtualSettings s;
    const JPH::RefConst<JPH::Shape> shape = FeetCapsule(desc.radius, desc.height);
    s.mShape = shape;
    s.mInnerBodyShape = shape;
    s.mInnerBodyLayer = ToLayer(ObjectLayer::Character);
    s.mMass = std::max(desc.mass, 1.0f);
    s.mMaxStrength = desc.pushForce;
    s.mMaxSlopeAngle = glm::radians(std::clamp(desc.maxSlopeDegrees, 1.0f, 89.0f));
    s.mUp = JPH::Vec3::sAxisY();
    // Only the lower hemisphere can stand on something.
    s.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -std::max(desc.radius, 0.05f));
    s.mEnhancedInternalEdgeRemoval = true; // no tripping over triangle edges of imported meshes
    slot.character = new JPH::CharacterVirtual(&s, ToJoltR(desc.position), JPH::Quat::sIdentity(), userData, m->system.get());
    slot.character->SetCharacterVsCharacterCollision(&m->characterPairs);
    if (slot.character->GetInnerBodyID().IsInvalid()) {
        Log::Error("PhysicsWorld: out of bodies for a character");
        return InvalidCharacter;
    }
    m->characterPairs.Add(slot.character);
    for (CharacterHandle h = 0; h < m->characters.size(); ++h)
        if (!m->characters[h].character) {
            m->characters[h] = std::move(slot);
            return h;
        }
    m->characters.push_back(std::move(slot));
    return static_cast<CharacterHandle>(m->characters.size() - 1);
}

void PhysicsWorld::DestroyCharacter(CharacterHandle h) {
    CharacterSlot* c = m->GetCharacter(h);
    if (!c) return;
    m->characterPairs.Remove(c->character);
    *c = CharacterSlot{};
}

BodyHandle PhysicsWorld::GetCharacterBody(CharacterHandle h) const {
    const CharacterSlot* c = m->GetCharacter(h);
    return c ? c->character->GetInnerBodyID().GetIndexAndSequenceNumber() : InvalidBody;
}

void PhysicsWorld::SetCharacterPosition(CharacterHandle h, const glm::vec3& feet) {
    CharacterSlot* c = m->GetCharacter(h);
    if (!c) return;
    c->character->SetPosition(ToJoltR(feet));
    c->character->SetLinearVelocity(JPH::Vec3::sZero());
    c->wasGrounded = false;
    c->teleported = true;
}

void PhysicsWorld::SetCharacterVelocity(CharacterHandle h, const glm::vec3& velocity) {
    CharacterSlot* c = m->GetCharacter(h);
    if (!c) return;
    c->character->SetLinearVelocity(ToJolt(velocity));
    c->launched = velocity.y > 0.1f;
}

bool PhysicsWorld::CharacterFits(float radius, float height, const glm::vec3& feet, BodyHandle ignore) const {
    // Slightly smaller than the real capsule, so that touching the floor or a wall is fine.
    const JPH::RefConst<JPH::Shape> shape = FeetCapsule(std::max(radius - kFitMargin, 0.05f), std::max(height - 2.0f * kFitMargin, 0.2f));
    const JPH::RMat44 at = JPH::RMat44::sTranslation(ToJoltR(feet + glm::vec3(0, kFitMargin, 0)) + shape->GetCenterOfMass());
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
    m->system->GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1.0f), at, JPH::CollideShapeSettings(), at.GetTranslation(), hit, {}, {},
                                                  JPH::IgnoreSingleBodyFilter(ToBodyID(ignore)));
    return !hit.HadHit();
}

CharacterState PhysicsWorld::UpdateCharacter(CharacterHandle h, const CharacterMove& move, float dt) {
    CharacterState out;
    CharacterSlot* c = m->GetCharacter(h);
    if (!c || dt <= 0.0f) return out;
    JPH::CharacterVirtual& ch = *c->character;
    const JPH::Vec3 gravity = m->system->GetGravity();
    const CharacterLayerFilter layers(!move.passThroughVehicles);

    if (c->teleported) { // contacts of the old place mean nothing here
        ch.RefreshContacts({}, layers, {}, {}, *m->tempAllocator);
        c->teleported = false;
    }
    ch.UpdateGroundVelocity();
    const JPH::Vec3 groundVelocity = ch.GetGroundVelocity();
    JPH::Vec3 velocity = ch.GetLinearVelocity();
    const bool standing = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    // Not "standing" while moving away from the ground (start of a jump).
    const bool grounded = standing && !c->launched && velocity.GetY() - groundVelocity.GetY() < 0.1f;

    glm::vec3 desired(move.velocity.x, 0.0f, move.velocity.z);
    if (glm::any(glm::isnan(desired)) || glm::any(glm::isinf(desired))) desired = glm::vec3(0.0f);

    // Horizontal: accelerate relative to whatever we stand on.
    glm::vec3 base = grounded ? ToGlm(groundVelocity) : glm::vec3(0.0f);
    const glm::vec3 current = glm::vec3(velocity.GetX(), 0.0f, velocity.GetZ()) - glm::vec3(base.x, 0.0f, base.z);
    glm::vec3 target = desired;
    if (!grounded && glm::length(desired) < 0.01f) target = current; // keep momentum in the air
    const float maxChange = std::max(grounded ? move.acceleration : move.airAcceleration, 0.0f) * dt;
    glm::vec3 change = target - current;
    if (const float len = glm::length(change); len > maxChange) change *= maxChange / len;
    const glm::vec3 horizontal = current + change;

    JPH::Vec3 next(horizontal.x + base.x, velocity.GetY(), horizontal.z + base.z);
    bool jumped = false;
    if (grounded) {
        next.SetY(groundVelocity.GetY());
        if (move.jumpSpeed > 0.0f) {
            next.SetY(groundVelocity.GetY() + move.jumpSpeed);
            jumped = true;
        }
    }
    next += gravity * dt;
    // Do not walk up slopes that are too steep to stand on.
    next = ch.CancelVelocityTowardsSteepSlopes(next);
    const float fallSpeed = -next.GetY();
    ch.SetLinearVelocity(next);

    JPH::CharacterVirtual::ExtendedUpdateSettings ext;
    ext.mWalkStairsStepUp = JPH::Vec3(0, c->desc.stepHeight, 0);
    ext.mStickToFloorStepDown = grounded && !jumped ? JPH::Vec3(0, -c->desc.stepHeight, 0) : JPH::Vec3::sZero();
    const JPH::RVec3 before = ch.GetPosition();
    ch.ExtendedUpdate(dt, gravity, ext, {}, layers, {}, {}, *m->tempAllocator);
    c->launched = false;

    // Hits by moving bodies: how much faster than us they came in, weighted by mass.
    float strongest = 0.0f;
    const JPH::BodyInterface& bodies = m->system->GetBodyInterface();
    for (const JPH::CharacterVirtual::Contact& contact : ch.GetActiveContacts()) {
        if (!contact.mHadCollision || contact.mBodyB.IsInvalid() || contact.mIsSensorB || contact.mMotionTypeB == JPH::EMotionType::Static) continue;
        if (contact.mContactNormal.GetY() > 0.7f) continue; // standing on it
        const float closing = (contact.mLinearVelocity - next).Dot(contact.mContactNormal);
        const float approach = contact.mLinearVelocity.Dot(contact.mContactNormal); // running into a parked car is not a hit
        const float speed = std::min(closing, approach);
        if (speed <= 0.0f) continue;
        const float mass = contact.mMotionTypeB == JPH::EMotionType::Dynamic ? GetMass(contact.mBodyB.GetIndexAndSequenceNumber()) : 1e9f;
        const float delta = speed * mass / (mass + ch.GetMass());
        if (delta <= strongest) continue;
        strongest = delta;
        out.impactSpeed = delta;
        out.impactDirection = ToGlm(contact.mContactNormal);
        out.impactUserData = bodies.GetUserData(contact.mBodyB);
    }

    out.position = ToGlm(ch.GetPosition());
    out.velocity = ToGlm(JPH::Vec3(ch.GetPosition() - before)) / dt;
    out.grounded = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && !jumped;
    out.sliding = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
    out.groundVelocity = out.grounded ? ToGlm(ch.GetGroundVelocity()) : glm::vec3(0.0f);
    out.groundNormal = out.grounded || out.sliding ? ToGlm(ch.GetGroundNormal()) : glm::vec3(0, 1, 0);
    out.jumped = jumped;
    if (out.grounded && !c->wasGrounded) out.landingSpeed = std::max(fallSpeed, 0.0f);
    c->wasGrounded = out.grounded;
    return out;
}
