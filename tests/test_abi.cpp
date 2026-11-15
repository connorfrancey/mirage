// The Mirage mod ABI (loader/include/mirage/api.h) is frozen: these are the sizes and offsets of ABI v1 on the game's 32-bit
// target. A failure here means a change that breaks mods already built: append to MirageApi instead (and add the new
// fields' offsets below), never insert, remove, reorder or retype.
#include "mirage/api.h"

#include <gtest/gtest.h>

#include <cstddef>

static_assert(sizeof(void*) == 4, "the ABI is frozen for the game's 32-bit target");

// ---- ABI v1 ------------------------------------------------------------------------------------------------------------
static_assert(MIRAGE_ABI_VERSION >= 1);
static_assert(MIRAGE_LOG_DEBUG == 0 && MIRAGE_LOG_INFO == 1 && MIRAGE_LOG_WARN == 2 && MIRAGE_LOG_ERROR == 3);
static_assert(MIRAGE_MODULE_ACTION == 0x1u && MIRAGE_MODULE_ENABLED_BY_DEFAULT == 0x2u);

static_assert(sizeof(MirageModuleDesc) >= 52);   // 52 in the first ABI v1 (a mod with that size still works)
static_assert(offsetof(MirageModuleDesc, size) == 0);
static_assert(offsetof(MirageModuleDesc, name) == 4);
static_assert(offsetof(MirageModuleDesc, tab) == 8);
static_assert(offsetof(MirageModuleDesc, description) == 12);
static_assert(offsetof(MirageModuleDesc, order) == 16);
static_assert(offsetof(MirageModuleDesc, key) == 20);
static_assert(offsetof(MirageModuleDesc, flags) == 24);
static_assert(offsetof(MirageModuleDesc, user) == 28);
static_assert(offsetof(MirageModuleDesc, on_enable) == 32);
static_assert(offsetof(MirageModuleDesc, on_disable) == 36);
static_assert(offsetof(MirageModuleDesc, on_tick) == 40);
static_assert(offsetof(MirageModuleDesc, on_render) == 44);
static_assert(offsetof(MirageModuleDesc, on_trigger) == 48);

