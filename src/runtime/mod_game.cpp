#include "runtime/mod_game.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/object_query.h"
#include "game/object_ref.h"
#include "game/player_state.h"
#include <nlohmann/json.hpp>
#include "game/uobject.h"
#include "gui.h"
#include "runtime/call.h"
#include "runtime/class_source.h"
#include "runtime/classes.h"
#include "runtime/layers.h"
#include "runtime/detours.h"
#include "runtime/fhooks.h"
#include "runtime/inject.h"
#include "runtime/natives.h"
#include "runtime/mod_internal.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// A call of a game function, as a mod sees it: one a hook runs in, or one the mod makes
struct MirageCall
{
	mirage::hooks::Call* hook = nullptr;				// A hooked call
	std::unique_ptr<mirage::call::Params> params;		// A call the mod makes
	std::list<std::wstring> strings;					// Its string parameters' text (the engine copies what it keeps)
	Classes::UObject* object = nullptr;
	std::string function;
};

namespace mirage::mods
{
	namespace
	{
		Classes::UObject* Obj(MirageObject* o) { return reinterpret_cast<Classes::UObject*>(o); }
		MirageObject* Handle(Classes::UObject* o) { return reinterpret_cast<MirageObject*>(o); }

		// The object is still one the engine has: a mod may hold a pointer past the object's life. The read of its
		// index is guarded (the memory may be gone), then the object list must hold it at that index.
		bool ReadIndex(const Classes::UObject* o, std::size_t* index)
		{
			__try {
				*index = static_cast<std::size_t>(o->ObjectInternalInteger);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		bool Alive(Classes::UObject* o)
		{
			std::size_t index = 0;
			return o && Classes::UObject::GObjects && ReadIndex(o, &index) && game::AliveAt(o, index);
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

		int32_t Read(const reflect::Value& v, MirageValue* out)
		{
			*out = MirageValue{};
			if (!v) return 0;
			using K = layout::Kind;
			switch (v.member->kind) {
				case K::Bool: out->type = MIRAGE_VALUE_BOOL; out->i = reflect::GetInt(v); break;
				case K::Int: case K::Byte: out->type = MIRAGE_VALUE_INT; out->i = reflect::GetInt(v); break;
				case K::Float: out->type = MIRAGE_VALUE_FLOAT; out->f = reflect::GetFloat(v); break;
				case K::Object: case K::Class: case K::Component: case K::Interface:
					out->type = MIRAGE_VALUE_OBJECT;
					out->o = Handle(reflect::GetObj(v));
					break;
				case K::Name: case K::Str: {
					out->type = MIRAGE_VALUE_TEXT;
					const std::string t = reflect::GetText(v);
					CopyOut(t, out->text, sizeof out->text);
					break;
				}
				case K::Struct: out->type = MIRAGE_VALUE_STRUCT; break;
				case K::Array: out->type = MIRAGE_VALUE_ARRAY; out->i = reflect::ArrayCount(v); break;
				default: return 0;
			}
			return 1;
		}

		int32_t Write(const reflect::Value& v, const MirageValue* in)
		{
			if (!v || !in) return 0;
			switch (in->type) {
				case MIRAGE_VALUE_BOOL: case MIRAGE_VALUE_INT: return reflect::SetInt(v, in->i) ? 1 : 0;
				case MIRAGE_VALUE_FLOAT: return reflect::SetFloat(v, in->f) ? 1 : 0;
				case MIRAGE_VALUE_OBJECT:
					if (in->o && !Alive(Obj(in->o))) return 0;
					return reflect::SetObj(v, Obj(in->o)) ? 1 : 0;
				case MIRAGE_VALUE_TEXT: {
					char text[sizeof in->text + 1]{};
					std::memcpy(text, in->text, sizeof in->text);
					return reflect::SetText(v, text) ? 1 : 0;
				}
				default: return 0;
			}
		}

		std::string FunctionPath(Classes::UObject* fn)
		{
			std::string path = fn ? fn->GetName() : std::string();
			for (auto* o = fn ? fn->Outer : nullptr; o; o = o->Outer) path = o->GetName() + "." + path;
			return path;
		}

		// ---- Hooks: the runtime's callback around a mod's ----------------------------------------------------------
		struct HookArgs
		{
			HookRecord* record;
			MirageCall* call;
		};
		void RunModHook(void* a)
		{
			auto* args = static_cast<HookArgs*>(a);
			args->record->fn(args->record->user, args->call);
		}
		void OnHook(void* user, hooks::Call& c)
		{
			auto* r = static_cast<HookRecord*>(user);
			if (!r->id || !detail::Running(*r->mod)) return;
			MirageCall call;
			call.hook = &c;
			call.object = c.object;
			call.function = r->function;
			HookArgs args{r, &call};
			detail::Enter(*r->mod, "hook", r->function, &RunModHook, &args);
		}

		// ---- Events ------------------------------------------------------------------------------------------------
		std::mutex g_subLock;
		std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Subscription>>>> g_subs{
			std::make_shared<const std::vector<std::shared_ptr<Subscription>>>()};

		void Republish()	// With g_subLock: the active subscriptions, as one snapshot the dispatchers read
		{
			auto next = std::make_shared<std::vector<std::shared_ptr<Subscription>>>();
			for (const auto& s : *g_subs.load())
				if (s->active.load()) next->push_back(s);
			g_subs.store(std::move(next));
		}

		struct EventArgs
		{
			Subscription* sub;
			const MirageEvent* event;
		};
		void RunEvent(void* a)
		{
			auto* args = static_cast<EventArgs*>(a);
			args->sub->fn(args->sub->user, args->event);
		}
		void Dispatch(const MirageEvent& e)
		{
			const auto subs = g_subs.load(std::memory_order_acquire);
			if (subs->empty()) return;
			static const char* const kNames[] = {"tick", "level load", "death", "player state", "key", "state restored"};
			for (const auto& s : *subs) {
				if (s->type != e.type || !s->active.load(std::memory_order_relaxed)) continue;
				EventArgs args{s.get(), &e};
				detail::Enter(*s->mod, "event", std::string(kNames[e.type]) + " event", &RunEvent, &args);
			}
		}
		MirageEvent Event(int32_t type)
		{
			MirageEvent e{};
			e.size = sizeof e;
			e.type = type;
			e.text = "";
			return e;
		}
	}

	namespace
	{
		// An injection comes out on the game thread (its function's bytecode is rebuilt); from another thread (a Lua
		// on_disable runs on the menu's) it is queued there. Its record is already off: the callback no longer runs.
		void RemoveInjection(int id)
		{
			if (game::IsGameThread()) inject::Remove(id);
			else game::RunOnGameThread([id] { inject::Remove(id); });
		}
	}

	void detail::RemoveGameHooks(MirageMod& mod)
	{
		for (auto& r : mod.hooks)
			if (r->id) {
				if (r->inject) RemoveInjection(r->id);
				else hooks::Remove(r->id);
				r->id = 0;
			}
		{
			std::lock_guard lock(g_subLock);
			for (auto& s : mod.subscriptions) s->active = false;
			Republish();
		}
		mod.subscriptions.clear();
		for (int id : mod.detours) detours::Remove(id);
		mod.detours.clear();
		ReleaseScripts(mod);
		for (auto& n : mod.natives) n->live = false;		// Its natives return zero from here (their index stays)
		natives::Release(mod.id);
		if (game::IsGameThread()) layers::Remove(mod.id);	// Its level additions (else the next tick sees it unloaded)
		// Its classes and the objects it kept go with it (on the game thread: actors are destroyed)
		if (!mod.classes.empty() || !mod.kept.empty()) {
			auto retire = [modClasses = mod.classes, kept = mod.kept] {
				for (void* o : kept)
					if (Alive(static_cast<Classes::UObject*>(o))) classes::Keep(static_cast<Classes::UObject*>(o), false);
				for (void* c : modClasses)
					if (Alive(static_cast<Classes::UObject*>(c))) classes::Retire(static_cast<Classes::UClass*>(c));
			};
			if (game::IsGameThread()) retire();
			else game::RunOnGameThread(retire);
			mod.classes.clear();
			mod.kept.clear();
		}
	}

	void DispatchTick(float seconds)
	{
		MirageEvent e = Event(MIRAGE_EVENT_TICK);
		e.number = seconds;
		Dispatch(e);
	}
	void DispatchLevelLoad(bool finished, const std::string& map)
	{
		MirageEvent e = Event(MIRAGE_EVENT_LEVEL_LOAD);
		e.value = finished ? 1 : 0;
		e.text = map.c_str();
		Dispatch(e);
	}
	void DispatchDeath(bool after)
	{
		MirageEvent e = Event(MIRAGE_EVENT_DEATH);
		e.value = after ? 1 : 0;
		Dispatch(e);
	}
	namespace
	{
		const MirageEvent* g_skippable = nullptr;	// The player state event being dispatched (game thread)
		bool g_skipped = false;
	}
	bool DispatchPlayerState()
	{
		static int previous = -1;
		auto* pawn = game::Pawn();
		if (!pawn) return false;
		MirageEvent e = Event(MIRAGE_EVENT_PLAYER_STATE);
		e.value = static_cast<int32_t>(static_cast<std::uint8_t>(pawn->MovementState.GetValue()));
		e.value2 = previous;
		previous = e.value;
		g_skippable = &e;
		g_skipped = false;
		Dispatch(e);
		g_skippable = nullptr;
		return g_skipped;
	}
	void DispatchStateRestored() { Dispatch(Event(MIRAGE_EVENT_STATE_RESTORED)); }
	void DispatchMemoryPressure(int level)
	{
		MirageEvent e = Event(MIRAGE_EVENT_MEMORY_PRESSURE);
		e.value = level;
		Dispatch(e);
	}

	void DispatchKey(int vk)
	{
		MirageEvent e = Event(MIRAGE_EVENT_KEY);
		e.value = vk;
		Dispatch(e);
	}

	namespace game_api
	{
		MirageSubscription* MIRAGE_CALL EventSubscribe(MirageMod* mod, int32_t type, MirageEventFn fn, void* user)
		{
			if (!mod || !fn || type < MIRAGE_EVENT_TICK || type > MIRAGE_EVENT_MEMORY_PRESSURE) return nullptr;
			auto s = std::make_shared<Subscription>();
			s->mod = mod;
			s->type = type;
			s->fn = fn;
			s->user = user;
			std::lock_guard lock(g_subLock);
			mod->subscriptions.push_back(s);
			auto next = std::make_shared<std::vector<std::shared_ptr<Subscription>>>(*g_subs.load());
			next->push_back(s);
			g_subs.store(std::move(next));
			return reinterpret_cast<MirageSubscription*>(s.get());
		}

		void MIRAGE_CALL EventUnsubscribe(MirageSubscription* subscription)
		{
			if (!subscription) return;
			auto* s = reinterpret_cast<Subscription*>(subscription);
			std::lock_guard lock(g_subLock);
			s->active = false;
			Republish();
		}

		MirageHook* MIRAGE_CALL HookFunction(MirageMod* mod, const char* function, int32_t when, int32_t priority,
											 MirageHookFn fn, void* user)
		{
			if (!mod || !function || !fn || when < MIRAGE_HOOK_BEFORE || when > MIRAGE_HOOK_REPLACE) return nullptr;
			auto r = std::make_unique<HookRecord>();
			r->mod = mod;
			r->fn = fn;
			r->user = user;
			r->function = function;
			std::string error;
			const auto w = when == MIRAGE_HOOK_BEFORE ? hooks::When::Before : when == MIRAGE_HOOK_AFTER ? hooks::When::After
																									  : hooks::When::Replace;
			r->id = hooks::Add(function, w, priority, &OnHook, r.get(), &error);
			if (!r->id) {
				spdlog::error("[mod {}] hook_function {}: {}", mod->id, function, error);
				return nullptr;
			}
			HookRecord* raw = r.get();
			mod->hooks.push_back(std::move(r));
			return reinterpret_cast<MirageHook*>(raw);
		}

		void MIRAGE_CALL HookRemove(MirageHook* hook)
		{
			auto* r = reinterpret_cast<HookRecord*>(hook);
			if (!r || !r->id) return;
			if (r->inject) RemoveInjection(r->id);
			else hooks::Remove(r->id);
			r->id = 0;
		}

		struct NativeArgs
		{
			NativeRecord* record;
			MirageNativeCall* call;
		};
		void RunModNative(void* a)
		{
			auto* args = static_cast<NativeArgs*>(a);
			args->record->fn(args->record->user, args->call);
		}
		void OnNative(void* user, natives::Call& call)
		{
			auto* r = static_cast<NativeRecord*>(user);
			if (!r->live || !detail::Running(*r->mod)) return;
			NativeArgs args{r, reinterpret_cast<MirageNativeCall*>(&call)};
			detail::Enter(*r->mod, "native", r->name, &RunModNative, &args);
		}

		int32_t MIRAGE_CALL NativeRegister(MirageMod* mod, const char* name, const char* signature, MirageNativeFn fn, void* user)
		{
			if (!mod || !name || !signature || !fn) return 0;
			auto r = std::make_unique<NativeRecord>();
			r->mod = mod;
			r->fn = fn;
			r->user = user;
			r->name = name;
			std::string error;
			const int index = natives::Register(mod->id, name, signature, &OnNative, r.get(), &error);
			if (!index) {
				spdlog::error("[mod {}] native_register {}: {}", mod->id, name, error);
				return 0;
			}
			mod->natives.push_back(std::move(r));
			return index;
		}
		int32_t MIRAGE_CALL NativeArg(MirageNativeCall* call, int32_t index, MirageValue* out)
		{
			auto* c = reinterpret_cast<natives::Call*>(call);
			if (!c || !out || index < 0 || index >= static_cast<int32_t>(c->args.size())) return 0;
			*out = c->args[static_cast<std::size_t>(index)];
			return 1;
		}
		void MIRAGE_CALL NativeReturn(MirageNativeCall* call, const MirageValue* value)
		{
			if (auto* c = reinterpret_cast<natives::Call*>(call); c && value) c->result = *value;
		}
		MirageObject* MIRAGE_CALL NativeSelf(MirageNativeCall* call)
		{
			auto* c = reinterpret_cast<natives::Call*>(call);
			return c ? Handle(c->self) : nullptr;
		}

		// An error for class_load / class_define: logged and copied for the mod
		MirageObject* ClassError(MirageMod* mod, const char* what, const std::string& why, char* error, uint32_t capacity)
		{
			spdlog::error("[mod {}] {}: {}", mod ? mod->id : "?", what, why);
			if (error && capacity) {
				const std::size_t n = (std::min)(why.size(), static_cast<std::size_t>(capacity - 1));
				std::memcpy(error, why.data(), n);
				error[n] = '\0';
			}
			return nullptr;
		}
		// A class of the mod's: tracked (retired at its unload), spawnable again if an earlier load retired it
		MirageObject* Adopt(MirageMod* mod, Classes::UClass* cls, const classes::BuiltClass& built, char* error, uint32_t capacity)
		{
			classes::Revive(cls);
			if (std::find(mod->classes.begin(), mod->classes.end(), cls) == mod->classes.end()) mod->classes.push_back(cls);
			spdlog::info("[mod {}] class {} loaded from {}{}", mod->id, built.className, built.package.filename().string(),
						 built.cached ? " (built before)" : fmt::format(" (built in {:.2f} s)", built.seconds));
			if (error && capacity) error[0] = '\0';
			return Handle(cls);
		}

		MirageObject* MIRAGE_CALL ClassLoad(MirageMod* mod, const char* path, char* error, uint32_t capacity)
		{
			const std::string what = std::string("class_load ") + (path ? path : "");
			if (!mod || !path) return ClassError(mod, what.c_str(), "no mod or path", error, capacity);
			if (!game::IsGameThread()) return ClassError(mod, what.c_str(), "only on the game thread (a tick event, a hook)", error, capacity);
			const auto utf8 = [](std::string_view t) { return std::filesystem::path(std::u8string(t.begin(), t.end())); };
			std::filesystem::path file = utf8(path);
			if (file.is_relative()) file = utf8(mod->dir) / file;
			std::string why;
			classes::BuiltClass built;
			Classes::UClass* cls = classes::LoadSource(file, &why, &built, mod->id);
			if (!cls) return ClassError(mod, what.c_str(), why, error, capacity);
			return Adopt(mod, cls, built, error, capacity);
		}

		MirageObject* MIRAGE_CALL ClassDefine(MirageMod* mod, const char* spec, char* error, uint32_t capacity)
		{
			if (!mod || !spec) return ClassError(mod, "class_define", "no mod or description", error, capacity);
			if (!game::IsGameThread()) return ClassError(mod, "class_define", "only on the game thread (a tick event, a hook)", error, capacity);
			classes::ClassSpec c;
			try {
				const nlohmann::json j = nlohmann::json::parse(spec);
				c.name = j.value("name", "");
				c.extends = j.value("extends", "");
				c.placeable = j.value("placeable", false);
				const auto list = [&](const char* key, std::vector<std::string>& out) {
					if (!j.contains(key)) return;
					for (const auto& v : j.at(key)) out.push_back(v.get<std::string>());
				};
				list("vars", c.vars);
				list("functions", c.functions);
				if (j.contains("defaults") && j.at("defaults").is_object()) {
					for (const auto& [k, v] : j.at("defaults").items())
						c.defaults.push_back(k + "=" + (v.is_string() ? v.get<std::string>() : v.is_boolean() ? (v.get<bool>() ? "True" : "False") : v.dump()));
				} else {
					list("defaults", c.defaults);
				}
				c.extra = j.value("extra", "");
			} catch (const std::exception& e) {
				return ClassError(mod, "class_define", std::string("the description isn't valid JSON: ") + e.what(), error, capacity);
			}
			const std::string what = "class_define " + c.name;
			std::string why;
			classes::BuiltClass built;
			Classes::UClass* cls = classes::DefineClass(c, mod->id, &why, &built);
			if (!cls) return ClassError(mod, what.c_str(), why, error, capacity);
			return Adopt(mod, cls, built, error, capacity);
		}

		uint32_t MIRAGE_CALL ObjectPath(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? Obj(object)->GetPathName() : std::string(), buffer, capacity);
		}

		MirageObject* MIRAGE_CALL LayerActor(MirageMod* mod, const char* modId, const char* name)
		{
			if (!name || !game::IsGameThread()) return nullptr;
			const std::string owner = modId && *modId ? modId : (mod ? mod->id : "");
			for (const layers::ActorInfo& a : layers::List())
				if (a.mod == owner && a.name == name && a.actor) return Handle(a.actor);
			return nullptr;
		}

		int32_t MIRAGE_CALL ObjectKeep(MirageMod* mod, MirageObject* object, int32_t keep)
		{
			if (!mod || !object || !Alive(Obj(object)) || !game::IsGameThread()) return 0;
			auto it = std::find(mod->kept.begin(), mod->kept.end(), static_cast<void*>(Obj(object)));
			if (keep && it == mod->kept.end()) mod->kept.push_back(Obj(object));
			if (!keep && it != mod->kept.end()) mod->kept.erase(it);
			classes::Keep(Obj(object), keep != 0);
			return 1;
		}

		MirageHook* MIRAGE_CALL InjectFunction(MirageMod* mod, const char* function, const char* point, int32_t priority,
											   MirageHookFn fn, void* user)
		{
			if (!mod || !function || !point || !fn) return nullptr;
			if (!game::IsGameThread()) {	// It rebuilds bytecode the VM may be running
				spdlog::error("[mod {}] inject_function {}: only on the game thread (a tick event, a hook)", mod->id, function);
				return nullptr;
			}
			auto r = std::make_unique<HookRecord>();
			r->mod = mod;
			r->fn = fn;
			r->user = user;
			r->function = function;
			r->inject = true;
			std::string error;
			r->id = inject::Add(function, point, priority, &OnHook, r.get(), mod->id, &error);
			if (!r->id) {
				spdlog::error("[mod {}] inject_function {} at {}: {}", mod->id, function, point, error);
				return nullptr;
			}
			HookRecord* raw = r.get();
			mod->hooks.push_back(std::move(r));
			return reinterpret_cast<MirageHook*>(raw);
		}

		MirageObject* MIRAGE_CALL CallObject(MirageCall* call) { return call ? Handle(call->object) : nullptr; }
		const char* MIRAGE_CALL CallFunction(MirageCall* call) { return call ? call->function.c_str() : ""; }
		void MIRAGE_CALL CallSkip(MirageCall* call)
		{
			if (call && call->hook && !call->hook->native) call->hook->skip = true;
		}

		int32_t MIRAGE_CALL CallGet(MirageCall* call, const char* path, MirageValue* out)
		{
			if (!call || !path || !out) return 0;
			if (call->hook) return Read(call->hook->Param(path), out);
			if (!call->params) return 0;
			return Read(reflect::Resolve(reinterpret_cast<Classes::UStruct*>(call->params->Function()), call->params->Data(), path), out);
		}

		int32_t MIRAGE_CALL CallSet(MirageCall* call, const char* path, const MirageValue* in)
		{
			if (!call || !path) return 0;
			if (call->hook) return Write(call->hook->Param(path), in);
			if (!call->params) return 0;
			const reflect::Value v = reflect::Resolve(reinterpret_cast<Classes::UStruct*>(call->params->Function()), call->params->Data(), path);
			if (v && in && in->type == MIRAGE_VALUE_TEXT && v.member->kind == layout::Kind::Str) {
				// A string parameter: its text lives in the call (the engine copies what it keeps)
				char text[sizeof in->text + 1]{};
				std::memcpy(text, in->text, sizeof in->text);
				const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
				std::wstring& w = call->strings.emplace_back(static_cast<std::size_t>(n > 0 ? n - 1 : 0), wchar_t{});
				if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, w.data(), n);
				const Classes::FString s(w.c_str());
				std::memcpy(v.address, &s, sizeof s);
				return 1;
			}
			return Write(v, in);
		}

		MirageCall* MIRAGE_CALL CallBegin(MirageObject* object, const char* function)
		{
			if (!object || !function || !Alive(Obj(object))) return nullptr;
			std::string_view name = function;
			if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) name.remove_prefix(dot + 1);
			auto params = std::make_unique<call::Params>(Obj(object), name);
			if (!params->Valid()) return nullptr;
			auto* c = new MirageCall;
			c->object = Obj(object);
			c->function = FunctionPath(reinterpret_cast<Classes::UObject*>(params->Function()));
			c->params = std::move(params);
			return c;
		}

