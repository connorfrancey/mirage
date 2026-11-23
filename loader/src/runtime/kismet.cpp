#include "runtime/kismet.h"

#include "game/game.h"
#include "runtime/classes.h"
#include "runtime/kismet_trace.h"
#include "runtime/maps.h"
#include "runtime/symbols.h"
#include "sdk/SDK.hpp"

#include "util/glz_compat.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The JSON names where they differ from the members: an op's class is "class", a variable link's paths "linked", and
// the kind is written as its name. Write only: mods read this, nothing reads it back.
template <>
struct glz::meta<mirage::kismet::VariableLink>
{
	using T = mirage::kismet::VariableLink;
	static constexpr auto value = object("desc", &T::desc, "linked", &T::variables);
};
template <>
struct glz::meta<mirage::kismet::Op>
{
	using T = mirage::kismet::Op;
	static constexpr auto value = object("path", &T::path, "name", &T::name, "class", &T::cls,
		"kind", [](const T& op) { return std::string_view(mirage::kismet::KindName(op.kind)); },
		"title", &T::title, "comment", &T::comment, "active", &T::active, "activations", &T::activations, "x", &T::x,
		"y", &T::y, "inputs", &T::inputs, "outputs", &T::outputs, "variables", &T::variables, "originator", &T::originator);
};

namespace mirage::kismet
{
	namespace
	{
		std::string Narrow(const Classes::FString& s)
		{
			std::string out;
			if (!s.IsValid()) return out;
			for (const wchar_t* c = s.c_str(); c && *c; ++c) out += *c < 0x80 ? static_cast<char>(*c) : '?';
			return out;
		}

		std::string PathOf(const Classes::UObject* o) { return o ? o->GetPathName() : std::string(); }

		bool IsDefault(const Classes::UObject* o)
		{
			return (*reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const char*>(o) + 8) & 0x600) != 0;
		}

		// The package a level's object is in: the outermost outer
		std::string LevelOf(const Classes::UObject* o)
		{
			while (o && o->Outer) o = o->Outer;
			return o ? o->GetName() : std::string();
		}

		// In a level: one of its outers is the level's world ("TheWorld")
		bool InWorld(const Classes::UObject* o)
		{
			for (; o; o = o->Outer)
				if (o->Class && o->Class->GetName() == "World") return true;
			return false;
		}