#define MIRAGE_AT(field, offset) static_assert(offsetof(MirageApi, field) == (offset), #field " moved")
MIRAGE_AT(size, 0);
MIRAGE_AT(abi_version, 4);
MIRAGE_AT(runtime_version, 8);
MIRAGE_AT(log, 12);
MIRAGE_AT(mod_id, 16);
MIRAGE_AT(mod_version, 20);
MIRAGE_AT(mod_dir, 24);
MIRAGE_AT(module_register, 28);
MIRAGE_AT(module_enabled, 32);
MIRAGE_AT(module_set_enabled, 36);
MIRAGE_AT(setting_checkbox, 40);
MIRAGE_AT(setting_slider, 44);
MIRAGE_AT(setting_slider_int, 48);
MIRAGE_AT(setting_text, 52);
MIRAGE_AT(setting_key, 56);
MIRAGE_AT(setting_get_int, 60);
MIRAGE_AT(setting_get_float, 64);
MIRAGE_AT(setting_set_int, 68);
MIRAGE_AT(setting_set_float, 72);
MIRAGE_AT(setting_get_text, 76);
MIRAGE_AT(setting_set_text, 80);
MIRAGE_AT(key_down, 84);
MIRAGE_AT(config_get, 88);
MIRAGE_AT(config_set, 92);
MIRAGE_AT(screen_size, 96);
MIRAGE_AT(draw_text, 100);
MIRAGE_AT(draw_line, 104);
MIRAGE_AT(draw_rect, 108);
MIRAGE_AT(draw_rect_filled, 112);
MIRAGE_AT(text_size, 116);
MIRAGE_AT(setting_find, 120);
MIRAGE_AT(setting_type, 124);
MIRAGE_AT(event_subscribe, 128);
MIRAGE_AT(event_unsubscribe, 132);
MIRAGE_AT(hook_function, 136);
MIRAGE_AT(hook_remove, 140);
MIRAGE_AT(call_object, 144);
MIRAGE_AT(call_function, 148);
MIRAGE_AT(call_skip, 152);
MIRAGE_AT(call_get, 156);
MIRAGE_AT(call_set, 160);
MIRAGE_AT(call_begin, 164);
MIRAGE_AT(call_invoke, 168);
MIRAGE_AT(call_end, 172);
MIRAGE_AT(player_pawn, 176);
MIRAGE_AT(player_controller, 180);
MIRAGE_AT(world_info, 184);
MIRAGE_AT(object_find, 188);
MIRAGE_AT(object_get, 192);
MIRAGE_AT(object_set, 196);
MIRAGE_AT(object_name, 200);
MIRAGE_AT(object_class_name, 204);
MIRAGE_AT(object_is_a, 208);
MIRAGE_AT(detour_create, 212);
MIRAGE_AT(detour_remove, 216);
MIRAGE_AT(find_pattern, 220);
// Step 8: script patching
MIRAGE_AT(script_open, 224);
MIRAGE_AT(script_close, 228);
MIRAGE_AT(script_count, 232);
MIRAGE_AT(script_get, 236);
MIRAGE_AT(script_set, 240);
MIRAGE_AT(script_add, 244);
MIRAGE_AT(script_children, 248);
MIRAGE_AT(script_set_children, 252);
MIRAGE_AT(script_apply, 256);
MIRAGE_AT(script_restore, 260);
MIRAGE_AT(script_dump, 264);
MIRAGE_AT(script_kind, 268);
MIRAGE_AT(script_kind_name, 272);
// Step 9: the menu
MIRAGE_AT(notify, 276);
MIRAGE_AT(draw_circle, 280);
MIRAGE_AT(draw_circle_filled, 284);
MIRAGE_AT(frame_rate, 288);
MIRAGE_AT(setting_color, 292);
MIRAGE_AT(objects_of_class, 296);
MIRAGE_AT(event_skip, 300);
MIRAGE_AT(menu_open, 304);
MIRAGE_AT(input_key, 308);
MIRAGE_AT(share_set, 312);
MIRAGE_AT(share_get, 316);
MIRAGE_AT(ui, 320);
MIRAGE_AT(setting_set_visible, 324);
MIRAGE_AT(draw_text_sized, 328);
MIRAGE_AT(text_size_sized, 332);
MIRAGE_AT(draw_rect_rounded, 336);
MIRAGE_AT(draw_triangle_filled, 340);
MIRAGE_AT(input_key_state, 344);
MIRAGE_AT(tas_session, 364);
MIRAGE_AT(file_read, 368);
MIRAGE_AT(file_open_folder, 384);
MIRAGE_AT(sdk_globals, 388);
MIRAGE_AT(module_state, 392);
MIRAGE_AT(theme_color, 396);
MIRAGE_AT(player_state_capture, 400);
MIRAGE_AT(player_reset_movement, 408);
MIRAGE_AT(module_set_state, 412);
MIRAGE_AT(world_draw_active, 416);
MIRAGE_AT(camera_get, 436);
MIRAGE_AT(imgui_context, 440);
MIRAGE_AT(reserved_keys, 452);
MIRAGE_AT(tas_available, 456);
MIRAGE_AT(tas_preflight, 496);
MIRAGE_AT(module_list, 500);
MIRAGE_AT(memory_stats, 504);
MIRAGE_AT(inject_function, 508);
MIRAGE_AT(native_register, 512);
MIRAGE_AT(native_arg, 516);
MIRAGE_AT(native_return, 520);
MIRAGE_AT(native_self, 524);
MIRAGE_AT(class_load, 528);
MIRAGE_AT(class_define, 532);
MIRAGE_AT(object_path, 536);
MIRAGE_AT(object_keep, 540);
MIRAGE_AT(layer_actor, 544);
MIRAGE_AT(object_handle, 548);
MIRAGE_AT(handle_object, 552);
MIRAGE_AT(actors_query, 556);
MIRAGE_AT(change_begin, 560);
MIRAGE_AT(change_end, 564);
MIRAGE_AT(change_revert, 568);
MIRAGE_AT(change_list, 572);
MIRAGE_AT(symbol_find, 576);
MIRAGE_AT(detour_symbol, 580);
MIRAGE_AT(mid_hook_create, 584);
MIRAGE_AT(mid_hook_remove, 588);
MIRAGE_AT(test_register, 592);
MIRAGE_AT(movement_get, 596);
MIRAGE_AT(kismet_read, 600);
static_assert(sizeof(MirageMovement) == 36 && offsetof(MirageMovement, move_object) == 32);
static_assert(sizeof(MirageRegisters) == 44 && offsetof(MirageRegisters, continue_at) == 40);
// managed/Mirage/Native/Structs.cs mirrors these by hand (Bridge.Initialize checks the same sizes in the game)
static_assert(sizeof(MirageModuleDesc) == 56 && sizeof(MirageEvent) == 28 && sizeof(MirageValue) == 272);
static_assert(sizeof(MirageMemoryStats) == 56 && offsetof(MirageMemoryStats, process_private) == 8);
static_assert(sizeof(MirageWorldVertex) == 16 && sizeof(MirageWorldLabel) == 20 && sizeof(MirageWorldMesh) == 36);
static_assert(sizeof(MirageCamera) == 32);
static_assert(sizeof(MirageKeyState) == 32 && offsetof(MirageKeyState, down) == 16);   // doubles: 8-aligned on Windows x86
static_assert(offsetof(MirageModuleDesc, on_panel) == 52, "MirageModuleDesc moved");
// MirageUi: its own append-only table
#define MIRAGE_UI_AT(field, offset) static_assert(offsetof(MirageUi, field) == (offset), #field " moved")
MIRAGE_UI_AT(window_begin, 4);
MIRAGE_UI_AT(text, 108);
MIRAGE_UI_AT(button, 132);
MIRAGE_UI_AT(table_begin, 212);
MIRAGE_UI_AT(item_hovered, 236);
MIRAGE_UI_AT(delta_time, 324);
static_assert(sizeof(MirageToken) == 384 && offsetof(MirageToken, name) == 48 && offsetof(MirageToken, target) == 120 &&
              offsetof(MirageToken, text) == 128, "MirageToken moved");
