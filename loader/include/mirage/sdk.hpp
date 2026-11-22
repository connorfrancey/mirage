#pragma once
/** @file
 * @ingroup mirage_cpp_api
 * @brief Bridge from the native mod interface to the generated UE3 SDK.
 */
// The generated SDK (sdk/, `mirage sdk`) in a native mod: game objects as C++ types, read and written directly.
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
	/// @brief Initialize the mod's SDK globals from the loader if needed.
	/// @return True when both engine global tables are available.
	/// @note Game thread only.
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

	/// @brief Interpret a borrowed mod object handle as an SDK object.
	/// @tparam T SDK type the caller has verified for this object.
	/// @param o Borrowed engine object handle.
	/// @return SDK pointer, or null when the handle or SDK globals are unavailable.
	/// @warning No runtime type check is performed. Use only on the game thread.
	template <class T = Classes::UObject>
	T* As(MirageObject* o) { return Ready() ? reinterpret_cast<T*>(o) : nullptr; }

	/// @return The current player pawn, or null. Borrowed for this game tick.
	inline Classes::ATdPlayerPawn* Pawn() { return As<Classes::ATdPlayerPawn>(Api().player_pawn()); }
	/// @return The current controller, or null. Borrowed for this game tick.
	inline Classes::ATdPlayerController* Controller() { return As<Classes::ATdPlayerController>(Api().player_controller()); }
	/// @return The current world info, or null. Borrowed for this game tick.
	inline Classes::AWorldInfo* World() { return As<Classes::AWorldInfo>(Api().world_info()); }

	/// @return The persistent level's package name, or an empty string. Game thread only.
	inline std::string MapName()
	{
		Classes::UObject* o = World();
		if (!o) return {};
		for (int guard = 0; o->Outer && guard < 16; ++guard) o = o->Outer;
		return o->GetName();
	}

	inline bool IsA(Classes::UObject* o, const char* className) { return o && Api().object_is_a(reinterpret_cast<MirageObject*>(o), className); }
}