		int32_t MIRAGE_CALL CallInvoke(MirageCall* call)
		{
			if (!call || !call->params || !Alive(call->object)) return 0;
			std::string error;
			if (call::Call(call->object, *call->params, &error)) return 1;
			spdlog::warn("Mods: call_invoke {}: {}", call->function, error);
			return 0;
		}

		void MIRAGE_CALL CallEnd(MirageCall* call)
		{
			if (call && !call->hook) delete call;	// A hook's call belongs to the hook
		}

		MirageObject* MIRAGE_CALL PlayerPawn() { return Handle(game::Pawn()); }
		MirageObject* MIRAGE_CALL PlayerController() { return Handle(game::Controller()); }
		MirageObject* MIRAGE_CALL WorldInfo() { return Handle(game::World()); }
		MirageObject* MIRAGE_CALL ObjectFind(const char* fullName)
		{
			return fullName ? Handle(game::FindObject<Classes::UObject>(fullName)) : nullptr;
		}

		int32_t MIRAGE_CALL ObjectGet(MirageObject* object, const char* path, MirageValue* out)
		{
			if (!out) return 0;
			*out = MirageValue{};
			if (!object || !path || !Alive(Obj(object))) return 0;
			return Read(reflect::Resolve(Obj(object)->Class, Obj(object), path), out);
		}

