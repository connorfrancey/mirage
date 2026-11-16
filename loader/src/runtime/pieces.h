#pragma once
// The game's traversal pieces from mods (step 7 of the next ten steps, the plan's step 21): swing poles and ziplines.
//
// The survey (the decompiled TdGame script and the natives): a swing pole is a TdSwingVolume, a zipline a
// TdZiplineVolume, both TdMovementVolumes, which are brush volumes. Neither move needs the brush. The pawn's tick calls
// ActiveMovementVolume.PawnUpdate, which tries the move (TdMove_Swing, TdMove_IntoZipLine); PawnEnteredVolume is all
// that sets ActiveMovementVolume. The swing reads only the volume's Location (the bar's middle), Rotation (X: the
// swing direction, Y: the bar) and its two flags. The zipline reads MoveDirection, LandingStrip and its spline:
// SplineLocations (NumSplineSegments + 1 points) through the natives FindClosestPointOnDSpline, GetLocationOnSpline and
// GetSlopeOnSpline, which read nothing else (FindClosestPointOnDSpline read in the executable). The editor computes
// the spline when a level is built; here it is filled from the piece's two ends.
//
// So a piece is its volume class spawned without a brush and without collision (Brush makes it bStatic and bNoDelete:
// the class defaults' bits are off for the spawn, as for triggers), and the runtime does the volume's part: each tick
// it gives the player's pawn the piece as ActiveMovementVolume when the pawn comes within `reach` of the bar or cable,
// and takes it back when the pawn goes away (unless a move took it already), as PawnEnteredVolume / PawnLeavingVolume
// do. That check is a point-to-segment distance per piece per tick.
//
// Springboards need nothing: TdMove_SpringBoard is geometry (a step 64 units high, then a ledge 80 to 148 units high
// 112 units further), so a mod places two meshes. Ladders, balance beams and ledge walks are refused for now (their
// moves read the brush or more volume state: not surveyed). Game thread.

#include <string>

namespace Classes
{
	class AActor;
}

namespace mirage::pieces
{
	enum class Kind { Swing, Zipline };

	struct Desc
	{
		Kind kind = Kind::Swing;
		float location[3]{};		// Swing: the bar's middle. Zipline: its start (the high end)
		float end[3]{};				// Zipline: its end
		int yaw = 0;				// Swing: the direction the player swings to (65536 = a full turn); the bar is across it
		float length = 200.f;		// Swing: the bar's length
		float reach = 0.f;			// How near the pawn has to come to the bar or cable (0: 150 for a swing, 120 for a zipline)
		float landingStrip = 0.f;	// Zipline: no getting on this near the end (0: the game's 500)
		int segments = 10;			// Zipline: spline segments (the game's default)
		bool thickGrip = true;		// Swing: the wide grip animation (the game's default)
		bool snapToCenter = true;	// Swing: no shimmying along the bar
	};

	// Parses "swing" / "zipline"; false for anything else (`why` says what isn't supported)
	bool KindFromName(const std::string& name, Kind* out, std::string* why);
	const char* KindName(Kind kind);

	// A piece of `owner` (a mod id). nullptr and `error` if it can't be made.
	Classes::AActor* Create(const std::string& owner, const Desc& desc, std::string* error);
	// Taken out: destroyed, and the player's pawn let go of it
	void Remove(Classes::AActor* piece);
	void RemoveAll(const std::string& owner);
	// Every engine tick: the player near a piece gets it as ActiveMovementVolume; gone pieces dropped
	void Tick();
}
