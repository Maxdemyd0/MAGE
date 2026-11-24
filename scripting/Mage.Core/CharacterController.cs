// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using Mage.Interop;

namespace Mage
{
    /// <summary>State of a walking character after the last physics step.</summary>
    public readonly struct CharacterState
    {
        public readonly Vector3 Velocity;
        public readonly Vector3 GroundNormal;
        /// <summary>Above zero only in the step that lands (m/s); use it for fall damage.</summary>
        public readonly float LandingSpeed;
        /// <summary>Velocity change caused by a moving body that hit the character in this step (m/s).</summary>
        public readonly float ImpactSpeed;
        public readonly uint ImpactEntity;
        private readonly int _flags;

        internal CharacterState(in NativeCharacterState s)
        {
            Velocity = s.Velocity;
            GroundNormal = s.GroundNormal;
            LandingSpeed = s.LandingSpeed;
            ImpactSpeed = s.ImpactSpeed;
            ImpactEntity = s.ImpactEntity;
            _flags = s.Flags;
        }

        public bool Enabled => (_flags & 1) != 0;
        public bool Grounded => (_flags & 2) != 0;
        /// <summary>On ground that is too steep to stand on.</summary>
        public bool Sliding => (_flags & 4) != 0;
        public bool KnockedDown => (_flags & 8) != 0;
    }

    /// <summary>Moves a character entity (spawned with <see cref="ModelPhysics.Character"/>).</summary>
    public sealed class CharacterController
    {
        public uint EntityId { get; }

        public CharacterController(uint entity) { EntityId = entity; }
        public CharacterController(Entity owner) : this(owner.ID) { }

        public CharacterState State
        {
            get
            {
                InternalCalls.GetCharacterState(EntityId, out NativeCharacterState s);
                return new CharacterState(s);
            }
        }

        /// <summary>Desired horizontal world velocity (m/s). Stays in effect until changed; the jump happens once.</summary>
        public void Move(Vector3 velocity, bool jump = false) => InternalCalls.SetCharacterMove(EntityId, velocity, jump);

        /// <summary>Removes and hides the character, for example while it sits in a car.</summary>
        public void Disable() => InternalCalls.SetCharacterEnabled(EntityId, false, default);

        /// <summary>Puts the character's feet at a position. False if there is no room for it.</summary>
        public bool Enable(Vector3 feet) => InternalCalls.SetCharacterEnabled(EntityId, true, feet);

        /// <summary>Throws the character over; it has no control until it gets up.</summary>
        public void KnockDown(Vector3 velocity, float seconds = 0f) => InternalCalls.KnockDown(EntityId, velocity, seconds);

        /// <summary>Set false to play your own clips with <see cref="Animator"/>.</summary>
        public void SetAutoAnimate(bool enabled) => InternalCalls.SetCharacterOptions(EntityId, -1f, -1f, enabled ? 1 : 0);

        /// <param name="jumpSpeed">Take-off speed in m/s.</param>
        /// <param name="knockdownSpeed">Hits stronger than this (m/s) knock the character over.</param>
        public void Configure(float jumpSpeed, float knockdownSpeed) => InternalCalls.SetCharacterOptions(EntityId, jumpSpeed, knockdownSpeed, -1);
    }

    /// <summary>The game camera. It follows a character or a vehicle.</summary>
    public static class Camera
    {
        public static Vector3 Position { get { InternalCalls.GetCamera(out Vector3 p, out _); return p; } }
        public static Vector3 Forward { get { InternalCalls.GetCamera(out _, out Vector3 f); return f; } }
        public static uint Target { get => InternalCalls.GetCamera(out _, out _); set => InternalCalls.SetCameraTarget(value); }
    }
}
