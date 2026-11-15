#pragma once
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// Every inline hook the loader makes goes through one MirageHook manager: one per process, because managers don't
// coordinate their suspensions of the other threads. Hooks are known by their target address, so a call site creates,
// enables, disables and removes a hook by the function it hooks.
//
// A hook is made disabled, with the pointer to the original function set before it can run; enabling it writes the
// jump while the other threads are suspended. Nothing here may run inside DllMain (MirageHook refuses under the loader
// lock): the loader hooks from its own start-up thread and the game thread.
namespace hooking
{
	// The outcome of a call: true when it worked, otherwise why not.
	struct Status
	{
		std::string error;
		bool alreadyHooked = false;		// The target has a hook of the loader's already
		explicit operator bool() const { return error.empty(); }
	};

	// A disabled hook on `target` that runs `detour`; *original is set to the way to the real function before this returns.
	Status Create(void* target, void* detour, void** original, std::string_view name);
	Status Enable(void* target);
	// Every one of `targets` enabled with one suspension of the other threads, or none of them.
	Status Enable(std::span<void* const> targets);
	Status Disable(void* target);
	// Disabled and forgotten. The code that calls the original stays allocated: a thread may still be inside the detour.
	Status Remove(void* target);
	// Create and Enable; nothing is left behind when either fails.
	Status Install(void* target, void* detour, void** original, std::string_view name);
	bool Hooked(void* target);

	// A mid-function hook at `at`: `callback(context)` runs there with the registers (MirageHook's MidHookContext,
	// passed as void*: core doesn't need its header), enabled at once. Removed with Remove(at).
	Status CreateMid(void* at, void (*callback)(void* context), void* user, std::string_view name);

	// Every hook disabled (at unload, before in-flight calls drain), then all of them removed with the manager.
	Status DisableAll();
	void Shutdown();

	// The same with function pointers: `hooking::Install(target, &MyDetour, &oOriginal, "Name")`. The two types may
	// differ: a __fastcall detour often stands in for a __thiscall original.
	template <class Detour, class Fn>
		requires std::is_pointer_v<Detour> && std::is_pointer_v<Fn>
	Status Create(void* target, Detour detour, Fn* original, std::string_view name)
	{
		return Create(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
	}
	template <class Detour, class Fn>
		requires std::is_pointer_v<Detour> && std::is_pointer_v<Fn>
	Status Install(void* target, Detour detour, Fn* original, std::string_view name)
	{
		return Install(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
	}
}
