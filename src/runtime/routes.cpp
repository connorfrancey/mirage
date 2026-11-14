#include "runtime/routes.h"

#include "game/game.h"
#include "game/levels.h"
#include "paths.h"
#include "runtime/mod_game.h"
#include "runtime/mods.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <utility>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace mirage::routes
{
	namespace
	{
		struct Sample
		{
			float t, x, y, z;
		};

		std::mutex g_mutex;				// Guards everything below (Tick on the game thread, the menu reads)
		std::vector<Route> g_routes;
		std::string g_signature;		// The mods the routes were read for
		State g_state;
		std::string g_pendingId;		// Loading: the route to set up once its map is there
		bool g_teleport = false;		// Ready: put the player at the start (Start, not Arm)
		bool g_inStart = false;			// Ready: the player has been inside the start's circle
		void* g_pawn = nullptr;			// The pawn the run is with (a new one: a restart, the run is off)
		std::vector<Sample> g_run;		// This run, by tick
		std::vector<Sample> g_ghost;	// The best run's
		bool g_ghostOn = true;
		bool g_levelChanged = false;	// A level loaded since the last tick with a player
		unsigned g_checkMapAt = 0;		// The tick to check the level is still the route's (0: none due)
		unsigned g_ticks = 0;
		int g_shareCount = 0;

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		fs::path Folder() { return paths::Dir() / "routes"; }
		std::string FileSafe(std::string s)
		{
			for (char& c : s)
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
			return s;
		}
		fs::path GhostFile(const Route& r) { return Folder() / "ghosts" / (FileSafe(r.mod) + "__" + FileSafe(r.name) + ".ghost"); }
		fs::path ResultsFile() { return Folder() / "results.toml"; }

		Point ReadPoint(const toml::value& v)
		{
			const auto& a = v.as_array();
			if (a.size() != 3) throw std::runtime_error("a point needs [x, y, z]");
			const auto n = [](const toml::value& x) { return x.is_floating() ? static_cast<float>(x.as_floating()) : static_cast<float>(x.as_integer()); };
			return {n(a[0]), n(a[1]), n(a[2])};
		}

		std::vector<Route> ReadAll(const std::vector<mods::Info>& list)
		{
			std::vector<Route> out;
			for (const mods::Info& mod : list) {
				if (!mod.loaded) continue;
				std::error_code ec;
				const fs::path dir = mod.dir / "routes";
				if (!fs::is_directory(dir, ec)) continue;
				for (const auto& e : fs::directory_iterator(dir, ec)) {
					if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".toml") continue;
					try {
						const toml::value root = toml::parse(e.path());
						if (!root.contains("route")) continue;
						for (const toml::value& v : toml::find<toml::array>(root, "route")) {
							Route r;
							r.mod = mod.id;
							r.map = Lower(e.path().stem().string());
							r.name = toml::find<std::string>(v, "name");
							r.start = ReadPoint(toml::find(v, "start"));
							r.finish = ReadPoint(toml::find(v, "finish"));
							r.yaw = static_cast<int>(toml::find_or<std::int64_t>(v, "yaw", 0));
							if (v.contains("checkpoints"))
								for (const toml::value& c : toml::find<toml::array>(v, "checkpoints")) r.checkpoints.push_back(ReadPoint(c));
							const auto number = [&](const char* key, float fallback) {
								if (!v.contains(key)) return fallback;
								const toml::value& x = toml::find(v, key);
								return x.is_floating() ? static_cast<float>(x.as_floating()) : static_cast<float>(x.as_integer());
							};
							r.radius = number("radius", 200.f);
							r.height = number("height", 200.f);
							if (v.contains("ratings"))
								for (const toml::value& t : toml::find<toml::array>(v, "ratings"))
									r.ratings.push_back(t.is_floating() ? t.as_floating() : static_cast<double>(t.as_integer()));
							out.push_back(std::move(r));
						}
					} catch (const std::exception& ex) {
						spdlog::warn("[mod {}] routes/{}: {}", mod.id, e.path().filename().string(), ex.what());
					}
				}
			}
			return out;
		}

		// ---- Results and ghosts ---------------------------------------------------------------------------------------
		double LoadBest(const Route& r)
		{
			std::error_code ec;
			if (!fs::is_regular_file(ResultsFile(), ec)) return -1.0;
			try {
				const toml::value root = toml::parse(ResultsFile());
				if (!root.contains(r.Id())) return -1.0;
				const toml::value& v = toml::find(root, r.Id());
				return toml::find_or<double>(v, "best", -1.0);
			} catch (const std::exception&) {
				return -1.0;
			}
		}
		// Every route's best, written with every digit (a time read back must compare equal to the run's)
		void SaveBest(const Route& r, double best)
		{
			std::error_code ec;
			fs::create_directories(Folder(), ec);
			std::map<std::string, double> all;
			if (fs::is_regular_file(ResultsFile(), ec)) {
				try {
					const toml::value root = toml::parse(ResultsFile());
					for (const auto& [key, v] : root.as_table())
						if (v.is_table()) all[key] = toml::find_or<double>(v, "best", -1.0);
				} catch (const std::exception&) {
				}
			}
			if (best < 0.0) all.erase(r.Id());		// Forgotten
			else all[r.Id()] = best;
			std::ofstream out(ResultsFile(), std::ios::binary | std::ios::trunc);
			out << "# Time-trial routes: the best time of each (memenu, runtime/routes.cpp)\n";
			for (const auto& [key, value] : all) {
				std::string quoted;
				for (const char c : key) quoted += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
				char number[40];
				std::snprintf(number, sizeof number, "%.17g", value);
				out << "\n[\"" << quoted << "\"]\nbest = " << number << "\n";
			}
		}
		std::vector<Sample> LoadGhost(const Route& r)
		{
			std::vector<Sample> out;
			std::ifstream in(GhostFile(r), std::ios::binary);
			Sample s;
			while (in.read(reinterpret_cast<char*>(&s), sizeof s)) out.push_back(s);
			return out;
		}
		void SaveGhost(const Route& r, const std::vector<Sample>& run)
		{
			std::error_code ec;
			fs::create_directories(GhostFile(r).parent_path(), ec);
			std::ofstream out(GhostFile(r), std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(run.data()), static_cast<std::streamsize>(run.size() * sizeof(Sample)));
		}
		Point GhostAt(double t)
		{
			if (g_ghost.empty()) return {};
			const auto it = std::lower_bound(g_ghost.begin(), g_ghost.end(), t, [](const Sample& s, double v) { return s.t < v; });
			if (it == g_ghost.begin()) return {it->x, it->y, it->z};
			if (it == g_ghost.end()) return {g_ghost.back().x, g_ghost.back().y, g_ghost.back().z};
			const Sample& a = *(it - 1);
			const Sample& b = *it;
			const float f = b.t > a.t ? static_cast<float>((t - a.t) / (b.t - a.t)) : 0.f;
			return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
		}

		// The speedrun timer follows the run: "<counter> <what>\t<route>\t<segment>\t..." on the menu's share
		void Tell(const char* what)
		{
			std::string text = std::to_string(++g_shareCount) + " " + what + "\t" + g_state.route.name;
			for (std::size_t i = 0; i < g_state.route.checkpoints.size(); ++i) text += "\tCheckpoint " + std::to_string(i + 1);
			text += "\tFinish";
			mods::Share("route.event", text);
		}

		bool Inside(const Classes::FVector& at, const Point& p, const Route& r)
		{
			const float dx = at.X - p.x, dy = at.Y - p.y;
			return dx * dx + dy * dy <= r.radius * r.radius && std::fabs(at.Z - p.z) <= r.height;
		}

		// Ready at the route's start (the player moved there for Start)
		void SetUp(const Route& r, bool teleport)
		{
			g_state = State{};
			g_state.route = r;
			g_state.phase = Phase::Ready;
			g_state.best = LoadBest(r);
			g_ghost = LoadGhost(r);
			g_state.ghost = !g_ghost.empty();
			g_teleport = teleport;
			g_inStart = false;
			g_run.clear();
			g_pawn = nullptr;
			spdlog::info("Routes: {} ready ({} checkpoint(s), best {})", r.Id(), r.checkpoints.size(), g_state.best);
		}

		const Route* Find(const std::string& id)
		{
			for (const Route& r : g_routes)
				if (r.Id() == id) return &r;
			return nullptr;
		}

		void Refresh()
		{
			const std::vector<mods::Info> list = mods::List();
			std::string signature;
			for (const mods::Info& m : list) {
				if (!m.loaded) continue;
				signature += m.id + ":" + std::to_string(m.reloads) + ";";
				// Route files changed (they're not part of a mod's hot reload: read again here)
				std::error_code ec;
				for (const auto& e : fs::directory_iterator(m.dir / "routes", ec))
					signature += std::to_string(e.last_write_time(ec).time_since_epoch().count()) + ",";
			}
			if (signature == g_signature) return;
			g_signature = signature;
			std::vector<Route> routes = ReadAll(list);
			std::lock_guard lock(g_mutex);
			g_routes = std::move(routes);
			if (g_state.phase != Phase::Idle && !Find(g_state.route.Id())) {		// Its mod went
				g_state = State{};
				g_pendingId.clear();
			}
		}
	}

	std::vector<Route> List()
	{
		std::lock_guard lock(g_mutex);
		return g_routes;
	}

	bool Start(const std::string& id, std::string* why)
	{
		Route route;
		{
			std::lock_guard lock(g_mutex);
			const Route* r = Find(id);
			if (!r) {
				if (why) *why = "no route " + id;
				return false;
			}
			route = *r;
		}
		game::RunOnGameThread([route] {
			if (Lower(game::MapName()) == route.map) {
				std::lock_guard lock(g_mutex);
				SetUp(route, true);
				return;
			}
			// Another level: its map first (the level select's entry, at its start)
			const auto maps = levels::Maps();
			int index = -1;
			for (int i = 0; i < static_cast<int>(maps.size()); ++i)
				if (Lower(maps[i].file) == route.map) index = i;
			std::lock_guard lock(g_mutex);
			if (index < 0) {
				spdlog::warn("Routes: {} is in {}, which isn't in the level list", route.Id(), route.map);
				return;
			}
			g_state = State{};
			g_state.route = route;
			g_state.phase = Phase::Loading;
			g_pendingId = route.Id();
			levels::Load(index, 0, levels::Method::Game, false);
		});
		return true;
	}

	bool Arm(const std::string& id, std::string* why)
	{
		Route route;
		{
			std::lock_guard lock(g_mutex);
			const Route* r = Find(id);
			if (!r) {
				if (why) *why = "no route " + id;
				return false;
			}
			route = *r;
		}
		game::RunOnGameThread([route] {
			std::lock_guard lock(g_mutex);
			SetUp(route, false);
		});
		return true;
	}

	void Stop()
	{
		game::RunOnGameThread([] {
			std::lock_guard lock(g_mutex);
			if (g_state.phase != Phase::Idle) Tell("stop");		// The speedrun timer back to its own category
			g_state = State{};
			g_pendingId.clear();
		});
	}

	State Current()
	{
		std::lock_guard lock(g_mutex);
		return g_state;
	}

	void Forget(const std::string& id)
	{
		std::lock_guard lock(g_mutex);
		const Route* r = Find(id);
		if (!r) return;
		SaveBest(*r, -1.0);
		std::error_code ec;
		fs::remove(GhostFile(*r), ec);
		if (g_state.route.Id() == id) {
			g_state.best = -1.0;
			g_state.ghost = false;
			g_ghost.clear();
		}
	}

	void SetGhost(bool on)
	{
		std::lock_guard lock(g_mutex);
		g_ghostOn = on;
	}
	bool GhostOn()
	{
		std::lock_guard lock(g_mutex);
		return g_ghostOn;
	}

	void OnLevelLoaded()
	{
		std::lock_guard lock(g_mutex);
		g_levelChanged = true;		// Checked on the next tick with a player: the map's name isn't there yet
	}

	void Tick(float seconds)
	{
		if (g_ticks++ % 60 == 0) Refresh();		// Mods loading, unloading, reloading
		auto* pawn = game::Pawn();
		std::lock_guard lock(g_mutex);
		if (g_state.phase == Phase::Idle || !pawn) return;

		// Another level loaded: a route of another map is off (one waiting for its map to load is set up below). Looked
		// at half a second later: the game's world and its name lag behind the load for a few ticks.
		if (std::exchange(g_levelChanged, false)) g_checkMapAt = g_ticks + 30;
		if (g_checkMapAt && g_ticks >= g_checkMapAt && !game::IsLoading()) {
			g_checkMapAt = 0;
			if (g_state.phase != Phase::Loading && Lower(game::MapName()) != g_state.route.map) {
				spdlog::info("Routes: {} is off (the level is {} now)", g_state.route.Id(), game::MapName());
				Tell("stop");
				g_state = State{};
				return;
			}
		}

		if (g_state.phase == Phase::Loading) {
			if (game::IsLoading() || Lower(game::MapName()) != g_state.route.map) return;
			const Route* r = Find(g_pendingId);
			g_pendingId.clear();
			if (r) SetUp(*r, true);
			else g_state = State{};
			return;
		}

		// A new pawn: a checkpoint restart or a death. The run is off; at the start again.
		if (g_pawn && pawn != g_pawn && g_state.phase != Phase::Finished) {
			if (g_state.phase == Phase::Running) Tell("reset");
			const Route r = g_state.route;
			const double best = g_state.best;
			SetUp(r, false);
			g_state.best = best;
		}
		g_pawn = pawn;
		const Route& r = g_state.route;

		if (g_state.phase == Phase::Ready) {
			if (g_teleport) {
				g_teleport = false;
				pawn->Location = {r.start.x, r.start.y, r.start.z};
				pawn->Velocity = {};
				if (auto* pc = game::Controller()) pc->Rotation.Yaw = r.yaw;
				pawn->Rotation.Yaw = r.yaw;
			}
			const bool inside = Inside(pawn->Location, r.start, r);
			if (inside) g_inStart = true;
			else if (g_inStart) {		// Left the start: the clock runs
				g_state.phase = Phase::Running;
				g_state.time = 0.0;
				g_run.clear();
				Tell("start");
				spdlog::info("Routes: {} started", r.Id());
			}
			return;
		}

		if (g_state.phase == Phase::Finished) {		// Back at the start: another run
			if (Inside(pawn->Location, r.start, r)) {
				const int runs = g_state.runs;
				SetUp(r, false);
				g_state.runs = runs;
				g_inStart = true;
			}
			return;
		}

		if (g_state.phase == Phase::Running) {
			g_state.time += seconds;
			g_run.push_back({static_cast<float>(g_state.time), pawn->Location.X, pawn->Location.Y, pawn->Location.Z});
			if (g_state.ghost) g_state.ghostAt = GhostAt(g_state.time);
			const std::size_t next = g_state.splits.size();
			if (next < r.checkpoints.size()) {
				if (Inside(pawn->Location, r.checkpoints[next], r)) {
					g_state.splits.push_back(g_state.time);
					Tell("split");
					spdlog::info("Routes: {} checkpoint {} at {:.3f} s", r.Id(), next + 1, g_state.time);
				}
			} else if (Inside(pawn->Location, r.finish, r)) {
				g_state.splits.push_back(g_state.time);
				g_state.phase = Phase::Finished;
				++g_state.runs;
				Tell("finish");
				g_state.stars = 0;
				for (const double t : r.ratings)
					if (g_state.time <= t) ++g_state.stars;
				g_state.newBest = g_state.best < 0.0 || g_state.time < g_state.best;
				if (g_state.newBest) {
					g_state.best = g_state.time;
					SaveBest(r, g_state.time);
					SaveGhost(r, g_run);
					g_ghost = g_run;
				}
				spdlog::info("Routes: {} finished in {:.3f} s{}", r.Id(), g_state.time, g_state.newBest ? " (best)" : "");
			}
		}
	}
}
