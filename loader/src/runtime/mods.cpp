#include "runtime/mods.h"
#include "core/events.h"
#include "core/notify.h"

#include "core/module.h"
#include "core/setting.h"
#include "game/game.h"
#include "runtime/content.h"
#include "runtime/manifest.h"
#include "runtime/mod_lua.h"
#include "runtime/mod_ui.h"
#include "runtime/mod_io.h"
#include "runtime/mod_world.h"
#include "runtime/mod_tas.h"
#include "runtime/mod_dotnet.h"
#include "runtime/mem/manager.h"
#include "sdk/SDK.hpp"
#include "theme.h"
#include "runtime/resolve.h"

#include <imgui.h>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <typeinfo>

#include "runtime/mod_game.h"
#include "runtime/mod_internal.h"

namespace mirage::mods
{
	namespace
	{
		std::string Utf8(const std::filesystem::path& p)
		{
			const std::u8string s = p.u8string();
			return std::string(s.begin(), s.end());
		}
		std::filesystem::path FromUtf8(const std::string& s)
		{
			return std::filesystem::path(std::u8string(s.begin(), s.end()));
		}
		const char* Str(const char* s) { return s ? s : ""; }

		// ---- Calling into a mod ------------------------------------------------------------------------------------
		// A crash (an access violation, a C++ exception thrown across the boundary) is caught and reported instead of
		// taking the game down. Plain functions: __try can't share a frame with C++ objects.
		struct Fault
		{
			DWORD code = 0;
			const void* address = nullptr;
		};
		// Guarded calls running on this thread: a fault inside one is the guard's, not the crash handler's
		thread_local int t_guarded = 0;

		int Catch(EXCEPTION_POINTERS* ep, Fault* f)
		{
			f->code = ep->ExceptionRecord->ExceptionCode;
			f->address = ep->ExceptionRecord->ExceptionAddress;
			return EXCEPTION_EXECUTE_HANDLER;
		}
		bool Guarded(MirageCallback cb, void* user, Fault* f)
		{
			++t_guarded;
			__try {
				cb(user);
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}
		bool GuardedLoad(MirageModLoadFn load, const MirageApi* api, MirageMod* mod, int32_t* result, Fault* f)
		{
			++t_guarded;
			__try {
				*result = load(api, mod);
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}
		bool GuardedUnload(MirageModUnloadFn unload, Fault* f)
		{
			++t_guarded;
			__try {
				unload();
				--t_guarded;
				return true;
			} __except (Catch(GetExceptionInformation(), f)) {
				--t_guarded;
				return false;
			}
		}

		// "hello.dll+0x1A2B" (the mod's own DLL by its name in mod.toml, not the hot reload copy)
		std::string Where(const MirageMod* mod, const void* address)
		{
			HMODULE m = nullptr;
			if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
												static_cast<LPCWSTR>(address), &m))
				return "an unknown address";
			std::string name;
			if (mod && m == mod->dll) {
				name = mod->manifest.entry;
			} else {
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(m, path, MAX_PATH);
				name = Utf8(std::filesystem::path(path).filename());
			}
			char off[32];
			std::snprintf(off, sizeof off, "+0x%X", static_cast<unsigned>(static_cast<const char*>(address) - reinterpret_cast<const char*>(m)));
			return name + off;
		}
		std::string Hex(DWORD code)
		{
			char buf[16];
			std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(code));
			return buf;
		}

		// What the mod manager shows: the mods switched off for the next start, and those that crashed
		std::mutex g_flagLock;
		std::set<std::string> g_userOff, g_crashed;

