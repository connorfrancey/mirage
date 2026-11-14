#pragma once
// Time-trial routes (Mirage, step 13): a mod's routes/<map>.toml defines runs through a level, the game's or a mod's
// map ("routes/edge_p.toml", "routes/miragetestmap.toml"):
//
//     [[route]]
//     name = "Rooftop dash"
//     start = [19309.0, -3009.0, 8420.0]          # the start: the clock starts when the player leaves its circle
//     yaw = 16384                                 # facing there when the menu puts the player at the start
//     checkpoints = [[19800.0, -3009.0, 8420.0], [20400.0, -2900.0, 8420.0]]   # in order
//     finish = [21000.0, -2800.0, 8420.0]
//     radius = 200.0                              # how near counts, around (default 200)
//     height = 200.0                              # and above or below (default 200)
//     ratings = [12.0, 10.0, 8.0]                 # one, two and three star times (optional)
//
// A run's time is game time (the engine's ticks), so a TAS replays it to the same time. Each checkpoint is a split and
// the finish ends the run; the speedrun timer follows (its category "TT - <route>", one segment per checkpoint and the
// finish). The best time of each route and its run (the ghost: where the player was, by time) are kept in
// <mirage>\routes. Game thread unless said otherwise.

#include <string>
#include <vector>

namespace mirage::routes
{
	struct Point
	{
		float x = 0.f, y = 0.f, z = 0.f;
	};

	struct Route
	{
		std::string mod, map, name;			// map: the package name, lower case
		Point start, finish;
		int yaw = 0;
		std::vector<Point> checkpoints;
		float radius = 200.f, height = 200.f;
		std::vector<double> ratings;
		std::string Id() const { return mod + "/" + map + "/" + name; }	// Unique: the same name may be on several maps
	};

	enum class Phase
	{
		Idle,		// No route
		Loading,	// Its map is loading (Start from another level)
		Ready,		// At (or on the way to) the start: the clock starts when the player leaves the start's circle
		Running,
		Finished
	};

	struct State
	{
		Phase phase = Phase::Idle;
		Route route;
		double time = 0.0;					// Game seconds into the run (the final time once finished)
		std::vector<double> splits;			// Time at each checkpoint reached, then the finish
		double best = -1.0;					// The route's best time (< 0: none yet)
		bool newBest = false;				// The finished run is the best
		bool ghost = false;					// A ghost of the best run is there to race
		Point ghostAt;						// Where the ghost is now (valid while running with `ghost`)
		int stars = 0;						// Of the finished run, from the ratings
		int runs = 0;						// Runs finished since the menu started
	};

	// Every loaded mod's routes (read again when mods load, unload or reload). Any thread.
	std::vector<Route> List();
	// To the start of `id` ("mod/map/name"): loads its map first when the player is in another one, puts the player there.
	// Any thread (runs on the game thread). False and `why` if there's no such route.
	bool Start(const std::string& id, std::string* why = nullptr);
	// Waits at the start of `id` without moving the player (a TAS that starts at a checkpoint, then runs the route).
	// Any thread.
	bool Arm(const std::string& id, std::string* why = nullptr);
	void Stop();						// Any thread
	State Current();					// Any thread
	void Forget(const std::string& id);	// Its best time and ghost gone. Any thread.
	void SetGhost(bool on);				// Race the best run's ghost (on by default). Any thread.
	bool GhostOn();

	void Tick(float seconds);			// Every engine tick (game seconds)
	void OnLevelLoaded();
}
