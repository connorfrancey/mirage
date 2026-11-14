/* Mirage mod API, ABI version 1 (Mirage step 4).
 *
 * The one header a native mod needs. Plain C, so a mod can be built with any compiler (another MSVC toolset, clang)
 * and still work: no STL, no exceptions and no C++ types cross the boundary, every struct starts with its size, and
 * every function pointer is __cdecl.
 *
 * A native mod is a 32-bit DLL (the game is 32-bit) next to its mod.toml (entry = "my_mod.dll") that exports
 *
 *     MIRAGE_EXPORT uint32_t mirage_mod_abi(void) { return MIRAGE_ABI_VERSION; }
 *     MIRAGE_EXPORT int32_t mirage_mod_load(const MirageApi* api, MirageMod* mod);    // 0 = loaded
 *     MIRAGE_EXPORT void mirage_mod_unload(void);                                      // optional
 *
 * mirage_mod_load registers the mod's modules and their settings through `api`, which stays valid until
 * mirage_mod_unload returns. The runtime refuses a mod whose mirage_mod_abi() is newer than its own ABI.
 *
 * Rules that keep old mods working (checked by tests/test_abi.cpp):
 *   - MirageApi only grows: functions are appended, never removed, reordered or changed. A mod built against a newer
 *     header checks `api->size` before using a function that may be missing (MIRAGE_API_HAS).
 *   - Structs the mod fills (MirageModuleDesc) start with `size`; the runtime reads only the fields that size covers.
 *   - Strings are UTF-8 and copied by the runtime: a mod may pass temporaries.
 *
 * Threads: on_tick runs on the game thread (the engine tick), on_render on the render thread, inside the menu's frame
 * (draw with api->draw_*). on_enable / on_disable run where the module is switched (usually the render thread).
 * Everything else may be called from any of these. */
#ifndef MIRAGE_API_H
#define MIRAGE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIRAGE_ABI_VERSION 1u

#if defined(_WIN32)
#define MIRAGE_CALL __cdecl
#define MIRAGE_EXPORT __declspec(dllexport)
#else
#define MIRAGE_CALL
#define MIRAGE_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define MIRAGE_EXTERN_C extern "C"
#else
#define MIRAGE_EXTERN_C
#endif

/* Handles: owned by the runtime, valid until the mod is unloaded */
typedef struct MirageMod MirageMod;            /* The mod itself (its id, folder, config) */
typedef struct MirageModule MirageModule;      /* A row in the menu */
typedef struct MirageSetting MirageSetting;    /* A control in a module's settings panel */

typedef enum MirageLogLevel {
    MIRAGE_LOG_DEBUG = 0,
    MIRAGE_LOG_INFO = 1,
    MIRAGE_LOG_WARN = 2,
    MIRAGE_LOG_ERROR = 3
} MirageLogLevel;

/* MirageModuleDesc.flags */
#define MIRAGE_MODULE_ACTION 0x1u                /* A button: on_trigger, no on/off state */
#define MIRAGE_MODULE_ENABLED_BY_DEFAULT 0x2u    /* On the first time the mod is loaded (the config remembers after) */
#define MIRAGE_MODULE_PANEL_OPEN 0x8u             /* Its settings panel is open the first time (a module that is
                                                    mostly its panel: a list) */
#define MIRAGE_MODULE_EVERY_TICK 0x4u            /* on_tick is never deferred when the menu is over its time budget
                                                    (a module that must see one exact frame: a jump on landing) */

/* setting_type: what a setting holds */
typedef enum MirageSettingType {
    MIRAGE_SETTING_BOOL = 0,
    MIRAGE_SETTING_INT = 1,
    MIRAGE_SETTING_FLOAT = 2,
    MIRAGE_SETTING_STRING = 3,
    MIRAGE_SETTING_CHOICE = 4,
    MIRAGE_SETTING_KEY = 5,
    MIRAGE_SETTING_COLOR = 6,   /* setting_get_int: the color packed as the draw functions take it (0xAABBGGRR) */
    MIRAGE_SETTING_UNKNOWN = -1
} MirageSettingType;

typedef void(MIRAGE_CALL* MirageCallback)(void* user);

/* What a module is: filled by the mod, copied by module_register */
typedef struct MirageModuleDesc {
    uint32_t size;                  /* sizeof(MirageModuleDesc) */
    const char* name;               /* Unique in the menu ("Hello"); the config saves it under this name */
    const char* tab;                /* Menu column ("Mods" if NULL); created when no module had it yet */
    const char* description;        /* One line, shown on hover (may be NULL) */
    int32_t order;                  /* Position in the column, lower first (100 if 0) */
    int32_t key;                    /* Default toggle key (virtual-key code), 0 = none */
    uint32_t flags;                 /* MIRAGE_MODULE_* */
    void* user;                     /* Passed back to every callback */
    MirageCallback on_enable;       /* Each may be NULL */
    MirageCallback on_disable;      /* Also on unload: undo what the module changed */
    MirageCallback on_tick;         /* Game thread, every tick while on */
    MirageCallback on_render;       /* Render thread, every frame while on */
    MirageCallback on_trigger;      /* Action modules: clicked or key pressed */
    /* Appended in step 9 (a mod built before has a smaller `size`: the runtime doesn't read these) */
    MirageCallback on_panel;        /* Render thread: draws (with api->ui()) in the module's settings panel, below its
                                       settings, while the panel is open. The panel's width: ui->content_width() */
} MirageModuleDesc;

/* ---- Game objects, events and function hooks (step 6) -------------------------------------------------------------- */
typedef struct MirageObject MirageObject;           /* A game object (an Actor, a Controller, ...); not owned: it lives
                                                       as long as the game keeps it, so look it up again rather than keep
                                                       it across ticks */
typedef struct MirageCall MirageCall;               /* A call of a game function: one a hook sees, or one the mod makes */
typedef struct MirageHook MirageHook;               /* A function hook */
typedef struct MirageSubscription MirageSubscription;   /* An event subscription */
typedef struct MirageDetour MirageDetour;           /* A detour of native code */
typedef struct MirageNativeCall MirageNativeCall;   /* One call of a mod native from the game's bytecode */

/* A value read from or written to a game object or a call, by dotted path ("Location.Z", "NewLocation", "ReturnValue") */
typedef enum MirageValueType {
    MIRAGE_VALUE_NONE = 0,      /* Nothing there (the path didn't resolve) */
    MIRAGE_VALUE_BOOL = 1,      /* i: 0/1 */
    MIRAGE_VALUE_INT = 2,       /* i (bytes and enums too) */
    MIRAGE_VALUE_FLOAT = 3,     /* f */
    MIRAGE_VALUE_OBJECT = 4,    /* o (NULL: none) */
    MIRAGE_VALUE_TEXT = 5,      /* text: a name or a string (written: names; strings only as a call's parameters,
                                   in a call the mod makes with call_begin) */
    MIRAGE_VALUE_STRUCT = 6,    /* A struct: reach its members with a longer path ("Location" -> "Location.X") */
    MIRAGE_VALUE_ARRAY = 7      /* An array: i = how many elements; reach one with "[n]" ("StreamingLevels[0]",
                                   0-based). Elements are read and written, the array isn't resized */
} MirageValueType;
typedef struct MirageValue {
    int32_t type;               /* MIRAGE_VALUE_* */
    int32_t i;
    float f;
    MirageObject* o;
    char text[256];             /* UTF-8, cut at 255 bytes */
} MirageValue;

/* hook_function: when the hook runs */
#define MIRAGE_HOOK_BEFORE 0    /* Before the function: may change its parameters, or call_skip() it */
#define MIRAGE_HOOK_AFTER 1     /* After it: its out parameters and "ReturnValue" are set */
#define MIRAGE_HOOK_REPLACE 2   /* Instead of it: set "ReturnValue" (the highest priority replace runs, alone) */
typedef void(MIRAGE_CALL* MirageHookFn)(void* user, MirageCall* call);
typedef void(MIRAGE_CALL* MirageNativeFn)(void* user, MirageNativeCall* call);

