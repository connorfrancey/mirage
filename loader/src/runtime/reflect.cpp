#include "runtime/reflect.h"

#include "game/uobject.h"
#include "sdk/SDK.hpp"

#include <cstring>
#include <unordered_map>

namespace mirage::reflect
{
	namespace
	{
		// UProperty (checked by the self-test "Mirage layouts" against every property of the game)
		constexpr int kArrayDim = 0x44;
		constexpr int kElementSize = 0x48;
		constexpr int kFlags = 0x4C;
		constexpr int kOffset = 0x64;
		constexpr int kReference = 0x84;	// BitMask, Struct, PropertyClass, ...
		constexpr int kMetaClass = 0x88;	// ClassProperty: the class it is limited to

		// Members by name, looked up by a string_view of the path (no string made per lookup)
		struct NameHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
		};
		using Members = std::unordered_map<std::string, Member, NameHash, std::equal_to<>>;

		std::uint32_t Dword(const void* p, int at)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}

		// Members by type, built on first use. Game thread only, like everything here.
		std::unordered_map<const Classes::UStruct*, Members> g_types;

		const Members& MembersOf(Classes::UStruct* type)
		{
			if (auto it = g_types.find(type); it != g_types.end()) return it->second;
			Members members;
			int depth = 0;
			for (Classes::UStruct* s = type; s && depth < 64; s = game::SuperOf(s), ++depth) {
				int count = 0;
				for (Classes::UField* f = game::ChildrenOf(s); f && count < 4096; f = game::NextOf(f), ++count) {
					if (!f->Class) continue;
					const layout::Kind kind = layout::KindFromClass(f->Class->GetName());
					if (kind == layout::Kind::Unknown) continue;	// A function, a const, an enum, a state...
					Member m;
					m.name = f->GetName();
					m.kind = kind;
					m.offset = Dword(f, kOffset);
					m.elementSize = Dword(f, kElementSize);
					m.arrayDim = Dword(f, kArrayDim);
					std::memcpy(&m.flags, reinterpret_cast<const std::uint8_t*>(f) + kFlags, sizeof m.flags);
					switch (kind) {
						case layout::Kind::Array:
							if (auto* in = game::layout::PtrAt<Classes::UField>(f, kReference); in && in->Class) {
								auto e = std::make_shared<Member>();
								e->name = m.name;
								e->kind = layout::KindFromClass(in->Class->GetName());
								e->elementSize = Dword(in, kElementSize);
								e->arrayDim = 1;
								switch (e->kind) {
									case layout::Kind::Bool: e->boolMask = Dword(in, kReference); break;
									case layout::Kind::Struct: e->structType = game::layout::PtrAt<Classes::UStruct>(in, kReference); break;
									case layout::Kind::Object: case layout::Kind::Component: case layout::Kind::Interface:
										e->objectClass = game::layout::PtrAt<Classes::UClass>(in, kReference);
										break;
									case layout::Kind::Class: e->objectClass = game::layout::PtrAt<Classes::UClass>(in, kMetaClass); break;
									default: break;
								}
								m.inner = std::move(e);
							}
							break;
						case layout::Kind::Bool: m.boolMask = Dword(f, kReference); break;
						case layout::Kind::Struct: m.structType = game::layout::PtrAt<Classes::UStruct>(f, kReference); break;
						case layout::Kind::Object: case layout::Kind::Component: case layout::Kind::Interface:
							m.objectClass = game::layout::PtrAt<Classes::UClass>(f, kReference);
							break;
						case layout::Kind::Class: m.objectClass = game::layout::PtrAt<Classes::UClass>(f, kMetaClass); break;
						default: break;
					}
					members.emplace(m.name, std::move(m));	// The most derived declaration wins (emplace keeps it)
				}
			}
			return g_types.emplace(type, std::move(members)).first->second;
		}

		bool Is(const Value& v, std::initializer_list<layout::Kind> kinds)
		{
			if (!v) return false;
			for (layout::Kind k : kinds)
				if (v.member->kind == k) return true;
			return false;
		}
	}

	std::vector<const Member*> List(Classes::UStruct* type)
	{
		std::vector<const Member*> out;
		if (!type) return out;
		const auto& members = MembersOf(type);
		out.reserve(members.size());
		for (const auto& [name, m] : members) out.push_back(&m);
		return out;
	}

	const Member* Find(Classes::UStruct* type, std::string_view name)
	{
		if (!type) return nullptr;
		const auto& members = MembersOf(type);
		const auto it = members.find(name);
		return it == members.end() ? nullptr : &it->second;
	}

	// ---- The path cache ------------------------------------------------------------------------------------------------
	// A path is parsed once into segments ("Pawn", "Location"[-1], "Z"); each segment keeps a one-entry inline cache of
	// the last type it was looked up in and the member found there. Mods read the same paths on the same types every
	// tick, so after the first call a lookup is a pointer compare instead of a hash of the name. Game thread, like
	// g_types; cleared with it.
	namespace
	{
		struct Segment
		{
			std::string name;
			long index = -1;
			Classes::UStruct* type = nullptr;		// Inline cache: the type last looked up in...
			const Member* member = nullptr;			// ...and what was found there (nullptr: nothing)
		};
		struct Plan
		{
			std::vector<Segment> segments;
			std::string error;						// The path doesn't parse
		};
		std::unordered_map<std::string, Plan, NameHash, std::equal_to<>> g_plans;
		constexpr std::size_t kMaxPlans = 4096;		// Paths are a mod's constants: many more means made-up ones
		bool g_usePlans = true;
		std::uint64_t g_planHits = 0, g_planMisses = 0, g_inlineHits = 0;

		Plan Parse(std::string_view path)
		{
			Plan plan;
			while (true) {
				const auto dot = path.find('.');
				std::string_view part = path.substr(0, dot);
				Segment seg;
				if (const auto open = part.find('['); open != std::string_view::npos) {
					const auto close = part.find(']', open);
					if (close != part.size() - 1 || close == open + 1) {
						plan.error = "bad index in '" + std::string(part) + "'";
						return plan;
					}
					long index = 0;
					for (std::size_t i = open + 1; i < close; ++i) {
						if (part[i] < '0' || part[i] > '9' || index > 10'000'000) {
							plan.error = "bad index in '" + std::string(part) + "'";
							return plan;
						}
						index = index * 10 + (part[i] - '0');
					}
					seg.index = index;
					part = part.substr(0, open);
				}
				seg.name = std::string(part);
				plan.segments.push_back(std::move(seg));
				if (dot == std::string_view::npos) return plan;
				path.remove_prefix(dot + 1);
			}
		}

		// In front of the map: the last plan found for a string at this address (callers pass the same constant or Lua
		// string every tick). Verified by length and contents, so a buffer reused for another path is never mistaken.
		struct Slot
		{
			const char* data = nullptr;
			std::size_t size = 0;
			const std::string* key = nullptr;
			Plan* plan = nullptr;
		};
		Slot g_slots[64];

		Plan* PlanFor(std::string_view path)
		{
			Slot& slot = g_slots[((reinterpret_cast<std::uintptr_t>(path.data()) >> 3) ^ path.size()) & 63];
			if (slot.data == path.data() && slot.size == path.size() && slot.key &&
				std::memcmp(slot.key->data(), path.data(), path.size()) == 0) {
				++g_planHits;
				return slot.plan;
			}
			Plan* plan;
			const std::string* key;
			if (auto it = g_plans.find(path); it != g_plans.end()) {
				++g_planHits;
				plan = &it->second;
				key = &it->first;
			} else {
				++g_planMisses;
				if (g_plans.size() >= kMaxPlans) {		// Bounded: start over rather than grow
					g_plans.clear();
					for (Slot& s : g_slots) s = {};
				}
				auto added = g_plans.emplace(std::string(path), Parse(path)).first;
				plan = &added->second;
				key = &added->first;
			}
			slot = {path.data(), path.size(), key, plan};		// Map nodes don't move: the pointers stay valid
			return plan;
		}
	}

	// The path parsed on every call: the reference (and the fallback for a path the cache won't keep)
	Value ResolveUncached(Classes::UStruct* type, void* base, std::string_view path)
	{
		Value v;
		auto* address = static_cast<std::uint8_t*>(base);
		while (true) {
			const auto dot = path.find('.');
			std::string_view part = path.substr(0, dot);
			// "Name[3]": an element
			long index = -1;
			if (const auto open = part.find('['); open != std::string_view::npos) {
				const auto close = part.find(']', open);
				if (close != part.size() - 1 || close == open + 1) {
					v.error = "bad index in '" + std::string(part) + "'";
					return v;
				}
				index = 0;
				for (std::size_t i = open + 1; i < close; ++i) {
					if (part[i] < '0' || part[i] > '9' || index > 10'000'000) {
						v.error = "bad index in '" + std::string(part) + "'";
						return v;
					}
					index = index * 10 + (part[i] - '0');
				}
				part = part.substr(0, open);
			}
			if (!type || !address) {
				v.error = "nothing to look '" + std::string(part) + "' up in";
				return v;
			}
			const Member* m = Find(type, part);
			if (!m) {
				v.error = "no member '" + std::string(part) + "'";
				return v;
			}
			address += m->offset;
			if (index >= 0) {
				if (m->kind == layout::Kind::Array) {
					ArrayHeader a{};
					std::memcpy(&a, address, sizeof a);
					if (!m->inner || index >= a.count || !a.data) {
						v.error = "'" + std::string(part) + "' has no element " + std::to_string(index);
						return v;
					}
					address = a.data + static_cast<std::size_t>(index) * m->inner->elementSize;
					m = m->inner.get();
				} else if (static_cast<std::uint32_t>(index) < m->arrayDim) {
					address += static_cast<std::size_t>(index) * m->elementSize;
				} else {
					v.error = "'" + std::string(part) + "' has no element " + std::to_string(index);
					return v;
				}
			}
			if (dot == std::string_view::npos) {
				v.member = m;
				v.address = address;
				return v;
			}
			if (m->kind == layout::Kind::Struct) {
				type = m->structType;
			} else if (m->kind == layout::Kind::Object || m->kind == layout::Kind::Component) {
				// Through an object reference: its own properties
				Classes::UObject* o = nullptr;
				std::memcpy(&o, address, sizeof o);
				if (!o) {
					v.error = "'" + std::string(part) + "' is none";
					return v;
				}
				type = o->Class;
				address = reinterpret_cast<std::uint8_t*>(o);
			} else {
				v.error = "'" + std::string(part) + "' has no members";
				return v;
			}
			path.remove_prefix(dot + 1);
		}
	}

	// The same walk as ResolveUncached, over a cached plan
	Value Resolve(Classes::UStruct* type, void* base, std::string_view path)
	{
		if (!g_usePlans) return ResolveUncached(type, base, path);
		Plan* plan = PlanFor(path);
		Value v;
		if (!plan->error.empty()) {
			v.error = plan->error;
			return v;
		}
		auto* address = static_cast<std::uint8_t*>(base);
		const std::size_t n = plan->segments.size();
		for (std::size_t i = 0; i < n; ++i) {
			Segment& seg = plan->segments[i];
			if (!type || !address) {
				v.error = "nothing to look '" + seg.name + "' up in";
				return v;
			}
			const Member* m;
			if (seg.type == type) {
				m = seg.member;
				++g_inlineHits;
			} else {
				m = Find(type, seg.name);
				seg.type = type;
				seg.member = m;
			}
			if (!m) {
				v.error = "no member '" + seg.name + "'";
				return v;
			}
			address += m->offset;
			if (seg.index >= 0) {
				const long index = seg.index;
				if (m->kind == layout::Kind::Array) {
					ArrayHeader a{};
					std::memcpy(&a, address, sizeof a);
					if (!m->inner || index >= a.count || !a.data) {
						v.error = "'" + seg.name + "' has no element " + std::to_string(index);
						return v;
					}
					address = a.data + static_cast<std::size_t>(index) * m->inner->elementSize;
					m = m->inner.get();
				} else if (static_cast<std::uint32_t>(index) < m->arrayDim) {
					address += static_cast<std::size_t>(index) * m->elementSize;
				} else {
					v.error = "'" + seg.name + "' has no element " + std::to_string(index);
					return v;
				}
			}
			if (i + 1 == n) {
				v.member = m;
				v.address = address;
				return v;
			}
			if (m->kind == layout::Kind::Struct) {
				type = m->structType;
			} else if (m->kind == layout::Kind::Object || m->kind == layout::Kind::Component) {
				Classes::UObject* o = nullptr;
				std::memcpy(&o, address, sizeof o);
				if (!o) {
					v.error = "'" + seg.name + "' is none";
					return v;
				}
				type = o->Class;
				address = reinterpret_cast<std::uint8_t*>(o);
			} else {
				v.error = "'" + seg.name + "' has no members";
				return v;
			}
		}
		return v;
	}

	void SetPathCache(bool on)
	{
		g_usePlans = on;
		if (!on) {
			g_plans.clear();
			for (Slot& slot : g_slots) slot = {};
		}
	}

	PathCacheStats GetPathCacheStats() { return {g_plans.size(), g_planHits, g_planMisses, g_inlineHits}; }

	std::int32_t GetInt(const Value& v)
	{
		if (Is(v, {layout::Kind::Int})) {
			std::int32_t x;
			std::memcpy(&x, v.address, sizeof x);
			return x;
		}
		if (Is(v, {layout::Kind::Byte})) return *v.address;
		if (Is(v, {layout::Kind::Bool})) return (Dword(v.address, 0) & v.member->boolMask) ? 1 : 0;
		if (Is(v, {layout::Kind::Float})) return static_cast<std::int32_t>(GetFloat(v));
		return 0;
	}

	float GetFloat(const Value& v)
	{
		if (Is(v, {layout::Kind::Float})) {
			float x;
			std::memcpy(&x, v.address, sizeof x);
			return x;
		}
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return static_cast<float>(GetInt(v));
		return 0.f;
	}

	Classes::UObject* GetObj(const Value& v)
	{
		if (!Is(v, {layout::Kind::Object, layout::Kind::Class, layout::Kind::Component, layout::Kind::Interface})) return nullptr;
		Classes::UObject* o = nullptr;
		std::memcpy(&o, v.address, sizeof o);
		return o;
	}

	std::string GetText(const Value& v)
	{
		if (Is(v, {layout::Kind::Name})) return reinterpret_cast<const Classes::FName*>(v.address)->GetName();
		if (Is(v, {layout::Kind::Str})) {
			const auto* s = reinterpret_cast<const Classes::FString*>(v.address);
			return s->IsValid() ? s->ToString() : std::string();
		}
		if (Is(v, {layout::Kind::Float})) return std::to_string(GetFloat(v));
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return std::to_string(GetInt(v));
		if (Classes::UObject* o = GetObj(v)) return NameOf(o);
		return {};
	}

	bool SetInt(const Value& v, std::int32_t x)
	{
		if (Is(v, {layout::Kind::Int})) {
			std::memcpy(v.address, &x, sizeof x);
			return true;
		}
		if (Is(v, {layout::Kind::Byte})) {
			*v.address = static_cast<std::uint8_t>(x);
			return true;
		}
		if (Is(v, {layout::Kind::Bool})) {
			std::uint32_t d = Dword(v.address, 0);
			d = x ? (d | v.member->boolMask) : (d & ~v.member->boolMask);
			std::memcpy(v.address, &d, sizeof d);
			return true;
		}
		if (Is(v, {layout::Kind::Float})) return SetFloat(v, static_cast<float>(x));
		return false;
	}

	bool SetFloat(const Value& v, float x)
	{
		if (Is(v, {layout::Kind::Float})) {
			std::memcpy(v.address, &x, sizeof x);
			return true;
		}
		if (Is(v, {layout::Kind::Int, layout::Kind::Byte, layout::Kind::Bool})) return SetInt(v, static_cast<std::int32_t>(x));
		return false;
	}

	bool SetObj(const Value& v, Classes::UObject* x)
	{
		if (!Is(v, {layout::Kind::Object, layout::Kind::Class, layout::Kind::Component})) return false;
		std::memcpy(v.address, &x, sizeof x);
		return true;
	}

	bool SetText(const Value& v, std::string_view x)
	{
		if (!Is(v, {layout::Kind::Name})) return false;
		const Classes::FName name(std::string(x).c_str());
		if (name.Index == 0 && x != "None") return false;		// Not a name the engine has
		std::memcpy(v.address, &name, sizeof name);
		return true;
	}

	std::int32_t ArrayCount(const Value& v)
	{
		if (!v) return 0;
		if (v.member->kind == layout::Kind::Array) {
			ArrayHeader a{};
			std::memcpy(&a, v.address, sizeof a);
			return a.count;
		}
		return v.member->arrayDim > 1 ? static_cast<std::int32_t>(v.member->arrayDim) : 0;
	}

	void ClearCache()
	{
		g_types.clear();
		g_plans.clear();		// Their inline caches point at members of the types just forgotten
		for (Slot& slot : g_slots) slot = {};
	}

	std::string NameOf(const Classes::UObject* object) { return object ? object->GetName() : std::string("None"); }
	std::string ClassNameOf(const Classes::UObject* object)
	{
		return object && object->Class ? object->Class->GetName() : std::string();
	}
}
