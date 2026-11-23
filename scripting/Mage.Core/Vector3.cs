// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using System.Runtime.InteropServices;

namespace Mage
{
    /// <summary>3D vector. Memory layout matches C++ <c>mage::interop::NVec3</c> (3 x float).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Vector3 : IEquatable<Vector3>
    {
        public float X, Y, Z;

        public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; }
        public Vector3(float s) { X = s; Y = s; Z = s; }

        public static Vector3 Zero => new(0f, 0f, 0f);
        public static Vector3 One => new(1f, 1f, 1f);
        public static Vector3 Up => new(0f, 1f, 0f);
        public static Vector3 Down => new(0f, -1f, 0f);
        /// <summary>Engine forward axis (+Z, matches Jolt's vehicle convention).</summary>
        public static Vector3 Forward => new(0f, 0f, 1f);
        public static Vector3 Back => new(0f, 0f, -1f);
        /// <summary>Right when looking down +Z with +Y up (right-handed): -X.</summary>
        public static Vector3 Right => new(-1f, 0f, 0f);
        public static Vector3 Left => new(1f, 0f, 0f);

        public readonly float LengthSquared => X * X + Y * Y + Z * Z;
        public readonly float Length => MathF.Sqrt(LengthSquared);

        public readonly Vector3 Normalized
        {
            get
            {
                float len = Length;
                return len > 1e-6f ? this / len : Zero;
            }
        }

        public static float Dot(Vector3 a, Vector3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;

        public static Vector3 Cross(Vector3 a, Vector3 b) =>
            new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);

        public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => a + (b - a) * t;
        public static float Distance(Vector3 a, Vector3 b) => (a - b).Length;

        public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static Vector3 operator -(Vector3 v) => new(-v.X, -v.Y, -v.Z);
        public static Vector3 operator *(Vector3 v, float s) => new(v.X * s, v.Y * s, v.Z * s);
        public static Vector3 operator *(float s, Vector3 v) => v * s;
        public static Vector3 operator /(Vector3 v, float s) => new(v.X / s, v.Y / s, v.Z / s);

        public readonly bool Equals(Vector3 other) => X == other.X && Y == other.Y && Z == other.Z;
        public override readonly bool Equals(object? obj) => obj is Vector3 v && Equals(v);
        public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z);
        public static bool operator ==(Vector3 a, Vector3 b) => a.Equals(b);
        public static bool operator !=(Vector3 a, Vector3 b) => !a.Equals(b);
        public override readonly string ToString() => $"({X:0.###}, {Y:0.###}, {Z:0.###})";
    }
}
