// The plain structs of api/mirage/api.h that cross the boundary, by hand (the function tables are generated: Api.g.cs).
// Layouts are checked against the C sizes when the host starts (Bridge.Initialize).
using System.Runtime.InteropServices;

namespace Mirage.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MirageModuleDesc
    {
        public uint size;
        public byte* name;
        public byte* tab;
        public byte* description;
        public int order;
        public int key;
        public uint flags;
        public void* user;
        public void* on_enable;
        public void* on_disable;
        public void* on_tick;
        public void* on_render;
        public void* on_trigger;
        public void* on_panel;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MirageEvent
    {
        public uint size;
        public int type;
        public int value, value2;
        public float number;
        public byte* text;
        public void* @object;       // Appended (step 14): the event's object (actor events)
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MirageHandle
    {
        public uint size;
        public void* @object;
        public uint slot;
        public int name_index, name_number;
        public void* object_class;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MirageActorQuery
    {
        public uint size;
        public byte* class_name;
        public byte* tag;
        public byte* name;
        public void* near_object;
        public float x, y, z;
        public float radius;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MirageValue
    {
        public int type;
        public int i;
        public float f;
        public void* o;
        public fixed byte text[256];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MirageMemoryStats
    {
        public uint size;
        public int pressure;
        public ulong process_private, process_working_set, address_free, address_largest_free, menu_heap, mod_lua;
    }

    public static class Constants
    {
        public const int LogDebug = 0, LogInfo = 1, LogWarn = 2, LogError = 3;
        public const uint ModuleAction = 0x1, ModuleEnabledByDefault = 0x2, ModuleEveryTick = 0x4, ModulePanelOpen = 0x8;
        public const int ValueNone = 0, ValueBool = 1, ValueInt = 2, ValueFloat = 3, ValueObject = 4, ValueText = 5, ValueStruct = 6, ValueArray = 7;
        public const int EventTick = 0, EventLevelLoad = 1, EventDeath = 2, EventPlayerState = 3, EventKey = 4, EventStateRestored = 5,
            EventMemoryPressure = 6, EventActorAdded = 7, EventActorRemoved = 8, EventLevelStreamed = 9, EventRestart = 10;
        public const int HookBefore = 0, HookAfter = 1, HookReplace = 2;
    }
}
