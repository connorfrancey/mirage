#include "runtime/mem/manager.h"

#include "core/cache_evict.h"
#include "core/cache_sched.h"
#include "core/events.h"
#include "paths.h"
#include "runtime/mem/arena.h"
#include "runtime/mem/pool.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace mirage::mem
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The real clock (heap.h): the game-thread job and the monitor thread compare times, and on the game thread during a
		// TAS session steady_clock runs on the replay's clock
		double Seconds()
		{
			static const std::uint64_t start = RealTimeMs();
			return static_cast<double>(RealTimeMs() - start) / 1000.0;
		}

		std::string Mb(double bytes)
		{
			char buf[32];
			std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
			return buf;
		}
		std::string Mb(std::uint64_t bytes) { return Mb(static_cast<double>(bytes)); }
		std::string Mb(std::int64_t bytes) { return Mb(static_cast<double>(bytes)); }

		struct Handler
		{
			int id;
			std::string name;
			int priority;
			std::function<std::size_t(Level, bool)> fn;
		};

		// A stack's live bytes over time (leak suspects): bounded to the biggest stacks
		struct SiteTrend
		{
			Trend trend{30};
			std::vector<std::uintptr_t> frames;
			TagId tag = 0;
			double lastSeen = 0.0;
		};

		struct State
		{
			std::mutex lock;			// Config, status, handlers, findings (the step itself runs on one thread)
			Config config;
			bool configDirty = true;
			Status status;
			std::vector<Handler> handlers;
			int nextHandler = 1;
			std::function<std::vector<std::pair<std::string, std::size_t>>()> modMemory;
			std::deque<Finding> recent;

			// The monitor thread only: measuring and reasoning (never on the game thread)
			std::thread monitor;
			std::mutex wakeLock;
			std::condition_variable wake;
			bool stopping = false;					// Under wakeLock
			double nextProfile = 0.0;
			AddressSpaceWalker walker;
			AnomalyDetector menu{"menu heap"};
			PressurePolicy pressure;
			Trend largestFree{60};
			std::map<std::uint32_t, SiteTrend> sites;
			std::atomic<bool> monitorFailed{false};
			std::atomic<bool> resetTrend{false};	// Set by a level load: the largest-free trend starts over

			// What it learns about levels: the monitor thread and the game thread (level loads)
			std::mutex learn;
			Predictor predictor;
			std::string map;
			bool profilesDirty = false;
			bool forcedElevated = false;

			// The game thread only (the job: acting on the caches, which are the game thread's)
			bool started = false;
			int job = -1;
			double nextAct = 0.0, nextActionRepeat = 0.0;
			BudgetController budget{16u << 20};
			Level acted = Level::Normal;
			std::uint64_t actions = 0, released = 0;
			std::string lastAction;
			bool acting = true;
			bool failed = false;

			// Requests from any thread
			std::atomic<bool> trimRequested{false};
			std::atomic<int> simulatedLevel{0};
			std::atomic<double> simulatedUntil{0.0};
		};

		State& S()
		{
			static State* s = new State();		// Never destroyed: allocations run until the DLL is gone
			return *s;
		}

		std::filesystem::path ProfilePath() { return paths::Dir() / "cache" / "memory_profiles.json"; }

		void LoadProfiles(Predictor& p)
		{
			std::ifstream in(ProfilePath());
			if (!in) return;
			const auto j = nlohmann::json::parse(in, nullptr, false);
			if (!j.is_object()) return;
			for (const auto& [level, v] : j.items()) {
				LevelProfile lp;
				lp.peakPrivate = v.value("peakPrivate", std::uint64_t{0});
				lp.minLargestFree = v.value("minLargestFree", std::uint64_t{0});
				lp.visits = v.value("visits", 0);
				p.Set(level, lp);
			}
		}

		void SaveProfiles(const Predictor& p)
		{
			nlohmann::json j = nlohmann::json::object();
			for (const auto& [level, lp] : p.All())
				j[level] = {{"peakPrivate", lp.peakPrivate}, {"minLargestFree", lp.minLargestFree}, {"visits", lp.visits}};
			std::error_code ec;
			std::filesystem::create_directories(ProfilePath().parent_path(), ec);
			const auto tmp = ProfilePath().string() + ".tmp";
			{
				std::ofstream out(tmp, std::ios::trunc);
				if (!out) return;
				out << j.dump(1, '\t');
			}
			std::filesystem::rename(tmp, ProfilePath(), ec);
		}

		void Note(State& s, Finding f, bool log)
		{
			if (log) {
				const std::string line = "Memory: " + f.subject + ": " + f.text;
				if (f.severity == Finding::Severity::Warning) spdlog::warn("{}", line);
				else spdlog::info("{}", line);
			}
			std::lock_guard<std::mutex> l(s.lock);
			s.recent.push_back(std::move(f));
			while (s.recent.size() > 50) s.recent.pop_front();
		}

		std::string Frames(const std::uintptr_t* frames, std::size_t n, std::size_t max = 6)
		{
			std::string out;
			for (std::size_t i = 0; i < n && i < max; ++i) {
				if (i) out += " < ";
				out += sampler::Describe(frames[i]);
			}
			return out;
		}

		// ---- One sample ------------------------------------------------------------------------------------------------
		void Sample(State& s, const Config& cfg, double now)
		{
			const ProcessSample proc = ReadProcess();
			const HeapCounters heap = ReadHeap();
			const AddressSpace& addr = s.walker.Result();

			// Anomalies in the menu's heap
			for (Finding& f : s.menu.Add({now, heap.allocations, heap.bytesAllocated, heap.live}))
				Note(s, std::move(f), cfg.logFindings);

			// Large allocations, with where they came from
			LargeAllocation large[8];
			const int n = DrainLarge(large, 8);
			for (int i = 0; i < n; ++i) {
				Finding f{Finding::Kind::LargeAllocation, Finding::Severity::Info, TagName(large[i].tag),
						  "allocated " + Mb(static_cast<std::uint64_t>(large[i].bytes)) + " at once: " +
							  Frames(large[i].frames, static_cast<std::size_t>(large[i].depth))};
				Note(s, std::move(f), cfg.logFindings);
			}

			// Pressure (a simulation for the self-test takes the place of the real level while it lasts)
			PressureInputs in;
			if (addr.complete) {
				in.addressFree = addr.free.totalFree;
				in.largestFree = addr.free.largestFree;
			} else if (proc.ok) {
				in.addressFree = proc.addressAvailable;
			}
			in.commitAvailable = proc.ok ? proc.commitAvailable : 0;
			in.menuLive = heap.live;
			in.menuBudget = static_cast<std::int64_t>(cfg.menuBudget);
			const Level before = s.pressure.Current();
			Level level = s.pressure.Add(in);
			if (now < S().simulatedUntil.load()) level = static_cast<Level>(S().simulatedLevel.load());

			// Prediction: the largest free block's trend, and what this level needed before
			std::string map;
			bool forcedElevated;
			{
				std::lock_guard<std::mutex> l(s.learn);
				if (addr.complete) {
					s.predictor.Observe(s.map, proc.privateBytes, addr.free.largestFree);
					s.profilesDirty = true;
				}
				map = s.map;
				forcedElevated = s.forcedElevated;
			}
			if (s.resetTrend.exchange(false)) s.largestFree.Clear();
			if (addr.complete) s.largestFree.Add(now, static_cast<double>(addr.free.largestFree));
			std::optional<double> toExhaustion;
			if (addr.complete)
				toExhaustion = Predictor::TimeToLimit(s.largestFree, static_cast<double>(addr.free.largestFree),
													   static_cast<double>(PressureRules{}.largest[3]));
			if (toExhaustion && *toExhaustion < 120.0 && level < Level::Elevated) {
				char buf[160];
				std::snprintf(buf, sizeof buf, "the largest free block is shrinking: at this rate it reaches the critical size in %.0f s",
							  *toExhaustion);
				static double lastWarned = -1e9;
				if (now - lastWarned > 120.0) {
					lastWarned = now;
					Note(s, {Finding::Kind::Prediction, Finding::Severity::Warning, "address space", buf}, cfg.logFindings);
				}
				level = Level::Elevated;			// Act early, gently
			}
			if (forcedElevated && level < Level::Elevated) level = Level::Elevated;

			if (level != before && now >= S().simulatedUntil.load()) {
				const std::string text = std::string(level > before ? "rose" : "fell") + " to " + LevelName(level) + ": " + s.pressure.Reason();
				Note(s, {Finding::Kind::Pressure, level >= Level::High ? Finding::Severity::Warning : Finding::Severity::Info,
						 "pressure", text}, true);
			}

			std::lock_guard<std::mutex> l(s.lock);
			s.status.level = level;
			s.status.reason = s.pressure.Reason();
			s.status.process = proc;
			if (addr.complete) s.status.address = addr;
			s.status.heap = heap;
			s.status.allocRate = s.menu.Rate();
			s.status.byteRate = s.menu.ByteRate();
			s.status.map = map;
			s.status.secondsToExhaustion = toExhaustion;
		}

		// Leak suspects: the stacks whose live bytes only grow (every 10 s, from the sampler)
		void Profile(State& s, const Config& cfg, double now)
		{
			const sampler::Report rep = sampler::Snapshot(0, 32);
			for (const sampler::Site& site : rep.sites) {
				SiteTrend& t = s.sites[site.stack];
				if (t.frames.empty()) {
					t.frames = site.frames;
					t.tag = site.tag;
				}
				t.trend.Add(now, static_cast<double>(site.liveBytes));
				t.lastSeen = now;
				// Growing over five minutes in a straight line, by at least 4 MB: reported once in a while
				if (t.trend.Full() && t.trend.Span() >= 280.0 && t.trend.Slope() > 0.0 && t.trend.R2() > 0.9 &&
					t.trend.Rise() >= 4.0 * 1024 * 1024) {
					static std::map<std::uint32_t, double> told;
					if (now - told[site.stack] > 600.0) {
						told[site.stack] = now;
						Note(s, {Finding::Kind::SteadyGrowth, Finding::Severity::Warning, TagName(site.tag),
								 "leak suspect: " + Mb(t.trend.Rise()) + " more live in " + std::to_string(static_cast<int>(t.trend.Span())) +
									 " s from " + Frames(site.frames.data(), site.frames.size())},
							 cfg.logFindings);
					}
				}
			}
			// Forget stacks not seen for a while (bounded memory)
			for (auto it = s.sites.begin(); it != s.sites.end();) {
				if (now - it->second.lastSeen > 120.0) it = s.sites.erase(it);
				else ++it;
			}
		}

		// ---- Acting ------------------------------------------------------------------------------------------------------
		void Act(State& s, const Config& cfg, double now)
		{
			Level level;
			{
				std::lock_guard<std::mutex> l(s.lock);
				level = s.status.level;
			}
			// The cache budget: adapted every sample, within the settings' floor and ceiling
			auto& ev = cache::Evictions();
			std::uint64_t hits = 0, trims = 0;
			for (const auto& v : ev.Snapshot()) {
				hits += v.hits;
				trims += v.trims;
			}
			BudgetRules rules;
			rules.min = cfg.cacheMin;
			rules.max = std::max(cfg.cacheMin, cfg.cacheMax);
			s.budget.SetRules(rules);
			std::string why;
			const std::size_t budget = s.budget.Decide({now, level, ev.Total(), hits, trims}, &why);
			if (budget != ev.Budget()) {
				ev.SetBudget(budget);
				if (!why.empty()) Note(s, {Finding::Kind::Pressure, Finding::Severity::Info, "caches", why + " (" + Mb(static_cast<std::uint64_t>(budget)) + ")"}, cfg.logFindings);
			}

			const bool trimRequested = s.trimRequested.exchange(false);
			const bool changed = level != s.acted;
			const bool repeat = level >= Level::High && now >= s.nextActionRepeat;
			if (!changed && !repeat && !trimRequested) return;
			const bool rising = level > s.acted;
			const Level previous = s.acted;
			s.acted = level;
			if (changed) events::Bus().Publish(events::MemoryPressure{static_cast<int>(level), static_cast<int>(previous)});
			if (level < Level::High && !trimRequested && !changed) return;

			std::size_t freed = 0;
			std::string what;
			if (level >= Level::High || trimRequested) {
				// Caches: at Critical (or asked) trimmed now; at High the eviction job trims them to the halved budget
				// over its next passes, so no single frame pays for all of it
				if (level == Level::Critical || trimRequested) {
					const auto before = ev.Total();
					ev.Enforce();
					freed += before > ev.Total() ? before - ev.Total() : 0;
				}
				freed += pools::TrimAll();		// Also asks unsynchronized pools and scratch arenas to trim themselves
				what = level == Level::Critical || trimRequested ? "caches, pools and arenas" : "pools and arenas (caches follow)";
				s.nextActionRepeat = now + 5.0;
			}
			// Handlers (mods, other subsystems): on every change of level, and while it stays High or Critical
			std::vector<Handler> handlers;
			{
				std::lock_guard<std::mutex> l(s.lock);
				handlers = s.handlers;
			}
			std::sort(handlers.begin(), handlers.end(), [](const Handler& a, const Handler& b) { return a.priority > b.priority; });
			for (const Handler& h : handlers) {
				try {
					freed += h.fn(level, rising);
				} catch (const std::exception& e) {
					spdlog::warn("Memory: pressure handler {} failed: {}", h.name, e.what());
				}
			}
			if (!handlers.empty()) what += what.empty() ? "handlers" : " and handlers";
			++s.actions;
			s.released += freed;
			s.lastAction = std::string(trimRequested ? "trim requested" : LevelName(level)) + ": " + what + ", " + Mb(static_cast<std::uint64_t>(freed)) + " given back";
			if (level >= Level::High || trimRequested) spdlog::info("Memory: {}", s.lastAction);
		}

		// ---- The monitor thread: measuring and reasoning, off the game thread ---------------------------------------
		// Read-only queries (VirtualQuery, GetProcessMemoryInfo, the sampler's table): nothing it does can stall the game.
		void MonitorLoop()
		{
			State& s = S();
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			SetThreadDescription(GetCurrentThread(), L"mirage memory monitor");
			for (;;) {
				Config cfg;
				{
					std::lock_guard<std::mutex> l(s.lock);
					cfg = s.config;
				}
				const double t0 = NowNs();
				try {
					while (!s.walker.Step(1e9)) {}		// A whole walk: a few ms, here where it costs the game nothing
					const double now = Seconds();
					Sample(s, cfg, now);
					if (now >= s.nextProfile && sampler::CurrentMode() != sampler::Mode::Off) {
						s.nextProfile = now + 10.0;
						Profile(s, cfg, now);
					}
				} catch (const std::exception& e) {
					// Safe fallback: no more measuring, and nothing acts on stale numbers
					s.monitorFailed = true;
					{
						std::lock_guard<std::mutex> l(s.lock);
						s.status.level = Level::Normal;
					}
					spdlog::error("Memory: the monitor stopped after an error ({}); the manager only watches from now on", e.what());
					return;
				}
				const double ms = (NowNs() - t0) / 1e6;
				{
					std::lock_guard<std::mutex> l(s.lock);
					s.status.monitorMs = ms;
					s.status.maxMonitorMs = std::max(s.status.maxMonitorMs, ms);
				}
				std::unique_lock<std::mutex> wl(s.wakeLock);
				if (s.wake.wait_for(wl, std::chrono::duration<double>(std::max(cfg.interval, 0.25)), [&] { return s.stopping; })) return;
			}
		}

		// ---- The game thread's job: settings, the mods' Lua memory, acting on the caches ------------------------------
		bool GameStep(double)
		{
			State& s = S();
			if (s.failed) return true;
			const double t0 = NowNs();
			Config cfg;
			bool dirty;
			std::function<std::vector<std::pair<std::string, std::size_t>>()> modMemory;
			{
				std::lock_guard<std::mutex> l(s.lock);
				cfg = s.config;
				dirty = s.configDirty;
				s.configDirty = false;
				modMemory = s.modMemory;
			}
			try {
				if (dirty) {
					sampler::SetMode(cfg.tracking, cfg.sampleInterval);
					SetLargeThreshold(cfg.largeAllocation);
				}
				s.acting = cfg.act && !s.monitorFailed.load();
				const double now = Seconds();
				const bool due = now >= s.nextAct;
				if (!due && !s.trimRequested.load()) return true;
				const double t1 = NowNs();
				if (due) {
					s.nextAct = now + std::max(cfg.interval, 0.25);
					auto modLua = modMemory ? modMemory() : std::vector<std::pair<std::string, std::size_t>>{};
					std::lock_guard<std::mutex> l(s.lock);
					s.status.modLua = std::move(modLua);
				}
				const double t2 = NowNs();
				if (s.acting) Act(s, cfg, now);
				else s.trimRequested = false;
				const double t3 = NowNs();
				const double ms = (t3 - t0) / 1e6;
				std::lock_guard<std::mutex> l(s.lock);
				s.status.stepParts[0] = (t1 - t0) / 1e6;
				s.status.stepParts[1] = (t2 - t1) / 1e6;
				s.status.stepParts[2] = (t3 - t2) / 1e6;
				s.status.stepMs = ms;
				s.status.maxStepMs = std::max(s.status.maxStepMs, ms);
				s.status.avgStepMs = s.status.avgStepMs == 0.0 ? ms : s.status.avgStepMs + 0.05 * (ms - s.status.avgStepMs);
				s.status.cacheBudget = cache::Evictions().Budget();
				s.status.cacheBytes = cache::Evictions().Total();
				s.status.actions = s.actions;
				s.status.bytesReleased = s.released;
				s.status.lastAction = s.lastAction;
				s.status.acting = s.acting;
				s.status.recent.assign(s.recent.begin(), s.recent.end());
				return true;
			} catch (const std::exception& e) {
				// Safe fallback: stop acting, keep the game going; the monitor keeps measuring
				s.acting = false;
				s.failed = true;
				spdlog::error("Memory: the manager stopped acting after an error ({}); nothing is changed by it from now on", e.what());
				return true;
			}
		}
	}

	// ---- Public ----------------------------------------------------------------------------------------------------------
	void Configure(const Config& config)
	{
		State& s = S();
		std::lock_guard<std::mutex> l(s.lock);
		const bool changed = config.act != s.config.act || config.tracking != s.config.tracking ||
							 config.sampleInterval != s.config.sampleInterval || config.cacheMin != s.config.cacheMin ||
							 config.cacheMax != s.config.cacheMax || config.menuBudget != s.config.menuBudget ||
							 config.largeAllocation != s.config.largeAllocation || config.logFindings != s.config.logFindings ||
							 config.interval != s.config.interval;
		if (!changed) return;
		s.config = config;
		s.configDirty = true;
	}

	Config Configured()
	{
		std::lock_guard<std::mutex> l(S().lock);
		return S().config;
	}

	void Start()
	{
		State& s = S();
		if (s.started) return;
		s.started = true;
		std::size_t profiled;
		{
			std::lock_guard<std::mutex> l(s.learn);
			LoadProfiles(s.predictor);
			profiled = s.predictor.All().size();
		}
		{
			std::lock_guard<std::mutex> l(s.lock);
			sampler::SetMode(s.config.tracking, s.config.sampleInterval);
			SetLargeThreshold(s.config.largeAllocation);
		}
		s.budget = BudgetController(cache::Evictions().Budget());
		s.job = cache::Maintenance().Add({"Memory manager", 0.25, 4, [](double budgetMs) { return GameStep(budgetMs); }});
		{
			std::lock_guard<std::mutex> l(s.wakeLock);
			s.stopping = false;
		}
		s.monitor = std::thread(MonitorLoop);
		spdlog::info("Memory: manager started (sampling every {} KB, {} profiled levels)", sampler::Interval() / 1024, profiled);
	}

	void Stop()
	{
		State& s = S();
		if (!s.started) return;
		s.started = false;
		{
			std::lock_guard<std::mutex> l(s.wakeLock);
			s.stopping = true;
		}
		s.wake.notify_all();
		if (s.monitor.joinable()) s.monitor.join();		// Never left running past the DLL
		if (s.job >= 0) cache::Maintenance().Remove(s.job);
		s.job = -1;
		{
			std::lock_guard<std::mutex> l(s.learn);
			if (!s.map.empty()) s.predictor.EndVisit(s.map);
			SaveProfiles(s.predictor);
		}
		const HeapCounters h = ReadHeap();
		spdlog::info("Memory: menu heap at unload {} live (peak {}), {} allocations; {} pressure actions gave back {}",
					 Mb(h.live), Mb(h.peakLive), h.allocations, s.actions, Mb(s.released));
	}

	void OnLevelLoad(bool finished, const std::string& map)
	{
		State& s = S();
		std::optional<LevelProfile> careful;
		{
			std::lock_guard<std::mutex> l(s.learn);
			if (!finished) {
				if (!s.map.empty()) s.predictor.EndVisit(s.map);
				if (s.profilesDirty) {
					SaveProfiles(s.predictor);
					s.profilesDirty = false;
				}
				s.map.clear();
				s.resetTrend = true;		// A load reshapes the address space: a new trend (the monitor's)
				return;
			}
			s.map = map;
			s.forcedElevated = false;
			// What this level needed before: if it took the largest free block close to the High limit, start careful
			if (const auto p = s.predictor.Profile(map); p && p->minLargestFree &&
				p->minLargestFree < PressureRules{}.largest[static_cast<int>(Level::High)] + (64ull << 20)) {
				s.forcedElevated = true;
				careful = p;
			}
		}
		if (careful)
			Note(s, {Finding::Kind::Prediction, Finding::Severity::Info, map,
					 "last time the largest free block went down to " + Mb(careful->minLargestFree) + " here: menu caches kept smaller in this level"},
				 Configured().logFindings);
	}

	int AddPressureHandler(std::string name, int priority, std::function<std::size_t(Level, bool)> fn)
	{
		State& s = S();
		std::lock_guard<std::mutex> l(s.lock);
		const int id = s.nextHandler++;
		s.handlers.push_back({id, std::move(name), priority, std::move(fn)});
		return id;
	}

	void RemovePressureHandler(int id)
	{
		State& s = S();
		std::lock_guard<std::mutex> l(s.lock);
		s.handlers.erase(std::remove_if(s.handlers.begin(), s.handlers.end(), [id](const Handler& h) { return h.id == id; }), s.handlers.end());
	}

	void SetModMemoryProvider(std::function<std::vector<std::pair<std::string, std::size_t>>()> fn)
	{
		std::lock_guard<std::mutex> l(S().lock);
		S().modMemory = std::move(fn);
	}

	void RequestTrim() { S().trimRequested = true; }

	void SimulatePressure(Level level, double seconds)
	{
		S().simulatedLevel = static_cast<int>(level);
		S().simulatedUntil = Seconds() + seconds;
	}

	Status GetStatus()
	{
		std::lock_guard<std::mutex> l(S().lock);
		return S().status;
	}

	// ---- Report ----------------------------------------------------------------------------------------------------------
	std::string Report(bool detailed)
	{
		const Status st = GetStatus();
		const Config cfg = Configured();
		std::string out;
		char buf[512];
		const auto line = [&](const char* fmt, auto... args) {
			std::snprintf(buf, sizeof buf, fmt, args...);
			out += buf;
			out += '\n';
		};
		const auto mb = [](double b) { return b / (1024.0 * 1024.0); };

		line("Pressure: %s%s%s", LevelName(st.level), st.reason.empty() ? "" : " - ", st.reason.c_str());
		line("Acting: %s; %llu actions gave back %.1f MB; last: %s", st.acting ? "yes" : "no (observe only)",
			 static_cast<unsigned long long>(st.actions), mb(static_cast<double>(st.bytesReleased)),
			 st.lastAction.empty() ? "none" : st.lastAction.c_str());
		if (st.secondsToExhaustion) line("Prediction: largest free block critical in %.0f s at the current trend", *st.secondsToExhaustion);
		if (!st.map.empty()) line("Level: %s", st.map.c_str());

		const ProcessSample& p = st.process;
		line("\nProcess: private %.1f MB (peak %.1f), working set %.1f MB (peak %.1f), %llu page faults", mb(static_cast<double>(p.privateBytes)),
			 mb(static_cast<double>(p.peakPrivate)), mb(static_cast<double>(p.workingSet)), mb(static_cast<double>(p.peakWorkingSet)),
			 static_cast<unsigned long long>(p.pageFaults));
		line("System: RAM %.0f of %.0f MB free (load %u%%), commit %.0f of %.0f MB free", mb(static_cast<double>(p.physicalAvailable)),
			 mb(static_cast<double>(p.physicalTotal)), p.memoryLoad, mb(static_cast<double>(p.commitAvailable)), mb(static_cast<double>(p.commitLimit)));
		const AddressSpace& a = st.address;
		if (a.complete) {
			line("Address space: %.0f MB free of %.0f MB, largest free block %.1f MB, fragmentation %.2f (%zu free regions, %zu under 1 MB)",
				 mb(static_cast<double>(a.free.totalFree)), mb(static_cast<double>(p.addressTotal)), mb(static_cast<double>(a.free.largestFree)),
				 a.free.Index(), a.free.freeRegions, a.free.smallRegions);
			line("  committed %.0f MB (private %.0f, images %.0f, mapped %.0f), reserved %.0f MB; %zu regions, walked in %.2f ms",
				 mb(static_cast<double>(a.committed)), mb(static_cast<double>(a.privateCommitted)), mb(static_cast<double>(a.image)),
				 mb(static_cast<double>(a.mapped)), mb(static_cast<double>(a.reserved)), a.regions, a.walkMs);
		}

		const HeapCounters& h = st.heap;
		line("\nMenu heap: %.2f MB live (peak %.2f), %llu allocations, %llu frees; now %.0f/s, %.2f MB/s", mb(static_cast<double>(h.live)),
			 mb(static_cast<double>(h.peakLive)), static_cast<unsigned long long>(h.allocations), static_cast<unsigned long long>(h.frees),
			 st.allocRate, mb(st.byteRate));
		std::string classes = "  by size:";
		for (int c = 0; c < kSizeClasses; ++c) {
			if (!h.sizeClass[c]) continue;
			const std::size_t limit = SizeClassLimit(c);
			if (limit == SIZE_MAX) std::snprintf(buf, sizeof buf, " larger %llu", static_cast<unsigned long long>(h.sizeClass[c]));
			else if (limit >= 1024 * 1024) std::snprintf(buf, sizeof buf, " <=%zuM %llu", limit >> 20, static_cast<unsigned long long>(h.sizeClass[c]));
			else if (limit >= 1024) std::snprintf(buf, sizeof buf, " <=%zuK %llu", limit >> 10, static_cast<unsigned long long>(h.sizeClass[c]));
			else std::snprintf(buf, sizeof buf, " <=%zu %llu", limit, static_cast<unsigned long long>(h.sizeClass[c]));
			classes += buf;
		}
		out += classes + "\n";
		for (bool alloc : {true, false}) {
			const Timing t = ReadTiming(alloc);
			line("  %s: %.0f ns average, %.0f ns max (%llu timed calls)", alloc ? "malloc" : "free", t.averageNs, t.maxNs,
				 static_cast<unsigned long long>(t.samples));
		}

		const sampler::Report rep = sampler::Snapshot(0, detailed ? 15 : 5);
		const char* modes[] = {"off", "sampled", "full"};
		line("\nProfiler: %s%s, %llu samples (%llu dropped), table %zu/%zu, stacks %zu/%zu", modes[static_cast<int>(rep.mode)],
			 rep.mode == sampler::Mode::Sampled ? (" every " + std::to_string(rep.interval / 1024) + " KB").c_str() : "",
			 static_cast<unsigned long long>(rep.samples), static_cast<unsigned long long>(rep.dropped), rep.tableUsed, rep.tableCapacity,
			 rep.stacksUsed, rep.stacksCapacity);
		line("  live (estimated): %.2f MB in %llu blocks; long-lived (over a minute): %.2f MB", mb(static_cast<double>(rep.liveBytes)),
			 static_cast<unsigned long long>(rep.liveBlocks), mb(static_cast<double>(rep.longLivedBytes)));
		double freed = 0.0;
		for (std::uint64_t v : rep.lifetime) freed += static_cast<double>(v);
		if (freed > 0.0)
			line("  freed bytes by lifetime: %.0f%% within a frame, %.0f%% < 1 s, %.0f%% < 1 min, %.0f%% < 10 min, %.0f%% longer",
				 100.0 * static_cast<double>(rep.lifetime[0]) / freed, 100.0 * static_cast<double>(rep.lifetime[1]) / freed,
				 100.0 * static_cast<double>(rep.lifetime[2]) / freed, 100.0 * static_cast<double>(rep.lifetime[3]) / freed,
				 100.0 * static_cast<double>(rep.lifetime[4]) / freed);

		line("\nTags: allocated so far / live (estimated)");
		const auto tags = ReadTags();
		for (std::size_t i = 0; i < tags.size(); ++i) {
			if (!tags[i].allocations && !rep.liveByTag[i]) continue;
			line("  %-20s %8.1f MB / %8.2f MB  (%llu allocations)", tags[i].name, mb(static_cast<double>(tags[i].bytes)),
				 mb(static_cast<double>(rep.liveByTag[i])), static_cast<unsigned long long>(tags[i].allocations));
		}

		line("\nBiggest live sites (estimated):");
		for (const sampler::Site& site : rep.sites) {
			line("  %8.2f MB  %6llu blocks  oldest %5.0f s  [%s]", mb(static_cast<double>(site.liveBytes)),
				 static_cast<unsigned long long>(site.liveBlocks), static_cast<double>(site.oldestAgeMs) / 1000.0, TagName(site.tag));
			if (!site.frames.empty()) out += "      " + Frames(site.frames.data(), site.frames.size(), detailed ? 10 : 4) + "\n";
		}

		line("\nCaches: %.2f MB of a %.2f MB budget (adaptive %.0f-%.0f MB)", mb(static_cast<double>(st.cacheBytes)),
			 mb(static_cast<double>(st.cacheBudget)), mb(static_cast<double>(cfg.cacheMin)), mb(static_cast<double>(cfg.cacheMax)));
		for (const auto& v : cache::Evictions().Snapshot())
			line("  %-28s %8.1f KB  %10llu hits  %llu trims", v.name.c_str(), static_cast<double>(v.bytes) / 1024.0,
				 static_cast<unsigned long long>(v.hits), static_cast<unsigned long long>(v.trims));

		const auto poolStats = pools::Snapshot();
		if (!poolStats.empty()) {
			line("\nPools:");
			for (const PoolStats& ps : poolStats)
				line("  %-28s %zu/%zu blocks of %zu B (peak %zu), %zu slabs, %llu fallbacks", ps.name.c_str(), ps.used, ps.capacity,
					 ps.blockSize, ps.peakUsed, ps.slabs, static_cast<unsigned long long>(ps.fallbacks));
		}
		line("\nArenas:");
		for (const ArenaStats& as : ScratchSnapshot())
			line("  %-28s %zu chunks %.0f KB, peak %.0f KB used, %llu oversized", as.name.c_str(), as.chunks,
				 static_cast<double>(as.chunkBytes) / 1024.0, static_cast<double>(as.peak) / 1024.0, static_cast<unsigned long long>(as.oversized));
		const ArenaStats ps = PersistentStats();
		line("  %-28s %.0f KB used in %zu chunks", "Persistent", static_cast<double>(ps.used) / 1024.0, ps.chunks);

		if (!st.modLua.empty()) {
			line("\nLua mods:");
			for (const auto& [id, bytes] : st.modLua) line("  %-28s %8.2f MB", id.c_str(), mb(static_cast<double>(bytes)));
		}

		if (!st.recent.empty()) {
			line("\nRecent findings:");
			for (const Finding& f : st.recent) line("  [%s] %s: %s", f.severity == Finding::Severity::Warning ? "warn" : "info",
													f.subject.c_str(), f.text.c_str());
		}
		line("\nManager: game thread %.3f ms a step (max %.3f); monitor thread %.2f ms a pass (max %.2f)", st.stepMs, st.maxStepMs,
			 st.monitorMs, st.maxMonitorMs);
		return out;
	}

	// ---- Leak check ------------------------------------------------------------------------------------------------------
	LeakCheck::LeakCheck() : thread_(GetCurrentThreadId()), saved_(Configured())
	{
		sampler::SetMode(sampler::Mode::Full);
		generation_ = sampler::Generation();
	}

	LeakCheck::~LeakCheck()
	{
		if (!done_) (void)Finish();
	}

	sampler::Report LeakCheck::Finish()
	{
		if (done_) return {};
		done_ = true;
		sampler::SetMode(saved_.tracking, saved_.sampleInterval);
		return sampler::Snapshot(generation_, 20, thread_);
	}
}
