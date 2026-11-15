#pragma once
// What level layers (runtime/layers) and Kismet layers (runtime/kismet_layers) share: a class named in a layer, and a
// property set from a TOML value. Game thread.

#include <toml.hpp>

#include <string>

namespace Classes
{
	class UClass;
	class UObject;
}
namespace mirage::mods
{
	struct Info;
}

namespace mirage::layers
{
	// A mod's .uc file ("classes/MyPad.uc": compiled on a worker, `pending` true until it is loaded: ask again on a
	// later tick) or a class path ("Engine.Trigger"), loaded if its package isn't yet. nullptr and `why` when there's none.
	Classes::UClass* ClassFor(const mods::Info& mod, const std::string& name, std::string* why, bool* pending);
	// A property by path from a TOML value: numbers, booleans, names (strings), objects ("Package.Object", "None"),
	// vectors and rotators ([x, y, z])
	bool SetValue(Classes::UObject* o, const std::string& path, const toml::value& v, std::string* why);
}