/* event_subscribe: what happened (MirageEvent.type) and what comes with it */
#define MIRAGE_EVENT_TICK 0           /* Game thread, every engine tick while in a level: number = seconds since the last */
#define MIRAGE_EVENT_LEVEL_LOAD 1     /* Game thread: value 0 a load starts, 1 it finished; text = the map ("edge_p") */
#define MIRAGE_EVENT_DEATH 2          /* Game thread: value 0 the player died (before the game handles it), 1 after */
#define MIRAGE_EVENT_PLAYER_STATE 3   /* Game thread: the player's move state changed: value = new EMovement, value2 = old */
#define MIRAGE_EVENT_KEY 4            /* A key went down (the game window's thread): value = virtual-key code */
#define MIRAGE_EVENT_STATE_RESTORED 5 /* Game thread: the TAS put back a memory snapshot. The game's memory went back in
                                         time (a mod's own memory didn't): every game object kept from before may be
                                         another or gone, so look them up again */
#define MIRAGE_EVENT_MEMORY_PRESSURE 6 /* Game thread: the memory manager's pressure level changed, or holds at High or
                                         Critical (again every 5 s): value = MIRAGE_MEMORY_* now. Give back what can be
                                         made again (caches; Lua: collectgarbage()). Never the game's memory */
/* World lifecycle (step 14). Game thread. Actors are compared tick to tick, only while a mod subscribes to them: an
 * actor made and destroyed within one tick isn't seen, and a level streaming in or out adds or removes its actors. */
#define MIRAGE_EVENT_ACTOR_ADDED 7    /* An actor came into play (spawned, or its level streamed in): object = it,
                                         text = its class's name */
#define MIRAGE_EVENT_ACTOR_REMOVED 8  /* An actor left play (destroyed, or its level went): object = the handle it had,
                                         to compare with ones kept; never use it (it is no longer valid) */
#define MIRAGE_EVENT_LEVEL_STREAMED 9 /* A streaming level became visible (value 1) or stopped being (value 0): text = its
                                         package name */
#define MIRAGE_EVENT_RESTART 10       /* A checkpoint restart: the player is a new pawn in the same level. Objects of the
                                         old player are gone */
typedef struct MirageEvent {
    uint32_t size;              /* sizeof(MirageEvent) */
    int32_t type;               /* MIRAGE_EVENT_* */
    int32_t value, value2;
    float number;
    const char* text;           /* Valid during the callback; "" if none */
    MirageObject* object;       /* The event's object (MIRAGE_EVENT_ACTOR_*), else NULL. Appended (step 14): check
                                   size >= offsetof(MirageEvent, object) + sizeof(void*) before reading it */
} MirageEvent;
typedef void(MIRAGE_CALL* MirageEventFn)(void* user, const MirageEvent* event);

/* objects_of_class: flags */
#define MIRAGE_OBJECTS_DEFAULTS 0x1u    /* Class default objects too */

/* ---- World access (step 14) -----------------------------------------------------------------------------------------
 * A MirageObject is the object's address: once the object is gone, another object can come at the same address and in
 * the same slot of the engine's object list, and the old pointer would quietly mean the new object. A MirageHandle
 * also keeps what identifies the object (its slot, its name, its class): handle_object gives the object back only
 * while all of it still matches, else NULL. Plain data: keep it as long as you like, across ticks and level loads. */
typedef struct MirageHandle {
    uint32_t size;              /* sizeof(MirageHandle) */
    MirageObject* object;
    uint32_t slot;              /* Its index in the engine's object list */
    int32_t name_index, name_number;    /* Its name (an FName) */
    MirageObject* object_class;
} MirageHandle;

/* actors_query: which actors (every field optional; zero = any). Actors of the levels in play (the persistent level and
 * the visible streaming levels), from the runtime's per-tick actor lists: no walk of the whole object list. */
typedef struct MirageActorQuery {
    uint32_t size;              /* sizeof(MirageActorQuery) */
    const char* class_name;     /* "TdBotPawn", or by path "TdGame.TdBotPawn": that class and its subclasses */
    const char* tag;            /* Actor.Tag */
    const char* name;           /* The object's name ("TdBotPawn_3") */
    MirageObject* near_object;  /* With radius > 0: around this actor's Location ... */
    float x, y, z;              /* ... or around this point (when near_object is NULL) */
    float radius;               /* > 0: only within this distance; the results then come nearest first */
} MirageActorQuery;

/* ---- Script patching (step 8) ---------------------------------------------------------------------------------------
 * A loaded UnrealScript function's bytecode, opened as a tree of tokens (script_open), edited (script_set, script_add,
 * script_set_children) and put in place of the function's own code (script_apply): the game's VM runs it, no native
 * detour. Jump targets and skip sizes are recomputed from the tree. script_restore, or the mod's unload, puts the
 * original back. Game thread. script_dump lists the tokens (with their indices) to find what to change. */
typedef struct MirageScript MirageScript;          /* A function's bytecode, open for editing (owned by the mod) */
typedef struct MirageToken {
    uint32_t size;              /* sizeof(MirageToken): set it before script_get / script_set / script_add */
    int32_t kind;               /* The token's kind: script_kind("FloatConst"), script_kind_name(kind) */
    int32_t native_index;       /* NativeFunction: the native's index (171: float * float, 174: float + float) */
    int32_t int_value;          /* IntConst */
    float floats[3];            /* FloatConst: floats[0]; VectorConst */
    int32_t ints[3];            /* RotationConst */
    MirageObject* object;       /* The object operand: a variable's property, a function, a class (NULL: none) */
    MirageObject* object2;      /* A second one (StructMember's struct, Context's property) */
    char name[64];              /* The name operand (VirtualFunction's function, NameConst); written: an existing name */
    uint16_t word, word2;       /* Skips and jump targets as decoded (script_apply recomputes them) */
    uint8_t byte_value, byte_value2, reserved[2];   /* ByteConst / IntConstByte, cast type, flags */
    int32_t target;             /* A jump added by the mod: the token it goes to (-1: as decoded, -2: the script's end) */
    uint32_t memory_offset;     /* Where it was in the code when opened (read only; 0 for an added token) */
    char text[256];             /* StringConst / UnicodeStringConst, UTF-8 */
} MirageToken;

/* ---- The UI layer (step 9): windows and widgets, immediate mode (they are made again every frame) ------------------
 * The menu's own ImGui, through a C table, so a mod never links ImGui (any version, any compiler). Call these only on
 * the render thread: in on_render (windows of the mod's own: window_begin / window_end) or in on_panel (widgets in the
 * module's settings panel). Every window_begin and child_begin has its end; table_begin, tree_node and popup_begin
 * too when they returned 1. Widgets are told apart by their label: two with the same label in one window need an
 * id_push or a "##suffix" ("Delete##3" shows "Delete"). Sizes are pixels; 0 means "fit". Colors are 0xAABBGGRR. */
typedef struct MirageUi MirageUi;

