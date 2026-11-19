#include "runtime/pieces.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "runtime/maps.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace mirage::pieces
{
	namespace
	{
		struct Piece
		{
			std::string owner;
			Classes::AActor* actor = nullptr;
			std::size_t slot = 0;
			Desc desc;
			Classes::FVector a{}, b{};		// The bar or the cable, end to end
			bool near = false;				// The pawn was within reach last tick
			bool Alive() const { return game::AliveAt(actor, slot) && !actor->bDeleteMe; }
		};
		std::vector<std::unique_ptr<Piece>> g_pieces;

		Classes::FVector V(const float (&p)[3]) { return {p[0], p[1], p[2]}; }
		Classes::FVector Sub(const Classes::FVector& a, const Classes::FVector& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
		Classes::FVector Lerp(const Classes::FVector& a, const Classes::FVector& b, float t)
		{
			return {a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
		}
		float Dot(const Classes::FVector& a, const Classes::FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
		float Size(const Classes::FVector& a) { return std::sqrt(Dot(a, a)); }

		float DistanceToSegment(const Classes::FVector& p, const Classes::FVector& a, const Classes::FVector& b)
		{
			const Classes::FVector ab = Sub(b, a);
			const float len2 = Dot(ab, ab);
			const float t = len2 > 0.f ? std::clamp(Dot(Sub(p, a), ab) / len2, 0.f, 1.f) : 0.f;
			return Size(Sub(p, Lerp(a, b, t)));
		}

		// The engine's arrays are freed by the engine (GMalloc): `count` vectors allocated through its Realloc
		bool AllocVectors(Classes::TArray<Classes::FVector>& arr, int count)
		{
			struct Raw
			{
				void* data;
				std::int32_t count, max;
			};
			auto& raw = reinterpret_cast<Raw&>(arr);
			void* p = maps::EngineRealloc(raw.data, count * sizeof(Classes::FVector));
			if (!p) return false;
			raw.data = p;
			raw.count = raw.max = count;
			return true;
		}

		// Lets the player's pawn go of a piece (as PawnLeavingVolume does)
		void Release(Classes::AActor* actor)
		{
			auto* pawn = game::Pawn();
			if (pawn && reinterpret_cast<Classes::AActor*>(pawn->ActiveMovementVolume) == actor) pawn->ActiveMovementVolume = nullptr;
		}

		void Destroy(Piece& p)
		{
			Release(p.actor);
			if (p.Alive()) p.actor->Destroy();
		}

		// The class defaults' bits that make Spawn refuse a volume (Brush: bStatic, bNoDelete) and give it collision
		// (Volume: bCollideActors), off for one spawn
		Classes::AActor* SpawnVolume(Classes::UClass* cls, const char* defaultsPath, const Classes::FVector& at, const Classes::FRotator& rot,
									 std::string* error)
		{
			auto* pawn = game::Pawn();
			if (!pawn) {
				*error = "no player to spawn from";
				return nullptr;
			}
			auto* defaults = game::FindObject<Classes::AActor>(defaultsPath);
			if (!cls || !defaults) {
				*error = std::string("the class or its defaults weren't found (") + defaultsPath + ")";
				return nullptr;
			}
			const bool isStatic = defaults->bStatic, noDelete = defaults->bNoDelete, collide = defaults->bCollideActors;
			defaults->bStatic = false;
			defaults->bNoDelete = false;
			defaults->bCollideActors = false;
			Classes::AActor* a = pawn->Spawn(cls, nullptr, Classes::FName{}, at, rot, nullptr, true);
			defaults->bStatic = isStatic;
			defaults->bNoDelete = noDelete;
			defaults->bCollideActors = collide;
			if (!a) {
				*error = "the engine didn't spawn the volume";
				return nullptr;
			}
			a->bStatic = false;
			a->bNoDelete = false;
			return a;
		}
	}

	bool KindFromName(const std::string& name, Kind* out, std::string* why)
	{
		if (name == "swing") *out = Kind::Swing;
		else if (name == "zipline") *out = Kind::Zipline;
		else {
			if (why) {
				if (name == "ladder" || name == "balance" || name == "ledge" || name == "ledgewalk")
					*why = "a " + name + " isn't supported yet (its move reads the volume's brush: not surveyed)";
				else if (name == "springboard")
					*why = "a springboard is geometry, not a piece: a 64-unit step, then a ledge 80 to 148 high 112 units on";
				else *why = "no piece kind '" + name + "' (swing, zipline)";
			}
			return false;
		}
		return true;
	}

	const char* KindName(Kind kind) { return kind == Kind::Swing ? "swing" : "zipline"; }

	Classes::AActor* Create(const std::string& owner, const Desc& desc, std::string* error)
	{
		std::string why;
		auto fail = [&](const std::string& w) -> Classes::AActor* {
			if (error) *error = w;
			return nullptr;
		};
		auto p = std::make_unique<Piece>();
		p->owner = owner;
		p->desc = desc;
		if (desc.kind == Kind::Swing) {
			if (!(desc.length > 0.f)) return fail("a swing's length must be above 0");
			if (p->desc.reach <= 0.f) p->desc.reach = 150.f;
			const Classes::FVector at = V(desc.location);
			Classes::AActor* a = SpawnVolume(Classes::ATdSwingVolume::StaticClass(), "TdSwingVolume TdGame.Default__TdSwingVolume", at,
											 Classes::FRotator{0, desc.yaw, 0}, &why);
			if (!a) return fail(why);
			auto* v = static_cast<Classes::ATdSwingVolume*>(a);
			v->bThickGrip = desc.thickGrip;
			v->bSnapToCenter = desc.snapToCenter;
			// The bar runs along the volume's Y axis (GetAxes: Y is the yaw turned a quarter)
			const float yaw = static_cast<float>(desc.yaw + 16384) * 3.14159265f / 32768.f, half = desc.length * 0.5f;
			const Classes::FVector along{std::cos(yaw) * half, std::sin(yaw) * half, 0.f};
			p->a = {at.X - along.X, at.Y - along.Y, at.Z};
			p->b = {at.X + along.X, at.Y + along.Y, at.Z};
			// The swing never reads Start and End: they hold the bar's ends, for whoever draws it (the Collision View)
			v->Start = p->a;
			v->End = p->b;
			p->actor = a;
		} else {
			const Classes::FVector s = V(desc.location), e = V(desc.end), d = Sub(e, s);
			const float len = Size(d);
			if (len < 100.f) return fail("a zipline's ends must be at least 100 units apart");
			const int segments = std::clamp(desc.segments, 1, 100);
			if (p->desc.reach <= 0.f) p->desc.reach = 120.f;
			const int yaw = static_cast<int>(std::atan2(d.Y, d.X) * 32768.f / 3.14159265f);
			Classes::AActor* a = SpawnVolume(Classes::ATdZiplineVolume::StaticClass(), "TdZiplineVolume TdGame.Default__TdZiplineVolume",
											 Lerp(s, e, 0.5f), Classes::FRotator{0, yaw, 0}, &why);
			if (!a) return fail(why);
			auto* v = static_cast<Classes::ATdZiplineVolume*>(a);
			if (desc.landingStrip > 0.f) v->LandingStrip = desc.landingStrip;
			if (!AllocVectors(v->SplineLocations, segments + 1)) {
				a->Destroy();
				return fail("the engine's allocator wasn't found (GMalloc)");
			}
			for (int i = 0; i <= segments; ++i) v->SplineLocations[i] = Lerp(s, e, static_cast<float>(i) / static_cast<float>(segments));
			v->NumSplineSegments = segments;
			v->Start = s;
			v->End = e;
			v->Middle = v->Center = Lerp(s, e, 0.5f);
			v->MoveDirection = {d.X / len, d.Y / len, d.Z / len};
			v->SplineLength = len;
			p->a = s;
			p->b = e;
			p->actor = a;
		}
		p->slot = game::IndexOf(p->actor);
		Classes::AActor* a = p->actor;
		spdlog::info("[mod {}] {} piece {} ({:.0f} {:.0f} {:.0f} to {:.0f} {:.0f} {:.0f}, reach {:.0f})", owner, KindName(desc.kind),
					 a->GetName(), p->a.X, p->a.Y, p->a.Z, p->b.X, p->b.Y, p->b.Z, p->desc.reach);
		g_pieces.push_back(std::move(p));
		return a;
	}

	void Remove(Classes::AActor* piece)
	{
		const auto it = std::find_if(g_pieces.begin(), g_pieces.end(), [&](const auto& p) { return p->actor == piece; });
		if (it == g_pieces.end()) return;
		Destroy(**it);
		g_pieces.erase(it);
	}

	void RemoveAll(const std::string& owner)
	{
		std::erase_if(g_pieces, [&](const auto& p) {
			if (p->owner != owner) return false;
			Destroy(*p);
			return true;
		});
	}

	void Tick()
	{
		std::erase_if(g_pieces, [](const auto& p) {
			if (p->Alive()) return false;
			Release(p->actor);		// Gone with its level, or destroyed by a layer
			return true;
		});
		auto* pawn = game::Pawn();
		if (!pawn) return;
		for (auto& p : g_pieces) {
			const bool near = DistanceToSegment(pawn->Location, p->a, p->b) <= p->desc.reach;
			auto* volume = reinterpret_cast<Classes::ATdMovementVolume*>(p->actor);
			if (near && !p->near && !pawn->ActiveMovementVolume) pawn->ActiveMovementVolume = volume;	// Entered
			else if (!near && p->near && pawn->ActiveMovementVolume == volume) pawn->ActiveMovementVolume = nullptr;	// Left
			p->near = near;
		}
	}
}
