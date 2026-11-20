#pragma once
// Mods' settings in the game's own options menu (Mirage, step 31): a slider a mod adds shows on a "Mod settings" page
// of the game's pause menu (Pause > Options > Mod settings), drawn by the game's UI like its own pages.
//
// Mirror's Edge's menus are UE3 UIScenes (no Scaleform). The pause menu's Options list (TdUIScene_PauseOptions) gets a
// button: a hook after its PostInitialize clones the last option button it shows into its ContentPanel, docked below
// it, with its siblings' click (the scene's OnOptionButtonClicked); a hook on HandleOptionButton (which picks a button
// by name) opens the page for it. The page is the game's own audio
// page (TdUI_Options.TdAudioSettings, a TdUIScene_OptionMenu: a panel of sliders with labels, the Accept / Cancel /
// Reset bar, the menu's look) retitled, its sliders unbound from the profile's volumes and given the mods' labels,
// ranges and values, the ones left over and the audio device choice hidden. Its Accept then saves nothing of the
// profile (no slider is bound to it), its Cancel puts the sliders back to what they were when the page opened, and
// its Defaults (hooked: the audio page's would reset the player's volumes) puts them back to their first values.
//
// Mods read their sliders' values (Value), which follow the page while it is open. Shortcut: one page, the first three
// sliders of all mods (the audio page has three); more need a page with more slider rows.

#include <string>

namespace mirage::options_menu
{
	// A slider of `owner` (a mod): its id (> 0). `value` is clamped to [min, max]; `step` is a nudge of the arrows.
	// Any thread.
	int AddSlider(const std::string& owner, const std::string& label, float min, float max, float step, float value);
	// Its value now; false for another owner's id or one that is gone. Any thread.
	bool Value(const std::string& owner, int id, float* value);
	// Every slider of `owner` gone (the mod unloads). Any thread.
	void RemoveAll(const std::string& owner);

	// The hooks in place (once). Game thread.
	void Install();
	// The open page's sliders read. Game thread, every tick.
	void Tick();

	// For the self-test: the pause menu's Options scene open now (nullptr if none) and the mods' page (nullptr if not open)
	void* OpenOptionsScene();
	void* OpenPage();
	// Clicks the "Mod settings" button of the open Options scene. False if there is none.
	bool ClickModSettings();
	// Sets the open page's slider `index` (0 to 2) as the player would. False if the page isn't open.
	bool SetPageSlider(int index, float value);
	// The Options scene's button: its name, flags and place on screen (for the self-test's notes)
	std::string DescribeButton();
	// The open page's slider widget `index` (a UISlider; nullptr when there is none)
	void* PageSlider(int index);
}