/* window_begin flags (ImGui's ImGuiWindowFlags) */
#define MIRAGE_WINDOW_NO_TITLE 0x1u
#define MIRAGE_WINDOW_NO_RESIZE 0x2u
#define MIRAGE_WINDOW_NO_MOVE 0x4u
#define MIRAGE_WINDOW_NO_SCROLLBAR 0x8u
#define MIRAGE_WINDOW_NO_COLLAPSE 0x20u
#define MIRAGE_WINDOW_AUTO_SIZE 0x40u
#define MIRAGE_WINDOW_NO_BACKGROUND 0x80u
#define MIRAGE_WINDOW_NO_INPUTS 0xC0200u        /* Click-through: an overlay */
#define MIRAGE_WINDOW_MENU_ONLY 0x80000000u     /* Mirage: shown only while the menu is open */
/* input_text flags */
#define MIRAGE_INPUT_ENTER_RETURNS 0x20u        /* Returns 1 when Enter is pressed, not on every change */
#define MIRAGE_INPUT_READ_ONLY 0x4000u
#define MIRAGE_INPUT_PASSWORD 0x8000u
#define MIRAGE_INPUT_MULTILINE 0x40000000u      /* Mirage: a text box of several lines (`height` tall) */
/* table_begin flags (ImGuiTableFlags) */
#define MIRAGE_TABLE_RESIZABLE 0x1u
#define MIRAGE_TABLE_ROW_BG 0x40u
#define MIRAGE_TABLE_BORDERS 0x780u
#define MIRAGE_TABLE_SIZING_FIXED 0x2000u
#define MIRAGE_TABLE_SCROLL_Y 0x2000000u        /* Rows scroll inside the given height */
/* table_column flags (ImGuiTableColumnFlags) */
#define MIRAGE_COLUMN_STRETCH 0x8u              /* Shares what is left */
#define MIRAGE_COLUMN_FIXED 0x10u               /* `width` pixels wide */
/* color_push: what is colored (ImGuiCol) */
#define MIRAGE_COLOR_TEXT 0
#define MIRAGE_COLOR_TEXT_DISABLED 1
#define MIRAGE_COLOR_WINDOW_BG 2
#define MIRAGE_COLOR_CHILD_BG 3
#define MIRAGE_COLOR_BORDER 5
#define MIRAGE_COLOR_FRAME_BG 7
#define MIRAGE_COLOR_BUTTON 21
#define MIRAGE_COLOR_BUTTON_HOVERED 22
#define MIRAGE_COLOR_BUTTON_ACTIVE 23
#define MIRAGE_COLOR_HEADER 24
#define MIRAGE_COLOR_PLOT_LINES 40
#define MIRAGE_COLOR_PLOT_HISTOGRAM 42

struct MirageUi {
    uint32_t size;                  /* sizeof(MirageUi) of the runtime: like MirageApi, it only grows */

    /* Windows. Always call window_end after window_begin, whatever it returned (0: collapsed or hidden, draw nothing
     * in it). `open` (may be NULL) gets a close button and is set to 0 when it's clicked. window_next_pos / _size go
     * before window_begin; unless `always`, only the first time the window is seen (the menu remembers where the
     * player put it). x or y < 0: centered on the screen. */
    int32_t(MIRAGE_CALL* window_begin)(const char* title, int32_t* open, uint32_t flags);
    void(MIRAGE_CALL* window_end)(void);
    void(MIRAGE_CALL* window_next_pos)(float x, float y, int32_t always);
    void(MIRAGE_CALL* window_next_size)(float width, float height, int32_t always);
    void(MIRAGE_CALL* window_next_bg_alpha)(float alpha);
    int32_t(MIRAGE_CALL* child_begin)(const char* id, float width, float height, int32_t border);  /* Always child_end */
    void(MIRAGE_CALL* child_end)(void);

    /* Layout */
    void(MIRAGE_CALL* same_line)(float offset, float spacing);       /* Next widget on this line (0, -1: the defaults) */
    void(MIRAGE_CALL* separator)(void);
    void(MIRAGE_CALL* spacing)(void);
    void(MIRAGE_CALL* indent)(float width);                          /* 0: the default; unindent by the same */
    void(MIRAGE_CALL* unindent)(float width);
    void(MIRAGE_CALL* next_width)(float width);                      /* The next widget's width; < 0: leave that much */
    float(MIRAGE_CALL* content_width)(void);                         /* Room left on this line */
    float(MIRAGE_CALL* content_height)(void);
    float(MIRAGE_CALL* line_height)(void);                           /* A line of text, with its spacing */
    void(MIRAGE_CALL* cursor_screen_pos)(float* x, float* y);        /* Where the next widget goes, screen pixels */
    void(MIRAGE_CALL* dummy)(float width, float height);             /* Empty space */
    void(MIRAGE_CALL* id_push)(const char* id);
    void(MIRAGE_CALL* id_push_int)(int32_t id);
    void(MIRAGE_CALL* id_pop)(void);
    void(MIRAGE_CALL* disabled_begin)(int32_t disabled);            /* Always disabled_end */
    void(MIRAGE_CALL* disabled_end)(void);
    void(MIRAGE_CALL* color_push)(int32_t what, uint32_t color);     /* MIRAGE_COLOR_*: pop as many as pushed */
    void(MIRAGE_CALL* color_pop)(int32_t count);
    void(MIRAGE_CALL* font_scale)(float scale);                     /* The current window's text size (1: normal) */

    /* Text */
    void(MIRAGE_CALL* text)(const char* text);
    void(MIRAGE_CALL* text_colored)(uint32_t color, const char* text);
    void(MIRAGE_CALL* text_disabled)(const char* text);
    void(MIRAGE_CALL* text_wrapped)(const char* text);
    void(MIRAGE_CALL* label_text)(const char* label, const char* text);     /* "text   label", aligned like a widget */
    void(MIRAGE_CALL* bullet_text)(const char* text);

    /* Widgets: 1 when the player used it this frame (clicked it, changed the value) */
    int32_t(MIRAGE_CALL* button)(const char* label, float width, float height);
    int32_t(MIRAGE_CALL* small_button)(const char* label);
    int32_t(MIRAGE_CALL* checkbox)(const char* label, int32_t* value);
    int32_t(MIRAGE_CALL* radio)(const char* label, int32_t active);
    int32_t(MIRAGE_CALL* slider_float)(const char* label, float* value, float min, float max, const char* format);
    int32_t(MIRAGE_CALL* slider_int)(const char* label, int32_t* value, int32_t min, int32_t max, const char* format);
    int32_t(MIRAGE_CALL* drag_float)(const char* label, float* value, float speed, float min, float max, const char* format);
    int32_t(MIRAGE_CALL* drag_int)(const char* label, int32_t* value, float speed, int32_t min, int32_t max);
    int32_t(MIRAGE_CALL* input_int)(const char* label, int32_t* value, int32_t step);
    int32_t(MIRAGE_CALL* input_float)(const char* label, float* value, float step, const char* format);
    /* Edits the text in `buffer` (`capacity` bytes, with the terminator). MIRAGE_INPUT_MULTILINE: `height` tall. */
    int32_t(MIRAGE_CALL* input_text)(const char* label, char* buffer, uint32_t capacity, uint32_t flags, float height);
    /* `items`: the choices, each ended by a zero byte, the list by one more. *current: the chosen index */
    int32_t(MIRAGE_CALL* combo)(const char* label, int32_t* current, const char* items);
    int32_t(MIRAGE_CALL* selectable)(const char* label, int32_t selected, float width, float height);
    int32_t(MIRAGE_CALL* color_edit)(const char* label, uint32_t* rgba);
    void(MIRAGE_CALL* progress_bar)(float fraction, float width, float height, const char* overlay);   /* overlay: NULL */
    /* A chart of `count` values; min == max: from the values */
    void(MIRAGE_CALL* plot_lines)(const char* label, const float* values, int32_t count, float min, float max,
                                   float width, float height, const char* overlay);
    void(MIRAGE_CALL* plot_histogram)(const char* label, const float* values, int32_t count, float min, float max,
                                       float width, float height, const char* overlay);

    /* Trees: tree_node returns 1 when open (then tree_pop); a collapsing header has no pop */
    int32_t(MIRAGE_CALL* tree_node)(const char* label, int32_t default_open);
    void(MIRAGE_CALL* tree_pop)(void);
    int32_t(MIRAGE_CALL* collapsing_header)(const char* label, int32_t default_open);

    /* Tables: table_begin returns 1 when shown (then table_end). The columns first, then row by row. */
    int32_t(MIRAGE_CALL* table_begin)(const char* id, int32_t columns, uint32_t flags, float width, float height);
    void(MIRAGE_CALL* table_column)(const char* name, uint32_t flags, float width);
    void(MIRAGE_CALL* table_headers)(void);                          /* A row with the columns' names */
    void(MIRAGE_CALL* table_next_row)(void);
    int32_t(MIRAGE_CALL* table_next_column)(void);                   /* 1: that column is visible */
    void(MIRAGE_CALL* table_end)(void);