		int32_t MIRAGE_CALL ObjectSet(MirageObject* object, const char* path, const MirageValue* in)
		{
			if (!object || !path || !Alive(Obj(object))) return 0;
			return Write(reflect::Resolve(Obj(object)->Class, Obj(object), path), in);
		}

		uint32_t MIRAGE_CALL ObjectName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::NameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		uint32_t MIRAGE_CALL ObjectClassName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::ClassNameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		// A detour's handle is its id (never 0), as a pointer
		MirageDetour* MIRAGE_CALL DetourCreate(MirageMod* mod, void* target, void* detour, int32_t priority, void** original)
		{
			if (!mod) return nullptr;
			std::string error;
			const int id = detours::Add(target, detour, priority, original, mod->id, &error);
			if (!id) {
				spdlog::error("[mod {}] detour_create: {}", mod->id, error);
				return nullptr;
			}
			mod->detours.push_back(id);
			return reinterpret_cast<MirageDetour*>(static_cast<std::uintptr_t>(id));
		}

		void MIRAGE_CALL DetourRemove(MirageDetour* detour)
		{
			if (detour) detours::Remove(static_cast<int>(reinterpret_cast<std::uintptr_t>(detour)));
		}

		void* MIRAGE_CALL FindPattern(const char* pattern, const char* mask)
		{
			return pattern && mask ? game::FindPattern(pattern, mask) : nullptr;
		}

