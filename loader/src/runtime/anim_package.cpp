// An AnimSet read from the package files (UnrealLib), for comparing with what the engine holds (anim.h)
#include "runtime/anim.h"

#include "runtime/packages.h"

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <variant>

namespace mirage::anim
{
	int ReadSetSequences(const std::string& path, std::string* error)
	{
		using namespace UnrealLib;
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return -1;
		};
		const std::size_t dot = path.find('.');
		if (dot == std::string::npos) return fail("not a path: " + path);
		const std::string package = path.substr(0, dot), inner = path.substr(dot + 1);
		try {
			Linker linker;
			linker.add_search_path(packages::CookedDir(), true);
			ReflectionSchema schema(linker);
			for (const char* name : {"Core", "Engine", "GameFramework", "TdGame"})		// AS_* are TdAnimSets
				if (const Package* s = linker.find_package(name)) schema.add_package(*s);
			const Package* p = linker.find_package(package);
			if (!p) return fail(package + " wasn't found under " + packages::CookedDir().string());
			const auto e = p->find_export(inner);
			if (!e) return fail(inner + " isn't an export of " + package);
			const ObjectProperties props = linker.read_properties({p, *e}, &schema);
			for (const Property& prop : props.properties)
				if (prop.name == "Sequences")
					if (const auto* arr = std::get_if<ArrayValue>(&prop.value.value)) return static_cast<int>(arr->elements.size());
			return fail("no Sequences property in " + path);
		} catch (const std::exception& ex) {
			return fail(ex.what());
		}
	}
}