    /* The last widget, and popups */
    int32_t(MIRAGE_CALL* item_hovered)(void);
    int32_t(MIRAGE_CALL* item_clicked)(int32_t button);              /* 0 left, 1 right, 2 middle */
    int32_t(MIRAGE_CALL* item_active)(void);                         /* Held down, or being edited */
    void(MIRAGE_CALL* item_rect)(float* x1, float* y1, float* x2, float* y2);   /* Its box, screen pixels */
    void(MIRAGE_CALL* tooltip)(const char* text);                    /* Shown now (call it when item_hovered) */
    void(MIRAGE_CALL* popup_open)(const char* id);
    int32_t(MIRAGE_CALL* popup_begin)(const char* id);               /* 1: open, draw it, then popup_end */
    int32_t(MIRAGE_CALL* popup_context_item)(const char* id);        /* Right-click on the last widget; 1: popup_end */
    void(MIRAGE_CALL* popup_end)(void);
    void(MIRAGE_CALL* popup_close)(void);                            /* The popup being drawn */
    void(MIRAGE_CALL* focus_keyboard)(void);                         /* The next widget gets the keyboard */

    /* Scrolling of the current window */
    float(MIRAGE_CALL* scroll_y)(void);
    float(MIRAGE_CALL* scroll_max_y)(void);
    void(MIRAGE_CALL* scroll_to_bottom)(void);

