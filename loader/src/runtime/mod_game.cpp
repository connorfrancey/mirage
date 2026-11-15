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
#include "runtime/changes.h"
#include "runtime/class_source.h"
#include "runtime/classes.h"
#include "runtime/layers.h"
#include "core/hooking.h"
#include "runtime/detours.h"
#include "runtime/symbols.h"
#include "runtime/fhooks.h"
#include "runtime/inject.h"
#include "runtime/kismet.h"
#include "runtime/kismet_layers.h"
#include "runtime/kismet_trace.h"
#include "runtime/natives.h"
#include "runtime/mod_internal.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <atomic>
#include <cstring>
#include <functional>
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
			static const char* const kNames[] = {"tick", "level load", "death", "player state", "key", "state restored",
												 "memory pressure", "actor added", "actor removed", "level streamed", "restart"};
			static_assert(std::size(kNames) == MIRAGE_EVENT_RESTART + 1);
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

	namespace
	{
		void RemoveMidHooks(MirageMod& mod);	// Below: they use game_api's records
		void CheckHookFaults();
		void ForgetTests(MirageMod& mod);
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
		RemoveMidHooks(mod);
		ForgetTests(mod);
		ReleaseScripts(mod);
		for (auto& n : mod.natives) n->live = false;		// Its natives return zero from here (their index stays)
		natives::Release(mod.id);
		if (game::IsGameThread()) {
			layers::Remove(mod.id);			// Its level additions (else the next tick sees it unloaded)
			kismet_layers::Remove(mod.id);	// Its Kismet layer's edits
			kismet_trace::Forget(mod.id);
		}
		// Its transactions put back (step 15), on the game thread where the objects are
		if (game::IsGameThread()) changes::RevertAndForget(mod.id);
		else game::RunOnGameThread([id = mod.id] { changes::RevertAndForget(id); });
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
		CheckHookFaults();
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

	void DispatchActor(bool added, void* actor, const std::string& className)
	{
		MirageEvent e = Event(added ? MIRAGE_EVENT_ACTOR_ADDED : MIRAGE_EVENT_ACTOR_REMOVED);
		e.object = static_cast<MirageObject*>(actor);
		e.text = className.c_str();
		Dispatch(e);
	}
	void DispatchLevelStreamed(bool visible, const std::string& level)
	{
		MirageEvent e = Event(MIRAGE_EVENT_LEVEL_STREAMED);
		e.value = visible ? 1 : 0;
		e.text = level.c_str();
		Dispatch(e);
	}
	void DispatchRestart() { Dispatch(Event(MIRAGE_EVENT_RESTART)); }

	bool HasSubscribers(int type)
	{
		const auto subs = g_subs.load(std::memory_order_acquire);
		for (const auto& s : *subs)
			if (s->type == type && s->active.load(std::memory_order_relaxed)) return true;
		return false;
	}

	namespace game_api
	{
		MirageSubscription* MIRAGE_CALL EventSubscribe(MirageMod* mod, int32_t type, MirageEventFn fn, void* user)
		{
			if (!mod || !fn || type < MIRAGE_EVENT_TICK || type > MIRAGE_EVENT_RESTART) return nullptr;
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
			const reflect::Value v = reflect::Resolve(Obj(object)->Class, Obj(object), path);
			if (changes::Recording() && v) {		// In a transaction (step 15): the value it had, to put back
				MirageValue old{};
				if (Read(v, &old)) changes::Record(object, path, old);
			}
			return Write(v, in);
		}

		uint32_t MIRAGE_CALL ObjectName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::NameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		uint32_t MIRAGE_CALL ObjectClassName(MirageObject* object, char* buffer, uint32_t capacity)
		{
			return CopyOut(object && Alive(Obj(object)) ? reflect::ClassNameOf(Obj(object)) : std::string(), buffer, capacity);
		}

		// A mid-function hook of a mod's: its callback runs behind an SEH guard (it has a known signature, unlike a
		// detour). A fault skips it from then on and is kept for CheckHookFaults (the game thread names the mod).
		struct MidRecord
		{
			std::string mod;
			MirageMidHookFn callback;
			void* user;
			void* at;
			std::atomic<bool> off{false};
		};
		struct MidFault
		{
			MidRecord* rec;
			unsigned long code;
			const void* address;
		};
		std::mutex g_midFaultLock;
		std::vector<MidFault> g_midFaults;

		bool CallGuarded(MidRecord* rec, MirageRegisters* r, unsigned long* code, const void** address)
		{
			__try {
				rec->callback(rec->user, r);
				return true;
			} __except (*code = GetExceptionCode(), *address = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
						EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// MirageHook's MidHookContext: user_data, fpu, then edi esi ebp esp ebx edx ecx eax eflags continue_at
		struct MidContext
		{
			void* user;
			void* fpu;
			std::uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, eflags, continueAt;
		};
		void __cdecl MidEntry(void* context)
		{
			auto* c = static_cast<MidContext*>(context);
			auto* rec = static_cast<MidRecord*>(c->user);
			if (rec->off.load(std::memory_order_acquire)) return;
			MirageRegisters r{sizeof(MirageRegisters), c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->eflags, 0};
			unsigned long code = 0;
			const void* address = nullptr;
			if (!CallGuarded(rec, &r, &code, &address)) {
				rec->off = true;
				std::lock_guard lock(g_midFaultLock);
				g_midFaults.push_back({rec, code, address});
				return;
			}
			c->eax = r.eax; c->ecx = r.ecx; c->edx = r.edx; c->ebx = r.ebx; c->ebp = r.ebp; c->esi = r.esi; c->edi = r.edi;
			c->eflags = r.eflags;
			c->continueAt = r.continue_at;
		}

		std::string MidPlace(void* at)
		{
			// A symbol a little before it names the place ("StaticLoadObject+0x12"), when one is that close
			for (const auto& s : symbols::List())
				if (s.address && reinterpret_cast<std::uintptr_t>(at) >= s.address && reinterpret_cast<std::uintptr_t>(at) - s.address < 0x400)
					return fmt::format(" ({}+0x{:X})", s.name, reinterpret_cast<std::uintptr_t>(at) - s.address);
			return {};
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

		void* MIRAGE_CALL SymbolFind(const char* name)
		{
			return name ? symbols::Find<void>(name) : nullptr;
		}

		MirageDetour* MIRAGE_CALL DetourSymbol(MirageMod* mod, const char* symbol, void* detour, int32_t priority, void** original)
		{
			if (!mod || !symbol) return nullptr;
			void* target = symbols::Find<void>(symbol);
			if (!target) {
				spdlog::error("[mod {}] detour_symbol: no symbol \"{}\" (not in the runtime's table, or not found on this game)",
							  mod->id, symbol);
				return nullptr;
			}
			return DetourCreate(mod, target, detour, priority, original);
		}

		// The mods' tests: records kept for the menu's life (a test may be running when its mod unloads)
		struct TestRecord
		{
			MirageMod* mod;
			std::string name;
			MirageTestFn fn;
			void* user;
			bool live = true;
		};
		std::mutex g_testLock;
		std::vector<std::unique_ptr<TestRecord>> g_tests;

		int32_t MIRAGE_CALL TestRegister(MirageMod* mod, const char* name, MirageTestFn fn, void* user)
		{
			if (!mod || !name || !*name || !fn) return 0;
			std::lock_guard lock(g_testLock);
			g_tests.push_back(std::make_unique<TestRecord>(TestRecord{mod, name, fn, user}));
			return 1;
		}

		uint32_t MIRAGE_CALL KismetRead(const char* level, char* buffer, uint32_t capacity)
		{
			std::vector<kismet::Sequence> all = kismet::Read();
			if (level && *level)
				std::erase_if(all, [&](const kismet::Sequence& s) { return _stricmp(s.level.c_str(), level) != 0; });
			return CopyOut(kismet::ToJson(all), buffer, capacity);
		}

		// ---- Kismet edits (step 5) ----------------------------------------------------------------------------------
		int32_t KismetFail(MirageMod* mod, const char* what, const std::string& why)
		{
			spdlog::error("[mod {}] {}: {}", mod ? mod->id : "?", what, why);
			return 0;
		}
		// The edit's undo, in the open transaction, or in one of its own
		void RecordEdit(MirageMod* mod, const std::string& label, std::function<bool()> undo)
		{
			if (changes::Recording()) {
				changes::RecordUndo(label, std::move(undo));
				return;
			}
			changes::Begin(mod->id, label);
			changes::RecordUndo(label, std::move(undo));
			changes::End();
		}
		MirageHandle HandleOf(MirageObject* o)
		{
			MirageHandle h{};
			h.size = sizeof h;
			ObjectHandle(o, &h);
			return h;
		}
		Classes::USequenceOp* OpOf(MirageObject* o)
		{
			Classes::UObject* u = Obj(o);
			return u && Alive(u) && u->IsA(Classes::USequenceOp::StaticClass()) ? static_cast<Classes::USequenceOp*>(u) : nullptr;
		}
		std::string LinkLabel(Classes::USequenceOp* from, int output, Classes::USequenceOp* to, int input)
		{
			return fmt::format("{}[{}] -> {}[{}]", from->GetName(), output, to->GetName(), input);
		}

		int32_t MIRAGE_CALL KismetLink(MirageMod* mod, MirageObject* from, const char* output, MirageObject* to, const char* input)
		{
			if (!mod) return 0;
			if (!game::IsGameThread()) return KismetFail(mod, "kismet_link", "only on the game thread");
			Classes::USequenceOp* a = OpOf(from);
			Classes::USequenceOp* b = OpOf(to);
			if (!a || !b) return KismetFail(mod, "kismet_link", "both ends must be sequence ops that exist");
			const int out = kismet::OutputIndex(a, output);
			const int in = kismet::InputIndex(b, input);
			if (out < 0) return KismetFail(mod, "kismet_link", fmt::format("{} has no output {}", a->GetName(), output ? output : ""));
			if (in < 0) return KismetFail(mod, "kismet_link", fmt::format("{} has no input {}", b->GetName(), input ? input : ""));
			std::string why;
			switch (kismet::AddLink(a, out, b, in, -1, &why)) {
				case kismet::LinkResult::Exists: return 1;
				case kismet::LinkResult::Failed: return KismetFail(mod, "kismet_link", why);
				case kismet::LinkResult::Added: break;
			}
			RecordEdit(mod, "link " + LinkLabel(a, out, b, in), [ha = HandleOf(from), hb = HandleOf(to), out, in] {
				auto* x = OpOf(HandleObject(&ha));
				auto* y = OpOf(HandleObject(&hb));
				return x && y && kismet::RemoveLink(x, out, y, in) >= 0;
			});
			return 1;
		}

		int32_t MIRAGE_CALL KismetUnlink(MirageMod* mod, MirageObject* from, const char* output, MirageObject* to, const char* input)
		{
			if (!mod) return 0;
			if (!game::IsGameThread()) return KismetFail(mod, "kismet_unlink", "only on the game thread");
			Classes::USequenceOp* a = OpOf(from);
			Classes::USequenceOp* b = OpOf(to);
			if (!a || !b) return KismetFail(mod, "kismet_unlink", "both ends must be sequence ops that exist");
			const int out = kismet::OutputIndex(a, output);
			const int in = kismet::InputIndex(b, input);
			const int at = out < 0 || in < 0 ? -1 : kismet::RemoveLink(a, out, b, in);
			if (at < 0) return KismetFail(mod, "kismet_unlink", "no such link " + (out < 0 || in < 0 ? std::string() : LinkLabel(a, out, b, in)));
			RecordEdit(mod, "unlink " + LinkLabel(a, out, b, in), [ha = HandleOf(from), hb = HandleOf(to), out, in, at] {
				auto* x = OpOf(HandleObject(&ha));
				auto* y = OpOf(HandleObject(&hb));
				return x && y && kismet::AddLink(x, out, y, in, at, nullptr) != kismet::LinkResult::Failed;
			});
			return 1;
		}

		MirageObject* MIRAGE_CALL KismetCreate(MirageMod* mod, MirageObject* sequence, const char* class_name)
		{
			if (!mod) return nullptr;
			auto fail = [&](const std::string& why) {
				KismetFail(mod, "kismet_create", why);
				return nullptr;
			};
			if (!game::IsGameThread()) return fail("only on the game thread");
			Classes::UObject* s = Obj(sequence);
			if (!s || !Alive(s) || !s->IsA(Classes::USequence::StaticClass())) return fail("not a sequence");
			if (!class_name || !*class_name) return fail("no class");
			Classes::UClass* cls = game::FindClass(class_name, game::Freshness::Exact);
			if (!cls) return fail(fmt::format("no class {} is loaded", class_name));
			std::string why;
			Classes::USequenceOp* op = kismet::Construct(static_cast<Classes::USequence*>(s), cls, &why);
			if (!op) return fail(why);
			MirageObject* h = Handle(op);
			RecordEdit(mod, fmt::format("create {} in {}", op->GetName(), s->GetName()), [hs = HandleOf(sequence), ho = HandleOf(h)] {
				auto* seq = Obj(HandleObject(&hs));
				auto* o = Obj(HandleObject(&ho));
				return seq && o && kismet::RemoveOp(static_cast<Classes::USequence*>(seq), static_cast<Classes::USequenceObject*>(o));
			});
			return h;
		}

		int32_t MIRAGE_CALL KismetFire(MirageMod* mod, MirageObject* event, const char* output, MirageObject* instigator)
		{
			// `mod` may be null (the self-test): it only names the caller in the log
			if (!game::IsGameThread()) return KismetFail(mod, "kismet_fire", "only on the game thread");
			Classes::UObject* e = Obj(event);
			if (!e || !Alive(e) || !e->IsA(Classes::USequenceEvent::StaticClass())) return KismetFail(mod, "kismet_fire", "not a sequence event");
			Classes::UObject* who = instigator ? Obj(instigator) : game::Pawn();
			if (who && (!Alive(who) || !who->IsA(Classes::AActor::StaticClass()))) return KismetFail(mod, "kismet_fire", "the instigator isn't an actor");
			auto* ev = static_cast<Classes::USequenceEvent*>(e);
			int index = kismet::OutputIndex(ev, output);
			if (index < 0) return KismetFail(mod, "kismet_fire", fmt::format("{} has no output {}", ev->GetName(), output ? output : ""));
			auto* by = static_cast<Classes::AActor*>(who);
			// SequenceEvent's own CheckActivate (virtual, slot 0x180), not the event class's: a class adds its own
			// conditions (a touch event only tests for an instigator that isn't overlapping its trigger). The general
			// ones stay: enabled, MaxTriggerCount, ReTriggerDelay. Taken from the class default object's vtable.
			static void* base = nullptr;
			if (!base)
				if (auto* cdo = game::FindObject<Classes::UObject>("SequenceEvent Engine.Default__SequenceEvent"))
					base = (*reinterpret_cast<void***>(cdo))[0x180 / 4];
			if (!base) return KismetFail(mod, "kismet_fire", "SequenceEvent's defaults weren't found");
			// Which output: a list of one, on the stack (CheckActivate only reads it)
			struct { int* data; std::int32_t count, max; } indices{&index, 1, 1};
			using CheckFn = int(__thiscall*)(Classes::USequenceEvent*, Classes::AActor*, Classes::AActor*, int, void*, int);
			return reinterpret_cast<CheckFn>(base)(ev, ev->Originator, by, 0, &indices, 0) ? 1 : 0;
		}
		void MIRAGE_CALL KismetTrace(MirageMod* mod, int32_t on)
		{
			if (mod) kismet_trace::Enable(mod->id, on != 0);
		}
		uint32_t MIRAGE_CALL KismetTraceRead(MirageMod* mod, char* buffer, uint32_t capacity)
		{
			if (!mod) return 0;
			return kismet_trace::Read(mod->id, buffer, capacity);
		}

		int32_t MIRAGE_CALL MovementGet(MirageMovement* out)
		{
			if (!out || out->size < sizeof(MirageMovement)) return 0;
			const uint32_t size = out->size;
			*out = MirageMovement{};
			out->size = size;
			auto* pawn = static_cast<Classes::ATdPawn*>(game::Pawn());
			if (!pawn || !pawn->IsA(Classes::ATdPawn::StaticClass())) return 0;
			out->move = static_cast<int32_t>(pawn->MovementState.GetValue());
			out->previous_move = static_cast<int32_t>(pawn->OldMovementState.GetValue());
			out->physics = static_cast<int32_t>(pawn->Physics.GetValue());
			out->velocity[0] = pawn->Velocity.X;
			out->velocity[1] = pawn->Velocity.Y;
			out->velocity[2] = pawn->Velocity.Z;
			out->speed = std::sqrt(pawn->Velocity.X * pawn->Velocity.X + pawn->Velocity.Y * pawn->Velocity.Y);
			if (out->move >= 0 && out->move < static_cast<int32_t>(pawn->Moves.Num()))
				out->move_object = Handle(pawn->Moves[out->move]);
			return 1;
		}

		MirageMidHook* MIRAGE_CALL MidHookCreate(MirageMod* mod, void* at, MirageMidHookFn callback, void* user)
		{
			if (!mod || !at || !callback) return nullptr;
			auto* rec = new MidRecord{mod->id, callback, user, at};	// Never freed: a thread may be in its callback
			const std::string name = "mid hook of " + mod->id;
			if (const hooking::Status st = hooking::CreateMid(at, &MidEntry, rec, name); !st) {
				spdlog::error("[mod {}] mid_hook_create at {}: {}", mod->id, at, st.error);
				return nullptr;
			}
			mod->midHooks.push_back(at);
			spdlog::info("[mod {}] mid-function hook at {}{}", mod->id, at, MidPlace(at));
			return reinterpret_cast<MirageMidHook*>(rec);
		}

		void MIRAGE_CALL MidHookRemove(MirageMidHook* hook)
		{
			if (!hook) return;
			auto* rec = reinterpret_cast<MidRecord*>(hook);
			rec->off = true;
			hooking::Remove(rec->at);
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

		// ---- Reversible changes (step 15) -------------------------------------------------------------------------
		int32_t MIRAGE_CALL ChangeBegin(MirageMod* mod, const char* name)
		{
			if (!mod || !game::IsGameThread()) return 0;
			return changes::Begin(mod->id, name ? name : "");
		}
		void MIRAGE_CALL ChangeEnd(MirageMod*) { changes::End(); }
		int32_t MIRAGE_CALL ChangeRevert(MirageMod* mod, int32_t id)
		{
			if (!mod || !game::IsGameThread()) return 0;
			return changes::Revert(mod->id, id).restored;
		}
		uint32_t MIRAGE_CALL ChangeList(MirageMod* mod, char* buffer, uint32_t capacity)
		{
			return CopyOut(mod ? changes::ListJson(mod->id) : std::string("[]"), buffer, capacity);
		}

		// ---- World access (step 14) ------------------------------------------------------------------------------
		// A handle is the object plus what identifies it: its slot, its name and its class. A new object in a freed
		// slot can come at the same address, but the engine names objects uniquely (Class_N, counting up), so it
		// doesn't have the old one's name: the handle no longer matches.
		int32_t MIRAGE_CALL ObjectHandle(MirageObject* object, MirageHandle* out)
		{
			if (!out || out->size < sizeof(MirageHandle)) return 0;
			const uint32_t size = out->size;
			*out = MirageHandle{};
			out->size = size;
			Classes::UObject* o = Obj(object);
			std::size_t slot = 0;
			if (!o || !Classes::UObject::GObjects || !ReadIndex(o, &slot) || !game::AliveAt(o, slot)) return 0;
			out->object = object;
			out->slot = static_cast<uint32_t>(slot);
			out->name_index = o->Name.Index;
			out->name_number = o->Name.Number;
			out->object_class = Handle(o->Class);
			return 1;
		}

		MirageObject* MIRAGE_CALL HandleObject(const MirageHandle* h)
		{
			if (!h || h->size < sizeof(MirageHandle) || !h->object || !Classes::UObject::GObjects) return nullptr;
			Classes::UObject* o = Obj(h->object);
			if (!game::AliveAt(o, h->slot)) return nullptr;		// The slot holds this address (reads the slot only)
			if (o->Name.Index != h->name_index || o->Name.Number != h->name_number || Handle(o->Class) != h->object_class)
				return nullptr;		// Another object at the same address
			// A destroyed actor stays in the object list (bDeleteMe) until the next garbage collection: gone already
			if (game::IsA(o, Classes::AActor::StaticClass()) && static_cast<Classes::AActor*>(o)->bDeleteMe) return nullptr;
			return h->object;
		}

		int32_t MIRAGE_CALL ActorsQuery(const MirageActorQuery* q, MirageObject** out, int32_t capacity)
		{
			if (!q || q->size < sizeof(MirageActorQuery)) return -1;
			Classes::UClass* cls = Classes::AActor::StaticClass();
			if (q->class_name && *q->class_name) {
				cls = game::FindClass(q->class_name);
				if (!cls) return 0;
			}
			const bool byTag = q->tag && *q->tag, byName = q->name && *q->name;
			Classes::FVector center{q->x, q->y, q->z};
			if (q->radius > 0.f && q->near_object) {
				auto* around = static_cast<Classes::AActor*>(Obj(q->near_object));
				if (!Alive(around) || !game::IsA(around, Classes::AActor::StaticClass())) return 0;
				center = around->Location;
			}
			const float r2 = q->radius * q->radius;
			std::vector<std::pair<float, Classes::AActor*>> found;
			for (Classes::AActor* a : game::ActorsOf(cls)) {
				if (a->bDeleteMe) continue;
				float d2 = 0.f;
				if (q->radius > 0.f) {
					const float dx = a->Location.X - center.X, dy = a->Location.Y - center.Y, dz = a->Location.Z - center.Z;
					d2 = dx * dx + dy * dy + dz * dz;
					if (d2 > r2) continue;
				}
				if (byTag && a->Tag.GetName() != q->tag) continue;
				if (byName && a->GetName() != q->name) continue;
				found.emplace_back(d2, a);
			}
			if (q->radius > 0.f)
				std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			if (out)
				for (int32_t i = 0; i < capacity && i < static_cast<int32_t>(found.size()); ++i) out[i] = Handle(found[static_cast<std::size_t>(i)].second);
			return static_cast<int32_t>(found.size());
		}

		void WireChanges()
		{
			changes::Writer w;
			w.handle = &ObjectHandle;
			w.resolve = &HandleObject;
			w.set = &ObjectSet;
			w.name = &ObjectName;
			changes::SetWriter(w);
		}
	}
}

namespace mirage::mods
{
	void Share(const std::string& key, const std::string& value)
	{
		std::lock_guard lock(game_api::g_shareLock);
		game_api::g_shared["mirage|" + key] = value;
	}
}

namespace mirage::mods::ui
{
	bool MenuVisible() { return gui::visible; }
}

namespace mirage::mods
{
	namespace
	{
		void RemoveMidHooks(MirageMod& mod)
		{
			for (void* at : mod.midHooks) hooking::Remove(at);
			mod.midHooks.clear();
		}

		// Faults the hooks' guards caught since the last tick (detours: runtime/detour_guard.h; mid-function hooks:
		// game_api::MidEntry): the mod is stopped and named. Game thread.
		void CheckHookFaults()
		{
			for (const detours::Fault& f : detours::TakeFaults()) {
				const std::string target = symbols::NameOf(reinterpret_cast<std::uintptr_t>(f.target));
				detail::HookCrashed(f.owner, "its detour", target.empty() ? fmt::format("{}", f.target) : target, f.code,
									reinterpret_cast<const void*>(f.address));
			}
			std::vector<game_api::MidFault> mids;
			{
				std::lock_guard lock(game_api::g_midFaultLock);
				mids.swap(game_api::g_midFaults);
			}
			for (const auto& f : mids)
				detail::HookCrashed(f.rec->mod, "its mid-function hook", fmt::format("{}{}", f.rec->at, game_api::MidPlace(f.rec->at)),
									f.code, f.address);
		}
	}
}

namespace mirage::mods
{
	std::vector<TestResult> RunTests(const std::vector<std::string>& mods)
	{
		std::vector<game_api::TestRecord*> tests;
		{
			std::lock_guard lock(game_api::g_testLock);
			for (auto& t : game_api::g_tests)
				if (t->live && (mods.empty() || std::find(mods.begin(), mods.end(), t->mod->id) != mods.end()))
					tests.push_back(t.get());
		}
		std::vector<TestResult> out;
		for (game_api::TestRecord* t : tests) {
			struct Run
			{
				game_api::TestRecord* t;
				char message[1024];
				int32_t result;
			} run{t, {}, 0};
			TestResult r;
			r.mod = t->mod->id;
			r.name = t->name;
			const auto start = std::chrono::steady_clock::now();
			const bool ran = detail::Enter(*t->mod, "its test \"" + t->name + "\"", "its tests", [](void* p) {
				auto* x = static_cast<Run*>(p);
				x->result = x->t->fn(x->t->user, x->message, sizeof x->message);
			}, &run);
			r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			run.message[sizeof run.message - 1] = 0;
			r.ok = ran && run.result == 1;
			r.message = !ran ? std::string("it didn't run (the mod isn't running, or it crashed: see the log)")
							 : r.ok ? std::string() : (run.message[0] ? std::string(run.message) : std::string("it failed (no message)"));
			out.push_back(std::move(r));
		}
		return out;
	}
}

namespace mirage::mods
{
	namespace
	{
		// Its tests aren't run any more (the records stay: one may be running)
		void ForgetTests(MirageMod& mod)
		{
			std::lock_guard lock(game_api::g_testLock);
			for (auto& t : game_api::g_tests)
				if (t->mod == &mod) t->live = false;
		}
	}
}
