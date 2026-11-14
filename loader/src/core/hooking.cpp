#include "core/hooking.h"

#include <MirageHook/core/HookManager.hpp>

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hooking
{
	namespace
	{
		std::mutex g_lock;
		// Made on first use and deleted by Shutdown. Never destroyed at process exit: that runs under the loader lock,
		// where removing hooks (which suspends threads) is refused.
		MirageHook::HookManager* g_manager = nullptr;
		std::unordered_map<void*, MirageHook::Hook> g_hooks;

		MirageHook::HookManager& Manager()
		{
			if (!g_manager) g_manager = new MirageHook::HookManager();
			return *g_manager;
		}

		Status Failed(const MirageHook::Error& e)
		{
			return {e.message(), e.code() == MirageHook::ErrorCode::AlreadyInstalled};
		}

		Status NotHooked() { return {"that function isn't hooked"}; }

		Status CreateLocked(void* target, void* detour, void** original, std::string_view name)
		{
			if (!target || !detour || !original) return {"a target, a detour and a place for the original are needed"};
			if (g_hooks.contains(target)) return {"the function is hooked already", true};
			auto hook = Manager().create_inline(MirageHook::Address(target), MirageHook::Address(detour),
												{.name = std::string(name)});
			if (!hook) return Failed(hook.error());
			*original = hook->original_address().as<void*>();
			g_hooks.emplace(target, std::move(*hook));
			return {};
		}

		Status RemoveLocked(void* target)
		{
			const auto it = g_hooks.find(target);
			if (it == g_hooks.end()) return NotHooked();
			auto removed = it->second.remove();
			g_hooks.erase(it);
			return removed ? Status{} : Failed(removed.error());
		}
	}

	Status Create(void* target, void* detour, void** original, std::string_view name)
	{
		std::lock_guard lock(g_lock);
		return CreateLocked(target, detour, original, name);
	}

	Status Enable(void* target)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_hooks.find(target);
		if (it == g_hooks.end()) return NotHooked();
		auto enabled = it->second.enable();
		return enabled ? Status{} : Failed(enabled.error());
	}

	Status Enable(std::span<void* const> targets)
	{
		std::lock_guard lock(g_lock);
		MirageHook::HookTransaction tx(Manager());
		for (void* target : targets) {
			const auto it = g_hooks.find(target);
			if (it == g_hooks.end()) return NotHooked();
			tx.enable(it->second);
		}
		auto committed = tx.commit();
		return committed ? Status{} : Failed(committed.error());
	}

	Status Disable(void* target)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_hooks.find(target);
		if (it == g_hooks.end()) return NotHooked();
		auto disabled = it->second.disable();
		return disabled ? Status{} : Failed(disabled.error());
	}

	Status Remove(void* target)
	{
		std::lock_guard lock(g_lock);
		return RemoveLocked(target);
	}

	Status Install(void* target, void* detour, void** original, std::string_view name)
	{
		std::lock_guard lock(g_lock);
		if (Status created = CreateLocked(target, detour, original, name); !created) return created;
		auto enabled = g_hooks.at(target).enable();
		if (enabled) return {};
		Status why = Failed(enabled.error());
		RemoveLocked(target);
		*original = nullptr;
		return why;
	}

	bool Hooked(void* target)
	{
		std::lock_guard lock(g_lock);
		return g_hooks.contains(target);
	}

	Status DisableAll()
	{
		std::lock_guard lock(g_lock);
		if (!g_manager) return {};
		auto disabled = g_manager->disable_all();
		return disabled ? Status{} : Failed(disabled.error());
	}

	void Shutdown()
	{
		std::lock_guard lock(g_lock);
		g_hooks.clear();		// Each handle removes its hook
		delete g_manager;
		g_manager = nullptr;
	}
}
