using Mage.Interop;

namespace Mage
{
    /// <summary>Snapshot of a vehicle's drivetrain state (from Jolt's WheeledVehicleController).</summary>
    public readonly struct VehicleState
    {
        public readonly float SpeedKmh;
        /// <summary>Signed speed along the chassis forward axis (m/s); negative when reversing.</summary>
        public readonly float ForwardSpeed;
        public readonly float EngineRPM;
        public readonly int Gear;
        public readonly int WheelsInContact;

        internal VehicleState(in NativeVehicleState s)
        {
            SpeedKmh = s.SpeedKmh;
            ForwardSpeed = s.ForwardSpeed;
            EngineRPM = s.EngineRPM;
            Gear = s.Gear;
            WheelsInContact = s.WheelsInContact;
        }

        public bool IsAirborne => WheelsInContact == 0;
    }

    /// <summary>
    /// Drives a native raycast vehicle (the entity must own a VehicleComponent).
    /// Inputs are latched and consumed by the engine on the next 60 Hz physics step.
    /// </summary>
    public sealed class VehicleController
    {
        public Entity Owner { get; }

        public float Throttle { get; private set; }
        public float Steering { get; private set; }
        public float Brake { get; private set; }
        public float HandBrake { get; private set; }

        public VehicleController(Entity owner) { Owner = owner; }

        /// <param name="throttle">-1..1, negative = reverse</param>
        /// <param name="steering">-1..1, positive = right</param>
        /// <param name="brake">0..1</param>
        /// <param name="handBrake">0..1 (rear wheels)</param>
        public void SetInput(float throttle, float steering, float brake, float handBrake)
        {
            Throttle = Clamp(throttle, -1f, 1f);
            Steering = Clamp(steering, -1f, 1f);
            Brake = Clamp(brake, 0f, 1f);
            HandBrake = Clamp(handBrake, 0f, 1f);
            InternalCalls.ApplyVehicleInput(Owner.ID, Throttle, Steering, Brake, HandBrake);
        }

        public VehicleState State
        {
            get
            {
                InternalCalls.GetVehicleState(Owner.ID, out NativeVehicleState s);
                return new VehicleState(s);
            }
        }

        private static float Clamp(float v, float min, float max) => v < min ? min : v > max ? max : v;
    }
}
