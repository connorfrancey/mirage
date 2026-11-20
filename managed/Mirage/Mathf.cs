using System;

namespace Mirage
{
    /// <summary>A vector in Unreal units (the game's Z is up), with its math: the C# counterpart of Lua's mirage.vec.
    /// Every location, velocity and hit the API gives is one.</summary>
    public readonly record struct Vec3(float X, float Y, float Z)
    {
        public static Vec3 Zero => new(0, 0, 0);
        public static Vec3 One => new(1, 1, 1);
        public static Vec3 Up => new(0, 0, 1);
        public static Vec3 Forward => new(1, 0, 0);
        public static Vec3 Right => new(0, 1, 0);

        /// <summary>All three components <paramref name="v"/></summary>
        public Vec3(float v) : this(v, v, v) { }

        public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static Vec3 operator -(Vec3 a) => new(-a.X, -a.Y, -a.Z);
        /// <summary>Each component times the other's</summary>
        public static Vec3 operator *(Vec3 a, Vec3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
        public static Vec3 operator *(Vec3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);
        public static Vec3 operator *(float s, Vec3 a) => a * s;
        public static Vec3 operator /(Vec3 a, Vec3 b) => new(a.X / b.X, a.Y / b.Y, a.Z / b.Z);
        public static Vec3 operator /(Vec3 a, float s) => new(a.X / s, a.Y / s, a.Z / s);

        public float LengthSquared => X * X + Y * Y + Z * Z;
        public float Length => MathF.Sqrt(LengthSquared);
        /// <summary>Length 1 in the same direction; the zero vector stays zero</summary>
        public Vec3 Normalized => Length is var l && l > 0 ? this / l : Zero;

        public float Dot(Vec3 b) => X * b.X + Y * b.Y + Z * b.Z;
        public Vec3 Cross(Vec3 b) => new(Y * b.Z - Z * b.Y, Z * b.X - X * b.Z, X * b.Y - Y * b.X);
        public float Distance(Vec3 b) => (this - b).Length;
        /// <summary>From this (t = 0) to <paramref name="b"/> (t = 1), not clamped</summary>
        public Vec3 Lerp(Vec3 b, float t) => this + (b - this) * t;
        public void Deconstruct(out float x, out float y, out float z) { x = X; y = Y; z = Z; }

        /// <summary>The direction a rotation in Unreal units (65536 a turn) faces: yaw around Z, pitch up from the ground</summary>
        public static Vec3 FromRotation(int pitch, int yaw)
        {
            float p = Mathf.UnrealToRadians(pitch), y = Mathf.UnrealToRadians(yaw);
            return new(MathF.Cos(p) * MathF.Cos(y), MathF.Cos(p) * MathF.Sin(y), MathF.Sin(p));
        }

        public override string ToString() => $"({X:G}, {Y:G}, {Z:G})";
    }

    /// <summary>Float helpers, and the game's rotation units (65536 a turn)</summary>
    public static class Mathf
    {
        public const float Pi = MathF.PI;
        public const float Deg2Rad = MathF.PI / 180f;
        public const float Rad2Deg = 180f / MathF.PI;
        /// <summary>Unreal rotation units in a full turn</summary>
        public const int UnrealTurn = 65536;

        public static float Clamp(float v, float min, float max) => v < min ? min : v > max ? max : v;
        public static float Clamp01(float v) => Clamp(v, 0f, 1f);
        /// <summary>From a (t = 0) to b (t = 1), not clamped</summary>
        public static float Lerp(float a, float b, float t) => a + (b - a) * t;
        /// <summary>Where v lies from a (0) to b (1); 0 when a == b</summary>
        public static float InverseLerp(float a, float b, float v) => a == b ? 0f : (v - a) / (b - a);
        /// <summary>Equal within a small tolerance (relative to their size, at least 1e-6)</summary>
        public static bool Approximately(float a, float b) =>
            MathF.Abs(a - b) <= MathF.Max(1e-6f * MathF.Max(MathF.Abs(a), MathF.Abs(b)), 1e-6f);

        public static float UnrealToDegrees(int units) => units * 360f / UnrealTurn;
        public static float UnrealToRadians(int units) => units * 2f * MathF.PI / UnrealTurn;
        public static int DegreesToUnreal(float degrees) => (int)MathF.Round(degrees * UnrealTurn / 360f);
    }
}
