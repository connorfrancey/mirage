#pragma once
// The memory manager: one place that watches the process's memory and the menu's own, reasons about it
// (runtime/mem/analysis.h) and, when it must, gives memory back: the menu's caches, pools and arenas, and the mods
// (MIRAGE_EVENT_MEMORY_PRESSURE). It never touches the game's memory or its allocators.
//
// Conservative by design:
//   - It acts only on the menu's own memory, and only what is safe to drop (caches, spare pool slabs and chunks).
//   - Pressure needs several samples to rise (Critical: one) and many to fall; actions are rate limited.
//   - The cache budget moves in small steps, between a floor and a ceiling from the settings.
//   - "Observe only" (Act off) keeps every measurement and report, and never changes anything.
//   - Measuring (the address space walk, the process, the profiler, the analysis) runs on its own low-priority thread,
//     with read-only queries, never on the game thread. Acting runs in the cache scheduler's slice on the game thread,
//     like the caches it trims. An error in either stops acting (logged); the game goes on untouched.
//
//   mirage::mem::Start();                          // dllmain, after game::Init
//   mirage::mem::AddPressureHandler("Mods", 100, [](Level level, bool rising) { ...; return bytesFreed; });
//   mirage::mem::Report(true);                     // the console's "memory" command, the Memory panel
#include "runtime/mem/analysis.h"
#include "runtime/mem/heap.h"
#include "runtime/mem/process.h"
#include "runtime/mem/sampler.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mirage::mem
{
	struct Config
	{
		bool act = true;							// Give memory back under pressure, adapt the cache budget
		sampler::Mode tracking = sampler::Mode::Sampled;
		std::size_t sampleInterval = 256 * 1024;	// Sampled: about one allocation recorded per this many bytes
		std::size_t cacheMin = 4u << 20, cacheMax = 64u << 20;
		std::size_t menuBudget = 256u << 20;		// The menu's heap over 1.5x this counts as pressure (Elevated)
		std::size_t largeAllocation = 8u << 20;		// Logged with their call stack (0 = off)
		bool logFindings = true;					// Anomalies and leak suspects in the log (always in the report)
		double interval = 1.0;						// Seconds between samples
	};
	void Configure(const Config& config);			// Any thread
	Config Configured();

	void Start();									// Game thread (registers its cache scheduler job)
	void Stop();									// Saves what it learnt about levels; before mods unload
	void OnLevelLoad(bool finished, const std::string& map);	// Game thread

	// Pressure response: Elevated shrinks the cache budget by a quarter per sample, High halves it per sample (the
	// eviction job trims over its next passes, never all in one frame), Critical drops it to the floor and trims now.
	// Called when the pressure level changes (`rising`), and again every 5 s while it is High or Critical. Returns the
	// bytes it gave back, if it knows. Game thread. Higher priority runs first.
	int AddPressureHandler(std::string name, int priority, std::function<std::size_t(Level level, bool rising)> fn);
	void RemovePressureHandler(int id);

	// Mods' Lua memory for the report (the runtime's mods layer provides it): {mod id, bytes}. Called on the game thread
	// at each sample; the result is kept in the status.
	void SetModMemoryProvider(std::function<std::vector<std::pair<std::string, std::size_t>>()> fn);

	// For the console and the self-test (any thread; they run at the manager's next step)
	void RequestTrim();								// Give back spare memory now (caches over budget, pools, arenas)
	void SimulatePressure(Level level, double seconds);	// Pretend the pressure is this high for a while (self-test)

	struct Status
	{
		Level level = Level::Normal;
		std::string reason;
		ProcessSample process;
		AddressSpace address;
		HeapCounters heap;
		double allocRate = 0.0, byteRate = 0.0;		// Menu heap, last interval
		std::size_t cacheBudget = 0, cacheBytes = 0;
		std::uint64_t actions = 0;					// Times it gave memory back
		std::uint64_t bytesReleased = 0;			// What that freed (as far as known)
		std::string lastAction;
		std::string map;							// The level now
		std::optional<double> secondsToExhaustion;	// Largest free block falling to Critical at the current trend
		double stepMs = 0.0, maxStepMs = 0.0;		// The manager's own cost on the game thread (acting)
		double stepParts[3] = {};					// Of the last step: settings, the mods' Lua memory, acting (ms)
		double avgStepMs = 0.0;						// Moving average (an action under pressure is the expensive kind)
		double monitorMs = 0.0, maxMonitorMs = 0.0;	// A measuring pass on its own thread (address walk, sample)
		bool acting = true;
		std::vector<Finding> recent;				// Newest last (the last 50)
		std::vector<std::pair<std::string, std::size_t>> modLua;	// As of the last sample
	};
	Status GetStatus();								// Any thread

	// Everything, as text (the console and the panel's "Copy report"). `detailed`: call stacks too.
	std::string Report(bool detailed);

	// A leak check of one operation on this thread: records every allocation while it lives (sampler Full mode for the
	// whole process, then back to the configured mode) and reports what is still live from this thread at the end.
	class LeakCheck
	{
	public:
		LeakCheck();
		~LeakCheck();
		LeakCheck(const LeakCheck&) = delete;
		LeakCheck& operator=(const LeakCheck&) = delete;
		sampler::Report Finish();		// Ends the check (once): what this thread allocated and didn't free
	private:
		std::uint32_t generation_;
		std::uint32_t thread_;
		bool done_ = false;
		Config saved_;
	};
}
