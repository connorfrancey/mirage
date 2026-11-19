#include "game/object_ref.h"
#include "core/string_cache.h"
#include "runtime/symbols.h"

#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

namespace game 
{
	namespace 
	{
		std::mutex g_nameMutex;
		std::unordered_map<str::Atom, int> g_names;		// Interned: a lookup builds no std::string
		cache::Stats& g_nameStats = cache::Register("FName lookups", cache::Scope::Manual);

	}
	
	bool IsDefaultObject(Classes::UObject* object) 
	{
		return object->GetName().rfind("Default__", 0) == 0;
	}

	int NameIndex(const char* name)
	{
		const str::Atom key = str::Intern(name);
		if (opt::kEnabled)	// No optimizations: walk the name table on every call
		{
			std::lock_guard lock(g_nameMutex);
			if (auto it = g_names.find(key); it != g_names.end())
			{
				g_nameStats.hits.fetch_add(1, std::memory_order_relaxed);
				return it->second;
			}
		}

		// Walk the engine's name table once (outside the lock: it can take a while).
		g_nameStats.misses.fetch_add(1, std::memory_order_relaxed);
		const clk::Ticks t0 = clk::Now();
		// Compared in place (no string built per entry: that made a miss cost ~15 ms)
		const int found = Classes::UObject::FindNameIndex(name);
		g_nameStats.computeTicks.fetch_add(clk::Now() - t0, std::memory_order_relaxed);

		// Only remember hits: a name may be added to the table later (when its package loads).
		if (opt::kEnabled && found >= 0)
		{
			std::lock_guard lock(g_nameMutex);
			g_names.emplace(key, found);
		}
		return found;
	}

	Classes::FName Name(const char* name)
	{
		const int index = NameIndex(name);
		return Classes::FName(index < 0 ? 0 : index);
	}

	Classes::FName AddName(const char* name)
	{
		static_assert(sizeof(Classes::FName) == 8, "FName is an index and a number");
		using Fn = void*(__thiscall*)(void* self, const wchar_t* text, int findType, int unused);
		auto ctor = reinterpret_cast<Fn>(mirage::symbols::Find("FName::FName"));
		if (!ctor || !name) return Classes::FName(0);
		const std::wstring wide(name, name + std::strlen(name));		// Names are ASCII
		std::uint32_t out[2] = {0, 0};
		ctor(out, wide.c_str(), 1 /* FNAME_Add */, 1);
		Classes::FName n(0);
		std::memcpy(&n, out, sizeof n);
		return n;
	}
}