#pragma once
// The levels' Kismet, read (step 5 of the next ten steps; the plan's step 18). Everything Kismet is is UObjects already in
// the engine: a level's sequences (USequence, nested ones inside), their objects (SequenceObjects: events, actions,
// conditions, variables), each op's input, output and variable links, and its runtime state. This reads them as they
// are in memory; changing them (links, new nodes, through the change log) is the next part of the step.
//
// Objects are named by their path ("Edge_p.TheWorld:PersistentLevel.Main_Sequence.SeqEvent_Touch_3"), the same name
// object_find takes. Game thread.

#include <string>
#include <vector>

namespace mirage::kismet
{
	struct Link
	{
		std::string op;				// The op it leads to (path)
		int input = 0;				// Which of that op's inputs
	};
	struct Output
	{
		std::string desc;			// "Out", "Touched", "Finished"
		std::vector<Link> links;
	};
	struct VariableLink
	{
		std::string desc;			// "Instigator", "Target"
		std::vector<std::string> variables;		// Paths of the SeqVar objects linked
	};
	struct Op
	{
		std::string path, name, cls;			// name: the object's name; cls: its class ("SeqEvent_Touch")
		std::string title, comment;				// ObjName (the editor's title) and ObjComment, when cooking kept them
		enum class Kind { Event, Action, Condition, Variable, Sequence, Other } kind = Kind::Other;
		bool active = false;
		int activations = 0;					// ActivateCount
		std::vector<std::string> inputs;		// Input links' descriptions
		std::vector<Output> outputs;
		std::vector<VariableLink> variables;
		std::string originator;					// An event's Originator (the actor that fires it), path
	};
	struct Sequence
	{
		std::string path, name;
		std::string level;			// The package of the level it belongs to ("Edge_p", a streamed sublevel's)
		std::string parent;			// The sequence it is nested in ("" for a level's root sequence)
		bool enabled = true;
		std::vector<Op> ops;		// Every object in SequenceObjects (variables too: Kind::Variable)
	};

	// Every sequence of the loaded levels, root ones first. Game thread.
	std::vector<Sequence> Read();
	// The same as JSON: [{"path", "name", "level", "parent", "enabled", "ops": [{"path", "name", "class", "kind", "title",
	// "comment", "active", "activations", "inputs": [...], "outputs": [{"desc", "links": [{"op", "input"}]}],
	// "variables": [{"desc", "linked": [...]}], "originator"}]}]
	std::string ToJson(const std::vector<Sequence>& sequences);
	const char* KindName(Op::Kind kind);
}
