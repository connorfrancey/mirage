#include "runtime/anim.h"

#include "game/uobject.h"
#include "runtime/call.h"
#include "sdk/SDK.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace mirage::anim
{
	namespace
	{
		constexpr int kSlots = 9;		// CNT_MAX
		const char* const kSlotNames[kSlots] = {"canned", "cannedupperbody", "fullbody", "fullbody_dir", "upperbody",
												"lowerbody", "camera", "weapon", "face"};

		Classes::ATdPawn* TdPawnOf(Classes::AActor* a)
		{
			return a && game::IsA(a, Classes::ATdPawn::StaticClass()) ? static_cast<Classes::ATdPawn*>(a) : nullptr;
		}

		Classes::UAnimNodeSequence* Node(Classes::ATdPawn* pawn, int slot)
		{
			call::Params p(pawn, "GetCustomAnimation");
			if (!p.Valid() || !p.Set("Type", static_cast<std::uint8_t>(slot)) || !call::Call(pawn, p)) return nullptr;
			Classes::UObject* o = p.GetObject("ReturnValue");
			return o && game::IsA(o, Classes::UAnimNodeSequence::StaticClass()) ? static_cast<Classes::UAnimNodeSequence*>(o) : nullptr;
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}
	}

	int SlotFromName(const std::string& name)
	{
		std::string n = Lower(name);
		if (n.rfind("cnt_", 0) == 0) n = n.substr(4);
		for (int i = 0; i < kSlots; ++i)
			if (n == kSlotNames[i]) return i;
		return -1;
	}

	bool Play(Classes::AActor* actor, int slot, const std::string& sequence, float rate, float blendIn, float blendOut, bool loop,
			  bool rootMotion, std::string* error)
	{
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		Classes::ATdPawn* pawn = TdPawnOf(actor);
		if (!pawn) return fail("not a TdPawn (the game's custom animation slots are TdPawn's)");
		if (slot < 0 || slot >= kSlots) return fail("no animation slot " + std::to_string(slot));
		if (sequence.empty()) return fail("no sequence");
		call::Params p(pawn, "PlayCustomAnim");
		if (!p.Valid()) return fail("TdPawn.PlayCustomAnim wasn't found");
		p.Set("Type", static_cast<std::uint8_t>(slot));
		p.SetName("AnimName", sequence.c_str());
		p.Set("Rate", rate > 0.f ? rate : 1.f);
		p.Set("BlendInTime", std::max(0.f, blendIn));
		p.Set("BlendOutTime", std::max(0.f, blendOut));
		p.Set("bLooping", loop);
		p.Set("bOverride", true);		// Over whatever the slot plays now, as PlayMoveAnim does
		p.Set("bRootMotion", rootMotion);
		p.Set("bRootRotation", false);
		std::string why;
		if (!call::Call(pawn, p, &why)) return fail("PlayCustomAnim: " + why);
		// The slot plays it only when the mesh has the sequence
		const State s = Get(pawn, slot);
		if (!s.valid) return fail("the slot has no sequence node");
		if (Lower(s.sequence) != Lower(sequence) || s.length <= 0.f)
			return fail("the player's meshes have no sequence '" + sequence + "' (anim_sets lists what they have)");
		return true;
	}

	bool Stop(Classes::AActor* actor, int slot, float blendOut)
	{
		Classes::ATdPawn* pawn = TdPawnOf(actor);
		if (!pawn || slot < 0 || slot >= kSlots) return false;
		call::Params p(pawn, "StopCustomAnim");
		if (!p.Valid()) return false;
		p.Set("Type", static_cast<std::uint8_t>(slot));
		p.Set("BlendOutTime", std::max(0.f, blendOut));
		return call::Call(pawn, p);
	}

	State Get(Classes::AActor* actor, int slot)
	{
		State s;
		Classes::ATdPawn* pawn = TdPawnOf(actor);
		if (!pawn || slot < 0 || slot >= kSlots) return s;
		Classes::UAnimNodeSequence* n = Node(pawn, slot);
		if (!n) return s;
		s.valid = true;
		// Stopping a slot blends it back to the tree's own animation and leaves its sequence node running (a looping one
		// keeps its bPlaying): the slot's bIsPlayingCustomAnim says whether it plays one
		bool slotPlays = true;
		for (std::size_t i = 0; i < n->ParentNodes.Num(); ++i)
			if (auto* parent = n->ParentNodes[i]; parent && game::IsA(parent, Classes::UAnimNodeSlot::StaticClass()))
				slotPlays = static_cast<Classes::UAnimNodeSlot*>(static_cast<Classes::UObject*>(parent))->bIsPlayingCustomAnim;
		s.playing = n->bPlaying && slotPlays;
		s.looping = n->bLooping;
		s.rate = n->Rate;
		s.position = n->CurrentTime;
		if (n->AnimSeq) {
			s.sequence = n->AnimSeqName.GetName();
			s.length = n->AnimSeq->SequenceLength;
		}
		return s;
	}

	std::string SetsJson(Classes::AActor* actor)
	{
		nlohmann::json out = nlohmann::json::array();
		if (!actor) return out.dump();
		std::vector<std::pair<const char*, Classes::USkeletalMeshComponent*>> meshes;
		if (Classes::ATdPawn* pawn = TdPawnOf(actor)) {
			meshes.emplace_back("Mesh1p", pawn->Mesh1p);
			meshes.emplace_back("Mesh3p", pawn->Mesh3p);
			if (pawn->Mesh && pawn->Mesh != pawn->Mesh1p && pawn->Mesh != pawn->Mesh3p) meshes.emplace_back("Mesh", pawn->Mesh);
		} else if (game::IsA(actor, Classes::APawn::StaticClass())) {
			meshes.emplace_back("Mesh", static_cast<Classes::APawn*>(actor)->Mesh);
		}
		for (const auto& [label, mesh] : meshes) {
			if (!mesh) continue;
			nlohmann::json sets = nlohmann::json::array();
			for (std::size_t i = 0; i < mesh->AnimSets.Num(); ++i) {
				Classes::UAnimSet* set = mesh->AnimSets[i];
				if (!set) continue;		// An empty slot (no weapon: slots 1 and 2)
				nlohmann::json seqs = nlohmann::json::array();
				for (std::size_t k = 0; k < set->Sequences.Num(); ++k) {
					Classes::UAnimSequence* q = set->Sequences[k];
					if (!q) continue;
					nlohmann::json notifies = nlohmann::json::array();
					for (std::size_t j = 0; j < q->Notifies.Num(); ++j) {
						const auto& e = q->Notifies[j];
						notifies.push_back({{"time", e.Time}, {"notify", e.Notify && e.Notify->Class ? e.Notify->Class->GetName() : ""},
											{"comment", e.Comment.GetName()}});
					}
					seqs.push_back({{"name", q->SequenceName.GetName()}, {"length", q->SequenceLength}, {"frames", q->NumFrames},
									{"rate", q->RateScale}, {"notifies", std::move(notifies)}});
				}
				sets.push_back({{"slot", i}, {"name", set->GetName()}, {"path", set->GetPathName()}, {"sequences", std::move(seqs)}});
			}
			out.push_back({{"mesh", label}, {"sets", std::move(sets)}});
		}
		return out.dump();
	}
}