		Op ReadOp(Classes::USequenceObject* so)
		{
			Op op;
			op.path = PathOf(so);
			op.name = so->GetName();
			op.cls = so->Class ? so->Class->GetName() : std::string();
			op.title = Narrow(so->ObjName);
			op.comment = Narrow(so->ObjComment);
			op.x = so->ObjPosX;
			op.y = so->ObjPosY;
			if (so->IsA(Classes::USequenceVariable::StaticClass())) {
				op.kind = Op::Kind::Variable;
				return op;
			}
			if (!so->IsA(Classes::USequenceOp::StaticClass())) return op;
			auto* o = static_cast<Classes::USequenceOp*>(so);
			op.kind = o->IsA(Classes::USequence::StaticClass())          ? Op::Kind::Sequence
					: o->IsA(Classes::USequenceEvent::StaticClass())     ? Op::Kind::Event
					: o->IsA(Classes::USequenceCondition::StaticClass()) ? Op::Kind::Condition
					: o->IsA(Classes::USequenceAction::StaticClass())    ? Op::Kind::Action
																		 : Op::Kind::Other;
			op.active = o->bActive;
			op.activations = o->ActivateCount;
			for (std::size_t i = 0; i < o->InputLinks.Num(); ++i) op.inputs.push_back(Narrow(o->InputLinks[i].LinkDesc));
			for (std::size_t i = 0; i < o->OutputLinks.Num(); ++i) {
				const auto& out = o->OutputLinks[i];
				Output x;
				x.desc = Narrow(out.LinkDesc);
				for (std::size_t k = 0; k < out.Links.Num(); ++k)
					if (out.Links[k].LinkedOp) x.links.push_back({PathOf(out.Links[k].LinkedOp), out.Links[k].InputLinkIdx});
				op.outputs.push_back(std::move(x));
			}
			for (std::size_t i = 0; i < o->VariableLinks.Num(); ++i) {
				const auto& v = o->VariableLinks[i];
				VariableLink x;
				x.desc = Narrow(v.LinkDesc);
				for (std::size_t k = 0; k < v.LinkedVariables.Num(); ++k)
					if (v.LinkedVariables[k]) x.variables.push_back(PathOf(v.LinkedVariables[k]));
				op.variables.push_back(std::move(x));
			}
			if (op.kind == Op::Kind::Event) op.originator = PathOf(static_cast<Classes::USequenceEvent*>(o)->Originator);
			return op;
		}
	}

	const char* KindName(Op::Kind kind)
	{
		switch (kind) {
			case Op::Kind::Event: return "event";
			case Op::Kind::Action: return "action";
			case Op::Kind::Condition: return "condition";
			case Op::Kind::Variable: return "variable";
			case Op::Kind::Sequence: return "sequence";
			default: return "other";
		}
	}

	Sequence ReadSequence(Classes::USequence* seq, bool withOps)
	{
		Sequence s;
		s.path = PathOf(seq);
		s.name = seq->GetName();
		s.level = LevelOf(seq);
		s.parent = PathOf(seq->ParentSequence);
		s.enabled = seq->bEnabled;
		if (withOps)
			for (std::size_t k = 0; k < seq->SequenceObjects.Num(); ++k)
				if (auto* so = seq->SequenceObjects[k]) s.ops.push_back(ReadOp(so));
		return s;
	}

	std::vector<Classes::USequence*> Sequences()
	{
		std::vector<Classes::USequence*> out;
		Classes::UClass* sequenceClass = Classes::USequence::StaticClass();
		const auto& objects = Classes::UObject::GetGlobalObjects();
		for (std::size_t i = 0; i < objects.Num(); ++i) {
			auto* o = objects.GetByIndex(i);
			if (o && !IsDefault(o) && o->IsA(sequenceClass) && InWorld(o)) out.push_back(static_cast<Classes::USequence*>(o));
		}
		return out;
	}

	std::vector<Sequence> Read()
	{
		std::vector<Sequence> out;
		Classes::UClass* sequenceClass = Classes::USequence::StaticClass();
		const auto& objects = Classes::UObject::GetGlobalObjects();
		for (std::size_t i = 0; i < objects.Num(); ++i) {
			auto* o = objects.GetByIndex(i);
			if (!o || IsDefault(o) || !o->IsA(sequenceClass)) continue;
			auto* seq = static_cast<Classes::USequence*>(o);
			if (!InWorld(seq)) continue;		// A level's, not the UI scenes' or a script package's templates
			out.push_back(ReadSequence(seq, true));
		}
		std::stable_sort(out.begin(), out.end(), [](const Sequence& a, const Sequence& b) { return a.parent.empty() && !b.parent.empty(); });
		return out;
	}

	std::vector<Classes::USequenceObject*> LevelObjects()
	{
		std::vector<Classes::USequenceObject*> out;
		Classes::UClass* cls = Classes::USequenceObject::StaticClass();
		const auto& objects = Classes::UObject::GetGlobalObjects();
		for (std::size_t i = 0; i < objects.Num(); ++i) {
			auto* o = objects.GetByIndex(i);
			if (o && !IsDefault(o) && o->IsA(cls) && InWorld(o)) out.push_back(static_cast<Classes::USequenceObject*>(o));
		}
		return out;
	}

	std::string LevelOf(const Classes::USequenceObject* o) { return LevelOf(static_cast<const Classes::UObject*>(o)); }

	std::string ToJson(const std::vector<Sequence>& sequences) { return jc::dump(sequences); }

	// ---- Editing -------------------------------------------------------------------------------------------------------
	namespace
	{
		struct RawArray
		{
			std::uint8_t* data;
			std::int32_t count, max;
		};
		static_assert(sizeof(RawArray) == 12);
		template <class T> RawArray& Raw(Classes::TArray<T>& a) { return reinterpret_cast<RawArray&>(a); }

		// Room for one more element of `size` bytes (GMalloc's Realloc: the engine frees and grows these arrays)
		bool Reserve(RawArray& a, std::size_t size)
		{
			if (a.count < a.max) return true;
			const std::int32_t grown = a.max < 4 ? 4 : a.max * 2;
			void* p = maps::EngineRealloc(a.data, grown * size);
			if (!p) return false;
			a.data = static_cast<std::uint8_t*>(p);
			a.max = grown;
			return true;
		}
		bool Insert(RawArray& a, std::size_t size, int at, const void* element)
		{
			if (!Reserve(a, size)) return false;
			if (at < 0 || at > a.count) at = a.count;
			std::memmove(a.data + (at + 1) * size, a.data + at * size, (a.count - at) * size);
			std::memcpy(a.data + at * size, element, size);
			++a.count;
			return true;
		}
		void Erase(RawArray& a, std::size_t size, int at)
		{
			std::memmove(a.data + at * size, a.data + (at + 1) * size, (a.count - at - 1) * size);
			--a.count;
		}

		template <class DescOf> int IndexOf(const char* which, int count, DescOf descOf)
		{
			if (!which || !*which) return count > 0 ? 0 : -1;
			char* end = nullptr;
			const long n = std::strtol(which, &end, 10);
			if (end != which && *end == '\0') return n >= 0 && n < count ? static_cast<int>(n) : -1;
			for (int i = 0; i < count; ++i)
				if (_stricmp(descOf(i).c_str(), which) == 0) return i;
			return -1;
		}

		bool Derives(const Classes::UClass* cls, const Classes::UClass* base)
		{
			for (const Classes::UStruct* s = cls; s; s = s->SuperField)
				if (s == base) return true;
			return false;
		}

	}

	int OutputIndex(Classes::USequenceOp* op, const char* which)
	{
		return IndexOf(which, static_cast<int>(op->OutputLinks.Num()), [&](int i) { return Narrow(op->OutputLinks[i].LinkDesc); });
	}
	int InputIndex(Classes::USequenceOp* op, const char* which)
	{
		return IndexOf(which, static_cast<int>(op->InputLinks.Num()), [&](int i) { return Narrow(op->InputLinks[i].LinkDesc); });
	}

	LinkResult AddLink(Classes::USequenceOp* from, int output, Classes::USequenceOp* to, int input, int at, std::string* error)
	{
		auto fail = [&](std::string why) {
			if (error) *error = std::move(why);
			return LinkResult::Failed;
		};
		if (!from || !to) return fail("no op");
		if (from->ParentSequence != to->ParentSequence) return fail("the ops are in different sequences");
		if (output < 0 || output >= static_cast<int>(from->OutputLinks.Num())) return fail("no such output");
		if (input < 0 || input >= static_cast<int>(to->InputLinks.Num())) return fail("no such input");
		auto& links = from->OutputLinks[output].Links;
		for (std::size_t i = 0; i < links.Num(); ++i)
			if (links[i].LinkedOp == to && links[i].InputLinkIdx == input) return LinkResult::Exists;
		const Classes::FSeqOpOutputInputLink link{to, input};
		if (!Insert(Raw(links), sizeof link, at, &link)) return fail("the engine's allocator isn't known");
		return LinkResult::Added;
	}

	int RemoveLink(Classes::USequenceOp* from, int output, Classes::USequenceOp* to, int input)
	{
		if (!from || output < 0 || output >= static_cast<int>(from->OutputLinks.Num())) return -1;
		auto& links = from->OutputLinks[output].Links;
		for (int i = 0; i < static_cast<int>(links.Num()); ++i)
			if (links[i].LinkedOp == to && links[i].InputLinkIdx == input) {
				Erase(Raw(links), sizeof(Classes::FSeqOpOutputInputLink), i);
				return i;
			}
		return -1;
	}

	Classes::USequenceOp* Construct(Classes::USequence* sequence, Classes::UClass* cls, std::string* error)
	{
		auto fail = [&](std::string why) -> Classes::USequenceOp* {
			if (error) *error = std::move(why);
			return nullptr;
		};
		if (!sequence || !cls) return fail("no sequence or class");
		if (!Derives(cls, Classes::USequenceOp::StaticClass())) return fail(cls->GetName() + " isn't a SequenceOp class");
		if (classes::ClassFlags(cls) & 0x1) return fail(cls->GetName() + " is abstract");
		auto* op = static_cast<Classes::USequenceOp*>(game::Construct(cls, sequence));
		if (!op) return fail("the engine didn't construct it");
		op->ParentSequence = sequence;
		kismet_trace::Invalidate();
		Classes::USequenceObject* object = op;
		if (!Insert(Raw(sequence->SequenceObjects), sizeof object, -1, &object)) return fail("the engine's allocator isn't known");
		return op;
	}

	bool RemoveOp(Classes::USequence* sequence, Classes::USequenceObject* op)
	{
		if (!sequence || !op) return false;
		bool found = false;
		RawArray& objects = Raw(sequence->SequenceObjects);
		for (int i = objects.count - 1; i >= 0; --i)
			if (sequence->SequenceObjects[i] == op) {
				Erase(objects, sizeof(void*), i);
				found = true;
			}
		RawArray& active = Raw(sequence->ActiveSequenceOps);
		for (int i = active.count - 1; i >= 0; --i)
			if (static_cast<Classes::USequenceObject*>(sequence->ActiveSequenceOps[i]) == op) Erase(active, sizeof(void*), i);
		RawArray& delayed = Raw(sequence->DelayedActivatedOps);
		for (int i = delayed.count - 1; i >= 0; --i) {
			const auto& d = sequence->DelayedActivatedOps[i];
			if (static_cast<Classes::USequenceObject*>(d.Op) == op || static_cast<Classes::USequenceObject*>(d.ActivatorOp) == op)
				Erase(delayed, sizeof(Classes::FActivateOp), i);
		}
		for (std::size_t k = 0; k < sequence->SequenceObjects.Num(); ++k) {
			auto* so = sequence->SequenceObjects[k];
			if (!so || !so->IsA(Classes::USequenceOp::StaticClass())) continue;
			auto* other = static_cast<Classes::USequenceOp*>(so);
			for (std::size_t o = 0; o < other->OutputLinks.Num(); ++o) {
				auto& links = other->OutputLinks[o].Links;
				RawArray& raw = Raw(links);
				for (int i = raw.count - 1; i >= 0; --i)
					if (static_cast<Classes::USequenceObject*>(links[i].LinkedOp) == op)
						Erase(raw, sizeof(Classes::FSeqOpOutputInputLink), i);
			}
		}
		if (found) op->ParentSequence = nullptr;
		return found;
	}
}
