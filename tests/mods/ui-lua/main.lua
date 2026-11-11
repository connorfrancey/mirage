-- Self-test mod: while on, draws a window with most of mirage.ui's widgets every frame, and its module panel. It also
-- leaves a window and a color open on purpose: the runtime must close them for it (the menu's frame goes on).
-- Shares what it drew: frames, panel_frames, last_error.
local ui = mirage.ui
local frames, panel_frames = 0, 0
local state = { on = true, f = 0.5, i = 3, text = "hello", choice = 2, color = mirage.rgba(10, 20, 30, 255), open = true }

local function widgets()
    ui.text("Text", 1, 2)
    ui.text_colored(mirage.rgba(255, 0, 0), "colored")
    ui.text_disabled("disabled")
    ui.text_wrapped("wrapped text that goes on for a while to wrap in a narrow window")
    ui.label_text("label", "value")
    ui.bullet_text("bullet")
    ui.separator()
    if ui.button("Button") then state.i = state.i + 1 end
    ui.same_line()
    ui.small_button("Small")
    local _
    _, state.on = ui.checkbox("Checkbox", state.on)
    _, state.f = ui.slider_float("Slider", state.f, 0, 1, "%.2f")
    _, state.i = ui.slider_int("Slider int", state.i, 0, 10)
    _, state.f = ui.drag_float("Drag", state.f, 0.01, 0, 1)
    _, state.i = ui.input_int("Input int", state.i)
    _, state.text = ui.input_text("Input text", state.text)
    _, state.choice = ui.combo("Combo", state.choice, { "One", "Two", "Three" })
    _, state.color = ui.color_edit("Color", state.color)
    ui.progress_bar(state.f, -1, 0, "progress")
    ui.plot_lines("Plot", { 1, 3, 2, 5, 4 }, 0, 0, 0, 40)
    if ui.tree_node("Tree", true) then
        ui.text("inside")
        ui.tree_pop()
    end
    if ui.table_begin("table", 2, ui.TABLE_BORDERS + ui.TABLE_ROW_BG) then
        ui.table_column("A", ui.COLUMN_STRETCH)
        ui.table_column("B", ui.COLUMN_FIXED, 60)
        ui.table_headers()
        for r = 1, 3 do
            ui.table_next_row()
            ui.table_next_column()
            ui.text("row " .. r)
            ui.table_next_column()
            ui.selectable("sel##" .. r, r == 2)
        end
        ui.table_end()
    end
    if ui.item_hovered() then ui.tooltip("tip") end
end

local m
m = mirage.module {
    name = "UI test (Lua)", tab = "Mods", description = "Self-test: the UI layer",
    on_render = function()
        ui.window_next_pos(40, 40)
        ui.window_next_size(320, 520)
        local shown, open = ui.window_begin("Mirage UI test", 0, state.open)
        state.open = open
        if shown then widgets() end
        ui.window_end()

        -- Left open on purpose: the runtime closes it (and pops the color) after this callback
        ui.color_push(ui.COLOR_TEXT, mirage.rgba(0, 255, 0))
        ui.window_begin("Mirage UI test (unclosed)", ui.WINDOW_AUTO_SIZE)
        ui.text("never closed by the mod")
        frames = frames + 1
        mirage.share("frames", frames)
    end,
    on_panel = function()
        ui.text("Panel of " .. mirage.id)
        widgets()
        panel_frames = panel_frames + 1
        mirage.share("panel_frames", panel_frames)
        Note:set_visible(state.on)
    end,
}
Note = mirage.setting("note")
