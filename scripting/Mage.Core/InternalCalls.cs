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
        public delegate* unmanaged<byte*, Vector3*, Quaternion*, Vector3*, int, uint> SpawnModel;
        public delegate* unmanaged<uint, int, byte*, int> SetEntityTexture;
        public delegate* unmanaged<byte*, float, float, int, Vector3*, uint> PlayAudio;
        public delegate* unmanaged<uint, void> StopAudio;
        public delegate* unmanaged<byte*, float, int> LoadFont;
        public delegate* unmanaged<byte*, float, float, float, uint, int, void> DrawLabel;
        public delegate* unmanaged<uint, byte*, float, int, float, int> PlayAnimation;
        public delegate* unmanaged<uint, void> StopAnimation;
        public delegate* unmanaged<uint, byte*, int> LoadAnimations;
        public delegate* unmanaged<uint, float*, float*, int> AnimationState;
        public delegate* unmanaged<uint, float, int, void> SetAnimation;
        public delegate* unmanaged<uint, byte*, Vector3*, Quaternion*, int> BonePosition;
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
        public const uint InteropVersion = 3;
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

        // UTF-8 copy of a string that lives for one native call.
        private readonly ref struct Utf8
        {
            private readonly IntPtr _ptr;
            public Utf8(string text) { _ptr = Marshal.StringToCoTaskMemUTF8(text); }
            public byte* Ptr => (byte*)_ptr;
            public void Dispose() => Marshal.FreeCoTaskMem(_ptr);
        }

        public static uint SpawnModel(string path, Vector3 position, Quaternion rotation, Vector3 scale, int physics)
        {
            using var p = new Utf8(path);
            return s_native.SpawnModel(p.Ptr, &position, &rotation, &scale, physics);
        }

        public static bool SetEntityTexture(uint entity, int slot, string path)
        {
            using var p = new Utf8(path);
            return s_native.SetEntityTexture(entity, slot, p.Ptr) != 0;
        }

        public static uint PlayAudio(string path, float volume, float pitch, bool loop, Vector3? position)
        {
            using var p = new Utf8(path);
            Vector3 pos = position ?? default;
            return s_native.PlayAudio(p.Ptr, volume, pitch, loop ? 1 : 0, position.HasValue ? &pos : null);
        }

        public static void StopAudio(uint sound) => s_native.StopAudio(sound);

        public static bool PlayAnimation(uint entity, string clip, float fade, bool loop, float speed)
        {
            using var c = new Utf8(clip);
            return s_native.PlayAnimation(entity, c.Ptr, fade, loop ? 1 : 0, speed) != 0;
        }

        public static void StopAnimation(uint entity) => s_native.StopAnimation(entity);

        public static int LoadAnimations(uint entity, string path)
        {
            using var p = new Utf8(path);
            return s_native.LoadAnimations(entity, p.Ptr);
        }

        public static int AnimationState(uint entity, out float time, out float duration)
        {
            float t = 0f, d = 0f;
            int state = s_native.AnimationState(entity, &t, &d);
            time = t;
            duration = d;
            return state;
        }

        public static void SetAnimation(uint entity, float speed, bool inPlace) => s_native.SetAnimation(entity, speed, inPlace ? 1 : 0);

        public static bool BonePosition(uint entity, string bone, out Vector3 position, out Quaternion rotation)
        {
            using var b = new Utf8(bone);
            Vector3 p = default;
            Quaternion q = default;
            bool ok = s_native.BonePosition(entity, b.Ptr, &p, &q) != 0;
            position = p;
            rotation = q;
            return ok;
        }

        public static int LoadFont(string path, float size)
        {
            using var p = new Utf8(path);
            return s_native.LoadFont(p.Ptr, size);
        }

        public static void DrawLabel(string text, float x, float y, float size, uint rgba, int font)
        {
            using var t = new Utf8(text);
            s_native.DrawLabel(t.Ptr, x, y, size, rgba, font);
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
