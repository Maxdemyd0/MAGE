// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using Mage;

namespace GameScripts
{
    /// <summary>
    /// The player character: walks relative to the camera, jumps, and enters or leaves the car.
    /// Controls: W/A/S/D or arrows = run, Left Shift = sprint, Left Alt = walk, Space = jump, F = enter / leave the car.
    /// </summary>
    public sealed class PlayerScript : Entity
    {
        public float WalkSpeed = 1.5f;
        public float RunSpeed = 3.6f;
        public float SprintSpeed = 5.8f;
        public float EnterDistance = 3.5f;
        public float MaxExitSpeedKmh = 15f;

        private CharacterController _character = null!;
        private bool _jump;
        private bool _driving;

        public override void OnCreate()
        {
            _character = new CharacterController(this);
            _driving = !_character.State.Enabled;
            if (TestVehicleScript.Instance != null) TestVehicleScript.Instance.HasDriver = _driving;
            if (!_driving) Camera.Target = ID;
        }

        public override void OnUpdate(float dt)
        {
            TestVehicleScript? car = TestVehicleScript.Instance;
            if (Input.IsKeyPressed(KeyCode.F) && car != null)
            {
                if (_driving) LeaveCar(car);
                else EnterCar(car);
            }
            if (_driving) return;
            if (Input.IsKeyPressed(KeyCode.Space)) _jump = true;
            if (car != null && CanEnter(car)) Text.Draw("F  enter car", 20f, 150f, 22f);
        }

        public override void OnFixedUpdate(float dt)
        {
            if (_driving) return;
            Vector3 forward = Camera.Forward;
            forward = new Vector3(forward.X, 0f, forward.Z).Normalized;
            Vector3 right = new(-forward.Z, 0f, forward.X);
            Vector3 move = forward * Input.GetAxis(KeyCode.S, KeyCode.W, KeyCode.Down, KeyCode.Up) +
                           right * Input.GetAxis(KeyCode.A, KeyCode.D, KeyCode.Left, KeyCode.Right);
            float speed = Input.IsKeyDown(KeyCode.LeftShift) ? SprintSpeed : Input.IsKeyDown(KeyCode.LeftAlt) ? WalkSpeed : RunSpeed;
            _character.Move(move.LengthSquared > 0.01f ? move.Normalized * speed : default, _jump);
            _jump = false;

            CharacterState state = _character.State;
            if (state.LandingSpeed > 9f) _character.KnockDown(new Vector3(state.Velocity.X, 0f, state.Velocity.Z));
            if (Position.Y < -50f) SetTransform(new Vector3(0f, 2f, -24f), Quaternion.Identity);
        }

        private bool CanEnter(TestVehicleScript car) =>
            !_character.State.KnockedDown && Vector3.Distance(Position, car.Position) < EnterDistance;

        private void EnterCar(TestVehicleScript car)
        {
            if (!CanEnter(car)) return;
            _character.Disable();
            _driving = true;
            car.HasDriver = true;
            Camera.Target = car.ID;
        }

        private void LeaveCar(TestVehicleScript car)
        {
            if (car.SpeedKmh > MaxExitSpeedKmh) return;
            // Driver's side first, then the other side, behind and in front, and finally on the roof.
            Vector3 left = car.Right * -1f;
            Vector3[] spots =
            {
                left * 1.5f, car.Right * 1.5f, car.Forward * -3.2f, car.Forward * 3.2f, new Vector3(0f, 1.6f, 0f),
            };
            foreach (Vector3 offset in spots)
            {
                Vector3 from = car.Position + offset + new Vector3(0f, 1f, 0f);
                // Nothing (like a wall) between the seat and the spot.
                Vector3 toSpot = offset;
                if (Physics.Raycast(car.Position + new Vector3(0f, 0.6f, 0f), toSpot.Normalized, toSpot.Length, out _, ignore: car.ID)) continue;
                if (!Physics.Raycast(from, Vector3.Down, 4f, out RaycastHit ground)) continue;
                SetTransform(ground.Point, Quaternion.AngleAxis(car.Rotation.Yaw, Vector3.Up));
                if (!_character.Enable(ground.Point + new Vector3(0f, 0.02f, 0f))) continue;
                _driving = false;
                car.HasDriver = false;
                Camera.Target = ID;
                return;
            }
        }
    }
}
