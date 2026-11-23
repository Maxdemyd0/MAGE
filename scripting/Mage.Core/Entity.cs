// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using Mage.Interop;

namespace Mage
{
    /// <summary>
    /// Base class for every gameplay script. Attach a script to an entity from C++ with a
    /// <c>ScriptComponent{ "Namespace.ClassName" }</c>; the engine then calls the lifecycle hooks.
    /// </summary>
    public abstract class Entity
    {
        /// <summary>Engine entity handle (index + generation).</summary>
        public uint ID { get; internal set; } = InternalCalls.InvalidEntity;

        protected Entity() { }

        // ---- Lifecycle -------------------------------------------------------------
        /// <summary>Called once, right after the script is attached.</summary>
        public virtual void OnCreate() { }
        /// <summary>Called once per rendered frame (variable rate).</summary>
        public virtual void OnUpdate(float dt) { }
        /// <summary>Called at the fixed 60 Hz simulation rate, before the physics step.</summary>
        public virtual void OnFixedUpdate(float fixedDt) { }
        /// <summary>Called when the entity is destroyed or the runtime shuts down.</summary>
        public virtual void OnDestroy() { }

        // ---- Transform -------------------------------------------------------------
        public Vector3 Position
        {
            get { InternalCalls.GetEntityTransform(ID, out Vector3 p, out _); return p; }
            set { InternalCalls.GetEntityTransform(ID, out _, out Quaternion r); InternalCalls.SetEntityTransform(ID, value, r); }
        }

        public Quaternion Rotation
        {
            get { InternalCalls.GetEntityTransform(ID, out _, out Quaternion r); return r; }
            set { InternalCalls.GetEntityTransform(ID, out Vector3 p, out _); InternalCalls.SetEntityTransform(ID, p, value.Normalized); }
        }

        /// <summary>Teleports the entity (physics bodies are moved and their velocity is cleared).</summary>
        public void SetTransform(Vector3 position, Quaternion rotation) =>
            InternalCalls.SetEntityTransform(ID, position, rotation.Normalized);

        public Vector3 Forward => Rotation * Vector3.Forward;
        public Vector3 Up => Rotation * Vector3.Up;
        public Vector3 Right => Rotation * Vector3.Right;

        public bool Exists => InternalCalls.EntityExists(ID);

        // ---- World ----------------------------------------------------------------
        /// <summary>Raycast that ignores this entity's own physics body.</summary>
        public bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit) =>
            Physics.Raycast(origin, direction, maxDistance, out hit, ignore: ID);

        public static uint Spawn(string mesh, Vector3 position, Quaternion rotation, Vector3 scale, bool dynamic = true) =>
            InternalCalls.SpawnEntity(mesh, position, rotation, scale, dynamic);

        public static void Destroy(uint entity) => InternalCalls.DestroyEntity(entity);

        public override string ToString() => $"{GetType().Name}#{ID}";
    }

    /// <summary>Frame timing, updated by the engine before scripts run.</summary>
    public static class Time
    {
        public static float DeltaTime { get; internal set; }
        public static float FixedDeltaTime { get; internal set; } = 1f / 60f;
        public static double Elapsed { get; internal set; }
        public static long FrameCount { get; internal set; }
    }

    /// <summary>Logging routed to the engine log / editor console.</summary>
    public static class Log
    {
        public static void Trace(string message) => InternalCalls.Log(0, message);
        public static void Info(string message) => InternalCalls.Log(1, message);
        public static void Warn(string message) => InternalCalls.Log(2, message);
        public static void Error(string message) => InternalCalls.Log(3, message);
    }

    public struct RaycastHit
    {
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
        /// <summary>Entity that owns the hit body, or <c>0xFFFFFFFF</c> if none.</summary>
        public uint Entity;
    }

    /// <summary>Queries against the Jolt physics world.</summary>
    public static class Physics
    {
        public const uint NoEntity = InternalCalls.InvalidEntity;

        public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit, uint ignore = NoEntity)
        {
            bool ok = InternalCalls.RaycastWorld(origin, direction, maxDistance, ignore, out NativeRaycastHit native);
            hit = new RaycastHit { Point = native.Point, Normal = native.Normal, Distance = native.Distance, Entity = native.Entity };
            return ok;
        }
    }
}
