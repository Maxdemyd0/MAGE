// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using System.Runtime.InteropServices;

namespace Mage.Interop
{
    // ---------------------------------------------------------------------------------------
    //  C++ <-> C# ABI. Mirrors src/scripting/InteropTypes.h EXACTLY (order, types, sizes).
    // ---------------------------------------------------------------------------------------

    /// <summary>Mirrors <c>mage::interop::NRaycastHit</c> (32 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeRaycastHit
    {
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
        public uint Entity;
    }

    /// <summary>Mirrors <c>mage::interop::NVehicleState</c> (20 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct NativeVehicleState
    {
        public float SpeedKmh;
        public float ForwardSpeed;
        public float EngineRPM;
        public int Gear;
        public int WheelsInContact;
    }

    /// <summary>Native engine functions handed to C# by ScriptHost.cpp.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct NativeBindings
    {
        public uint StructSize;
        public uint Version;
        public delegate* unmanaged<int, byte*, void> Log;
        public delegate* unmanaged<uint, Vector3*, Quaternion*, void> GetEntityTransform;
        public delegate* unmanaged<uint, Vector3*, Quaternion*, void> SetEntityTransform;
        public delegate* unmanaged<uint, float, float, float, float, void> ApplyVehicleInput;
        public delegate* unmanaged<uint, NativeVehicleState*, int> GetVehicleState;
        public delegate* unmanaged<Vector3*, Vector3*, float, uint, NativeRaycastHit*, int> RaycastWorld;
        public delegate* unmanaged<byte*, Vector3*, Quaternion*, Vector3*, int, uint> SpawnEntity;
        public delegate* unmanaged<uint, void> DestroyEntity;
        public delegate* unmanaged<int, int> IsKeyDown;
        public delegate* unmanaged<int, int> IsKeyPressed;
        public delegate* unmanaged<uint, int> EntityExists;
    }

    /// <summary>Managed lifecycle entry points that C# hands back to the engine.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ManagedCallbacks
    {
        public delegate* unmanaged<uint, byte*, int> CreateScript;
        public delegate* unmanaged<uint, void> DestroyScript;
        public delegate* unmanaged<float, void> OnUpdate;
        public delegate* unmanaged<float, void> OnFixedUpdate;
        public delegate* unmanaged<void> Shutdown;
    }

    /// <summary>
    /// Thin, allocation-free wrappers over the native function pointers.
    /// Everything in Mage.Core goes through here; game code uses the friendly APIs instead.
    /// </summary>
    internal static unsafe class InternalCalls
    {
        public const uint InteropVersion = 1;
        public const uint InvalidEntity = 0xFFFFFFFF;

        private static NativeBindings s_native;
        public static bool IsBound { get; private set; }

        internal static void Bind(in NativeBindings bindings)
        {
            s_native = bindings;
            IsBound = true;
        }

        internal static void Unbind()
        {
            IsBound = false;
            s_native = default;
        }

        public static void Log(int level, string message)
        {
            if (!IsBound) { Console.WriteLine(message); return; }
            int max = System.Text.Encoding.UTF8.GetMaxByteCount(message.Length) + 1;
            if (max <= 1024)
            {
                byte* buffer = stackalloc byte[max];
                WriteUtf8(message, buffer, max);
                s_native.Log(level, buffer);
            }
            else
            {
                IntPtr ptr = Marshal.StringToCoTaskMemUTF8(message);
                try { s_native.Log(level, (byte*)ptr); }
                finally { Marshal.FreeCoTaskMem(ptr); }
            }
        }

        public static void GetEntityTransform(uint entity, out Vector3 position, out Quaternion rotation)
        {
            Vector3 p = default;
            Quaternion r = Quaternion.Identity;
            s_native.GetEntityTransform(entity, &p, &r);
            position = p;
            rotation = r;
        }

        public static void SetEntityTransform(uint entity, Vector3 position, Quaternion rotation) =>
            s_native.SetEntityTransform(entity, &position, &rotation);

        public static void ApplyVehicleInput(uint entity, float throttle, float steering, float brake, float handBrake) =>
            s_native.ApplyVehicleInput(entity, throttle, steering, brake, handBrake);

        public static bool GetVehicleState(uint entity, out NativeVehicleState state)
        {
            NativeVehicleState s = default;
            bool ok = s_native.GetVehicleState(entity, &s) != 0;
            state = s;
            return ok;
        }

        public static bool RaycastWorld(Vector3 origin, Vector3 direction, float maxDistance, uint ignoreEntity, out NativeRaycastHit hit)
        {
            NativeRaycastHit h = default;
            bool ok = s_native.RaycastWorld(&origin, &direction, maxDistance, ignoreEntity, &h) != 0;
            hit = h;
            return ok;
        }

        public static uint SpawnEntity(string mesh, Vector3 position, Quaternion rotation, Vector3 scale, bool dynamic)
        {
            IntPtr name = Marshal.StringToCoTaskMemUTF8(mesh);
            try { return s_native.SpawnEntity((byte*)name, &position, &rotation, &scale, dynamic ? 1 : 0); }
            finally { Marshal.FreeCoTaskMem(name); }
        }

        public static void DestroyEntity(uint entity) => s_native.DestroyEntity(entity);
        public static bool IsKeyDown(int key) => s_native.IsKeyDown(key) != 0;
        public static bool IsKeyPressed(int key) => s_native.IsKeyPressed(key) != 0;
        public static bool EntityExists(uint entity) => s_native.EntityExists(entity) != 0;

        // capacity must be >= Encoding.UTF8.GetMaxByteCount(text.Length) + 1.
        private static void WriteUtf8(string text, byte* buffer, int capacity)
        {
            fixed (char* chars = text)
            {
                int written = System.Text.Encoding.UTF8.GetBytes(chars, text.Length, buffer, capacity - 1);
                buffer[written] = 0;
            }
        }
    }
}
