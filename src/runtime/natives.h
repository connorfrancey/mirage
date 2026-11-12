#pragma once
// Mod natives (Mirage, step 11): a mod's function registered as an engine native, so UnrealScript bytecode (a script
// patch of step 8, or step 12's new classes) calls it like engine code: `NativeFunction <index>` with its arguments.
//
// Each native gets a free slot of the engine's native table (GNatives, runtime/inject.h) and a thunk there that reads
// its arguments from the caller's bytecode by the types of its signature ("float(float,int)"), calls the mod and writes
// the return value where the VM wants it. A mod registering the same name again (a hot reload) gets the same index
// back. When the mod is gone its natives still read their arguments and return zero: bytecode that still calls one
// never reaches the engine's "undefined native" error.
//
// Types: int, float, bool, byte, object; name as an argument (it arrives as text); void as the return type only.

#include "mirage/api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Classes
{
	class UObject;
}

namespace mirage::natives
{
	// One call of a mod native: what its callback sees (MirageNativeCall in the API)
	struct Call
	{
		Classes::UObject* self = nullptr;		// The object the calling script runs on
		std::vector<MirageValue> args;
		MirageValue result{};					// Set by the callback; converted to the return type
	};
	using Callback = void (*)(void* user, Call& call);

	// `owner` + `name` identify it (the same pair again keeps its index; its signature can't change until a restart).
	// Returns the native index for bytecode (> 0), 0 and `error` when it can't be registered.
	int Register(const std::string& owner, const std::string& name, const std::string& signature, Callback callback,
				 void* user, std::string* error = nullptr);
	// The owner's natives stop calling it (they keep reading their arguments and return zero)
	void Release(const std::string& owner);

	struct Info
	{
		std::string owner, name, signature;
		int index = 0;
		bool live = false;		// Its mod's callback is there
		std::uint64_t calls = 0;
	};
	std::vector<Info> List();
}
