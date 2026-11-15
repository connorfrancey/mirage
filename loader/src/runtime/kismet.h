#pragma once
// The levels' Kismet, read (step 5 of the next ten steps; the plan's step 18). Everything Kismet is is UObjects already in
// the engine: a level's sequences (USequence, nested ones inside), their objects (SequenceObjects: events, actions,
// conditions, variables), each op's input, output and variable links, and its runtime state. This reads them as they
// are in memory. The edit primitives below change them; the mod API (kismet_link, kismet_unlink, kismet_create) records
// each edit's undo in the change log, so a mod's edits go when it unloads.
//
// Objects are named by their path ("Edge_p.TheWorld:PersistentLevel.Main_Sequence.SeqEvent_Touch_3"), the same name
// object_find takes. Game thread.

#include <string>
#include <vector>

namespace Classes
{
	class UClass;
	class USequence;
	class USequenceObject;
	class USequenceOp;
}

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

	// Every sequence object of the loaded levels (sequences too), unordered. Game thread.
	std::vector<Classes::USequenceObject*> LevelObjects();
	// The package of the level an object is in ("Edge_p")
	std::string LevelOf(const Classes::USequenceObject* o);

	// The level package's own Kismet, read from its file (UnrealLib; any thread, seconds: it reads the script packages'
	// reflection too): every SequenceObject export as an Op with path, name, class and output links (inputs, variables
	// and runtime state left empty), in export order. Empty and `error` when the package can't be read.
	std::vector<Op> ReadPackage(const std::string& level, std::string* error);

	// ---- Editing (game thread; the callers check the objects are alive) ----------------------------------------------
	// An output's (input's) index from its description ("Touched") or a decimal index ("0"); NULL or "": 0. -1: none such
	int OutputIndex(Classes::USequenceOp* op, const char* which);
	int InputIndex(Classes::USequenceOp* op, const char* which);

	enum class LinkResult { Added, Exists, Failed };
	// from.OutputLinks[output] -> to.InputLinks[input], both ops in the same sequence. `at`: where in the output's list
	// (-1: the end). Engine arrays grow through GMalloc.
	LinkResult AddLink(Classes::USequenceOp* from, int output, Classes::USequenceOp* to, int input, int at, std::string* error);
	// Removes that link; returns where it was in the output's list, -1 if it wasn't there
	int RemoveLink(Classes::USequenceOp* from, int output, Classes::USequenceOp* to, int input);
	// A new op of `cls` (a SequenceOp class, not abstract) in `sequence`: constructed by the engine
	// (StaticConstructObject: its links come from the class defaults), its ParentSequence set, added to SequenceObjects
	Classes::USequenceOp* Construct(Classes::USequence* sequence, Classes::UClass* cls, std::string* error);
	// Takes `op` out of `sequence`: SequenceObjects, the active and delayed lists, every link to it. The GC frees it.
	bool RemoveOp(Classes::USequence* sequence, Classes::USequenceObject* op);
}
