using System;
using Mirage.Native;

namespace Mirage
{
    /// <summary>
    /// A game object (an Actor, a Controller, ...), read and written by dotted path ("Location.Z", "Controller.Pawn").
    /// Not owned: the game may destroy it, so look it up again each tick instead of keeping it. Game thread only.
    /// </summary>
    public readonly unsafe struct GameObject : IEquatable<GameObject>
    {
        internal readonly void* Handle;
        internal GameObject(void* handle) => Handle = handle;

        public bool IsValid => Handle != null;

        public static GameObject Player => new(Bridge.Api->player_pawn());
        public static GameObject Controller => new(Bridge.Api->player_controller());
        public static GameObject World => new(Bridge.Api->world_info());
        /// <summary>"TdGame.Default__TdPawn", "Class Engine.Actor"</summary>
        public static GameObject Find(string fullName) => new(Bridge.Api->object_find(Utf8.Intern(fullName)));

        public string Name
        {
            get
            {
                if (Handle == null) return "None";
                var buf = stackalloc byte[256];
                uint n = Bridge.Api->object_name(Handle, buf, 256);
                return Utf8.Read(buf, (int)Math.Min(n, 255u));
            }
        }

        public string ClassName
        {
            get
            {
                if (Handle == null) return "";
                var buf = stackalloc byte[256];
                uint n = Bridge.Api->object_class_name(Handle, buf, 256);
                return Utf8.Read(buf, (int)Math.Min(n, 255u));
            }
        }

        public bool IsA(string className) => Handle != null && Bridge.Api->object_is_a(Handle, Utf8.Intern(className)) != 0;

        public Value Get(string path)
        {
            if (Handle == null) return default;
            MirageValue v;
            return Bridge.Api->object_get(Handle, Utf8.Intern(path), &v) != 0 ? new Value(v) : default;
        }

        public bool Set(string path, Value value)
        {
            if (Handle == null) return false;
            var v = value.ToNative();
            return Bridge.Api->object_set(Handle, Utf8.Intern(path), &v) != 0;
        }

        public float GetFloat(string path) => Get(path).Float;
        public int GetInt(string path) => Get(path).Int;
        public bool GetBool(string path) => Get(path).Bool;
        public GameObject GetObject(string path) => Get(path).Object;
        public bool SetFloat(string path, float f) => Set(path, Value.From(f));
        public bool SetInt(string path, int i) => Set(path, Value.From(i));
        public bool SetBool(string path, bool b) => Set(path, Value.From(b));

        public bool Equals(GameObject other) => Handle == other.Handle;
        public override bool Equals(object? obj) => obj is GameObject o && Equals(o);
        public override int GetHashCode() => ((IntPtr)Handle).GetHashCode();
        public override string ToString() => Name;
    }

    public enum ValueType { None = 0, Bool = 1, Int = 2, Float = 3, Object = 4, Text = 5, Struct = 6, Array = 7 }

    /// <summary>A value read from or written to a game object or a call.</summary>
    public readonly unsafe struct Value
    {
        public ValueType Type { get; }
        public int Int { get; }
        public float Float { get; }
        public GameObject Object { get; }
        public string Text { get; }
        public bool Bool => Int != 0;
        public bool Exists => Type != ValueType.None;
        /// <summary>An array's length (Type Array)</summary>
        public int Count => Type == ValueType.Array ? Int : 0;

        internal Value(in MirageValue v)
        {
            Type = (ValueType)v.type;
            Int = v.i;
            Float = Type == ValueType.Int || Type == ValueType.Bool ? v.i : v.f;
            Object = new GameObject(v.o);
            var copy = v;      // A local: its fixed buffer has an address that doesn't move
            Text = Type == ValueType.Text ? Utf8.Read(copy.text) : "";
        }

        private Value(ValueType type, int i, float f, GameObject o, string text)
        {
            Type = type;
            Int = i;
            Float = f;
            Object = o;
            Text = text;
        }

        public static Value From(bool b) => new(ValueType.Bool, b ? 1 : 0, b ? 1 : 0, default, "");
        public static Value From(int i) => new(ValueType.Int, i, i, default, "");
        public static Value From(float f) => new(ValueType.Float, (int)f, f, default, "");
        public static Value From(GameObject o) => new(ValueType.Object, 0, 0, o, "");
        /// <summary>A name (strings only as parameters of a call the mod makes)</summary>
        public static Value From(string text) => new(ValueType.Text, 0, 0, default, text);

        internal MirageValue ToNative()
        {
            var v = new MirageValue { type = (int)Type, i = Int, f = Float, o = Object.Handle };
            if (Type == ValueType.Text && !string.IsNullOrEmpty(Text)) {
                var bytes = System.Text.Encoding.UTF8.GetBytes(Text);
                int n = Math.Min(bytes.Length, 255);
                for (int k = 0; k < n; ++k) v.text[k] = bytes[k];
                v.text[n] = 0;
            }
            return v;
        }

        public override string ToString() => Type switch
        {
            ValueType.Bool => Bool.ToString(),
            ValueType.Int => Int.ToString(),
            ValueType.Float => Float.ToString("0.###"),
            ValueType.Object => Object.ToString(),
            ValueType.Text => Text,
            ValueType.Array => $"[{Count}]",
            ValueType.Struct => "{...}",
            _ => "none",
        };
    }

    /// <summary>Drawing on the screen (in OnRender, render thread). Colors are 0xAABBGGRR.</summary>
    public static unsafe class Draw
    {
        public static (float Width, float Height) Screen
        {
            get
            {
                float w, h;
                Bridge.Api->screen_size(&w, &h);
                return (w, h);
            }
        }
        public static void Text(float x, float y, uint color, string text) =>
            Utf8.With(text, t => { Bridge.Api->draw_text(x, y, color, (byte*)t); return 0; });
        public static void Line(float x1, float y1, float x2, float y2, uint color, float thickness = 1f) =>
            Bridge.Api->draw_line(x1, y1, x2, y2, color, thickness);
        public static void Rect(float x1, float y1, float x2, float y2, uint color, float thickness = 1f) =>
            Bridge.Api->draw_rect(x1, y1, x2, y2, color, thickness);
        public static void RectFilled(float x1, float y1, float x2, float y2, uint color) =>
            Bridge.Api->draw_rect_filled(x1, y1, x2, y2, color);
        /// <summary>A point in the world on the screen (false: behind the camera)</summary>
        public static bool WorldToScreen(float x, float y, float z, out float sx, out float sy)
        {
            float a, b;
            bool ok = Bridge.Api->world_to_screen(x, y, z, &a, &b) != 0;
            sx = a;
            sy = b;
            return ok;
        }
    }
}