// Size of ABI v1's table: a later version only makes it larger
static_assert(sizeof(MirageApi) >= 224);
static_assert(sizeof(MirageValue) == 272 && offsetof(MirageValue, text) == 16);
static_assert(sizeof(MirageEvent) == 28 && offsetof(MirageEvent, number) == 16 && offsetof(MirageEvent, text) == 20 &&
              offsetof(MirageEvent, object) == 24, "MirageEvent moved (step 14 appended object)");
static_assert(sizeof(MirageHandle) == 24 && offsetof(MirageHandle, slot) == 8 && offsetof(MirageHandle, object_class) == 20);
static_assert(sizeof(MirageActorQuery) == 36 && offsetof(MirageActorQuery, near_object) == 16 && offsetof(MirageActorQuery, radius) == 32);
static_assert(MIRAGE_EVENT_MEMORY_PRESSURE == 6 && MIRAGE_EVENT_ACTOR_ADDED == 7 && MIRAGE_EVENT_ACTOR_REMOVED == 8 &&
              MIRAGE_EVENT_LEVEL_STREAMED == 9 && MIRAGE_EVENT_RESTART == 10);
static_assert(MIRAGE_HOOK_BEFORE == 0 && MIRAGE_HOOK_AFTER == 1 && MIRAGE_HOOK_REPLACE == 2);
static_assert(MIRAGE_EVENT_TICK == 0 && MIRAGE_EVENT_LEVEL_LOAD == 1 && MIRAGE_EVENT_DEATH == 2 &&
              MIRAGE_EVENT_PLAYER_STATE == 3 && MIRAGE_EVENT_KEY == 4 && MIRAGE_EVENT_STATE_RESTORED == 5);
static_assert(MIRAGE_VALUE_NONE == 0 && MIRAGE_VALUE_TEXT == 5 && MIRAGE_VALUE_STRUCT == 6 && MIRAGE_VALUE_ARRAY == 7);
static_assert(MIRAGE_SETTING_BOOL == 0 && MIRAGE_SETTING_INT == 1 && MIRAGE_SETTING_FLOAT == 2 &&
              MIRAGE_SETTING_STRING == 3 && MIRAGE_SETTING_CHOICE == 4 && MIRAGE_SETTING_KEY == 5);

TEST(Abi, NewerRuntimeTableCoversOlderMods)
{
	// A runtime whose table stops before a field doesn't have it, as a mod built against a newer header sees it
	MirageApi api{};
	api.size = offsetof(MirageApi, text_size);
	api.text_size = nullptr;
	EXPECT_FALSE(MIRAGE_API_HAS(&api, text_size));
	api.size = sizeof(MirageApi);
	api.text_size = [](const char*, float*, float*) {};
	EXPECT_TRUE(MIRAGE_API_HAS(&api, text_size));
}
