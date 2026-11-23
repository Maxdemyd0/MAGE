// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using System.Runtime.InteropServices;

namespace Mage
{
    /// <summary>Rotation quaternion. Layout matches C++ <c>mage::interop::NQuat</c> (x, y, z, w).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Quaternion : IEquatable<Quaternion>
    {
        public float X, Y, Z, W;

        public Quaternion(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }

        public static Quaternion Identity => new(0f, 0f, 0f, 1f);

        /// <summary>Rotation of <paramref name="radians"/> around <paramref name="axis"/>.</summary>
        public static Quaternion AngleAxis(float radians, Vector3 axis)
        {
            Vector3 n = axis.Normalized;
            float half = radians * 0.5f;
            float s = MathF.Sin(half);
            return new Quaternion(n.X * s, n.Y * s, n.Z * s, MathF.Cos(half));
        }

        /// <summary>Yaw (Y), then pitch (X), then roll (Z), all in radians.</summary>
        public static Quaternion FromEuler(float pitch, float yaw, float roll) =>
            AngleAxis(yaw, Vector3.Up) * AngleAxis(pitch, new Vector3(1f, 0f, 0f)) * AngleAxis(roll, new Vector3(0f, 0f, 1f));

        public readonly float Length => MathF.Sqrt(X * X + Y * Y + Z * Z + W * W);

        public readonly Quaternion Normalized
        {
            get
            {
                float len = Length;
                return len > 1e-6f ? new Quaternion(X / len, Y / len, Z / len, W / len) : Identity;
            }
        }

        public readonly Quaternion Conjugate => new(-X, -Y, -Z, W);

        /// <summary>Heading around +Y of this rotation's forward (+Z) axis, in radians.</summary>
        public readonly float Yaw
        {
            get
            {
                Vector3 f = this * Vector3.Forward;
                return MathF.Atan2(f.X, f.Z);
            }
        }

        public static Quaternion operator *(Quaternion a, Quaternion b) => new(
            a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
            a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
            a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
            a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

        /// <summary>Rotates a vector: v' = v + 2w(q x v) + 2 q x (q x v).</summary>
        public static Vector3 operator *(Quaternion q, Vector3 v)
        {
            Vector3 u = new(q.X, q.Y, q.Z);
            Vector3 t = 2f * Vector3.Cross(u, v);
            return v + q.W * t + Vector3.Cross(u, t);
        }

        public static Quaternion Slerp(Quaternion a, Quaternion b, float t)
        {
            float cos = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
            if (cos < 0f) { b = new Quaternion(-b.X, -b.Y, -b.Z, -b.W); cos = -cos; }
            if (cos > 0.9995f)
                return new Quaternion(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t).Normalized;
            float angle = MathF.Acos(cos);
            float sa = MathF.Sin(angle);
            float wa = MathF.Sin((1f - t) * angle) / sa, wb = MathF.Sin(t * angle) / sa;
            return new Quaternion(a.X * wa + b.X * wb, a.Y * wa + b.Y * wb, a.Z * wa + b.Z * wb, a.W * wa + b.W * wb);
        }

        public readonly bool Equals(Quaternion o) => X == o.X && Y == o.Y && Z == o.Z && W == o.W;
        public override readonly bool Equals(object? obj) => obj is Quaternion q && Equals(q);
        public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);
        public static bool operator ==(Quaternion a, Quaternion b) => a.Equals(b);
        public static bool operator !=(Quaternion a, Quaternion b) => !a.Equals(b);
        public override readonly string ToString() => $"({X:0.###}, {Y:0.###}, {Z:0.###}, {W:0.###})";
    }
}
