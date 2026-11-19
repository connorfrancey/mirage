// Custom animations (see anim.h, "Custom animations"): clips as JSON, baked from the game's sequences, and installed as
// sequences of the player's meshes that the engine poses from raw keys.
#include "runtime/anim.h"

#include "game/game.h"
#include "game/object_ref.h"
#include "game/uobject.h"
#include "runtime/call.h"
#include "runtime/classes.h"
#include "runtime/maps.h"
#include "sdk/SDK.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mirage::anim
{
	namespace
	{
		constexpr int kFullBody = 2;		// CNT_FullBody
		constexpr int kMaxFrames = 10000;

		struct RawArray
		{
			std::uint8_t* data;
			std::int32_t count, max;
		};
		static_assert(sizeof(RawArray) == 12);
		template <class T> RawArray& Raw(Classes::TArray<T>& a) { return reinterpret_cast<RawArray&>(a); }
		constexpr std::size_t kTrack = 0x24;		// FRawAnimSequenceTrack: PosKeys, RotKeys, KeyTimes
		static_assert(sizeof(Classes::FRawAnimSequenceTrack) == kTrack);
		static_assert(sizeof(Classes::FQuat) == 16 && sizeof(Classes::FBoneAtom) == 32);

		// `count` elements of `size` bytes in a new engine array (GMalloc: the engine frees what its objects hold)
		bool Fill(RawArray& a, const void* src, int count, std::size_t size)
		{
			a = {nullptr, 0, 0};
			if (count <= 0) return true;
			auto* p = static_cast<std::uint8_t*>(maps::EngineRealloc(nullptr, count * size));
			if (!p) return false;
			std::memcpy(p, src, count * size);
			a = {p, count, count};
			return true;
		}
		void Free(RawArray& a)
		{
			if (a.data) maps::EngineRealloc(a.data, 0);
			a = {nullptr, 0, 0};
		}
		void FreeTracks(Classes::UAnimSequence* s)
		{
			RawArray& raw = Raw(s->RawAnimData);
			for (int t = 0; t < raw.count; ++t) {
				auto* arrays = reinterpret_cast<RawArray*>(raw.data + t * kTrack);
				for (int k = 0; k < 3; ++k) Free(arrays[k]);
			}
			Free(raw);
		}

		Classes::ATdPawn* TdPawnOf(Classes::AActor* a)
		{
			return a && game::IsA(a, Classes::ATdPawn::StaticClass()) ? static_cast<Classes::ATdPawn*>(a) : nullptr;
		}

		struct MeshRef
		{
			const char* label;
			Classes::USkeletalMeshComponent* mesh;
			Classes::UAnimNodeSlot* fullBody;		// The mesh's full-body custom animation slot
		};
		std::vector<MeshRef> Meshes(Classes::ATdPawn* pawn)
		{
			return {{"Mesh1p", pawn->Mesh1p, pawn->CustomFullBodyNode1p}, {"Mesh3p", pawn->Mesh3p, pawn->CustomFullBodyNode3p}};
		}
		Classes::UAnimSet* SetOf(Classes::USkeletalMeshComponent* mesh)
		{
			return mesh && mesh->AnimSets.Num() > 0 ? mesh->AnimSets[0] : nullptr;
		}

		// Where each of the set's bones is in the mesh's skeleton (the index into LocalAtoms; -1 when the mesh hasn't it)
		std::vector<int> TrackBones(Classes::USkeletalMeshComponent* mesh, Classes::UAnimSet* set)
		{
			std::vector<int> out;
			for (std::size_t t = 0; t < set->TrackBoneNames.Num(); ++t) {
				call::Params p(mesh, "MatchRefBone");
				p.SetRaw("BoneName", &set->TrackBoneNames[t], sizeof(Classes::FName));
				out.push_back(p.Valid() && call::Call(mesh, p) ? p.GetInt("ReturnValue") : -1);
			}
			return out;
		}
		Classes::FBoneAtom AtomOf(Classes::USkeletalMeshComponent* mesh, int index)
		{
			Classes::FBoneAtom a{};
			a.Rotation.W = 1.f;
			a.Scale = 1.f;
			if (index >= 0 && static_cast<std::size_t>(index) < mesh->LocalAtoms.Num()) a = mesh->LocalAtoms[index];
			return a;
		}

		// The sequence node under `slot` that holds `sequence`
		Classes::UAnimNodeSequence* SequenceUnder(Classes::UAnimNodeSlot* slot, const std::string& sequence)
		{
			if (!slot) return nullptr;
			for (std::size_t i = 0; i < slot->Children.Num(); ++i) {
				Classes::UAnimNode* n = slot->Children[i].Anim;
				if (!n || !game::IsA(n, Classes::UAnimNodeSequence::StaticClass())) continue;
				auto* s = static_cast<Classes::UAnimNodeSequence*>(n);
				if (s->AnimSeq && s->AnimSeqName.GetName() == sequence) return s;
			}
			return nullptr;
		}

		// A clip's keys: `n` is 1 or the frame count; every number finite
		template <std::size_t N> bool ReadKeys(const nlohmann::json& j, int frames, std::vector<std::array<float, N>>& out, std::string& why)
		{
			if (!j.is_array() || (j.size() != 1 && static_cast<int>(j.size()) != frames)) {
				why = "needs 1 key or one a frame (" + std::to_string(frames) + ")";
				return false;
			}
			for (const auto& k : j) {
				if (!k.is_array() || k.size() != N) {
					why = "a key isn't " + std::to_string(N) + " numbers";
					return false;
				}
				std::array<float, N> v{};
				for (std::size_t i = 0; i < N; ++i) {
					if (!k[i].is_number() || !std::isfinite(k[i].get<double>())) {
						why = "a key isn't a finite number";
						return false;
					}
					v[i] = k[i].get<float>();
				}
				out.push_back(v);
			}
			return true;
		}

		// ---- Registered clips ------------------------------------------------------------------------------------------
		struct Made
		{
			Classes::UAnimSequence* sequence;
			Classes::UAnimSet* set;
			std::string name;
		};
		std::mutex g_lock;
		std::vector<std::pair<std::string, Clip>> g_clips;		// Mod, clip
		std::vector<Made> g_made;							// Sequences Install made (game thread)
		const void* g_installedOn = nullptr;				// The pawn the clips are on
		std::vector<const void*> g_installedSets;			// Its meshes' first sets then

		void RemoveFromSet(const Made& m)
		{
			auto& raw = Raw(m.set->Sequences);
			auto** seqs = reinterpret_cast<Classes::UAnimSequence**>(raw.data);
			for (int i = 0; i < raw.count; ++i)
				if (seqs[i] == m.sequence) {
					std::memmove(seqs + i, seqs + i + 1, (raw.count - i - 1) * sizeof *seqs);
					--raw.count;
					break;
				}
		}

		// Sets the clips were installed on are still what the pawn's meshes have
		std::vector<const void*> CurrentSets(Classes::ATdPawn* pawn)
		{
			std::vector<const void*> sets;
			for (const auto& m : Meshes(pawn)) sets.push_back(SetOf(m.mesh));
			return sets;
		}

		void InstallAll(Classes::ATdPawn* pawn)
		{
			std::vector<std::pair<std::string, Clip>> clips;
			{
				std::lock_guard lock(g_lock);
				clips = g_clips;
			}
			for (const auto& [mod, clip] : clips) {
				std::string error;
				if (!Install(pawn, clip, &error)) spdlog::warn("[mod {}] animation {}: {}", mod, clip.name, error);
			}
			g_installedOn = pawn;
			g_installedSets = CurrentSets(pawn);
		}
	}

	bool ClipFromJson(const std::string& text, Clip* out, std::string* error)
	{
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		const auto j = nlohmann::json::parse(text, nullptr, false);
		if (!j.is_object()) return fail("not a JSON object");
		Clip c;
		c.name = j.value("name", std::string());
		c.length = j.value("length", 0.f);
		c.rate = j.value("rate", 1.f);
		c.frames = j.value("frames", 0);
		if (c.frames < 1 || c.frames > kMaxFrames) return fail("frames must be 1 to " + std::to_string(kMaxFrames));
		if (!std::isfinite(c.length) || c.length < 0.f || (c.frames > 1 && c.length <= 0.f)) return fail("length must be above 0");
		if (!std::isfinite(c.rate) || c.rate <= 0.f) return fail("rate must be above 0");
		if (!j.contains("meshes") || !j["meshes"].is_object()) return fail("no meshes");
		for (const auto& [mesh, bones] : j["meshes"].items()) {
			if (mesh != "Mesh1p" && mesh != "Mesh3p") return fail("meshes: '" + mesh + "' isn't Mesh1p or Mesh3p");
			if (!bones.is_object()) return fail(mesh + " isn't an object of bones");
			for (const auto& [bone, keys] : bones.items()) {
				BoneKeys k;
				std::string why;
				if (!keys.is_object() || !keys.contains("pos") || !keys.contains("rot")) return fail(mesh + "." + bone + ": needs pos and rot");
				if (!ReadKeys(keys["pos"], c.frames, k.pos, why)) return fail(mesh + "." + bone + ".pos " + why);
				if (!ReadKeys(keys["rot"], c.frames, k.rot, why)) return fail(mesh + "." + bone + ".rot " + why);
				for (auto& q : k.rot) {		// Unit rotations: a stray length scales the bone
					const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
					if (len < 1e-6f) return fail(mesh + "." + bone + ".rot: a zero rotation");
					for (float& v : q) v /= len;
				}
				c.meshes[mesh][bone] = std::move(k);
			}
		}
		*out = std::move(c);
		return true;
	}

	std::string ClipToJson(const Clip& clip)
	{
		nlohmann::json meshes = nlohmann::json::object();
		for (const auto& [mesh, bones] : clip.meshes)
			for (const auto& [bone, k] : bones) meshes[mesh][bone] = {{"pos", k.pos}, {"rot", k.rot}};
		const nlohmann::json j = {{"name", clip.name}, {"length", clip.length}, {"rate", clip.rate}, {"frames", clip.frames}, {"meshes", meshes}};
		return j.dump(1);
	}

	bool Bake(Classes::AActor* actor, const std::string& sequence, int frames, Clip* out, std::string* error)
	{
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		Classes::ATdPawn* pawn = TdPawnOf(actor);
		if (!pawn) return fail("not a TdPawn");
		frames = std::clamp(frames, 2, kMaxFrames);
		if (!Play(pawn, kFullBody, sequence, 1.f, 0.f, 0.f, false, false, error)) return false;
		Clip c;
		c.name = sequence;
		c.frames = frames;
		for (const MeshRef& m : Meshes(pawn)) {
			Classes::UAnimNodeSequence* node = SequenceUnder(m.fullBody, sequence);
			if (!m.mesh || !node) continue;
			Classes::UAnimSet* set = SetOf(m.mesh);
			if (!set) continue;
			c.length = node->AnimSeq->SequenceLength;
			c.rate = node->AnimSeq->RateScale;
			const std::vector<int> index = TrackBones(m.mesh, set);
			auto& bones = c.meshes[m.label];
			for (int f = 0; f < frames; ++f) {
				call::Params at(node, "SetPosition");
				at.Set("NewTime", c.length * static_cast<float>(f) / static_cast<float>(frames - 1));
				at.Set("bFireNotifies", false);
				call::Call(node, at);
				call::Params update(m.mesh, "ForceSkelUpdate");
				call::Call(m.mesh, update);
				for (std::size_t t = 0; t < index.size(); ++t) {
					if (index[t] < 0) continue;
					const Classes::FBoneAtom a = AtomOf(m.mesh, index[t]);
					BoneKeys& k = bones[set->TrackBoneNames[t].GetName()];
					k.pos.push_back({a.Translation.X, a.Translation.Y, a.Translation.Z});
					k.rot.push_back({a.Rotation.X, a.Rotation.Y, a.Rotation.Z, a.Rotation.W});
				}
			}
			// A bone that holds still is one key
			for (auto& [name, k] : bones) {
				if (std::all_of(k.pos.begin(), k.pos.end(), [&](const auto& v) { return v == k.pos.front(); })) k.pos.resize(1);
				if (std::all_of(k.rot.begin(), k.rot.end(), [&](const auto& v) { return v == k.rot.front(); })) k.rot.resize(1);
			}
		}
		Stop(pawn, kFullBody, 0.f);
		if (c.meshes.empty()) return fail("no mesh of the player plays '" + sequence + "'");
		*out = std::move(c);
		return true;
	}

	bool Install(Classes::AActor* actor, const Clip& clip, std::string* error)
	{
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		Classes::ATdPawn* pawn = TdPawnOf(actor);
		if (!pawn) return fail("not a TdPawn");
		if (clip.name.empty() || clip.frames < 1) return fail("the clip has no name or no frames");
		int installed = 0;
		for (const MeshRef& m : Meshes(pawn)) {
			const auto section = clip.meshes.find(m.label);
			Classes::UAnimSet* set = SetOf(m.mesh);
			if (section == clip.meshes.end() || !set) continue;

			// The sequence: one made before under this name (its keys replaced), or a new one at the end of the set
			Classes::UAnimSequence* seq = nullptr;
			for (std::size_t i = 0; i < set->Sequences.Num(); ++i)
				if (auto* s = set->Sequences[i]; s && s->SequenceName.GetName() == clip.name) seq = s;
			if (seq && std::none_of(g_made.begin(), g_made.end(), [&](const Made& x) { return x.sequence == seq; }))
				return fail(std::string(m.label) + "'s animations already have a '" + clip.name + "' of the game's");
			if (seq) {
				FreeTracks(seq);
			} else {
				seq = static_cast<Classes::UAnimSequence*>(game::Construct(Classes::UAnimSequence::StaticClass(), set));
				if (!seq) return fail("the engine didn't make a sequence");
				RawArray& list = Raw(set->Sequences);
				if (list.count == list.max) {
					auto* p = static_cast<std::uint8_t*>(maps::EngineRealloc(list.data, (list.max + 8) * sizeof(void*)));
					if (!p) return fail("no memory for the set's list");
					list.data = p;
					list.max += 8;
				}
				reinterpret_cast<Classes::UAnimSequence**>(list.data)[list.count++] = seq;
				// Kept from the garbage collector: a level load collected it while the set still listed it (the set's
				// list is not enough to keep a sequence made at runtime), leaving the set a dangling pointer
				classes::Keep(seq, true);
				g_made.push_back({seq, set, clip.name});
			}

			// One raw track per bone of the set, in its order; a bone the clip lacks holds where it is now
			const std::vector<int> index = TrackBones(m.mesh, set);
			const int tracks = static_cast<int>(set->TrackBoneNames.Num());
			RawArray& raw = Raw(seq->RawAnimData);
			raw.data = static_cast<std::uint8_t*>(maps::EngineRealloc(nullptr, tracks * kTrack));
			if (!raw.data && tracks) return fail("no memory for the keys");
			std::memset(raw.data, 0, tracks * kTrack);
			raw.count = raw.max = tracks;
			std::vector<float> times(clip.frames);
			for (int f = 0; f < clip.frames; ++f) times[f] = clip.frames > 1 ? clip.length * f / (clip.frames - 1) : 0.f;
			for (int t = 0; t < tracks; ++t) {
				const std::string bone = set->TrackBoneNames[t].GetName();
				BoneKeys held;
				const BoneKeys* keys = nullptr;
				if (const auto it = section->second.find(bone); it != section->second.end()) keys = &it->second;
				else {
					const Classes::FBoneAtom a = AtomOf(m.mesh, index[t]);
					held.pos.push_back({a.Translation.X, a.Translation.Y, a.Translation.Z});
					held.rot.push_back({a.Rotation.X, a.Rotation.Y, a.Rotation.Z, a.Rotation.W});
					keys = &held;
				}
				auto* arrays = reinterpret_cast<RawArray*>(raw.data + t * kTrack);
				// Animation tracks keep a non-root bone's rotation as the conjugate of its bone-space rotation (what
				// LocalAtoms and the clip hold): found by baking a copy back, positions exact and rotations inverted
				std::vector<Classes::FQuat> quats;
				const float sign = t == 0 ? 1.f : -1.f;
				for (const auto& q : keys->rot) quats.push_back({q[0] * sign, q[1] * sign, q[2] * sign, q[3]});
				const int keyCount = static_cast<int>(std::max(keys->pos.size(), keys->rot.size()));
				if (!Fill(arrays[0], keys->pos.data(), static_cast<int>(keys->pos.size()), 12) ||
					!Fill(arrays[1], quats.data(), static_cast<int>(quats.size()), 16) ||
					!Fill(arrays[2], times.data(), keyCount == 1 ? 1 : clip.frames, 4))
					return fail("no memory for the keys");
			}
			seq->SequenceName = game::AddName(clip.name.c_str());		// A new name to the game
			seq->SequenceLength = clip.length;
			seq->NumFrames = clip.frames;
			seq->RateScale = clip.rate;
			Raw(seq->CompressedTrackOffsets).count = 0;		// Posed from RawAnimData: nothing compressed
			std::memset(&seq->CompressedOffsetPtr, 0, sizeof seq->CompressedOffsetPtr);
			std::memset(&seq->CompressedStreamPtr, 0, sizeof seq->CompressedStreamPtr);
			++installed;
		}
		if (!installed) return fail("the clip has no keys for the player's meshes (Mesh1p, Mesh3p)");
		return true;
	}

	void Register(const std::string& mod, std::vector<Clip> clips)
	{
		{
			std::lock_guard lock(g_lock);
			std::erase_if(g_clips, [&](const auto& c) { return c.first == mod; });
			for (auto& c : clips) g_clips.emplace_back(mod, std::move(c));
		}
		g_installedOn = nullptr;		// Installed by the next Tick
	}

	void Unregister(const std::string& mod)
	{
		std::vector<std::string> names;
		{
			std::lock_guard lock(g_lock);
			for (const auto& [m, c] : g_clips)
				if (m == mod) names.push_back(c.name);
			std::erase_if(g_clips, [&](const auto& c) { return c.first == mod; });
		}
		Classes::ATdPawn* pawn = game::Pawn();
		const std::vector<const void*> live = pawn ? CurrentSets(pawn) : std::vector<const void*>{};
		std::erase_if(g_made, [&](const Made& m) {
			if (std::find(names.begin(), names.end(), m.name) == names.end()) return false;
			// Only from sets the player has now: a set gone with a level load took its sequences with it
			if (std::find(live.begin(), live.end(), m.set) != live.end()) RemoveFromSet(m);
			classes::Keep(m.sequence, false);
			return true;
		});
	}

	void Tick()
	{
		Classes::ATdPlayerPawn* pawn = game::Pawn();
		if (!pawn) return;
		{
			std::lock_guard lock(g_lock);
			if (g_clips.empty()) return;
		}
		if (g_installedOn == pawn && g_installedSets == CurrentSets(pawn)) return;
		// A new pawn or new sets: what was made for the old ones is gone with them
		const std::vector<const void*> live = CurrentSets(pawn);
		std::erase_if(g_made, [&](const Made& m) {
			if (std::find(live.begin(), live.end(), m.set) != live.end()) return false;
			classes::Keep(m.sequence, false);		// Its set is gone: the collector may have it
			return true;
		});
		InstallAll(pawn);
	}

	std::vector<std::pair<std::string, std::string>> Registered()
	{
		std::lock_guard lock(g_lock);
		std::vector<std::pair<std::string, std::string>> out;
		for (const auto& [mod, c] : g_clips) out.emplace_back(mod, c.name);
		return out;
	}

	bool RegisteredClip(const std::string& name, Clip* out)
	{
		std::lock_guard lock(g_lock);
		for (const auto& [mod, c] : g_clips)
			if (c.name == name) {
				*out = c;
				return true;
			}
		return false;
	}

	bool IsCustom(const Classes::UAnimSequence* sequence)
	{
		return std::any_of(g_made.begin(), g_made.end(), [&](const Made& m) { return m.sequence == sequence; });
	}
}
