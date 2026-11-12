#pragma once
// The generated SDK (sdk/, `memenu sdk`) in a native mod: game objects as C++ types, read and written directly.
//
//     #include <mirage/sdk.hpp>          // links mirage::sdk
//     if (auto* pawn = mirage::sdk::Pawn()) speed = pawn->Velocity.X;
//
// The SDK's globals are set from the runtime (MirageApi::sdk_globals) on first use. Game thread only, like every game
// object; a pointer is good for this tick (look it up again next tick). Calls of script functions go through the
// object's ProcessEvent (the SDK's wrappers), or through MirageApi::call_begin.
#include "mirage/mirage.hpp"

#include <sdk/SDK.hpp>

#include <string>

namespace mirage::sdk
{
	inline bool Ready()
	{
		if (Classes::UObject::GObjects && Classes::FName::GNames) return true;
		void* names = nullptr;
		void* objects = nullptr;
		Api().sdk_globals(&names, &objects);
		Classes::FName::GNames = static_cast<decltype(Classes::FName::GNames)>(names);
		Classes::UObject::GObjects = static_cast<decltype(Classes::UObject::GObjects)>(objects);
		return names && objects;
	}

	template <class T = Classes::UObject>
	T* As(MirageObject* o) { return Ready() ? reinterpret_cast<T*>(o) : nullptr; }

	inline Classes::ATdPlayerPawn* Pawn() { return As<Classes::ATdPlayerPawn>(Api().player_pawn()); }
	inline Classes::ATdPlayerController* Controller() { return As<Classes::ATdPlayerController>(Api().player_controller()); }
	inline Classes::AWorldInfo* World() { return As<Classes::AWorldInfo>(Api().world_info()); }

	// The level the player is in: the persistent level's package ("Edge_p"), "" in none
	inline std::string MapName()
	{
		Classes::UObject* o = World();
		if (!o) return {};
		for (int guard = 0; o->Outer && guard < 16; ++guard) o = o->Outer;
		return o->GetName();
	}

	inline bool IsA(Classes::UObject* o, const char* className) { return o && Api().object_is_a(reinterpret_cast<MirageObject*>(o), className); }
}
