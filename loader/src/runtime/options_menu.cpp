// Mods' settings in the game's options menu (options_menu.h)
#include "runtime/options_menu.h"

#include "game/object_cache.h"
#include "game/object_query.h"
#include "game/uobject.h"
#include "runtime/call.h"
#include "runtime/fhooks.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace mirage::options_menu
{
	namespace
	{
		constexpr int kRows = 3;		// The audio page's slider rows
		// The audio page's rows: slider and its label, top to bottom
		constexpr const char* kSliders[kRows] = {"MasterVolumeSlider", "MusicVolumeSlider", "DialogueVolumeSlider"};
		constexpr const char* kLabels[kRows] = {"MasterVolumeLabel", "MusicVolumeLabel", "DialogueVolumeLabel"};

		struct Slider
		{
			std::string owner;
			int id = 0;
			std::string label;
			float min = 0, max = 1, step = 0.1f, value = 0;
			float initial = 0;		// Its value when added: what the page's Defaults puts back
		};
		std::mutex g_lock;
		std::vector<Slider> g_sliders;
		int g_nextId = 1;

		// The open page (game thread): the scene and, row by row, its slider widget and the slider's id
		Classes::UObject* g_page = nullptr;
		std::size_t g_pageSlot = 0;
		Classes::UUISlider* g_rows[kRows] = {};
		int g_rowIds[kRows] = {};
		bool g_installed = false;
		// The button of the Options scene made last (the engine names a new widget itself: it is known by its address)
		Classes::UObject* g_button = nullptr;
		std::size_t g_buttonSlot = 0;
		bool IsButton(const Classes::UObject* o) { return o && o == g_button && game::AliveAt(g_button, g_buttonSlot); }

		// Open: in its scene client's active scenes (a closed scene keeps its SceneClient)
		bool IsOpen(const Classes::UObject* o)
		{
			auto* scene = static_cast<const Classes::UUIScene*>(o);
			auto* client = scene ? static_cast<const Classes::UGameUISceneClient*>(static_cast<const Classes::UObject*>(scene->SceneClient)) : nullptr;
			if (!client) return false;
			for (std::size_t i = 0; i < client->ActiveScenes.Num(); ++i)
				if (client->ActiveScenes[i] == scene) return true;
			return false;
		}

		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		Classes::UObject* FindChild(Classes::UObject* parent, const char* name)
		{
			if (!parent) return nullptr;
			call::Params p(parent, "FindChild");
			p.SetName("WidgetName", name);
			p.Set("bRecurse", true);
			return call::Call(parent, p) ? p.GetObject("ReturnValue") : nullptr;
		}

		// A widget's data binding: its markup, plain text for a label ("Mod settings")
		void Bind(Classes::UObject* widget, const std::string& markup)
		{
			if (!widget) return;
			const std::wstring text = Wide(markup);
			Classes::FString s(text.c_str());
			call::Params p(widget, "SetDataStoreBinding");
			p.SetRaw("MarkupText", &s, sizeof s);
			p.Set("BindingIndex", std::int32_t{-1});
			call::Call(widget, p);
		}

		// A widget's face (left 0, top 1, right 2, bottom 3) on screen, in pixels
		float Face(Classes::UObject* widget, std::uint8_t face)
		{
			call::Params p(widget, "GetPosition");
			p.Set("Face", face);
			p.Set("OutputType", std::uint8_t{1});		// EVALPOS_PixelViewport
			return call::Call(widget, p) ? p.GetFloat("ReturnValue") : 0.f;
		}

		void Show(Classes::UObject* widget, bool visible)
		{
			if (!widget) return;
			call::Params p(widget, "SetVisibility");
			p.Set("bIsVisible", visible);
			call::Call(widget, p);
		}

		void Dock(Classes::UObject* widget, std::uint8_t face, Classes::UObject* target, std::uint8_t targetFace)
		{
			call::Params dock(widget, "SetDockTarget");
			dock.Set("SourceFace", face);
			dock.Set("Target", target);
			dock.Set("TargetFace", targetFace);
			call::Call(widget, dock);
		}

		// After TdUIScene_PauseOptions.PostInitialize: the button, at the bottom of the list, clicking the way its siblings
		// do. The list stacks from the bottom up: the lowest button docks to the panel's bottom (faces: left 0, top 1,
		// right 2, bottom 3), each one above it to the top of the one below. So the new button is a clone of the lowest
		// (which docks it where that one was), and the lowest docks onto it the way the row above docks onto the lowest.
		void OnOptionsInit(void*, hooks::Call& c)
		{
			{
				std::lock_guard lock(g_lock);
				if (g_sliders.empty()) return;
			}
			auto* panel = static_cast<Classes::UUIObject*>(FindChild(c.object, "ContentPanel"));
			if (!panel) return;
			const auto isButton = [](Classes::UUIObject* w) { return w && w->IsA(Classes::UUILabelButton::StaticClass()); };
			Classes::UUIObject *lowest = nullptr, *above = nullptr;
			for (std::size_t i = 0; i < panel->Children.Num(); ++i)
				if (auto* w = panel->Children[i]; isButton(w) && w->DockTargets.TargetWidget[3] == panel) lowest = w;
			for (std::size_t i = 0; lowest && i < panel->Children.Num(); ++i)
				if (auto* w = panel->Children[i]; isButton(w) && !w->bHidden && w->DockTargets.TargetWidget[3] == lowest) above = w;
			if (!lowest || !above) {
				spdlog::warn("Options menu: the Options list isn't stacked the way it was (no Mod settings button)");
				return;
			}
			call::Params create(panel, "CreateWidget");
			create.Set("Owner", static_cast<Classes::UObject*>(panel));
			create.Set("WidgetClass", static_cast<Classes::UObject*>(lowest->Class));
			create.Set("WidgetArchetype", static_cast<Classes::UObject*>(lowest));
			auto* button = static_cast<Classes::UUIObject*>(call::Call(panel, create) ? create.GetObject("ReturnValue") : nullptr);
			if (!button) {
				spdlog::warn("Options menu: the Mod settings button wasn't made");
				return;
			}
			button->WidgetTag = button->Name;		// Its own: a panel takes no second child with a sibling's tag
			call::Params insert(panel, "InsertChild");
			insert.Set("NewChild", static_cast<Classes::UObject*>(button));
			insert.Set("InsertIndex", std::int32_t{-1});
			insert.Set("bRenameExisting", true);
			if (!call::Call(panel, insert) || insert.GetInt("ReturnValue") < 0) {
				spdlog::warn("Options menu: the panel didn't take the Mod settings button");
				return;
			}
			// The clone docks as the lowest did, to the panel's bottom; the lowest goes on top of it, padded as the row
			// above it is on top of it
			for (std::uint8_t face = 0; face < 4; ++face)
				Dock(button, face, lowest->DockTargets.TargetWidget[face], static_cast<std::uint8_t>(lowest->DockTargets.TargetFace[face].GetValue()));
			button->DockTargets.DockPadding = lowest->DockTargets.DockPadding;
			for (std::uint8_t face : {std::uint8_t{1}, std::uint8_t{3}}) {
				Dock(lowest, face, button, static_cast<std::uint8_t>(above->DockTargets.TargetFace[face].GetValue()));
				lowest->DockTargets.DockPadding.PaddingValue[face] = above->DockTargets.DockPadding.PaddingValue[face];
				lowest->DockTargets.DockPadding.PaddingScaleType[face] = above->DockTargets.DockPadding.PaddingScaleType[face];
			}
			Bind(button, "MOD SETTINGS");		// Its caption, plain text instead of the clone's localized one (upper case, as theirs)
			// Its click: the scene's, as PostInitialize gave its siblings (OnClicked -> OnOptionButtonClicked ->
			// HandleOptionButton, which the hook below answers for this button)
			const reflect::Value from = reflect::Resolve(lowest->Class, lowest, "__OnClicked__Delegate");
			const reflect::Value to = reflect::Resolve(button->Class, button, "__OnClicked__Delegate");
			if (from && to) std::memcpy(to.address, from.address, from.member->elementSize);
			Show(button, true);
			// The scene lays itself out again: docking, positions, focus links and styles take the new widget in
			call::Params update(c.object, "RequestSceneUpdate");
			update.Set("bDockingStackChanged", true);
			update.Set("bPositionsChanged", true);
			update.Set("bNavLinksOutdated", true);
			update.Set("bWidgetStylesChanged", true);
			call::Call(c.object, update);
			g_button = button;
			g_buttonSlot = game::IndexOf(button);
			spdlog::info("Options menu: Mod settings button added under {}", lowest->GetName());
		}

		// The page opened over `scene`: the audio page, made the mods'
		void OpenPageOver(Classes::UObject* scene)
		{
			auto* content = game::FindObject<Classes::UObject>("TdHUDContent TdGame.Default__TdHUDContent");
			if (!content) return;
			call::Params get(content, "GetUISceneByName");
			get.SetName("UISceneName", "TdAudioSettings");
			Classes::UObject* archetype = call::Call(content, get) ? get.GetObject("ReturnValue") : nullptr;
			if (!archetype) {
				spdlog::warn("Options menu: no TdAudioSettings scene to make the page from");
				return;
			}
			call::Params open(scene, "OpenScene");
			open.Set("SceneToOpen", archetype);
			Classes::UObject* page = call::Call(scene, open) ? open.GetObject("ReturnValue") : nullptr;
			if (!page) {
				spdlog::warn("Options menu: the page didn't open");
				return;
			}
			std::vector<Slider> sliders;
			{
				std::lock_guard lock(g_lock);
				sliders = g_sliders;
			}
			Bind(FindChild(page, "TitleLabel"), "Mod settings");
			// shortcut: the description follows the focused widget (the audio page's own texts), so it is hidden; a
			// slider's own description needs the page's focus events
			Show(FindChild(page, "DescriptionLabel"), false);
			Show(FindChild(page, "AudioDeviceOptionButton"), false);
			for (int r = 0; r < kRows; ++r) {
				auto* slider = static_cast<Classes::UUISlider*>(FindChild(page, kSliders[r]));
				g_rows[r] = nullptr;
				g_rowIds[r] = 0;
				if (!slider) continue;
				if (r >= static_cast<int>(sliders.size())) {
					Show(slider, false);
					continue;
				}
				const Slider& s = sliders[static_cast<std::size_t>(r)];
				Bind(slider, "");		// Not the profile's volume any more
				slider->SliderValue.MinValue = s.min;
				slider->SliderValue.MaxValue = s.max;
				slider->SliderValue.NudgeValue = s.step;
				slider->SliderValue.bIntRange = 0;
				call::Params set(slider, "SetValue");
				set.Set("NewValue", s.value);
				set.Set("bPercentageValue", false);
				call::Call(slider, set);
				Bind(FindChild(page, kLabels[r]), s.label);
				g_rows[r] = slider;
				g_rowIds[r] = s.id;
			}
			// Cancel puts back what the sliders hold now, not the profile's volumes the page read when it opened
			call::Params backup(page, "BackupWidgetValues");
			call::Call(page, backup);
			g_page = page;
			g_pageSlot = game::IndexOf(page);
			spdlog::info("Options menu: Mod settings page open ({} slider(s))", std::min<std::size_t>(sliders.size(), kRows));
		}

		// Before TdUIScene_AudioSettings.ResetSettingsToDefault (the page's Defaults): on the mods' page, the mods' sliders
		// go back to their first values instead of the profile's volumes to theirs
		void OnResetDefaults(void*, hooks::Call& c)
		{
			if (c.object != g_page || !game::AliveAt(g_page, g_pageSlot)) return;
			std::vector<float> initial(kRows, 0.f);
			{
				std::lock_guard lock(g_lock);
				for (int r = 0; r < kRows; ++r)
					for (const Slider& s : g_sliders)
						if (s.id == g_rowIds[r]) initial[static_cast<std::size_t>(r)] = s.initial;
			}
			for (int r = 0; r < kRows; ++r) {
				if (!g_rows[r]) continue;
				call::Params set(g_rows[r], "SetValue");
				set.Set("NewValue", initial[static_cast<std::size_t>(r)]);
				set.Set("bPercentageValue", false);
				call::Call(g_rows[r], set);
			}
			c.skip = true;
		}

		// Before TdUIScene_PauseOptions.HandleOptionButton: ours opens the page
		void OnOptionButton(void*, hooks::Call& c)
		{
			Classes::UObject* button = reflect::GetObj(c.Param("OptionButton"));
			if (!IsButton(button)) return;
			OpenPageOver(c.object);
			if (const auto r = c.Param("ReturnValue")) reflect::SetInt(r, 1);
			c.skip = true;
		}
	}

	int AddSlider(const std::string& owner, const std::string& label, float min, float max, float step, float value)
	{
		if (!(max > min)) std::swap(min, max);
		std::lock_guard lock(g_lock);
		Slider s{owner, g_nextId++, label, min, max, step > 0 ? step : (max - min) / 10, std::clamp(value, min, max)};
		s.initial = s.value;
		g_sliders.push_back(s);
		return s.id;
	}

	bool Value(const std::string& owner, int id, float* value)
	{
		std::lock_guard lock(g_lock);
		for (const Slider& s : g_sliders)
			if (s.id == id && s.owner == owner) {
				if (value) *value = s.value;
				return true;
			}
		return false;
	}

	void RemoveAll(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		std::erase_if(g_sliders, [&](const Slider& s) { return s.owner == owner; });
	}

	void Install()
	{
		if (g_installed) return;
		g_installed = true;
		std::string error;
		if (!hooks::Add("TdGame.TdUIScene_PauseOptions.PostInitialize", hooks::When::After, 0, OnOptionsInit, nullptr, &error))
			spdlog::warn("Options menu: {}", error);
		if (!hooks::Add("TdGame.TdUIScene_PauseOptions.HandleOptionButton", hooks::When::Before, 0, OnOptionButton, nullptr, &error))
			spdlog::warn("Options menu: {}", error);
		if (!hooks::Add("TdGame.TdUIScene_AudioSettings.ResetSettingsToDefault", hooks::When::Before, 0, OnResetDefaults, nullptr, &error))
			spdlog::warn("Options menu: {}", error);
	}

	void Tick()
	{
		if (!g_page) return;
		if (!game::AliveAt(g_page, g_pageSlot)) {
			g_page = nullptr;
			return;
		}
		{
			// Read once more after it closes: Cancel puts the sliders back as it closes the page
			std::lock_guard lock(g_lock);
			for (int r = 0; r < kRows; ++r) {
				if (!g_rows[r]) continue;
				for (Slider& s : g_sliders)
					if (s.id == g_rowIds[r]) s.value = std::clamp(g_rows[r]->SliderValue.CurrentValue, s.min, s.max);
			}
		}
		if (!IsOpen(g_page)) g_page = nullptr;
	}

	void* OpenOptionsScene()
	{
		for (auto* s : game::ObjectsOf<Classes::UTdUIScene_PauseOptions>(game::Freshness::Exact))
			if (IsOpen(s)) return s;
		return nullptr;
	}

	void* OpenPage() { return g_page && game::AliveAt(g_page, g_pageSlot) ? g_page : nullptr; }

	bool ClickModSettings()
	{
		auto* scene = static_cast<Classes::UObject*>(OpenOptionsScene());
		Classes::UObject* button = scene && IsButton(g_button) ? g_button : nullptr;
		if (!button) return false;
		call::Params p(scene, "HandleOptionButton");
		p.Set("OptionButton", button);
		return call::Call(scene, p) && p.GetBool("ReturnValue");
	}

	std::string DescribeButton()
	{
		if (!IsButton(g_button)) return "no button";
		auto* b = static_cast<Classes::UUIObject*>(g_button);
		const auto pos = [&](std::uint8_t face) { return Face(b, face); };
		call::Params vis(b, "IsVisible");
		const bool visible = call::Call(b, vis) && vis.GetBool("ReturnValue");
		return b->GetName() + " hidden " + std::to_string(static_cast<int>(b->bHidden)) + " visible " + std::to_string(visible) +
			   " left " + std::to_string(pos(0)) + " top " + std::to_string(pos(1)) + " right " + std::to_string(pos(2)) + " bottom " +
			   std::to_string(pos(3));
	}

	void* PageSlider(int index) { return OpenPage() && index >= 0 && index < kRows ? g_rows[index] : nullptr; }

	bool SetPageSlider(int index, float value)
	{
		if (!OpenPage() || index < 0 || index >= kRows || !g_rows[index]) return false;
		call::Params p(g_rows[index], "SetValue");
		p.Set("NewValue", value);
		p.Set("bPercentageValue", false);
		return call::Call(g_rows[index], p);
	}
}