		int32_t MIRAGE_CALL ObjectsOfClass(const char* className, uint32_t flags, MirageObject** out, int32_t capacity)
		{
			if (!className) return -1;
			Classes::UClass* cls = game::FindClass(className);
			if (!cls) return -1;
			int32_t n = 0;
			for (Classes::UObject* o : game::ObjectsOf(cls)) {
				if (!(flags & MIRAGE_OBJECTS_DEFAULTS) && game::IsDefaultObject(o)) continue;
				if (out && n < capacity) out[n] = Handle(o);
				++n;
			}
			return n;
		}

		void MIRAGE_CALL EventSkip(const MirageEvent* event)
		{
			if (event && event == g_skippable) g_skipped = true;
		}

		int32_t MIRAGE_CALL MenuOpen() { return gui::visible ? 1 : 0; }

		uint32_t MIRAGE_CALL PlayerStateCapture(char* buffer, uint32_t capacity)
		{
			const nlohmann::json j = player_state::Capture();
			return CopyOut(j.is_null() ? std::string() : j.dump(), buffer, capacity);
		}
		int32_t MIRAGE_CALL PlayerStateApply(const char* state)
		{
			if (!state) return 0;
			const auto j = nlohmann::json::parse(state, nullptr, false);
			return !j.is_discarded() && player_state::Apply(j) ? 1 : 0;
		}
		void MIRAGE_CALL PlayerResetMovement()
		{
			auto* pawn = game::Pawn();
			auto* controller = game::Controller();
			if (pawn && controller) player_state::ResetMovement(pawn, controller);
		}

