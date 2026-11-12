using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;

namespace Mirage.Native
{
    /// <summary>UTF-8, zero-terminated, for the C API. Constant strings (paths, names) are converted once and kept.</summary>
    internal static unsafe class Utf8
    {
        private static readonly ConcurrentDictionary<string, IntPtr> Interned = new();

        /// <summary>A pointer that stays valid for the process (for strings used again and again: paths, names).</summary>
        public static byte* Intern(string s)
        {
            if (Interned.TryGetValue(s, out var p)) return (byte*)p;
            if (Interned.Count > 8192) return (byte*)Interned.GetOrAdd(s, Allocate);   // Bounded growth: still correct
            return (byte*)Interned.GetOrAdd(s, Allocate);
        }

        private static IntPtr Allocate(string s)
        {
            var bytes = Encoding.UTF8.GetBytes(s);
            var p = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, p, bytes.Length);
            ((byte*)p)[bytes.Length] = 0;
            return p;
        }

        public static string Read(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((IntPtr)p) ?? "";

        public static string Read(byte* p, int length) => p == null || length <= 0 ? "" : Encoding.UTF8.GetString(p, length);

        /// <summary>Calls `fn` with a temporary zero-terminated copy of `s` (stack for short strings).</summary>
        public static T With<T>(string s, Func<IntPtr, T> fn)
        {
            int max = Encoding.UTF8.GetMaxByteCount(s.Length) + 1;
            if (max <= 512) {
                byte* buf = stackalloc byte[max];
                int n = Encoding.UTF8.GetBytes(s, new Span<byte>(buf, max));
                buf[n] = 0;
                return fn((IntPtr)buf);
            }
            var heap = new byte[max];
            int len = Encoding.UTF8.GetBytes(s, 0, s.Length, heap, 0);
            heap[len] = 0;
            fixed (byte* p = heap) return fn((IntPtr)p);
        }
    }
}
