// The levels' Kismet read from the package files (UnrealLib), for comparing with what the engine holds (kismet.h)
#include "runtime/kismet.h"

#include "runtime/packages.h"

#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>

#include <map>
#include <variant>

namespace mirage::kismet
{
	namespace
	{
		using namespace UnrealLib;

		const Property* Field(const std::vector<Property>& list, std::string_view name)
		{
			for (const Property& p : list)
				if (p.name == name) return &p;
			return nullptr;
		}
		template <class T> const T* As(const Property* p) { return p ? std::get_if<T>(&p->value.value) : nullptr; }
	}

	std::vector<Op> ReadPackage(const std::string& level, std::string* error)
	{
		std::vector<Op> out;
		try {
			Linker linker;
			linker.add_search_path(packages::CookedDir(), true);
			ReflectionSchema schema(linker);
			for (const char* name : {"Core", "Engine", "GameFramework", "TdGame"})
				if (const Package* s = linker.find_package(name)) schema.add_package(*s);
			const Package* p = linker.find_package(level);
			if (!p) {
				if (error) *error = level + " wasn't found under " + packages::CookedDir().string();
				return out;
			}
			// Paths as the engine names them: the package first
			const auto pathOf = [&](ObjectIndex i) {
				std::string path = p->object_path(i);
				const std::string prefix = p->name() + ".";
				return path.size() >= prefix.size() && _strnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0 ? path : prefix + path;
			};
			for (std::size_t e = 0; e < p->exports().size(); ++e) {
				const ObjectIndex index = ObjectIndex::from_export(e);
				const ObjectRef ref{p, index};
				if (p->object_name(index).starts_with("Default__") || linker.is_a(ref, "Engine.SequenceObject") != true) continue;
				Op op;
				op.path = pathOf(index);
				op.name = p->object_name(index);
				op.cls = p->class_name(index);
				if (linker.is_a(ref, "Engine.SequenceOp") == true) {
					const ObjectProperties props = linker.read_properties(ref, &schema);
					if (const auto* outputs = As<ArrayValue>(Field(props.properties, "OutputLinks")))
						for (const PropertyValue& v : outputs->elements) {
							Output o;
							const auto* link = std::get_if<StructValue>(&v.value);
							if (link)
								if (const auto* links = As<ArrayValue>(Field(link->fields, "Links")))
									for (const PropertyValue& l : links->elements) {
										const auto* s = std::get_if<StructValue>(&l.value);
										const auto* to = s ? As<ObjectValue>(Field(s->fields, "LinkedOp")) : nullptr;
										if (!to || to->value.is_null()) continue;
										const auto* input = As<std::int32_t>(Field(s->fields, "InputLinkIdx"));
										o.links.push_back({pathOf(to->value), input ? *input : 0});
									}
							op.outputs.push_back(std::move(o));
						}
				}
				out.push_back(std::move(op));
			}
		} catch (const std::exception& e) {
			if (error) *error = e.what();
			out.clear();
		}
		return out;
	}
}