		void MIRAGE_CALL InputKey(int32_t vk, int32_t down)
		{
			if (vk <= 0 || vk > 0xFE) return;
			INPUT in{};
			in.type = INPUT_KEYBOARD;
			in.ki.wVk = static_cast<WORD>(vk);
			in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
			SendInput(1, &in, sizeof in);
		}

		std::mutex g_shareLock;
		std::map<std::string, std::string, std::less<>> g_shared;	// "<mod id>|<key>" (no id has a '|')
		void MIRAGE_CALL ShareSet(MirageMod* mod, const char* key, const char* value)
		{
			if (!mod || !key) return;
			std::lock_guard lock(g_shareLock);
			g_shared[mod->id + '|' + key] = value ? value : "";
		}
		uint32_t MIRAGE_CALL ShareGet(const char* modId, const char* key, char* buffer, uint32_t capacity)
		{
			std::string v;
			if (modId && key) {
				std::lock_guard lock(g_shareLock);
				if (const auto it = g_shared.find(std::string(modId) + '|' + key); it != g_shared.end()) v = it->second;
			}
			return CopyOut(v, buffer, capacity);
		}

		int32_t MIRAGE_CALL ObjectIsA(MirageObject* object, const char* className)
		{
			if (!object || !className || !Alive(Obj(object))) return 0;
			int depth = 0;
			for (Classes::UStruct* c = Obj(object)->Class; c && depth < 64; c = game::SuperOf(c), ++depth)
				if (c->GetName() == className) return 1;
			return 0;
		}
	}
}

namespace mirage::mods
{
	void Share(const std::string& key, const std::string& value)
	{
		std::lock_guard lock(game_api::g_shareLock);
		game_api::g_shared["memenu|" + key] = value;
	}
}

namespace mirage::mods::ui
{
	bool MenuVisible() { return gui::visible; }
}
