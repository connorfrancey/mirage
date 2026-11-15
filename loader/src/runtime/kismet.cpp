#include "runtime/kismet.h"

#include "sdk/SDK.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

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
			Sequence s;
			s.path = PathOf(seq);
			s.name = seq->GetName();
			s.level = LevelOf(seq);
			s.parent = PathOf(seq->ParentSequence);
			s.enabled = seq->bEnabled;
			for (std::size_t k = 0; k < seq->SequenceObjects.Num(); ++k)
				if (auto* so = seq->SequenceObjects[k]) s.ops.push_back(ReadOp(so));
			out.push_back(std::move(s));
		}
		std::stable_sort(out.begin(), out.end(), [](const Sequence& a, const Sequence& b) { return a.parent.empty() && !b.parent.empty(); });
		return out;
	}

	std::string ToJson(const std::vector<Sequence>& sequences)
	{
		nlohmann::json all = nlohmann::json::array();
		for (const Sequence& s : sequences) {
			nlohmann::json ops = nlohmann::json::array();
			for (const Op& op : s.ops) {
				nlohmann::json outputs = nlohmann::json::array();
				for (const Output& o : op.outputs) {
					nlohmann::json links = nlohmann::json::array();
					for (const Link& l : o.links) links.push_back({{"op", l.op}, {"input", l.input}});
					outputs.push_back({{"desc", o.desc}, {"links", links}});
				}
				nlohmann::json vars = nlohmann::json::array();
				for (const VariableLink& v : op.variables) vars.push_back({{"desc", v.desc}, {"linked", v.variables}});
				ops.push_back({{"path", op.path}, {"name", op.name}, {"class", op.cls}, {"kind", KindName(op.kind)},
							   {"title", op.title}, {"comment", op.comment}, {"active", op.active},
							   {"activations", op.activations}, {"inputs", op.inputs}, {"outputs", outputs},
							   {"variables", vars}, {"originator", op.originator}});
			}
			all.push_back({{"path", s.path}, {"name", s.name}, {"level", s.level}, {"parent", s.parent},
						   {"enabled", s.enabled}, {"ops", ops}});
		}
		return all.dump();
	}
}
