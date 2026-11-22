#pragma once
/** @file
 * @ingroup mirage_hooking
 * @brief Process-wide registration and teardown of loader hooks.
 * @warning Never call under the loader lock, including from DllMain.
 */
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
	/// @ingroup mirage_hooking
	/// @brief Hook operation status and diagnostic on failure.
	struct Status
	{
		std::string error;
		bool alreadyHooked = false;		// The target has a hook of the loader's already
		explicit operator bool() const { return error.empty(); }
	};

	/// @brief Register a disabled hook and prepare its trampoline.
	/// @param target Function address to patch when enabled.
	/// @param detour Replacement function with a compatible calling convention.
	/// @param[out] original Storage for the trampoline to the original function.
	/// @param name Diagnostic name.
	/// @return Operation status. On success the trampoline is set before the hook can run.
	/// @see Install
	Status Create(void* target, void* detour, void** original, std::string_view name);
	/// @brief Activate the hook installed at a target.
	Status Enable(void* target);
	/// @brief Activate every target in one batch, or report failure without enabling any.
	Status Enable(std::span<void* const> targets);
	/// @brief Disable future entries into the hook installed at a target.
	Status Disable(void* target);
	/// @brief Disable and forget a hook.
	/// @note Trampoline code stays allocated because another thread may still be in the detour.
	Status Remove(void* target);
	/// @brief Create and enable a hook in one operation. A failed installation leaves no registered hook.
	Status Install(void* target, void* detour, void** original, std::string_view name);
	/// @return Whether this manager has a hook at the target address.
	bool Hooked(void* target);

	/// @brief Install an enabled mid-function hook.
	/// @note The callback runs on the calling thread and borrows its register context.
	/// @see Remove
	Status CreateMid(void* at, void (*callback)(void* context), void* user, std::string_view name);

	/// @brief Disable all hooks before in-flight calls drain.
	Status DisableAll();
	/// @brief Destroy the process-wide hook manager after teardown.
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