    /* Drawing in the current window (over its widgets), screen pixels like draw_* */
    void(MIRAGE_CALL* window_draw_rect)(float x1, float y1, float x2, float y2, uint32_t color, float rounding,
                                         float thickness);          /* thickness 0: filled */
    void(MIRAGE_CALL* window_draw_text)(float x, float y, uint32_t color, const char* text);
    void(MIRAGE_CALL* window_draw_line)(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    void(MIRAGE_CALL* window_draw_circle)(float x, float y, float radius, uint32_t color, float thickness);  /* 0: filled */

    /* Input as the menu's ImGui sees it */
    int32_t(MIRAGE_CALL* key_pressed)(int32_t vk);                   /* Went down this frame (repeats when held) */
    int32_t(MIRAGE_CALL* wants_keyboard)(void);                      /* A text box has the keyboard */
    void(MIRAGE_CALL* mouse_pos)(float* x, float* y);
    float(MIRAGE_CALL* time)(void);                                  /* Seconds the menu has run */
    float(MIRAGE_CALL* delta_time)(void);                            /* Seconds since the last frame */

    /* Appended */
    void(MIRAGE_CALL* next_open)(int32_t open, int32_t always);      /* The next tree node opened or closed */
    int32_t(MIRAGE_CALL* input_text_hint)(const char* label, const char* hint, char* buffer, uint32_t capacity,
                                          uint32_t flags);           /* input_text with a grey hint while empty */
    void(MIRAGE_CALL* align_text_to_frame)(void);                    /* Text on this line lines up with widgets */
    void(MIRAGE_CALL* text_size)(const char* text, float* width, float* height);
    float(MIRAGE_CALL* frame_height)(void);                          /* A widget's height */
    void(MIRAGE_CALL* window_rect)(float* x1, float* y1, float* x2, float* y2);  /* The current window's box */
    /* Drawing over every window (the foreground), screen pixels; thickness 0: filled */
    void(MIRAGE_CALL* overlay_rect)(float x1, float y1, float x2, float y2, uint32_t color, float rounding, float thickness);
    /* Text `size` pixels tall in the current window (0: the window's size) */
    void(MIRAGE_CALL* window_draw_text_sized)(float x, float y, uint32_t color, float size, const char* text);
    /* Style of what comes next until style_pop: MIRAGE_STYLE_* (x, and y where it has one) */
    void(MIRAGE_CALL* style_push)(int32_t what, float x, float y);
    void(MIRAGE_CALL* style_pop)(int32_t count);
    void(MIRAGE_CALL* font_push)(int32_t font);                      /* MIRAGE_FONT_*; pop with font_pop */
    void(MIRAGE_CALL* font_pop)(void);
    void(MIRAGE_CALL* clipboard_set)(const char* text);
    int32_t(MIRAGE_CALL* item_double_clicked)(void);                 /* The last widget, left button */
    int32_t(MIRAGE_CALL* item_deactivated)(void);                    /* The last widget stopped being edited this frame */
};

/* font_push */
#define MIRAGE_FONT_MENU 0
#define MIRAGE_FONT_MONO 1        /* Monospace (the console's) */

/* style_push: what */
#define MIRAGE_STYLE_WINDOW_PADDING 0     /* x, y */
#define MIRAGE_STYLE_WINDOW_BORDER 1      /* x: the border's thickness (0: none) */
#define MIRAGE_STYLE_ITEM_SPACING 2       /* x, y */
#define MIRAGE_STYLE_FRAME_PADDING 3      /* x, y */

/* theme_color: which */
#define MIRAGE_THEME_ACCENT 0
#define MIRAGE_THEME_TEXT 1
#define MIRAGE_THEME_TEXT_DIM 2
#define MIRAGE_THEME_PANEL 3      /* A settings panel's background */

/* ---- 3D drawing in the world (step 9) ------------------------------------------------------------------------------
 * Flat-colored triangles and lines in world units, drawn by the runtime's Direct3D renderer inside the game's frame
 * (hidden behind the world where its depth buffer is there, else an overlay), with the engine's own view. One scene at
 * a time (the last mod to set it): the collision view's. Layers are replaced whole; set them from the game thread. */
typedef struct MirageWorldVertex {
    float x, y, z;
    uint32_t color;             /* 0xAARRGGBB (Direct3D's order, not the draw functions') */
} MirageWorldVertex;
typedef struct MirageWorldLabel {
    float x, y, z;
    uint32_t color;             /* 0xAARRGGBB */
    const char* text;           /* Copied */
} MirageWorldLabel;
typedef struct MirageWorldMesh {
    uint32_t size;              /* sizeof(MirageWorldMesh) */
    const MirageWorldVertex* triangles;     /* A triangle list: 3 vertices each */
    uint32_t triangle_vertices;
    const MirageWorldVertex* lines;         /* A line list: 2 vertices each */
    uint32_t line_vertices;
    const MirageWorldLabel* labels;         /* Text at a world position (MIRAGE_WORLD_SCENE only) */
    uint32_t label_count;
    uint32_t shapes;            /* For the stats: how many shapes it is */
    float build_ms;             /* For the stats: how long building it took */
} MirageWorldMesh;
/* world_draw_set: the layer */
#define MIRAGE_WORLD_SCENE 0      /* The main scene (rebuilt now and then) */
#define MIRAGE_WORLD_MOVING 1     /* Small geometry that moves, replaced every tick */
#define MIRAGE_WORLD_STATIC 2     /* Large world geometry; its faces at static_opacity */
typedef struct MirageWorldStyle {
    uint32_t size;              /* sizeof(MirageWorldStyle) */
    float opacity;              /* Faces (outlines are opaque) */
    int32_t overlay;            /* 1: always on top (no depth test) */
    float fov_offset;           /* Degrees added to the camera's */
    float aspect;               /* 0: from the viewport */
    float near_plane;           /* Used when the engine's is unknown */
    float static_opacity;       /* Faces of MIRAGE_WORLD_STATIC */
} MirageWorldStyle;
typedef struct MirageWorldStats {
    uint32_t size;              /* sizeof(MirageWorldStats): set it before world_draw_stats */
    int32_t drawn;              /* The last frame drew something */
    int32_t in_scene;           /* ... inside the game's frame (depth tested); 0: as an overlay */
    int32_t overlay_fallback;   /* The game's scene pass wasn't found: overlay only */
    uint32_t triangles, lines, shapes, labels, static_triangles, static_lines;
    float build_ms;
} MirageWorldStats;
/* camera_get: the player's view */
typedef struct MirageCamera {
    uint32_t size;              /* sizeof(MirageCamera): set it before the call */
    float x, y, z;
    int32_t pitch, yaw, roll;   /* Unreal rotation units (65536 a turn) */
    float fov;                  /* Degrees */
} MirageCamera;

/* memory_stats: the process's memory and the mod's share (the runtime's memory manager) */
#define MIRAGE_MEMORY_NORMAL 0
#define MIRAGE_MEMORY_ELEVATED 1
#define MIRAGE_MEMORY_HIGH 2
#define MIRAGE_MEMORY_CRITICAL 3
typedef struct MirageMemoryStats {
    uint32_t size;                  /* sizeof(MirageMemoryStats): set it before the call */
    int32_t pressure;               /* MIRAGE_MEMORY_* */
    uint64_t process_private;       /* The process's commit charge (bytes) */
    uint64_t process_working_set;
    uint64_t address_free;          /* Free address space (a 32-bit game: what runs out first) */
    uint64_t address_largest_free;  /* Its largest block; 0 = not measured yet */
    uint64_t menu_heap;             /* The menu's own heap, live */
    uint64_t mod_lua;               /* This mod's Lua state(s), 0 for a native mod (its own CRT is its own) */
} MirageMemoryStats;

/* tas_command: what to do (the runtime's TAS engine) */
#define MIRAGE_TAS_PLAY 0
#define MIRAGE_TAS_RESTART 1
#define MIRAGE_TAS_STOP 2
#define MIRAGE_TAS_TOGGLE_PAUSE 3
#define MIRAGE_TAS_ADVANCE 4          /* arg: frames */
#define MIRAGE_TAS_TAKE_OVER 5
#define MIRAGE_TAS_GOTO 6             /* arg: the frame */
#define MIRAGE_TAS_FRAME_BACK 7       /* arg: frames */
#define MIRAGE_TAS_GOTO_BOOKMARK 8    /* arg: +1 next, -1 previous */
#define MIRAGE_TAS_REWIND 9
#define MIRAGE_TAS_KEY_CAMERA 10      /* arg: the frame: a camera key from the current view */
#define MIRAGE_TAS_CLEAR 11
#define MIRAGE_TAS_RESET_SYNC 12
#define MIRAGE_TAS_UNSAVED 13         /* Returns 1 when the replay has changes not saved to a file */

/* input_key_state: a key as the game gets it */
typedef struct MirageKeyState {
    uint32_t size;          /* sizeof(MirageKeyState): set it before the call */
    int32_t held;
    uint32_t presses;       /* Since the counts were last reset (input_reset_counts) */
    double down, up;        /* When it last went down / came up, on input_now()'s clock; < 0: never */
} MirageKeyState;

/* The runtime's functions. Only grows; `size` says how much of it this runtime has. */
typedef struct MirageApi {
    uint32_t size;                  /* sizeof(MirageApi) of the runtime */
    uint32_t abi_version;           /* MIRAGE_ABI_VERSION of the runtime */
    const char* runtime_version;    /* "0.1.0" */

    /* ---- Logging: into the menu's log, prefixed with the mod's id ---------------------------------------------- */
    void(MIRAGE_CALL* log)(MirageMod* mod, int32_t level, const char* text);

    /* ---- The mod ------------------------------------------------------------------------------------------------ */
    const char*(MIRAGE_CALL* mod_id)(MirageMod* mod);            /* "hello" (from mod.toml) */
    const char*(MIRAGE_CALL* mod_version)(MirageMod* mod);       /* "1.0.0" */
    const char*(MIRAGE_CALL* mod_dir)(MirageMod* mod);           /* Its folder, UTF-8, no trailing slash */

    /* ---- Modules ------------------------------------------------------------------------------------------------ */
    /* NULL when the name is taken or the description is invalid (the reason is logged) */
    MirageModule*(MIRAGE_CALL* module_register)(MirageMod* mod, const MirageModuleDesc* desc);
    int32_t(MIRAGE_CALL* module_enabled)(MirageModule* module);
    void(MIRAGE_CALL* module_set_enabled)(MirageModule* module, int32_t on);

    /* ---- Settings: controls in the module's panel, saved with the menu's config ------------------------------- */
    /* Made in mod_load, before the config is read: the saved value replaces `value`. NULL if the name is taken. */
    MirageSetting*(MIRAGE_CALL* setting_checkbox)(MirageModule* module, const char* name, int32_t value,
                                                  const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_slider)(MirageModule* module, const char* name, float value, float min,
                                                float max, const char* format, const char* tooltip);
    /* min and max are clamped to +-(INT32_MAX / 2), the most an int slider takes */
    MirageSetting*(MIRAGE_CALL* setting_slider_int)(MirageModule* module, const char* name, int32_t value, int32_t min,
                                                    int32_t max, const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_text)(MirageModule* module, const char* name, const char* value,
                                              const char* tooltip);
    MirageSetting*(MIRAGE_CALL* setting_key)(MirageModule* module, const char* name, int32_t vk, const char* tooltip);

    /* Values. bool/int/float read any numeric setting (a checkbox is 0/1); a key setting's value is its key. */
    int32_t(MIRAGE_CALL* setting_get_int)(MirageSetting* setting);
    float(MIRAGE_CALL* setting_get_float)(MirageSetting* setting);
    void(MIRAGE_CALL* setting_set_int)(MirageSetting* setting, int32_t value);      /* Clamped to the range */
    void(MIRAGE_CALL* setting_set_float)(MirageSetting* setting, float value);
    /* Text: copies up to `capacity` bytes (with the terminator) and returns the full length */
    uint32_t(MIRAGE_CALL* setting_get_text)(MirageSetting* setting, char* buffer, uint32_t capacity);
    void(MIRAGE_CALL* setting_set_text)(MirageSetting* setting, const char* value);
    /* A key setting is held down right now (with the modifiers it was bound with) */
    int32_t(MIRAGE_CALL* key_down)(MirageSetting* setting);

    /* ---- Per-mod config: <mod folder>/config.toml, for what isn't a setting ----------------------------------- */
    /* Copies up to `capacity` bytes and returns the full length; `fallback` (may be NULL) when the key isn't set */
    uint32_t(MIRAGE_CALL* config_get)(MirageMod* mod, const char* key, const char* fallback, char* buffer,
                                      uint32_t capacity);
    void(MIRAGE_CALL* config_set)(MirageMod* mod, const char* key, const char* value);    /* Written right away */

    /* ---- Drawing: only in on_render, in screen pixels, colors 0xAABBGGRR (as ImGui's IM_COL32) ---------------- */
    void(MIRAGE_CALL* screen_size)(float* width, float* height);
    void(MIRAGE_CALL* draw_text)(float x, float y, uint32_t color, const char* text);
    void(MIRAGE_CALL* draw_line)(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    void(MIRAGE_CALL* draw_rect)(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    void(MIRAGE_CALL* draw_rect_filled)(float x1, float y1, float x2, float y2, uint32_t color);
    void(MIRAGE_CALL* text_size)(const char* text, float* width, float* height);

    /* ---- Settings declared in mod.toml ([settings.<key>]) ---------------------------------------------------------
     * The runtime makes them when their module registers (module_register returns with them in place). Their values
     * are read and written with the setting_get / setting_set functions above (a choice is its option's index as an
     * int, its option's text as text). */
    MirageSetting*(MIRAGE_CALL* setting_find)(MirageMod* mod, const char* key);    /* NULL if the mod has none such */
    int32_t(MIRAGE_CALL* setting_type)(MirageSetting* setting);                    /* MIRAGE_SETTING_* */

    /* ---- Events (step 6). Each callback runs on the thread named at its MIRAGE_EVENT_*. Removed at the mod's unload. */
    MirageSubscription*(MIRAGE_CALL* event_subscribe)(MirageMod* mod, int32_t type, MirageEventFn fn, void* user);
    void(MIRAGE_CALL* event_unsubscribe)(MirageSubscription* subscription);

    /* ---- Function hooks (step 6): the game's UnrealScript functions, by path ("Engine.Actor.SetLocation", a state's
     * "TdGame.TdPawn.Walking.EndState"). Game thread. Hooks of one function run by priority, higher first. A native
     * function (FUNC_Native) can be hooked before and after, without parameters; one called by native index from
     * script isn't seen at all. NULL when it can't be hooked (the log says why); a function not loaded yet (a level's)
     * is hooked when it is. Removed at the mod's unload. */
    MirageHook*(MIRAGE_CALL* hook_function)(MirageMod* mod, const char* function, int32_t when, int32_t priority,
                                            MirageHookFn fn, void* user);
    void(MIRAGE_CALL* hook_remove)(MirageHook* hook);

    /* ---- Calls: what a hook sees, or a call the mod makes (call_begin, set its parameters, call_invoke, read its
     * out parameters and "ReturnValue", call_end). Game thread. */
    MirageObject*(MIRAGE_CALL* call_object)(MirageCall* call);          /* The object it's called on */
    const char*(MIRAGE_CALL* call_function)(MirageCall* call);          /* "Engine.Actor.SetLocation" */
    void(MIRAGE_CALL* call_skip)(MirageCall* call);                     /* A before hook: the function doesn't run */
    int32_t(MIRAGE_CALL* call_get)(MirageCall* call, const char* path, MirageValue* out);       /* 0: no such parameter */
    int32_t(MIRAGE_CALL* call_set)(MirageCall* call, const char* path, const MirageValue* in);  /* 0: not set */
    MirageCall*(MIRAGE_CALL* call_begin)(MirageObject* object, const char* function);  /* Its class's (or a parent's) */
    int32_t(MIRAGE_CALL* call_invoke)(MirageCall* call);                /* 1: it ran */
    void(MIRAGE_CALL* call_end)(MirageCall* call);

    /* ---- Game objects (step 6). Game thread. NULL while loading or outside a level. */
    MirageObject*(MIRAGE_CALL* player_pawn)(void);
    MirageObject*(MIRAGE_CALL* player_controller)(void);
    MirageObject*(MIRAGE_CALL* world_info)(void);
    MirageObject*(MIRAGE_CALL* object_find)(const char* full_name);    /* "TdGame.Default__TdPawn", "Class Engine.Actor" */
    int32_t(MIRAGE_CALL* object_get)(MirageObject* object, const char* path, MirageValue* out);
    int32_t(MIRAGE_CALL* object_set)(MirageObject* object, const char* path, const MirageValue* in);
    uint32_t(MIRAGE_CALL* object_name)(MirageObject* object, char* buffer, uint32_t capacity);         /* "TdPlayerPawn_0" */
    uint32_t(MIRAGE_CALL* object_class_name)(MirageObject* object, char* buffer, uint32_t capacity);   /* "TdPlayerPawn" */
    int32_t(MIRAGE_CALL* object_is_a)(MirageObject* object, const char* class_name);   /* An instance of it or a subclass */

    /* ---- Native code (step 6): detours of the game's functions, chained by the runtime ----------------------------
     * Several mods may detour one function: they run by priority, higher first, each calling its *original (the next
     * detour, or the real function); the runtime keeps every *original right as detours come and go, so keep it in a
     * variable that lives as long as the mod. A function the menu hooks itself is refused. Removed at the mod's unload. */
    MirageDetour*(MIRAGE_CALL* detour_create)(MirageMod* mod, void* target, void* detour, int32_t priority, void** original);
    void(MIRAGE_CALL* detour_remove)(MirageDetour* detour);
    /* A byte pattern in the game's code ('x' in `mask` must match, '?' is any byte), NULL if not found */
    void*(MIRAGE_CALL* find_pattern)(const char* pattern, const char* mask);

    /* ---- Script patching (step 8; see MirageToken). Game thread. -------------------------------------------------
     * Token indices are 0-based; script_children / script_set_children with index -1 are the top-level statements. */
    MirageScript*(MIRAGE_CALL* script_open)(MirageMod* mod, const char* function);   /* NULL: none, or no script */
    void(MIRAGE_CALL* script_close)(MirageScript* script);
    int32_t(MIRAGE_CALL* script_count)(MirageScript* script);                        /* Tokens */
    int32_t(MIRAGE_CALL* script_get)(MirageScript* script, int32_t index, MirageToken* out);       /* 0: no such */
    int32_t(MIRAGE_CALL* script_set)(MirageScript* script, int32_t index, const MirageToken* in);  /* 0: not set */
    int32_t(MIRAGE_CALL* script_add)(MirageScript* script, const MirageToken* in);   /* Its index, -1 if refused */
    /* Copies up to `capacity` indices and returns how many there are (-1: no such token) */
    int32_t(MIRAGE_CALL* script_children)(MirageScript* script, int32_t index, int32_t* out, int32_t capacity);
    int32_t(MIRAGE_CALL* script_set_children)(MirageScript* script, int32_t index, const int32_t* children, int32_t count);
    int32_t(MIRAGE_CALL* script_apply)(MirageScript* script);        /* 1: in place (the log says why not) */
    int32_t(MIRAGE_CALL* script_restore)(MirageMod* mod, const char* function);      /* 1: it was patched */
    uint32_t(MIRAGE_CALL* script_dump)(MirageScript* script, char* buffer, uint32_t capacity);    /* Full length */
    int32_t(MIRAGE_CALL* script_kind)(const char* name);            /* -1: no such kind */
    const char*(MIRAGE_CALL* script_kind_name)(int32_t kind);        /* "Bad" if none */

    /* ---- The menu (step 9) ---------------------------------------------------------------------------------------- */
    void(MIRAGE_CALL* notify)(MirageMod* mod, const char* text);   /* A notification on screen for a few seconds. Any thread */
    /* More drawing (on_render only, like the others) */
    void(MIRAGE_CALL* draw_circle)(float x, float y, float radius, uint32_t color, float thickness);
    void(MIRAGE_CALL* draw_circle_filled)(float x, float y, float radius, uint32_t color);
    float(MIRAGE_CALL* frame_rate)(void);       /* Frames per second, averaged over the last second or so */
    /* A color picker; `rgba` packed like the draw colors (0xAABBGGRR). Its value: setting_get_int */
    MirageSetting*(MIRAGE_CALL* setting_color)(MirageModule* module, const char* name, uint32_t rgba, const char* tooltip);

    /* ---- More game objects (step 9). Game thread. ----------------------------------------------------------------
     * The live objects of a class or its subclasses: `class_name` is a short name ("GameEngine") or a path
     * ("Engine.GameEngine"). Class default objects ("Default__GameEngine") only with MIRAGE_OBJECTS_DEFAULTS.
     * Copies up to `capacity` handles into `out` (may be NULL) and returns how many there are; -1 when no such class is
     * loaded. Like any MirageObject, look them up again rather than keep them across ticks. */
    int32_t(MIRAGE_CALL* objects_of_class)(const char* class_name, uint32_t flags, MirageObject** out, int32_t capacity);

    /* ---- The player and input (step 9) ------------------------------------------------------------------------------
     * In a MIRAGE_EVENT_PLAYER_STATE callback: the game doesn't handle the new move state this tick (the engine's move
     * state handler is skipped for it, so a jump made in the callback takes its place). Anywhere else: nothing. */
    void(MIRAGE_CALL* event_skip)(const MirageEvent* event);
    int32_t(MIRAGE_CALL* menu_open)(void);                /* The menu is open (it has the mouse and the keyboard) */
    /* A real key press (down 1) or release (down 0) of a virtual-key code, as the keyboard sends it: the game reads it
     * as the player's own. Any thread. A mod releases what it pressed (on_disable). */
    void(MIRAGE_CALL* input_key)(int32_t vk, int32_t down);
    /* Values a mod shares with other mods and the menu's tests, as text by key ("kick_buffer.held"). Any thread.
     * share_get copies up to `capacity` bytes and returns the full length (0, "" when not set). */
    void(MIRAGE_CALL* share_set)(MirageMod* mod, const char* key, const char* value);
    uint32_t(MIRAGE_CALL* share_get)(const char* mod_id, const char* key, char* buffer, uint32_t capacity);

    /* ---- The UI layer (step 9; see MirageUi) ------------------------------------------------------------------------ */
    const MirageUi*(MIRAGE_CALL* ui)(void);
    /* Shows or hides a setting in its module's panel (one that only matters while another is on). Any thread. */
    void(MIRAGE_CALL* setting_set_visible)(MirageSetting* setting, int32_t visible);
    /* More drawing (on_render, like draw_*): text `size` pixels tall (0: the menu's), rounded rectangles, triangles */
    void(MIRAGE_CALL* draw_text_sized)(float x, float y, uint32_t color, float size, const char* text);
    void(MIRAGE_CALL* text_size_sized)(const char* text, float size, float* width, float* height);
    void(MIRAGE_CALL* draw_rect_rounded)(float x1, float y1, float x2, float y2, uint32_t color, float rounding,
                                         float thickness);          /* thickness 0: filled */
    void(MIRAGE_CALL* draw_triangle_filled)(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color);

    /* ---- The game's input (step 9): what the engine really gets from the keyboard and mouse. The player's own while
     * they play (not while the menu is open: the game doesn't get it then), a TAS macro's while one records or plays.
     * Key names are the engine's, any case: "W", "SpaceBar", "LeftMouseButton", "MouseScrollUp", "one". Any thread.
     * Without the monitor (a build without the TAS engine) input_key_state returns 0: read the keyboard yourself. */
    int32_t(MIRAGE_CALL* input_key_state)(const char* key, MirageKeyState* out);    /* 1: the monitor has it */
    void(MIRAGE_CALL* input_mouse)(float window_seconds, float* x, float* y);       /* Movement in the last window */
    int32_t(MIRAGE_CALL* input_kps)(void);                                          /* Presses in the last second */
    double(MIRAGE_CALL* input_now)(void);                                           /* The monitor's clock (seconds) */
    void(MIRAGE_CALL* input_reset_counts)(void);
    int32_t(MIRAGE_CALL* tas_session)(void);    /* 0: no TAS session; 1 recording; 2 playing back */

    /* ---- Files in the mod's own folder (step 9). `path` is relative to it ("layouts/Movement.json"); "..", drive
     * letters and absolute paths are refused. Any thread. */
    /* Copies up to `capacity` bytes and returns the file's size; -1: no such file */
    int32_t(MIRAGE_CALL* file_read)(MirageMod* mod, const char* path, char* buffer, uint32_t capacity);
    int32_t(MIRAGE_CALL* file_write)(MirageMod* mod, const char* path, const char* data, uint32_t size);   /* 1: written */
    /* The names of the files in a folder (`extension` like ".json", or NULL: all), one per line, sorted; full length */
    uint32_t(MIRAGE_CALL* file_list)(MirageMod* mod, const char* folder, const char* extension, char* buffer, uint32_t capacity);
    uint64_t(MIRAGE_CALL* file_stamp)(MirageMod* mod, const char* path);            /* Last write; 0: no such file */
    void(MIRAGE_CALL* file_open_folder)(MirageMod* mod, const char* folder);        /* In Explorer (made first) */

    /* ---- The generated SDK (sdk/, step 3) in a native mod. The SDK's headers are public: a native mod that links
     * mirage::sdk sets its globals from these once (in mirage_mod_load), then reads game objects directly:
     *     void* names; void* objects; api->sdk_globals(&names, &objects);
     *     Classes::FName::GNames = (decltype(Classes::FName::GNames))names; ... GObjects likewise
     * Memory the SDK reaches is the game's: the game thread only, like every game object. */
    void(MIRAGE_CALL* sdk_globals)(void** gnames, void** gobjects);

    /* ---- The menu (step 9) */
    int32_t(MIRAGE_CALL* module_state)(const char* name);     /* Any module, by name: 1 on, 0 off, -1 no such module */
    uint32_t(MIRAGE_CALL* theme_color)(int32_t which);        /* MIRAGE_THEME_*, packed like the draw colors */

    /* ---- The player's state (step 9): everything plain about the player (position, speed, move, health, timers of
     * their moves, the camera...) as JSON text, put back later: what save states are made of. Game thread.
     * player_state_capture copies up to `capacity` bytes and returns the full length (0: no player, or the layout of
     * the game's values couldn't be worked out). player_state_apply puts one back: 1 done, 0 not (another level,
     * another player class, no player, the player can't be moved now; the log says which). Every apply counts in
     * mirage.shared("memenu", "save_state.loads"). */
    uint32_t(MIRAGE_CALL* player_state_capture)(char* buffer, uint32_t capacity);
    int32_t(MIRAGE_CALL* player_state_apply)(const char* state);
    void(MIRAGE_CALL* player_reset_movement)(void);   /* Back to plain walking: out of a death fall, a slide, stuck input */
    /* Any module, by name (another mod's too), switched on or off as the player would. 0: no such module. Any thread. */
    int32_t(MIRAGE_CALL* module_set_state)(const char* name, int32_t on);

    /* ---- 3D drawing in the world and the camera (step 9; see MirageWorldMesh). ---------------------------------------
     * world_draw_active: on, the renderer draws the layers (and follows the camera); off, nothing. The mod's unload
     * switches it off if it was the one that switched it on. */
    void(MIRAGE_CALL* world_draw_active)(MirageMod* mod, int32_t on);
    void(MIRAGE_CALL* world_draw_set)(int32_t layer, const MirageWorldMesh* mesh);     /* NULL clears the layer */
    void(MIRAGE_CALL* world_draw_style)(const MirageWorldStyle* style);
    void(MIRAGE_CALL* world_draw_stats)(MirageWorldStats* out);
    /* A world position on screen (pixels) with the last frame's view; 0: behind the camera or no frame yet */
    int32_t(MIRAGE_CALL* world_to_screen)(float x, float y, float z, float* sx, float* sy);
    /* The player's view as the game has it this tick; 0: no camera yet. Any thread. */
    int32_t(MIRAGE_CALL* camera_get)(MirageCamera* out);

    /* ---- The menu's ImGui for a native mod built against the same ImGui (step 9) -----------------------------------
     * For a mod whose UI is more than MirageUi has (a timeline, a file browser): the menu's ImGui context, when the mod
     * was built with the same ImGui version and the same struct sizes (else NULL, with the reason in the log). The mod
     * calls ImGui::SetCurrentContext(ctx) and ImGui::SetAllocatorFunctions(alloc, free, user) in each of its render
     * callbacks (api/mirage/imgui.hpp does it). MirageUi is the stable way: this one ties the mod to an ImGui version. */
    void*(MIRAGE_CALL* imgui_context)(const char* version, uint32_t io_size, uint32_t style_size, uint32_t vertex_size,
                                       void** alloc_fn, void** free_fn, void** user);
    /* A setting's own control, drawn where the panel is (on_panel; a setting set_visible 0 is only drawn this way) */
    void(MIRAGE_CALL* setting_draw)(MirageSetting* setting, float width);
    /* A choice (a combo box): `items` like MirageUi::combo's ("A\0B\0"), `index` the default */
    MirageSetting*(MIRAGE_CALL* setting_choice)(MirageModule* module, const char* name, const char* items, int32_t index,
                                                 const char* tooltip);
    /* Keys the mod is using right now (a TAS session's controls): the menu's own hotkeys don't fire on them. Replaces
     * the mod's previous set; count 0 clears it. Any thread. */
    void(MIRAGE_CALL* reserved_keys)(MirageMod* mod, const int32_t* vks, int32_t count);

    /* ---- The TAS engine (step 9): the runtime's (src/tas), for its UI. Options and status are JSON; the replay is in
     * the .merp format both ways. Any thread (commands run on the game thread). */
    int32_t(MIRAGE_CALL* tas_available)(void);                                   /* The engine's hooks were found */
    uint32_t(MIRAGE_CALL* tas_status)(char* json, uint32_t capacity);            /* Full length */
    void(MIRAGE_CALL* tas_set_options)(const char* json);
    int32_t(MIRAGE_CALL* tas_command)(int32_t command, int64_t arg);             /* MIRAGE_TAS_* */
    void(MIRAGE_CALL* tas_record)(int32_t start, const char* command, const char* map, const char* checkpoint);
    uint64_t(MIRAGE_CALL* tas_replay_revision)(void);                            /* Changes whenever the replay does */
    uint32_t(MIRAGE_CALL* tas_replay_get)(char* data, uint32_t capacity);        /* Its size; copied only when it fits */
    int32_t(MIRAGE_CALL* tas_replay_set)(const char* data, uint32_t size);       /* 1: it replaced the replay */
    int32_t(MIRAGE_CALL* tas_file)(int32_t save, const char* path, char* error, uint32_t capacity);  /* 1: done */
    uint32_t(MIRAGE_CALL* tas_folder)(int32_t autosaves, char* path, uint32_t capacity);   /* replays\ or its autosaves */
    uint32_t(MIRAGE_CALL* tas_preflight)(char* json, uint32_t capacity);         /* What may desync a playback */

    /* Every module in the menu (every mod's and the runtime's own), as JSON: [{"name", "tab", "enabled", "action"}, ...]
     * in the menu's order. Copies up to `capacity` bytes, returns the full length. Any thread. */
    uint32_t(MIRAGE_CALL* module_list)(char* json, uint32_t capacity);

    /* ---- Memory (the runtime's memory manager) ---------------------------------------------------------------------
     * The process's memory, the pressure level and the mod's own Lua memory. 1: filled. Any thread. To be told when
     * memory gets short, subscribe to MIRAGE_EVENT_MEMORY_PRESSURE. */
    int32_t(MIRAGE_CALL* memory_stats)(MirageMod* mod, MirageMemoryStats* out);

    /* ---- Script injection (step 11): the mod's code called from inside a script function's bytecode, at a point:
     *   "head"                  before its first statement
     *   "return"                before every return
     *   "before:<Function>[#n]" before each statement that calls <Function> (only the n-th one with #n)
     *   "after:<Function>[#n]"  after it
     *   "token:<i>"             before the statement holding token i (script_dump's indices)
     * The callback gets a MirageCall: call_get / call_set read and write the function's parameters AND its locals by
     * name; call_skip returns from the function there, with what "ReturnValue" holds (ignored inside a ForEach).
     * Callbacks at one place run by priority, higher first. Game thread. NULL when the point matches nothing or the
     * function isn't loaded (the log says why). hook_remove removes it; so does the mod's unload. */
    MirageHook*(MIRAGE_CALL* inject_function)(MirageMod* mod, const char* function, const char* point, int32_t priority,
                                              MirageHookFn fn, void* user);

    /* ---- Mod natives (step 11): a mod function the game's bytecode calls like engine code. `signature` is
     * "float(float, int)": argument types int, float, bool, byte, object, name (arrives as text); return int, float,
     * bool, byte, object or void. Returns the native index to put in a script patch's NativeFunction token (> 0), 0 when
     * it can't be registered (the log says why). The same mod and name again (a hot reload) gets the same index. After
     * the mod's unload its natives return zero. Game thread for the calls. */
    int32_t(MIRAGE_CALL* native_register)(MirageMod* mod, const char* name, const char* signature, MirageNativeFn fn, void* user);
    int32_t(MIRAGE_CALL* native_arg)(MirageNativeCall* call, int32_t index, MirageValue* out);    /* 0: no such argument */
    void(MIRAGE_CALL* native_return)(MirageNativeCall* call, const MirageValue* value);
    MirageObject*(MIRAGE_CALL* native_self)(MirageNativeCall* call);    /* The object the calling script runs on */

    /* ---- Classes from UnrealScript (step 12): `path` (relative to the mod's folder, or absolute) is a `.uc` file
     * holding one class, named like the file ("MirageBouncePad.uc": `class MirageBouncePad extends Trigger;`, its
     * variables, functions and events, defaultproperties). It is compiled into a script package and the engine loads
     * it: the class (a UClass) spawns and reflects like a cooked one. NULL when it doesn't compile or load: `error`
     * gets why (up to `capacity` bytes; may be NULL). The same source again gives the same class; an edited one (a hot
     * reload) gives a new class of the same name. Game thread. */
    MirageObject*(MIRAGE_CALL* class_load)(MirageMod* mod, const char* path, char* error, uint32_t capacity);
    /* A class described instead of written, as JSON: {"name": "MyPad", "extends": "Trigger", "placeable": true,
     * "vars": ["float Strength", "var() int Count"], "functions": ["Touch", "function int Count()"],
     * "defaults": ["Strength=1500.0"], "extra": "<more UnrealScript>"}. A function given by name overrides the parent's
     * (its declaration, an empty body); one given as a declaration is new (an empty body too). Their code is the mod's:
     * inject_function(<the class's path>.<function>, "head", ...) reads the parameters and self, sets ReturnValue and
     * returns early. The runtime writes the .uc to memenu\classes\generated\<mod>\ and loads it as class_load does.
     * Game thread. */
    MirageObject*(MIRAGE_CALL* class_define)(MirageMod* mod, const char* spec_json, char* error, uint32_t capacity);
    /* "Package.Outer.Name" ("MirageBouncePad_1a2b3c4d.MirageBouncePad"): copies up to capacity, returns the full length */
    uint32_t(MIRAGE_CALL* object_path)(MirageObject* object, char* buffer, uint32_t capacity);
    /* Keeps an object from the garbage collector (keep 1) or lets it go (0); the mod's unload lets go of all it kept.
     * A mod's classes go with it: unloading destroys their actors and they can't be spawned until loaded again.
     * Game thread. */
    int32_t(MIRAGE_CALL* object_keep)(MirageMod* mod, MirageObject* object, int32_t keep);

    /* ---- Level additions (step 13): a mod's levels/<map>.toml places actors into a level (spawned on level load and
     * again, fresh, at every checkpoint restart; removed with the mod). The actor named `name` there in the current
     * level, of the mod `mod_id` (NULL or "": the calling mod's own). NULL: none (not spawned, another level). Game thread. */
    MirageObject*(MIRAGE_CALL* layer_actor)(MirageMod* mod, const char* mod_id, const char* name);

    /* ---- World access (step 14; see MirageHandle, MirageActorQuery, MIRAGE_EVENT_ACTOR_*). Game thread. */
    /* The handle of a live object: 1, or 0 (no object, or it is gone). Set out->size first. */
    int32_t(MIRAGE_CALL* object_handle)(MirageObject* object, MirageHandle* out);
    /* The object a handle names, while it is still that object; NULL once it is gone (or another took its place) */
    MirageObject*(MIRAGE_CALL* handle_object)(const MirageHandle* handle);
    /* The actors in play that match, up to `capacity` of them into `out` (may be NULL to count); returns how many match */
    int32_t(MIRAGE_CALL* actors_query)(const MirageActorQuery* query, MirageObject** out, int32_t capacity);
} MirageApi;

/* The runtime has `field` (a mod built against a newer header, running on an older runtime) */
#define MIRAGE_API_HAS(api, field) \
    ((api)->size >= offsetof(MirageApi, field) + sizeof((api)->field) && (api)->field != NULL)

/* What a native mod exports */
typedef uint32_t(MIRAGE_CALL* MirageModAbiFn)(void);
typedef int32_t(MIRAGE_CALL* MirageModLoadFn)(const MirageApi* api, MirageMod* mod);
typedef void(MIRAGE_CALL* MirageModUnloadFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MIRAGE_API_H */
