// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using Mage;

namespace GameScripts
{
    /// <summary>Strolls between random places, pauses, and runs from a car that comes close fast.</summary>
    public sealed class PedestrianScript : Entity
    {
        public float WanderRadius = 25f;
        public float WalkSpeed = 1.4f;
        public float FleeSpeed = 4.2f;
        public float DangerDistance = 12f;
        public float DangerSpeedKmh = 15f;

        private static readonly Random s_random = new();
        private NavAgent _agent = null!;
        private CharacterController _character = null!;
        private float _pause;
        private float _flee;

        public override void OnCreate()
        {
            _agent = new NavAgent(this);
            _character = new CharacterController(this);
            WalkSpeed *= 0.85f + 0.3f * s_random.NextSingle();
            _pause = s_random.NextSingle();
        }

        public override void OnFixedUpdate(float dt)
        {
            if (_character.State.KnockedDown) return;
            _flee -= dt;

            TestVehicleScript? car = TestVehicleScript.Instance;
            if (car != null && _flee <= 0f && car.SpeedKmh > DangerSpeedKmh)
            {
                Vector3 away = Position - car.Position;
                away = new Vector3(away.X, 0f, away.Z);
                // Only if it is heading this way.
                if (away.Length < DangerDistance && Vector3.Dot(away.Normalized, car.Forward) > 0.3f && Flee(away, car))
                {
                    _flee = 1.5f;
                    return;
                }
            }
            if (_flee > 0f) return;

            switch (_agent.Status)
            {
                case NavStatus.Moving:
                case NavStatus.Waiting:
                    return;
                default:
                    _pause -= dt;
                    if (_pause > 0f) return;
                    if (Navigation.RandomPoint(Position, WanderRadius, out Vector3 target)) _agent.MoveTo(target, WalkSpeed);
                    _pause = 0.5f + 3f * s_random.NextSingle();
                    return;
            }
        }

        private bool Flee(Vector3 away, TestVehicleScript car)
        {
            // Sideways out of its way, to the side we are already on.
            Vector3 side = car.Right * (Vector3.Dot(away, car.Right) >= 0f ? 1f : -1f);
            foreach (Vector3 direction in new[] { side, side * -1f, away.Normalized })
            {
                if (!Navigation.NearestPoint(Position + direction * 7f, out Vector3 spot)) continue;
                return _agent.MoveTo(spot, FleeSpeed);
            }
            return false;
        }
    }
}