		// ---- Crash records: <mods folder>/crashes.toml, one table per mod ------------------------------------------
		std::mutex g_crashLock;
		void RecordCrash(const MirageMod& mod, const std::string& module, const std::string& what, DWORD code,
						 const std::string& where)
		{
			std::lock_guard lock(g_crashLock);
			const auto file = mod.manifest.dir.parent_path() / "crashes.toml";
			toml::ordered_value root = toml::ordered_table{};
			try {
				if (std::filesystem::exists(file)) root = toml::parse<toml::ordered_type_config>(file);
			} catch (const std::exception&) {
				root = toml::ordered_table{};
			}
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			char when[32];
			std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
			toml::ordered_value entry = toml::ordered_table{};
			entry["time"] = std::string(when);
			entry["version"] = mod.version;
			entry["module"] = module;
			entry["call"] = what;
			entry["exception"] = Hex(code);
			entry["at"] = where;
			root[mod.id] = entry;
			{
				std::lock_guard flags(g_flagLock);
				g_crashed.insert(mod.id);
			}
			std::ofstream(file, std::ios::binary | std::ios::trunc)
				<< "# Mods that crashed. A mod listed here isn't loaded: delete its table to try it again.\n\n"
				<< toml::format(root);
		}
		std::map<std::string, std::string> ReadCrashes(const std::filesystem::path& folder)
		{
			std::map<std::string, std::string> off;
			const auto file = folder / "crashes.toml";
			if (!std::filesystem::exists(file)) return off;
			try {
				const auto root = toml::parse<toml::ordered_type_config>(file);
				for (const auto& [id, e] : root.as_table()) {
					const auto field = [&](const char* k) {
						return e.is_table() && e.contains(k) && e.at(k).is_string() ? e.at(k).as_string() : std::string("?");
					};
					off[id] = "it crashed the last time (" + field("call") + " of " + field("module") + ": exception " +
							  field("exception") + " at " + field("at") + "); delete its entry in " + Utf8(file) +
							  " to load it again";
				}
			} catch (const std::exception& e) {
				spdlog::warn("Mods: {} can't be read: {}", Utf8(file), e.what());
			}
			return off;
		}
	}

	// ---- A mod's module: a slot that keeps its settings and state across reloads of the mod's code ------------------
	class ModModule final : public Module
	{
	public:
		ModModule(MirageMod* mod, const MirageModuleDesc& d)
			: Module(d.name, d.tab && *d.tab ? d.tab : "Mods", Str(d.description), d.order ? d.order : 100), mod_(mod)
		{
			key = d.key;
			enabledByDefault = (d.flags & MIRAGE_MODULE_ENABLED_BY_DEFAULT) != 0;
			if (d.flags & MIRAGE_MODULE_EVERY_TICK) schedule.priority = sched::Priority::Critical;
			if (d.flags & MIRAGE_MODULE_PANEL_OPEN) expanded = true;
			Bind(d);
		}

		// Its code, from the current load of the mod
		void Bind(const MirageModuleDesc& d)
		{
			desc_ = d;
			action_ = (d.flags & MIRAGE_MODULE_ACTION) != 0;
			bound_ = true;
		}
		void Unbind()
		{
			bound_ = false;
			desc_ = {};
		}
		bool Bound() const { return bound_; }

		bool IsAction() const override { return action_; }
		// A module whose mod isn't running (crashed, failed to reload, or no longer registered it) isn't listed
		bool Hidden() const override
		{
			const State s = mod_->state.load();
			return !bound_ || (s != State::Active && s != State::Loading);
		}
		void OnEnable() override { Call(desc_.on_enable, "on_enable"); }
		void OnDisable() override { Call(desc_.on_disable, "on_disable"); }
		void OnTick() override { Call(desc_.on_tick, "on_tick"); }
		void OnRender() override
		{
			ui::Guard guard;		// What the mod left open in ImGui is closed after it
			Call(desc_.on_render, "on_render");
		}
		void DrawPanel(float) override
		{
			if (!desc_.on_panel) return;
			ui::Guard guard;
			ImGui::PushID(this);
			ImGui::BeginGroup();
			Call(desc_.on_panel, "on_panel");
			ImGui::EndGroup();
			ImGui::PopID();
		}
		void OnTrigger() override { Call(desc_.on_trigger, "on_trigger"); }

		MirageMod* Mod() const { return mod_; }
		std::vector<std::unique_ptr<ctl::Setting>> owned;	// The settings the mod made

	private:
		void Call(MirageCallback cb, const char* what);

		MirageMod* mod_;
		MirageModuleDesc desc_{};
		bool action_ = false;
		bool bound_ = false;
	};

	namespace
	{
		bool Running(const MirageMod& mod)
		{
			const State s = mod.state.load();
			return s == State::Active || s == State::Loading;
		}

		// A crash in `mod`'s code: everything of it stops, it is named, and it won't load next time
		void Crashed(MirageMod& mod, const std::string& module, const std::string& what, const Fault& f)
		{
			const std::string where = Where(&mod, f.address);
			mod.state = State::Crashed;
			mod.problem = "crashed: " + what + " of " + module + ", exception " + Hex(f.code) + " at " + where;
			spdlog::error("[mod {}] CRASHED in {} of {}: exception {} at {}. The mod is stopped (the rest of the menu "
						  "and the other mods go on) and won't be loaded next time (crashes.toml)",
						  mod.id, what, module, Hex(f.code), where);
			RecordCrash(mod, module, what, f.code, where);
		}
	}

	void ModModule::Call(MirageCallback cb, const char* what)
	{
		if (!cb || !bound_ || !Running(*mod_)) return;
		mod_->inflight.fetch_add(1);
		if (!Running(*mod_)) {		// Stopped meanwhile (a reload waits for this count)
			mod_->inflight.fetch_sub(1);
			return;
		}
		Fault f;
		const bool ok = Guarded(cb, desc_.user, &f);
		mod_->inflight.fetch_sub(1);
		if (ok) return;
		Crashed(*mod_, Name(), what, f);
		for (ModModule* m : mod_->modules)		// Off without calling it again (its code is in an unknown state)
			if (m->Enabled()) m->SetEnabled(false);
		detail::RemoveGameHooks(*mod_);
	}

	// A content mod's switch: its overrides on (applied on the game thread) or off (put back as they were)
	class ContentModule final : public Module
	{
	public:
		ContentModule(MirageMod* mod, const std::string& name)
			: Module(name, "Mods", mod->manifest.description.empty() ? "What this mod changes in the game" : mod->manifest.description, 50),
			  mod_(mod)
		{
			enabledByDefault = true;
		}
		bool Hidden() const override
		{
			const State st = mod_->state.load();
			return st != State::Active && st != State::Loading;
		}
		void OnEnable() override
		{
			auto set = mod_->content;
			if (set) game::RunOnGameThread([set] { content::Apply(*set); });
		}
		void OnDisable() override
		{
			auto set = mod_->content;
			if (set) game::RunOnGameThread([set] { if (content::Applied(*set)) content::Restore(*set); });
		}

	private:
		MirageMod* mod_;
	};

	bool detail::Running(const MirageMod& mod)
	{
		const State st = mod.state.load();
		return st == State::Active || st == State::Loading;
	}

	bool detail::Enter(MirageMod& mod, const std::string& what, const std::string& where, void (*fn)(void*), void* arg)
	{
		if (!Running(mod)) return false;
		mod.inflight.fetch_add(1);
		if (!Running(mod)) {
			mod.inflight.fetch_sub(1);
			return false;
		}
		Fault f;
		const bool ok = Guarded(fn, arg, &f);
		mod.inflight.fetch_sub(1);
		if (ok) return true;
		Crashed(mod, where, what, f);
		for (ModModule* m : mod.modules)
			if (m->Enabled()) m->SetEnabled(false);
		RemoveGameHooks(mod);
		return false;
	}

	namespace
	{
		ctl::Setting* SettingOf(MirageSetting* s) { return reinterpret_cast<ctl::Setting*>(s); }
		MirageSetting* Handle(ctl::Setting* s) { return reinterpret_cast<MirageSetting*>(s); }
		ModModule* ModuleOf(MirageModule* m) { return reinterpret_cast<ModModule*>(m); }

		// A setting made in code. On a reload the module has it already: the same name and kind gives it back, value
		// and all.
		template <class T, class... Args>
		MirageSetting* MakeSetting(MirageModule* module, const char* name, Args&&... args)
		{
			ModModule* m = ModuleOf(module);
			if (!m || !name || !*name) return nullptr;
			if (ctl::Setting* existing = m->FindSetting(name)) {
				if (typeid(*existing) == typeid(T)) return Handle(existing);
				spdlog::warn("[mod {}] {}: a setting named '{}' already exists", m->Mod()->id, m->Name(), name);
				return nullptr;
			}
			auto s = std::make_unique<T>(m, name, std::forward<Args>(args)...);	// Registers itself with m
			ctl::Setting* raw = s.get();
			m->owned.push_back(std::move(s));
			return Handle(raw);
		}

		uint32_t CopyOut(const std::string& s, char* buffer, uint32_t capacity)
		{
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(s.size(), capacity - 1);
				std::memcpy(buffer, s.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(s.size());
		}

		void ReadConfig(MirageMod* mod)
		{
			if (mod->configRead) return;
			mod->configRead = true;
			mod->config = toml::table{};
			const auto file = FromUtf8(mod->dir) / "config.toml";
			if (!std::filesystem::exists(file)) return;
			try {
				mod->config = toml::parse(file);
			} catch (const std::exception& e) {
				spdlog::warn("[mod {}] config.toml can't be read, starting empty: {}", mod->id, e.what());
				mod->config = toml::table{};
			}
		}

		// The settings mod.toml declares for this module ([settings.<key>] with module = its name, or no module and
		// this is the mod's first module), in the file's order. On a reload the ones made before are kept.
		void AddDeclaredSettings(MirageMod& mod, ModModule& m, bool firstModule)
		{
			using T = SettingDecl::Type;
			for (const SettingDecl& d : mod.manifest.settings) {
				if (mod.declared.count(d.key)) continue;
				if (d.module.empty() ? !firstModule : d.module != m.Name()) continue;
				if (m.FindSetting(d.name)) {
					spdlog::warn("[mod {}] {}: setting '{}' from mod.toml clashes with one of the same name", mod.id, m.Name(), d.name);
					continue;
				}
				std::unique_ptr<ctl::Setting> s;
				switch (d.type) {
					case T::Bool: s = std::make_unique<ctl::Checkbox>(&m, d.name, d.boolDefault, d.description); break;
					case T::Int:
						// Within ImGui's int slider range (see SettingSliderInt)
						s = std::make_unique<ctl::SliderInt>(&m, d.name, static_cast<int>(d.intDefault),
															 static_cast<int>(std::clamp<double>(d.min, -(INT32_MAX / 2), INT32_MAX / 2)),
															 static_cast<int>(std::clamp<double>(d.max, -(INT32_MAX / 2), INT32_MAX / 2)),
															 d.format.empty() ? std::string("%d") : d.format, 0, d.description);
						break;
					case T::Float:
						s = std::make_unique<ctl::Slider>(&m, d.name, static_cast<float>(d.floatDefault), static_cast<float>(d.min),
														  static_cast<float>(d.max), d.format.empty() ? std::string("%.2f") : d.format,
														  0.f, d.description);
						break;
					case T::String: s = std::make_unique<ctl::Text>(&m, d.name, d.stringDefault, d.description); break;
					case T::Color: s = std::make_unique<ctl::ColorPick>(&m, d.name, d.colorDefault, d.description); break;
					case T::Choice:
						s = std::make_unique<ctl::Combo>(&m, d.name, d.options, static_cast<int>(d.intDefault), d.description);
						break;
					case T::Key: s = std::make_unique<ctl::KeyHold>(&m, d.name, static_cast<int>(d.intDefault), d.description); break;
				}
				mod.declared[d.key] = s.get();
				m.owned.push_back(std::move(s));
			}
		}

		// ---- The table -------------------------------------------------------------------------------------------
		void MIRAGE_CALL Log(MirageMod* mod, int32_t level, const char* text)
		{
			const std::string id = mod ? mod->id : "?";
			switch (level) {
				case MIRAGE_LOG_DEBUG: spdlog::debug("[mod {}] {}", id, Str(text)); break;
				case MIRAGE_LOG_WARN: spdlog::warn("[mod {}] {}", id, Str(text)); break;
				case MIRAGE_LOG_ERROR: spdlog::error("[mod {}] {}", id, Str(text)); break;
				default: spdlog::info("[mod {}] {}", id, Str(text)); break;
			}
		}
		const char* MIRAGE_CALL ModId(MirageMod* mod) { return mod ? mod->id.c_str() : ""; }
		const char* MIRAGE_CALL ModVersion(MirageMod* mod) { return mod ? mod->version.c_str() : ""; }
		const char* MIRAGE_CALL ModDir(MirageMod* mod) { return mod ? mod->dir.c_str() : ""; }

		MirageModule* MIRAGE_CALL ModuleRegister(MirageMod* mod, const MirageModuleDesc* desc)
		{
			if (!mod || !desc) return nullptr;
			if (mod->state.load() != State::Loading) {
				spdlog::error("[mod {}] module_register: modules are registered while the mod loads", mod->id);
				return nullptr;
			}
			// Only the part of the description the mod's header had; the rest stays zero
			MirageModuleDesc d{};
			std::memcpy(&d, desc, std::min<std::size_t>(desc->size, sizeof d));
			d.size = sizeof d;
			if (desc->size < offsetof(MirageModuleDesc, user) || !d.name || !*d.name) {
				spdlog::error("[mod {}] module_register: a module needs a name", mod->id);
				return nullptr;
			}
			// A module this mod registered in an earlier load: the same slot, its settings and state kept
			for (ModModule* m : mod->modules)
				if (m->Name() == d.name) {
					if (m->Bound()) {
						spdlog::error("[mod {}] module_register: '{}' is registered twice", mod->id, d.name);
						return nullptr;
					}
					m->Bind(d);
					AddDeclaredSettings(*mod, *m, m == mod->modules.front());	// New keys in mod.toml
					return reinterpret_cast<MirageModule*>(m);
				}
			if (modules::NameTaken(d.name)) {
				spdlog::error("[mod {}] module_register: a module named '{}' already exists", mod->id, d.name);
				return nullptr;
			}
			auto owned = std::make_unique<ModModule>(mod, d);
			ModModule* m = owned.get();
			const bool first = mod->modules.empty();
			mod->modules.push_back(m);
			AddDeclaredSettings(*mod, *m, first);
			mod->pending.push_back(std::move(owned));	// Joins the menu when the load is done
			return reinterpret_cast<MirageModule*>(m);
		}
		int32_t MIRAGE_CALL ModuleEnabled(MirageModule* m) { return m && ModuleOf(m)->Enabled() ? 1 : 0; }
		void MIRAGE_CALL ModuleSetEnabled(MirageModule* m, int32_t on)
		{
			if (m) ModuleOf(m)->SetEnabled(on != 0);
		}

		MirageSetting* MIRAGE_CALL SettingCheckbox(MirageModule* m, const char* name, int32_t value, const char* tooltip)
		{
			return MakeSetting<ctl::Checkbox>(m, name, value != 0, std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingSlider(MirageModule* m, const char* name, float value, float min, float max,
												 const char* format, const char* tooltip)
		{
			if (!(min < max)) return nullptr;
			return MakeSetting<ctl::Slider>(m, name, value, min, max, std::string(format && *format ? format : "%.2f"),
											0.f, std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingSliderInt(MirageModule* m, const char* name, int32_t value, int32_t min,
													int32_t max, const char* tooltip)
		{
			// ImGui's int sliders take at most half of int32's range (it asserts otherwise): 1 << 30 is one too many
			constexpr int32_t kLimit = INT32_MAX / 2;
			min = std::clamp(min, -kLimit, kLimit);
			max = std::clamp(max, -kLimit, kLimit);
			if (!(min < max)) return nullptr;
			return MakeSetting<ctl::SliderInt>(m, name, int(value), int(min), int(max), std::string("%d"), 0,
											   std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingText(MirageModule* m, const char* name, const char* value, const char* tooltip)
		{
			return MakeSetting<ctl::Text>(m, name, std::string(Str(value)), std::string(Str(tooltip)));
		}
		MirageSetting* MIRAGE_CALL SettingKey(MirageModule* m, const char* name, int32_t vk, const char* tooltip)
		{
			return MakeSetting<ctl::KeyHold>(m, name, int(vk), std::string(Str(tooltip)));
		}

		int32_t MIRAGE_CALL SettingGetInt(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* c = dynamic_cast<ctl::ColorPick*>(s)) return static_cast<int32_t>(c->Packed());
			if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) return b->Get() ? 1 : 0;
			if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) return i->Get();
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) return static_cast<int32_t>(f->Get());
			return 0;
		}
		float MIRAGE_CALL SettingGetFloat(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) return f->Get();
			if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) return static_cast<float>(i->Get());
			if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) return b->Get() ? 1.f : 0.f;
			return 0.f;
		}
		void MIRAGE_CALL SettingSetInt(MirageSetting* h, int32_t v)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* c = dynamic_cast<ctl::ColorPick*>(s)) {
				const auto u = static_cast<uint32_t>(v);
				c->Set({(u & 0xFF) / 255.f, ((u >> 8) & 0xFF) / 255.f, ((u >> 16) & 0xFF) / 255.f, ((u >> 24) & 0xFF) / 255.f});
			} else if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) b->Set(v != 0);
			else if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) i->Set(v);
			else if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) f->Set(static_cast<float>(v));
			else return;
			s->Owner().MarkDirty();
		}
		void MIRAGE_CALL SettingSetFloat(MirageSetting* h, float v)
		{
			ctl::Setting* s = SettingOf(h);
			if (auto* f = dynamic_cast<ctl::Value<float>*>(s)) f->Set(v);
			else if (auto* i = dynamic_cast<ctl::Value<int>*>(s)) i->Set(static_cast<int>(v));
			else if (auto* b = dynamic_cast<ctl::Value<bool>*>(s)) b->Set(v != 0.f);
			else return;
			s->Owner().MarkDirty();
		}
		uint32_t MIRAGE_CALL SettingGetText(MirageSetting* h, char* buffer, uint32_t capacity)
		{
			if (auto* c = dynamic_cast<ctl::Combo*>(SettingOf(h))) return CopyOut(c->Selected(), buffer, capacity);
			auto* t = dynamic_cast<ctl::Value<std::string>*>(SettingOf(h));
			return CopyOut(t ? t->Get() : std::string(), buffer, capacity);
		}
		void MIRAGE_CALL SettingSetText(MirageSetting* h, const char* v)
		{
			if (auto* c = dynamic_cast<ctl::Combo*>(SettingOf(h))) {	// A choice: by its option's text
				const int before = c->Get();
				for (int i = 0; i < 4096; ++i) {
					c->Set(i);
					if (c->Get() != i) break;		// Past the last option (clamped)
					if (c->Selected() == Str(v)) {
						c->Owner().MarkDirty();
						return;
					}
				}
				c->Set(before);		// Not one of its options: unchanged
				return;
			}
			if (auto* t = dynamic_cast<ctl::Value<std::string>*>(SettingOf(h))) {
				t->Set(Str(v));
				t->Owner().MarkDirty();
			}
		}
		int32_t MIRAGE_CALL KeyDown(MirageSetting* h)
		{
			auto* k = dynamic_cast<ctl::KeyHold*>(SettingOf(h));
			return k && k->Down() ? 1 : 0;
		}

		uint32_t MIRAGE_CALL ConfigGet(MirageMod* mod, const char* key, const char* fallback, char* buffer,
									   uint32_t capacity)
		{
			std::string value = Str(fallback);
			if (mod && key) {
				std::lock_guard lock(mod->configLock);
				ReadConfig(mod);
				if (mod->config.is_table() && mod->config.contains(key)) {
					const toml::value& v = mod->config.at(key);
					value = v.is_string() ? v.as_string() : toml::format(v);
				}
			}
			return CopyOut(value, buffer, capacity);
		}
		void MIRAGE_CALL ConfigSet(MirageMod* mod, const char* key, const char* value)
		{
			if (!mod || !key || !*key) return;
			std::lock_guard lock(mod->configLock);
			ReadConfig(mod);
			mod->config.as_table()[key] = std::string(Str(value));
			const auto file = FromUtf8(mod->dir) / "config.toml";
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			if (out) out << toml::format(mod->config);
			else spdlog::warn("[mod {}] can't write {}", mod->id, Utf8(file));
		}

		void MIRAGE_CALL ScreenSize(float* w, float* h)
		{
			const ImVec2 s = ImGui::GetCurrentContext() ? ImGui::GetIO().DisplaySize : ImVec2{};
			if (w) *w = s.x;
			if (h) *h = s.y;
		}
		ImDrawList* DrawList() { return ImGui::GetCurrentContext() ? ImGui::GetBackgroundDrawList() : nullptr; }
		void MIRAGE_CALL DrawText(float x, float y, uint32_t color, const char* text)
		{
			if (ImDrawList* dl = DrawList(); dl && text) dl->AddText({x, y}, color, text);
		}
		void MIRAGE_CALL DrawLine(float x1, float y1, float x2, float y2, uint32_t color, float thickness)
		{
			if (ImDrawList* dl = DrawList()) dl->AddLine({x1, y1}, {x2, y2}, color, thickness);
		}
		void MIRAGE_CALL DrawRect(float x1, float y1, float x2, float y2, uint32_t color, float thickness)
		{
			if (ImDrawList* dl = DrawList()) dl->AddRect({x1, y1}, {x2, y2}, color, 0.f, 0, thickness);
		}
		void MIRAGE_CALL DrawRectFilled(float x1, float y1, float x2, float y2, uint32_t color)
		{
			if (ImDrawList* dl = DrawList()) dl->AddRectFilled({x1, y1}, {x2, y2}, color);
		}
		void MIRAGE_CALL DrawCircle(float x, float y, float radius, uint32_t color, float thickness)
		{
			if (ImDrawList* dl = DrawList()) dl->AddCircle({x, y}, radius, color, 0, thickness);
		}
		void MIRAGE_CALL DrawCircleFilled(float x, float y, float radius, uint32_t color)
		{
			if (ImDrawList* dl = DrawList()) dl->AddCircleFilled({x, y}, radius, color);
		}
		float MIRAGE_CALL FrameRate() { return ImGui::GetCurrentContext() ? ImGui::GetIO().Framerate : 0.f; }
		MirageSetting* MIRAGE_CALL SettingColor(MirageModule* m, const char* name, uint32_t rgba, const char* tooltip)
		{
			const std::array<float, 4> c{(rgba & 0xFF) / 255.f, ((rgba >> 8) & 0xFF) / 255.f, ((rgba >> 16) & 0xFF) / 255.f,
										 ((rgba >> 24) & 0xFF) / 255.f};
			return MakeSetting<ctl::ColorPick>(m, name, c, std::string(Str(tooltip)));
		}
		void MIRAGE_CALL TextSize(const char* text, float* w, float* h)
		{
			const ImVec2 s = ImGui::GetCurrentContext() && text ? ImGui::CalcTextSize(text) : ImVec2{};
			if (w) *w = s.x;
			if (h) *h = s.y;
		}
		MirageSetting* MIRAGE_CALL SettingFind(MirageMod* mod, const char* key)
		{
			if (!mod || !key) return nullptr;
			const auto it = mod->declared.find(key);
			return it == mod->declared.end() ? nullptr : Handle(it->second);
		}
		int32_t MIRAGE_CALL SettingType(MirageSetting* h)
		{
			ctl::Setting* s = SettingOf(h);
			if (!s) return MIRAGE_SETTING_UNKNOWN;
			if (dynamic_cast<ctl::ColorPick*>(s)) return MIRAGE_SETTING_COLOR;
			if (dynamic_cast<ctl::Checkbox*>(s)) return MIRAGE_SETTING_BOOL;
			if (dynamic_cast<ctl::Combo*>(s)) return MIRAGE_SETTING_CHOICE;
			if (dynamic_cast<ctl::KeyHold*>(s)) return MIRAGE_SETTING_KEY;
			if (dynamic_cast<ctl::Value<int>*>(s)) return MIRAGE_SETTING_INT;
			if (dynamic_cast<ctl::Value<float>*>(s)) return MIRAGE_SETTING_FLOAT;
			if (dynamic_cast<ctl::Value<std::string>*>(s)) return MIRAGE_SETTING_STRING;
			return MIRAGE_SETTING_UNKNOWN;
		}

		const MirageUi* MIRAGE_CALL Ui() { return ui::Table(); }

		void MIRAGE_CALL SdkGlobals(void** names, void** objects)
		{
			if (names) *names = Classes::FName::GNames;
			if (objects) *objects = Classes::UObject::GObjects;
		}
		int32_t MIRAGE_CALL ModuleState(const char* name)
		{
			const Module* m = name ? modules::Find(name) : nullptr;
			return m ? (m->Enabled() ? 1 : 0) : -1;
		}
		int32_t MIRAGE_CALL ModuleSetState(const char* name, int32_t on)
		{
			Module* m = name ? modules::Find(name) : nullptr;
			if (!m) return 0;
			if (m->Enabled() != (on != 0)) m->SetEnabled(on != 0);
			return 1;
		}
		void* MIRAGE_CALL ImGuiContext(const char* version, uint32_t ioSize, uint32_t styleSize, uint32_t vertexSize,
										 void** allocFn, void** freeFn, void** user)
		{
			if (!ImGui::GetCurrentContext()) return nullptr;
			if (!version || std::strcmp(version, IMGUI_VERSION) != 0 || ioSize != sizeof(ImGuiIO) ||
				styleSize != sizeof(ImGuiStyle) || vertexSize != sizeof(ImDrawVert)) {
				static std::atomic<bool> told{false};
				if (!told.exchange(true))
					spdlog::warn("Mods: a mod built with ImGui {} asked for the menu's ImGui {}: refused (another version)",
								 version ? version : "?", IMGUI_VERSION);
				return nullptr;
			}
			ImGuiMemAllocFunc a = nullptr;
			ImGuiMemFreeFunc f = nullptr;
			void* u = nullptr;
			ImGui::GetAllocatorFunctions(&a, &f, &u);
			if (allocFn) *allocFn = reinterpret_cast<void*>(a);
			if (freeFn) *freeFn = reinterpret_cast<void*>(f);
			if (user) *user = u;
			return ImGui::GetCurrentContext();
		}

		void MIRAGE_CALL SettingDraw(MirageSetting* h, float width)
		{
			if (ctl::Setting* s = SettingOf(h); s && ImGui::GetCurrentContext()) {
				ImGui::PushID(s);
				s->Show(width > 0.f ? width : ImGui::GetContentRegionAvail().x);
				ImGui::PopID();
			}
		}

		MirageSetting* MIRAGE_CALL SettingChoice(MirageModule* module, const char* name, const char* items, int32_t index,
												 const char* tooltip)
		{
			std::vector<std::string> options;
			for (const char* p = items; p && *p; p += std::strlen(p) + 1) options.emplace_back(p);
			if (options.empty()) return nullptr;
			return MakeSetting<ctl::Combo>(module, name, options, std::clamp<int>(index, 0, static_cast<int>(options.size()) - 1),
										   std::string(Str(tooltip)));
		}

		void MIRAGE_CALL ReservedKeys(MirageMod* mod, const int32_t* vks, int32_t count)
		{
			if (!mod) return;
			static std::mutex lock;
			static std::map<std::string, std::vector<int>> byMod;
			std::lock_guard guard(lock);
			auto& keys = byMod[mod->id];
			keys.assign(vks, vks + std::max(count, 0));
			std::vector<int> all;
			for (const auto& [id, k] : byMod) all.insert(all.end(), k.begin(), k.end());
			modules::SetReservedKeys(std::move(all));
		}

		uint32_t MIRAGE_CALL ModuleList(char* buffer, uint32_t capacity)
		{
			std::string out = "[";
			const auto quote = [](const std::string& s) {
				std::string q = "\"";
				for (char c : s) {
					if (c == '"' || c == '\\') q += '\\';
					if (static_cast<unsigned char>(c) >= 0x20) q += c;
				}
				return q + "\"";
			};
			modules::ForEach([&](modules::Tab& tab, Module& m) {
				if (m.Hidden()) return;
				if (out.size() > 1) out += ",";
				out += "{\"name\":" + quote(m.Name()) + ",\"tab\":" + quote(tab.name) + ",\"enabled\":" +
					   (m.Enabled() ? "true" : "false") + ",\"action\":" + (m.IsAction() ? "true" : "false") + "}";
			});
			out += "]";
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(out.size(), capacity - 1);
				std::memcpy(buffer, out.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(out.size());
		}

		// The memory manager's last sample, and this mod's Lua memory as of it (any thread: nothing here reads a mod)
		int32_t MIRAGE_CALL MemoryStats(MirageMod* mod, MirageMemoryStats* out)
		{
			if (!out || out->size < sizeof(uint32_t)) return 0;
			MirageMemoryStats s{};
			s.size = sizeof s;
			const mem::Status st = mem::GetStatus();
			s.pressure = static_cast<int32_t>(st.level);
			s.process_private = st.process.privateBytes;
			s.process_working_set = st.process.workingSet;
			s.address_free = st.address.complete ? st.address.free.totalFree : st.process.addressAvailable;
			s.address_largest_free = st.address.complete ? st.address.free.largestFree : 0;
			s.menu_heap = static_cast<uint64_t>(std::max<std::int64_t>(st.heap.live, 0));
			if (mod)
				for (const auto& [id, bytes] : st.modLua)
					if (id == mod->id) s.mod_lua = bytes;
			std::memcpy(out, &s, std::min<std::size_t>(out->size, sizeof s));
			return 1;
		}

		uint32_t MIRAGE_CALL ThemeColor(int32_t which)
		{
			const theme::MenuColors& t = theme::Menu();
			const ImVec4 c = which == MIRAGE_THEME_ACCENT ? t.accent : which == MIRAGE_THEME_TEXT ? t.text
						   : which == MIRAGE_THEME_TEXT_DIM ? t.textDim : t.settingsBg;
			return ImGui::ColorConvertFloat4ToU32(c);
		}

		void MIRAGE_CALL SettingSetVisible(MirageSetting* h, int32_t visible)
		{
			ctl::Setting* s = SettingOf(h);
			if (!s) return;
			auto flag = std::make_shared<std::atomic<bool>>(visible != 0);
			s->VisibleIf([flag] { return flag->load(); });
		}

		void MIRAGE_CALL DrawTextSized(float x, float y, uint32_t color, float size, const char* text)
		{
			ImDrawList* dl = DrawList();
			if (!dl || !text) return;
			dl->AddText(nullptr, size > 0.f ? size : ImGui::GetFontSize(), {x, y}, color, text);
		}
		void MIRAGE_CALL TextSizeSized(const char* text, float size, float* w, float* h)
		{
			ImVec2 s{};
			if (ImGui::GetCurrentContext() && text) {
				ImFont* font = ImGui::GetFont();
				s = font->CalcTextSizeA(size > 0.f ? size : ImGui::GetFontSize(), FLT_MAX, 0.f, text);
			}
			if (w) *w = s.x;
			if (h) *h = s.y;
		}
		void MIRAGE_CALL DrawRectRounded(float x1, float y1, float x2, float y2, uint32_t color, float rounding, float thickness)
		{
			ImDrawList* dl = DrawList();
			if (!dl) return;
			if (thickness <= 0.f) dl->AddRectFilled({x1, y1}, {x2, y2}, color, rounding);
			else dl->AddRect({x1, y1}, {x2, y2}, color, rounding, 0, thickness);
		}
		void MIRAGE_CALL DrawTriangleFilled(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color)
		{
			if (ImDrawList* dl = DrawList()) dl->AddTriangleFilled({x1, y1}, {x2, y2}, {x3, y3}, color);
		}

		// A notification on screen (the menu's toasts), prefixed with nothing: the mod writes what the player reads
		void MIRAGE_CALL Notify(MirageMod* mod, const char* text)
		{
			if (!mod || !text || !*text) return;
			notify::Push(text);
		}

		const MirageApi kApi = {
			sizeof(MirageApi), MIRAGE_ABI_VERSION, kRuntimeVersion,
			Log, ModId, ModVersion, ModDir,
			ModuleRegister, ModuleEnabled, ModuleSetEnabled,
			SettingCheckbox, SettingSlider, SettingSliderInt, SettingText, SettingKey,
			SettingGetInt, SettingGetFloat, SettingSetInt, SettingSetFloat, SettingGetText, SettingSetText, KeyDown,
			ConfigGet, ConfigSet,
			ScreenSize, DrawText, DrawLine, DrawRect, DrawRectFilled, TextSize,
			SettingFind, SettingType,
			game_api::EventSubscribe, game_api::EventUnsubscribe, game_api::HookFunction, game_api::HookRemove,
			game_api::CallObject, game_api::CallFunction, game_api::CallSkip, game_api::CallGet, game_api::CallSet,
			game_api::CallBegin, game_api::CallInvoke, game_api::CallEnd,
			game_api::PlayerPawn, game_api::PlayerController, game_api::WorldInfo, game_api::ObjectFind,
			game_api::ObjectGet, game_api::ObjectSet, game_api::ObjectName, game_api::ObjectClassName, game_api::ObjectIsA,
			game_api::DetourCreate, game_api::DetourRemove, game_api::FindPattern,
			game_api::ScriptOpen, game_api::ScriptClose, game_api::ScriptCount, game_api::ScriptGet, game_api::ScriptSet,
			game_api::ScriptAdd, game_api::ScriptChildren, game_api::ScriptSetChildren, game_api::ScriptApply,
			game_api::ScriptRestore, game_api::ScriptDump, game_api::ScriptKind, game_api::ScriptKindName,
			Notify, DrawCircle, DrawCircleFilled, FrameRate, SettingColor,
			game_api::ObjectsOfClass, game_api::EventSkip, game_api::MenuOpen, game_api::InputKey, game_api::ShareSet,
			game_api::ShareGet,
			Ui, SettingSetVisible, DrawTextSized, TextSizeSized, DrawRectRounded, DrawTriangleFilled,
			io::InputKeyState, io::InputMouse, io::InputKps, io::InputNow, io::InputResetCounts, io::TasSession,
			io::FileRead, io::FileWrite, io::FileList, io::FileStamp, io::FileOpenFolder,
			SdkGlobals, ModuleState, ThemeColor,
			game_api::PlayerStateCapture, game_api::PlayerStateApply, game_api::PlayerResetMovement, ModuleSetState,
			world::DrawActive, world::DrawSet, world::DrawStyle, world::DrawStats, world::WorldToScreen, world::CameraGet,
			ImGuiContext, SettingDraw, SettingChoice, ReservedKeys,
			tasapi::Available, tasapi::Status, tasapi::SetOptions, tasapi::Command, tasapi::Record, tasapi::ReplayRevision,
			tasapi::ReplayGet, tasapi::ReplaySet, tasapi::File, tasapi::Folder, tasapi::Preflight, ModuleList,
			MemoryStats, game_api::InjectFunction, game_api::NativeRegister, game_api::NativeArg, game_api::NativeReturn,
			game_api::NativeSelf, game_api::ClassLoad, game_api::ClassDefine, game_api::ObjectPath, game_api::ObjectKeep,
			game_api::LayerActor, game_api::ObjectHandle, game_api::HandleObject, game_api::ActorsQuery,
			game_api::ChangeBegin, game_api::ChangeEnd, game_api::ChangeRevert, game_api::ChangeList,
			game_api::SymbolFind, game_api::DetourSymbol, game_api::MidHookCreate, game_api::MidHookRemove,
			game_api::TestRegister,
		};

		// ---- Loading -----------------------------------------------------------------------------------------------
		std::vector<std::unique_ptr<MirageMod>> g_mods;		// Discovered and to be loaded, in load order
		// g_mods only grows on the game thread (LoadNew); readers on other threads (List) hold this while iterating
		std::mutex g_modsLock;
		std::vector<Info> g_excluded;						// Discovered and left out
		int g_loadCount = 0;
		std::atomic<int> g_shadowCounter{0};

		// The watcher: a thread reading the change notifications of the mods folders (ReadDirectoryChangesW, recursive),
		// which name the file. A change marks the mod whose folder it is in; what the runtime and the mods write
		// themselves (config.toml, disabled.toml, crashes.toml, .mirage copies, build output) marks nothing. ReloadChanged
		// walks only the marked mods, so the check on the game thread is free while nothing changes and runs at Normal
		// priority (never deferred under load).
		std::mutex g_dirtyLock;
		std::set<std::wstring> g_dirty;						// Folder names (= mod ids) changed since the last check
		std::atomic<bool> g_dirtyAll{true};					// Check every mod (the start, a lost notification, no watcher)
		std::set<std::wstring> g_settling;					// Changed last check, still being written: checked again
		std::thread g_watcher;
		HANDLE g_watcherStop = nullptr;

		bool Ignored(const std::wstring& relative)		// "<mod>\sub\file" relative to a mods folder
		{
			const auto slash = relative.find(L'\\');
			if (slash == std::wstring::npos) return relative == L"disabled.toml" || relative == L"crashes.toml";
			const std::wstring rest = relative.substr(slash + 1);
			if (rest == L"config.toml") return true;	// Written by the mod itself
			for (const wchar_t* dir : {L".mirage", L"obj", L"bin"})
				if (rest.starts_with(std::wstring(dir) + L"\\") || rest == dir) return true;
			return false;
		}

		void StartWatcher(const std::vector<std::filesystem::path>& folders)
		{
			struct Watch
			{
				HANDLE dir = INVALID_HANDLE_VALUE;
				OVERLAPPED ov{};
				alignas(DWORD) std::byte buffer[32 * 1024];
			};
			auto watches = std::make_shared<std::vector<std::unique_ptr<Watch>>>();
			const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
								 FILE_NOTIFY_CHANGE_LAST_WRITE;
			const auto arm = [filter](Watch& w) {
				ResetEvent(w.ov.hEvent);
				return ReadDirectoryChangesW(w.dir, w.buffer, sizeof w.buffer, TRUE, filter, nullptr, &w.ov, nullptr) != 0;
			};
			for (const auto& f : folders) {
				auto w = std::make_unique<Watch>();
				w->dir = CreateFileW(f.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
									 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
				if (w->dir == INVALID_HANDLE_VALUE) continue;
				w->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!w->ov.hEvent || !arm(*w)) {
					if (w->ov.hEvent) CloseHandle(w->ov.hEvent);
					CloseHandle(w->dir);
					continue;
				}
				watches->push_back(std::move(w));
			}
			if (watches->empty()) return;		// ReloadChanged then walks every mod each time
			g_watcherStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			g_watcher = std::thread([watches, arm] {
				std::vector<HANDLE> events;
				for (const auto& w : *watches) events.push_back(w->ov.hEvent);
				events.push_back(g_watcherStop);
				const DWORD n = static_cast<DWORD>(events.size());
				for (;;) {
					const DWORD r = WaitForMultipleObjects(n, events.data(), FALSE, INFINITE);
					if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + n - 1) break;		// The stop event, or an error
					Watch& w = *(*watches)[r - WAIT_OBJECT_0];
					DWORD bytes = 0;
					if (!GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE) || bytes == 0) {
						g_dirtyAll = true;		// The buffer overflowed: what changed is unknown
					} else {
						std::lock_guard lock(g_dirtyLock);
						for (std::size_t at = 0;;) {
							const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(w.buffer + at);
							const std::wstring path(info->FileName, info->FileNameLength / sizeof(wchar_t));
							if (!Ignored(path)) g_dirty.insert(path.substr(0, path.find(L'\\')));
							if (info->NextEntryOffset == 0) break;
							at += info->NextEntryOffset;
						}
					}
					if (!arm(w)) g_dirtyAll = true;
				}
				for (const auto& w : *watches) {
					CancelIoEx(w->dir, &w->ov);
					DWORD ignored = 0;
					GetOverlappedResult(w->dir, &w->ov, &ignored, TRUE);	// The kernel is done with the buffer
					CloseHandle(w->ov.hEvent);
					CloseHandle(w->dir);
				}
			});
		}

		void StopWatcher()
		{
			if (!g_watcher.joinable()) return;
			SetEvent(g_watcherStop);
			g_watcher.join();
			CloseHandle(g_watcherStop);
			g_watcherStop = nullptr;
		}

		// The files of a mod, summed up: changes when one is added, removed, rewritten or touched
		std::uintmax_t Fingerprint(const std::filesystem::path& dir)
		{
			std::uintmax_t h = 1469598103934665603ull;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
				 it.increment(ec)) {
				const auto& p = it->path();
				if (it->is_directory()) {
					if (p.filename() == ".mirage") it.disable_recursion_pending();	// Its hot reload copies
					// Level layers and routes (step 13): data the runtime reads again by itself when it changes (the
					// in-game editor saves layers while playing): no reload of the whole mod for them
					if (p.parent_path() == dir && (p.filename() == "levels" || p.filename() == "routes")) it.disable_recursion_pending();
					continue;
				}
				if (p.filename() == "config.toml") continue;	// Written by the mod itself
				const auto mix = [&](std::uintmax_t v) { h = (h ^ v) * 1099511628211ull; };
				mix(std::hash<std::wstring>{}(p.wstring()));
				mix(static_cast<std::uintmax_t>(it->last_write_time(ec).time_since_epoch().count()));
				mix(it->file_size(ec));
			}
			return h;
		}

		void UnloadCode(MirageMod& mod)
		{
			detail::RemoveGameHooks(mod);	// Before its code goes
			world::Unloading(&mod);			// Its 3D drawing too
			if (mod.content && content::Applied(*mod.content)) content::Restore(*mod.content);
			if (!mod.manifest.dotnet.empty()) dotnet::UnloadMod(&mod);	// OnUnload, then its assembly context goes
			mod.lua.reset();
			if (mod.unload) {
				Fault f;
				if (!GuardedUnload(mod.unload, &f))
					spdlog::error("[mod {}] mirage_mod_unload crashed: exception {} at {}", mod.id, Hex(f.code), Where(&mod, f.address));
			}
			mod.unload = nullptr;
			if (mod.dll) FreeLibrary(mod.dll);
			mod.dll = nullptr;
			if (!mod.shadow.empty()) {
				std::error_code ec;
				std::filesystem::remove(mod.shadow, ec);
				mod.shadow.clear();
			}
		}

		// The native part. "" when it loaded, else why not.
		std::string LoadNative(MirageMod& mod)
		{
			auto path = mod.manifest.dir / FromUtf8(mod.manifest.entry);
			if (mod.manifest.hotReload) {
				// Run a copy, so a new build can replace the DLL while the game runs
				const auto folder = mod.manifest.dir / ".mirage";
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				for (const auto& old : std::filesystem::directory_iterator(folder, ec))
					std::filesystem::remove(old.path(), ec);	// Left by a crash; skipped when still loaded
				mod.shadow = folder / (path.stem().wstring() + L"-" + std::to_wstring(++g_shadowCounter) + L".dll");
				if (!std::filesystem::copy_file(path, mod.shadow, std::filesystem::copy_options::overwrite_existing, ec))
					return "its DLL can't be copied for hot reload (" + ec.message() + ")";
				path = mod.shadow;
			}
			// The mod's own folder is searched for the DLLs it depends on
			mod.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!mod.dll) return "its DLL can't be loaded (error " + std::to_string(GetLastError()) + "; is it 32-bit?)";
			const auto abi = reinterpret_cast<MirageModAbiFn>(GetProcAddress(mod.dll, "mirage_mod_abi"));
			const auto load = reinterpret_cast<MirageModLoadFn>(GetProcAddress(mod.dll, "mirage_mod_load"));
			mod.unload = reinterpret_cast<MirageModUnloadFn>(GetProcAddress(mod.dll, "mirage_mod_unload"));
			if (!abi || !load) return "its DLL doesn't export mirage_mod_abi and mirage_mod_load";
			const uint32_t version = abi();
			if (version < 1 || version > MIRAGE_ABI_VERSION)
				return "it was built for Mirage ABI " + std::to_string(version) + ", this runtime has ABI " +
					   std::to_string(MIRAGE_ABI_VERSION);
			int32_t result = 0;
			Fault f;
			if (!GuardedLoad(load, &kApi, &mod, &result, &f)) {
				Crashed(mod, "(loading)", "mirage_mod_load", f);
				return mod.problem;
			}
			if (result != 0) return "mirage_mod_load returned " + std::to_string(result);
			return {};
		}

		// Loads (or reloads) a mod's code. "" when it loaded, else why not (then nothing of it runs).
		std::string LoadCode(MirageMod& mod)
		{
			static const auto kTag = mem::RegisterTag("Mods");
			mem::TagScope tag(kTag);
			mod.state = State::Loading;
			mod.pending.clear();
			for (ModModule* m : mod.modules) m->Unbind();
			mod.fingerprint = Fingerprint(mod.manifest.dir);
			std::string problem;
			if (!mod.manifest.entry.empty()) problem = LoadNative(mod);
			if (problem.empty() && !mod.manifest.dotnet.empty()) {		// C# (runtime/mod_dotnet.h)
				const std::string error = dotnet::LoadMod(&mod, mod.manifest.dir / FromUtf8(mod.manifest.dotnet));
				// A script's compile errors already start with the file ("main.cs(5,17): error CS0029: ...")
				if (!error.empty()) problem = error.starts_with(mod.manifest.dotnet) ? error : mod.manifest.dotnet + ": " + error;
			}
			if (problem.empty() && !mod.manifest.lua.empty()) {
				std::string error;
				mod.lua = LoadLua(&kApi, &mod, mod.manifest.dir / FromUtf8(mod.manifest.lua), error);
				if (!mod.lua) problem = mod.manifest.lua + ": " + error;
			}
			if (!problem.empty()) {
				const bool crashed = mod.state.load() == State::Crashed;
				if (!crashed) mod.state = State::Failed;
				for (ModModule* m : mod.modules) m->Unbind();
				UnloadCode(mod);
				// Modules new in this load never reach the menu
				for (const auto& p : mod.pending) {
					std::erase(mod.modules, static_cast<ModModule*>(p.get()));
					std::erase_if(mod.declared, [&](const auto& kv) { return &kv.second->Owner() == p.get(); });
				}
				mod.pending.clear();
				mod.problem = problem;
				return problem;
			}
			// Its content: read again on every load (a reload picks up edited files)
			if (!mod.manifest.content.empty()) {
				std::vector<std::filesystem::path> folders;
				for (const auto& c : mod.manifest.content) folders.push_back(mod.manifest.dir / FromUtf8(c));
				std::vector<std::string> problems;
				auto set = content::Load(mod.id, folders, problems);
				for (const auto& w : problems) {
					spdlog::warn("[mod {}] content: {}", mod.id, w);
					mod.warnings.push_back("content: " + w);
				}
				const bool on = mod.contentModule && mod.contentModule->Enabled();
				if (mod.content) {
					if (content::Applied(*mod.content)) content::Restore(*mod.content);
					content::Unregister(*mod.content);
				}
				mod.content = content::Empty(*set) ? nullptr : set;
				if (mod.content) {
					content::Register(mod.content);
					if (!mod.contentModule) {
						std::string name = mod.manifest.name;
						if (modules::NameTaken(name) || std::any_of(mod.pending.begin(), mod.pending.end(), [&](const auto& m) { return m->Name() == name; }))
							name += " (content)";
						auto module = std::make_unique<ContentModule>(&mod, name);
						mod.contentModule = module.get();
						mod.pending.push_back(std::move(module));
					} else if (on) {
						auto applied = mod.content;
						game::RunOnGameThread([applied] { content::Apply(*applied); });
					}
				}
			}
			for (const SettingDecl& d : mod.manifest.settings)
				if (!mod.declared.count(d.key)) {
					const std::string w = "setting " + d.key + " isn't shown: " +
						(d.module.empty() ? std::string("the mod registered no module") : "it registered no module named '" + d.module + "'");
					spdlog::warn("[mod {}] {}", mod.id, w);
					mod.warnings.push_back(w);
				}
			modules::Publish(std::move(mod.pending));
			mod.pending.clear();
			mod.problem.clear();
			mod.state = State::Active;
			return {};
		}

		std::string Kind(const Manifest& m)
		{
			std::string kind;
			for (const auto& [has, name] : {std::pair{!m.entry.empty(), "native"}, {!m.dotnet.empty(), "C#"}, {!m.lua.empty(), "lua"}})
				if (has) kind += (kind.empty() ? "" : " + ") + std::string(name);
			return kind.empty() ? "content" : kind;
		}
	}

	const MirageApi* Api() { return &kApi; }

	std::map<std::string, std::string> CrashedMods(const std::filesystem::path& folder) { return ReadCrashes(folder); }

	void Discover(const std::vector<std::filesystem::path>& folders)
	{
		std::vector<Manifest> manifests;
		std::map<std::string, std::string> off;
		for (const auto& folder : folders) {
			std::error_code ec;
			if (!std::filesystem::is_directory(folder, ec)) {
				spdlog::info("Mods: no {} folder", Utf8(folder));
				continue;
			}
			for (const std::string& id : ReadDisabled(folder)) {
				off.emplace(id, "switched off in the mod manager");
				std::lock_guard lock(g_flagLock);
				g_userOff.insert(id);
			}
			for (const auto& [id, why] : ReadCrashes(folder)) {
				off.emplace(id, why);
				std::lock_guard lock(g_flagLock);
				g_crashed.insert(id);
			}
			std::vector<std::filesystem::path> dirs;
			for (const auto& e : std::filesystem::directory_iterator(folder, ec))
				if (e.is_directory() && std::filesystem::exists(e.path() / "mod.toml")) dirs.push_back(e.path());
			std::sort(dirs.begin(), dirs.end());
			for (const auto& dir : dirs) {
				Parsed p = Load(dir);
				for (const auto& w : p.warnings) spdlog::warn("Mods: {}", w);
				if (!p.manifest) {
					Info info;
					info.dir = dir;
					info.id = Utf8(dir.filename());
					info.warnings = p.warnings;
					info.problem = p.errors.empty() ? "mod.toml is invalid" : p.errors.front();
					for (const auto& e : p.errors) spdlog::error("Mods: {}", e);
					g_excluded.push_back(std::move(info));
					continue;
				}
				manifests.push_back(std::move(*p.manifest));
			}
		}

		const Resolution r = Resolve(manifests, *semver::Version::Parse(kRuntimeVersion), off);
		for (const auto& w : r.warnings) spdlog::warn("Mods: {}", w);
		for (const Manifest& m : manifests) {
			const auto it = r.excluded.find(m.id);
			if (it == r.excluded.end()) continue;
			Info info;
			info.id = m.id;
			info.name = m.name;
			info.version = m.version.ToString();
			info.dir = m.dir;
			info.phase = m.phase;
			info.kind = Kind(m);
			info.problem = it->second;
			spdlog::error("Mods: {} {} is not loaded: {}", m.id, info.version, info.problem);
			g_excluded.push_back(std::move(info));
		}
		for (const std::string& id : r.order) {
			const auto m = std::find_if(manifests.begin(), manifests.end(), [&](const Manifest& x) { return x.id == id; });
			auto mod = std::make_unique<MirageMod>();
			mod->manifest = *m;
			mod->id = m->id;
			mod->version = m->version.ToString();
			mod->dir = Utf8(m->dir);
			std::lock_guard lock(g_modsLock);
			g_mods.push_back(std::move(mod));
		}
		std::vector<std::filesystem::path> watched;
		for (const auto& f : folders)
			if (std::error_code ec; std::filesystem::is_directory(f, ec)) watched.push_back(f);
		StartWatcher(watched);
		if (!r.order.empty()) {
			std::string order;
			for (const auto& id : r.order) order += (order.empty() ? "" : ", ") + id;
			spdlog::info("Mods: load order {}", order);
		}
	}

	void LoadPhase(Phase phase)
	{
		for (auto& owned : g_mods) {
			MirageMod& mod = *owned;
			if (mod.manifest.phase != phase || mod.state.load() != State::NotLoaded) continue;
			// A required mod that failed to load (or crashed) takes this one with it
			std::string problem;
			for (const Dependency& d : mod.manifest.required) {
				const auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](const auto& x) { return x->id == d.id; });
				if (it == g_mods.end() || (*it)->state.load() != State::Active) {
					problem = "it needs " + d.id + ", which didn't load";
					break;
				}
			}
			if (!problem.empty()) {
				mod.state = State::Failed;
				mod.problem = problem;
				spdlog::error("Mods: {} {} is not loaded: {}", mod.id, mod.version, problem);
				continue;
			}
			problem = LoadCode(mod);
			if (!problem.empty()) {
				spdlog::error("Mods: {} {} is not loaded: {}", mod.id, mod.version, problem);
				continue;
			}
			mod.loadIndex = g_loadCount++;
			spdlog::info("Mods: loaded {} {} ({} phase, {}, {} module(s){})", mod.id, mod.version, PhaseName(phase),
						 Kind(mod.manifest), mod.modules.size(),
						 mod.lua ? ", Lua " + std::to_string(LuaMemory(*mod.lua) / 1024) + " KB" : std::string());
		}
	}

	std::string Reload(const std::string& id)
	{
		const auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](const auto& m) { return m->id == id; });
		if (it == g_mods.end()) return "no loaded mod has the id " + id;
		MirageMod& mod = **it;
		const State state = mod.state.load();
		if (state == State::Crashed) return "it crashed: restart the game to load it again";
		if (state != State::Active && state != State::Failed) return "it isn't loaded";
		if (!mod.manifest.hotReload) return "it doesn't support hot reload (hot_reload = true in mod.toml)";

		Parsed p = Load(mod.manifest.dir);
		if (!p.manifest) return "mod.toml: " + (p.errors.empty() ? std::string("invalid") : p.errors.front());
		if (p.manifest->id != mod.id) return "mod.toml: the id changed";

		// The old code undoes its changes, then stops
		std::vector<ModModule*> wasOn;
		for (ModModule* m : mod.modules)
			if (m->Enabled()) {
				wasOn.push_back(m);
				m->SetEnabled(false);
			}
		mod.state = State::Detached;
		const auto start = std::chrono::steady_clock::now();
		while (mod.inflight.load() > 0) {		// A call on the render thread still running
			if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
				spdlog::error("[mod {}] hot reload: a call into it didn't return in 2 s; reload skipped", mod.id);
				mod.state = State::Active;
				for (ModModule* m : wasOn) m->SetEnabled(true);
				return "a call into it didn't return";
			}
			Sleep(0);
		}
		UnloadCode(mod);

		mod.manifest = std::move(*p.manifest);
		mod.version = mod.manifest.version.ToString();
		const std::string problem = LoadCode(mod);
		++mod.reloads;
		if (!problem.empty()) {
			spdlog::error("[mod {}] hot reload failed: {} (its modules are off until it loads again)", mod.id, problem);
			events::Bus().Publish(events::ModReloaded{mod.id, false});
			return problem;
		}
		for (ModModule* m : wasOn)
			if (m->Bound()) m->SetEnabled(true);
		spdlog::info("[mod {}] reloaded ({} time(s))", mod.id, mod.reloads);
		events::Bus().Publish(events::ModReloaded{mod.id, true});
		return {};
	}

	void ReloadChanged()
	{
		// Which mods to look at: the ones the watcher marked, the ones still settling, or all
		std::set<std::wstring> check;
		{
			std::lock_guard lock(g_dirtyLock);
			check.swap(g_dirty);
		}
		check.merge(g_settling);
		g_settling.clear();
		const bool all = g_dirtyAll.exchange(!g_watcher.joinable());	// Without a watcher: every mod, every time
		if (!all && check.empty()) return;
		for (auto& owned : g_mods) {
			MirageMod& mod = *owned;
			const State s = mod.state.load();
			if (s != State::Active && s != State::Failed) continue;
			if (mod.loadIndex < 0) continue;	// Never loaded (left out at its phase)
			const std::wstring folder = mod.manifest.dir.filename().wstring();
			if (!all && !check.count(folder)) continue;
			const std::uintmax_t now = Fingerprint(mod.manifest.dir);
			if (now == mod.fingerprint) continue;
			if (now != mod.seen) {	// Still being written (an editor saving, a build): the next check decides
				mod.seen = now;
				g_settling.insert(folder);
				continue;
			}
			if (!mod.manifest.hotReload) {
				if (!mod.warnedNoReload)
					spdlog::info("[mod {}] its files changed: restart the game to load them (or hot_reload = true)", mod.id);
				mod.warnedNoReload = true;
				mod.fingerprint = now;
				continue;
			}
			Reload(mod.id);		// Sets the fingerprint again, failed or not
		}
	}

	std::string ModOfModule(const void* module)
	{
		if (!module) return {};
		for (auto& owned : g_mods)
			if (owned->dll && static_cast<const void*>(owned->dll) == module) return owned->id;
		return {};
	}

	std::string AttributeCrash(const void* address, unsigned long code, bool* guarded)
	{
		if (t_guarded > 0) {	// The guard around the call catches it (and names the mod)
			if (guarded) *guarded = true;
			return {};
		}
		HMODULE m = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								static_cast<LPCWSTR>(address), &m))
			return {};
		for (auto& owned : g_mods)
			if (owned->dll && owned->dll == m) {
				Fault f;
				f.code = code;
				f.address = address;
				Crashed(*owned, "(its own thread)", "code outside a callback", f);
				return owned->id;
			}
		return {};
	}

	void UnloadAll()
	{
		StopWatcher();
		std::vector<MirageMod*> loaded;
		for (auto& m : g_mods)
			if (m->loadIndex >= 0) loaded.push_back(m.get());
		std::sort(loaded.begin(), loaded.end(), [](const MirageMod* a, const MirageMod* b) { return a->loadIndex > b->loadIndex; });
		for (MirageMod* mod : loaded) {
			if (mod->state.load() == State::Active) mod->state = State::Detached;
			UnloadCode(*mod);
			spdlog::info("Mods: unloaded {}", mod->id);
		}
		// The records stay: the menu's module list still holds the mods' modules (never called again) until the DLL
		// goes, and they point at their mod
	}

	std::vector<std::pair<std::string, std::size_t>> LuaMemoryByMod()
	{
		std::vector<std::pair<std::string, std::size_t>> out;
		for (const auto& owned : g_mods)
			if (owned->lua && owned->state.load() == State::Active) out.emplace_back(owned->id, LuaMemory(*owned->lua));
		std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		return out;
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		std::lock_guard modsLock(g_modsLock);
		for (const auto& owned : g_mods) {
			const MirageMod& mod = *owned;
			Info info;
			info.id = mod.id;
			info.name = mod.manifest.name;
			info.version = mod.version;
			info.dir = mod.manifest.dir;
			info.phase = mod.manifest.phase;
			info.kind = Kind(mod.manifest);
			info.loaded = mod.state.load() == State::Active;
			info.script = !mod.manifest.lua.empty() || mod.manifest.dotnet.ends_with(".cs");
			info.loadIndex = mod.loadIndex;
			info.reloads = mod.reloads;
			info.problem = mod.state.load() == State::NotLoaded ? std::string("not loaded yet (") + PhaseName(mod.manifest.phase) + " phase)"
															   : mod.problem;
			for (const ModModule* m : mod.modules)
				if (m->Bound()) info.modules.push_back(m->Name());
			info.warnings = mod.warnings;
			info.description = mod.manifest.description;
			for (const auto& a : mod.manifest.authors) info.authors += (info.authors.empty() ? "" : ", ") + a;
			const auto deps = [](const std::vector<Dependency>& list, std::vector<std::string>& to) {
				for (const auto& d : list) to.push_back(d.id + " " + d.range.Text());
			};
			deps(mod.manifest.required, info.required);
			deps(mod.manifest.optional, info.optional);
			deps(mod.manifest.incompatible, info.incompatible);
			for (const auto& other : g_mods)
				for (const auto& d : other->manifest.required)
					if (d.id == mod.id) info.dependents.push_back(other->id);
			{
				std::lock_guard lock(g_flagLock);
				info.offNextStart = g_userOff.count(mod.id) != 0;
				info.crashed = g_crashed.count(mod.id) != 0;
			}
			out.push_back(std::move(info));
		}
		std::stable_sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
			const auto rank = [](const Info& i) { return i.loadIndex < 0 ? 1 << 30 : i.loadIndex; };
			return rank(a) < rank(b);
		});
		out.insert(out.end(), g_excluded.begin(), g_excluded.end());
		return out;
	}

	namespace
	{
		const MirageMod* ModById(const std::string& id)		// The record lives as long as the menu: safe after the lock
		{
			std::lock_guard lock(g_modsLock);
			for (const auto& m : g_mods)
				if (m->id == id) return m.get();
			return nullptr;
		}
	}

	std::string LoadNew(const std::filesystem::path& dir)
	{
		Parsed p = Load(dir);
		for (const auto& w : p.warnings) spdlog::warn("Mods: {}", w);
		if (!p.manifest) return "mod.toml: " + (p.errors.empty() ? std::string("invalid") : p.errors.front());
		const Manifest& m = *p.manifest;
		if (ModById(m.id) || std::any_of(g_excluded.begin(), g_excluded.end(), [&](const Info& i) { return i.id == m.id; }))
			return "a mod with the id " + m.id + " is already there";
		if (!m.mirage.Matches(*semver::Version::Parse(kRuntimeVersion)))
			return "it needs Mirage " + m.mirage.Text() + ", this is " + kRuntimeVersion;
		for (const Dependency& d : m.required) {
			const MirageMod* other = ModById(d.id);
			if (!other || other->state.load() != State::Active) return "it needs " + d.id + ", which isn't loaded";
		}
		for (const Dependency& d : m.incompatible)
			if (const MirageMod* other = ModById(d.id); other && other->state.load() == State::Active)
				return "it is incompatible with " + d.id + ", which is loaded";

		auto owned = std::make_unique<MirageMod>();
		owned->manifest = m;
		owned->id = m.id;
		owned->version = m.version.ToString();
		owned->dir = Utf8(m.dir);
		MirageMod& mod = *owned;
		{
			std::lock_guard lock(g_modsLock);
			g_mods.push_back(std::move(owned));
		}
		mod.loadIndex = g_loadCount++;		// Even when it fails: ReloadChanged retries it once its files change
		const std::string problem = LoadCode(mod);
		if (!problem.empty()) {
			spdlog::error("Mods: new mod {} {} didn't load: {}", mod.id, mod.version, problem);
			return problem;
		}
		spdlog::info("Mods: loaded new mod {} {} ({}, {} module(s))", mod.id, mod.version, Kind(mod.manifest), mod.modules.size());
		return {};
	}

	bool SetEnabledNextStart(const std::string& id, bool on)
	{
		const MirageMod* mod = ModById(id);
		if (!mod) return false;
		const auto folder = mod->manifest.dir.parent_path();
		std::set<std::string> off = ReadDisabled(folder);
		if (on) off.erase(id);
		else off.insert(id);
		if (!WriteDisabled(folder, off)) {
			spdlog::error("Mods: {} can't be written", Utf8(folder / "disabled.toml"));
			return false;
		}
		{
			std::lock_guard lock(g_flagLock);
			if (on) g_userOff.erase(id);
			else g_userOff.insert(id);
		}
		if (!on)
			for (ModModule* m : mod->modules)
				if (m->Enabled()) m->SetEnabled(false);
		spdlog::info("Mods: {} switched {} for the next start", id, on ? "on" : "off");
		return true;
	}

	bool ForgetCrash(const std::string& id)
	{
		const MirageMod* mod = ModById(id);
		if (!mod) return false;
		std::lock_guard lock(g_crashLock);
		const auto file = mod->manifest.dir.parent_path() / "crashes.toml";
		try {
			if (!std::filesystem::exists(file)) return false;
			toml::ordered_value root = toml::parse<toml::ordered_type_config>(file);
			if (!root.is_table() || !root.contains(id)) return false;
			root.as_table().erase(id);
			std::ofstream(file, std::ios::binary | std::ios::trunc)
				<< "# Mods that crashed. A mod listed here isn't loaded: delete its table to try it again.\n\n"
				<< toml::format(root);
		} catch (const std::exception& e) {
			spdlog::error("Mods: {} can't be changed: {}", Utf8(file), e.what());
			return false;
		}
		{
			std::lock_guard flags(g_flagLock);
			g_crashed.erase(id);
		}
		spdlog::info("Mods: {} will be tried again at the next start", id);
		return true;
	}

	void detail::HookCrashed(const std::string& modId, const std::string& what, const std::string& of, unsigned long code, const void* address)
	{
		for (auto& m : g_mods) {
			if (m->id != modId) continue;
			if (!Running(*m)) return;		// Stopped already (a second fault of the same mod)
			Fault f;
			f.code = code;
			f.address = address;
			Crashed(*m, of, what, f);
			for (ModModule* mm : m->modules)
				if (mm->Enabled()) mm->SetEnabled(false);
			RemoveGameHooks(*m);
			return;
		}
	}
}
