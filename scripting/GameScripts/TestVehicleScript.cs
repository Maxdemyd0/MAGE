// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using Mage;

namespace GameScripts
{
    /// <summary>
    /// Drives the testbed's 4-wheel Jolt raycast vehicle from C#.
    ///
    ///   C#  : reads input, smooths steering, decides brake-vs-reverse, auto-recovers when flipped
    ///   C++ : Jolt simulates suspension / tires / drivetrain at 60 Hz and renders everything
    ///
    /// Controls: W/S or Up/Down = throttle / brake-reverse, A/D or Left/Right = steer,
    ///           Space = handbrake, R = reset, E = spawn a crate in front of the car.
    /// </summary>
    public sealed class TestVehicleScript : Entity
    {
        // Tunables (plain fields: easy to tweak and hot-recompile).
        public float SteerSpeed = 3.0f;           // steering units / second
        public float SteerReturnSpeed = 5.0f;     // faster centring when the key is released
        public float HighSpeedSteerFactor = 0.45f; // steering authority left at 120 km/h
        public float FlippedResetDelay = 2.5f;    // seconds upside-down before auto reset

        private VehicleController _vehicle = null!;
        private Vector3 _spawnPosition;
        private Quaternion _spawnRotation;
        private float _steer;
        private float _previousForward = 1f;
        private float _flippedTime;
        private float _telemetryTimer;
        private int _spawnedCrates;

        public override void OnCreate()
        {
            _vehicle = new VehicleController(this);
            _spawnPosition = Position;
            _spawnRotation = Rotation;
            Log.Info($"TestVehicleScript attached to entity {ID} at {_spawnPosition}");
        }

        public override void OnUpdate(float dt)
        {
            if (Input.IsKeyPressed(KeyCode.R)) ResetVehicle("manual reset");
            if (Input.IsKeyPressed(KeyCode.E)) SpawnCrate();

            // Periodic telemetry to the engine console.
            _telemetryTimer += dt;
            if (_telemetryTimer >= 10f)
            {
                _telemetryTimer = 0f;
                VehicleState s = _vehicle.State;
                Log.Trace($"[Vehicle] {s.SpeedKmh:0} km/h, gear {s.Gear}, {s.EngineRPM:0} rpm, wheels down {s.WheelsInContact}/4");
            }
        }

        public override void OnFixedUpdate(float dt)
        {
            float forward = Input.GetAxis(KeyCode.S, KeyCode.W, KeyCode.Down, KeyCode.Up);
            float right = Input.GetAxis(KeyCode.A, KeyCode.D, KeyCode.Left, KeyCode.Right);
            float handBrake = Input.IsKeyDown(KeyCode.Space) ? 1f : 0f;
            float brake = 0f;

            VehicleState state = _vehicle.State;

            // Pressing the opposite direction while still rolling = brake, then reverse once stopped.
            if (_previousForward * forward < 0f &&
                ((forward > 0f && state.ForwardSpeed < -0.1f) || (forward < 0f && state.ForwardSpeed > 0.1f)))
            {
                forward = 0f;
                brake = 1f;
            }
            else if (forward != 0f)
            {
                _previousForward = forward;
            }

            // Speed-sensitive, rate-limited steering.
            float speedFactor = 1f - (1f - HighSpeedSteerFactor) * Math.Clamp(state.SpeedKmh / 120f, 0f, 1f);
            float target = right * speedFactor;
            float rate = MathF.Abs(target) < MathF.Abs(_steer) ? SteerReturnSpeed : SteerSpeed;
            _steer = MoveTowards(_steer, target, rate * dt);

            _vehicle.SetInput(forward, _steer, brake, handBrake);

            // Auto-recover when the car is on its roof (checked with the car's up vector + a ground probe).
            bool upsideDown = Up.Y < 0.25f;
            bool grounded = Raycast(Position, Vector3.Down, 3f, out _);
            if (upsideDown && grounded && state.SpeedKmh < 5f)
            {
                _flippedTime += dt;
                if (_flippedTime > FlippedResetDelay) ResetVehicle("flipped");
            }
            else
            {
                _flippedTime = 0f;
            }

            // Fell off the world.
            if (Position.Y < -50f) ResetVehicle("fell out of the world");
        }

        public override void OnDestroy()
        {
            Log.Info($"TestVehicleScript on entity {ID} destroyed ({_spawnedCrates} crates spawned)");
        }

        private void ResetVehicle(string reason)
        {
            // Keep the current heading when flipping back over, otherwise return to spawn.
            if (reason == "flipped")
            {
                Quaternion upright = Quaternion.AngleAxis(Rotation.Yaw, Vector3.Up);
                SetTransform(Position + new Vector3(0f, 1.5f, 0f), upright);
            }
            else
            {
                SetTransform(_spawnPosition, _spawnRotation);
            }
            _steer = 0f;
            _flippedTime = 0f;
            Log.Info($"Vehicle reset ({reason})");
        }

        private void SpawnCrate()
        {
            Vector3 spawn = Position + Forward * 7f + new Vector3(0f, 2.5f, 0f);
            // Drop the crate on whatever is below the spawn point.
            if (Physics.Raycast(spawn, Vector3.Down, 20f, out RaycastHit hit))
                spawn = hit.Point + new Vector3(0f, 2f, 0f);
            uint crate = Spawn("cube", spawn, Quaternion.AngleAxis(Rotation.Yaw, Vector3.Up), new Vector3(1.2f), dynamic: true);
            _spawnedCrates++;
            Log.Info($"Spawned crate entity {crate} at {spawn}");
        }

        private static float MoveTowards(float current, float target, float maxDelta) =>
            MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;
    }
}
