#pragma once
// The memory manager's reasoning, without the game or Windows (unit tested in tests/test_memory.cpp): trends, anomaly
// detection, address-space fragmentation, the pressure level, the adaptive cache budget and the per-level predictor.
// Every rule here leans conservative: it needs several samples to act, acts in small steps, and calms down slowly.
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mirage::mem
{
	// ---- Statistics ----------------------------------------------------------------------------------------------------
	// Exponentially weighted moving average; the first sample primes it
	struct Ewma
	{
		double alpha = 0.1;
		double value = 0.0;
		bool primed = false;
		void Add(double v)
		{
			value = primed ? value + alpha * (v - value) : v;
			primed = true;
		}
	};

	// Least squares over the last `capacity` (t, v) samples: the slope (v per unit of t) and how well a line fits (r^2)
	class Trend
	{
	public:
		explicit Trend(std::size_t capacity = 60) : capacity_(capacity ? capacity : 1) {}
		void Add(double t, double v);
		void Clear() { points_.clear(); }
		std::size_t Count() const { return points_.size(); }
		bool Full() const { return points_.size() >= capacity_; }
		double Slope() const;
		double R2() const;
		double Rise() const;			// Last value minus first
		double Span() const;			// Last t minus first
	private:
		struct Point { double t, v; };
		std::vector<Point> points_;		// Oldest first (capacity is small: erase at the front is fine)
		std::size_t capacity_;
	};

	// ---- Anomalies ------------------------------------------------------------------------------------------------------
	// One sample of an allocation stream (the menu's heap, or one tag of it): counters since start, live bytes now
	struct StreamSample
	{
		double t = 0.0;					// Seconds
		std::uint64_t allocations = 0;	// Total so far
		std::uint64_t bytes = 0;		// Allocated so far
		std::int64_t live = 0;			// Bytes live now (an estimate for a tag)
	};

	struct Finding
	{
		enum class Kind { RateSpike, SteadyGrowth, Churn, LargeAllocation, Pressure, Prediction } kind;
		enum class Severity { Info, Warning } severity = Severity::Info;
		std::string subject;			// "menu", a tag, a level
		std::string text;
	};

	struct AnomalyRules
	{
		int warmup = 10;					// Samples before anything is reported (startup allocates in bursts)
		double spikeFactor = 8.0;			// Allocation rate over this many times its average...
		double spikeMinRate = 20000.0;		// ...and over this many allocations a second
		int spikeSamples = 2;				// For this many samples in a row
		std::size_t growthWindow = 60;		// Samples the growth trend looks at (about a minute at 1 Hz)
		double growthMinBytesPerSec = 64.0 * 1024.0;	// Steady growth faster than this...
		double growthMinR2 = 0.9;			// ...that a line fits this well...
		double growthMinRise = 8.0 * 1024 * 1024;		// ...and adds at least this much over the window
		double churnMinRate = 50000.0;		// Allocations a second with live bytes flat: churn (info)
		double cooldown = 120.0;			// Seconds before the same kind is reported again for a subject
	};

	// Watches one stream; Add returns what is new about it
	class AnomalyDetector
	{
	public:
		explicit AnomalyDetector(std::string subject, AnomalyRules rules = {});
		std::vector<Finding> Add(const StreamSample& s);
		double Rate() const { return rate_; }				// Allocations a second, last interval
		double ByteRate() const { return byteRate_; }		// Bytes a second, last interval
		double AverageRate() const { return avgRate_.value; }
		const Trend& Growth() const { return growth_; }
	private:
		bool Due(Finding::Kind kind, double t);
		std::string subject_;
		AnomalyRules rules_;
		std::optional<StreamSample> last_;
		int samples_ = 0, spikeRun_ = 0;
		double rate_ = 0.0, byteRate_ = 0.0;
		Ewma avgRate_{0.05};
		Trend growth_;
		std::map<int, double> reportedAt_;
	};

	// ---- Fragmentation --------------------------------------------------------------------------------------------------
	// Of the process's address space (a 32-bit game: running out of large free blocks is what fails first, long before
	// the bytes run out), from the sizes of its free regions
	struct Fragmentation
	{
		std::uint64_t totalFree = 0;
		std::uint64_t largestFree = 0;
		std::size_t freeRegions = 0;
		std::size_t smallRegions = 0;		// Free regions under 1 MB: unusable for most of the game's allocations
		// 0 = all free space in one block, towards 1 = scattered in small pieces (1 - largest / total)
		double Index() const { return totalFree ? 1.0 - static_cast<double>(largestFree) / static_cast<double>(totalFree) : 0.0; }
		void AddFree(std::uint64_t bytes)
		{
			totalFree += bytes;
			if (bytes > largestFree) largestFree = bytes;
			++freeRegions;
			if (bytes < (1u << 20)) ++smallRegions;
		}
	};

	// ---- Pressure -------------------------------------------------------------------------------------------------------
	enum class Level : int { Normal = 0, Elevated = 1, High = 2, Critical = 3 };
	const char* LevelName(Level level);

	struct PressureInputs
	{
		std::uint64_t addressFree = 0;		// Free address space in the process (0 = unknown)
		std::uint64_t largestFree = 0;		// Its largest block (0 = unknown)
		std::uint64_t commitAvailable = 0;	// The system's commit charge left (page file + RAM; 0 = unknown)
		std::int64_t menuLive = 0;			// The menu's own heap
		std::int64_t menuBudget = 0;		// What the menu is meant to stay under (0 = no limit)
	};

	struct PressureRules
	{
		// A level is reached when any of its limits is crossed (largest free block, total free, commit left)
		std::uint64_t largest[4] = {0, 256ull << 20, 128ull << 20, 48ull << 20};
		std::uint64_t free[4] = {0, 512ull << 20, 256ull << 20, 96ull << 20};
		std::uint64_t commit[4] = {0, 1024ull << 20, 512ull << 20, 192ull << 20};
		double menuOverBudget = 1.5;		// The menu's heap at this many times its budget: Elevated (at least)
		int riseSamples = 2;				// Samples a higher level must hold before it is taken (Critical: at once)
		int calmSamples = 10;				// Samples a lower level must hold before it is taken
	};

	// Hysteresis: quick (but not jumpy) to go up, slow to come down
	class PressurePolicy
	{
	public:
		explicit PressurePolicy(PressureRules rules = {}) : rules_(rules) {}
		Level Add(const PressureInputs& in);	// The level now
		Level Current() const { return level_; }
		static Level Raw(const PressureInputs& in, const PressureRules& rules);	// Without hysteresis
		std::string Reason() const { return reason_; }
	private:
		PressureRules rules_;
		Level level_ = Level::Normal;
		Level pending_ = Level::Normal;
		int pendingRun_ = 0;
		std::string reason_;
	};

	// ---- Adaptive cache budget ------------------------------------------------------------------------------------------
	struct BudgetRules
	{
		std::size_t min = 4u << 20, max = 64u << 20;
		double grow = 1.125, shrink = 0.75;	// Per decision
		double cooldown = 30.0;				// Seconds between growing steps (shrinking under pressure doesn't wait)
		double fullAt = 0.85;				// The caches hold this share of the budget: they would use more
		double minHitsPerKb = 0.5;			// Since the last decision: only caches that are used earn more memory
	};

	struct BudgetInputs
	{
		double t = 0.0;
		Level level = Level::Normal;
		std::size_t cacheBytes = 0;			// What the caches hold now
		std::uint64_t hits = 0;				// The caches' hits so far
		std::uint64_t trims = 0;			// Times the eviction manager trimmed so far
	};

	class BudgetController
	{
	public:
		BudgetController(std::size_t start, BudgetRules rules = {});
		// The budget to use now; `why` says what changed it (empty: unchanged)
		std::size_t Decide(const BudgetInputs& in, std::string* why = nullptr);
		std::size_t Budget() const { return budget_; }
		void SetRules(const BudgetRules& rules);
	private:
		BudgetRules rules_;
		std::size_t budget_;
		std::size_t recoverTo_ = 0;		// The budget before pressure took it down: back to it in steps once calm
		double lastGrow_ = -1e9;
		std::uint64_t lastHits_ = 0, lastTrims_ = 0;
		bool primed_ = false;
	};

	// ---- Prediction -----------------------------------------------------------------------------------------------------
	// What a level needs, learnt from the times it was played: the highest process private bytes and the lowest largest
	// free block seen in it. Persisted between sessions (manager.cpp writes it as JSON).
	struct LevelProfile
	{
		std::uint64_t peakPrivate = 0;
		std::uint64_t minLargestFree = 0;	// 0 = never seen
		int visits = 0;
	};

	class Predictor
	{
	public:
		void Observe(const std::string& level, std::uint64_t privateBytes, std::uint64_t largestFree);
		void EndVisit(const std::string& level);		// The level was left (counts a visit)
		std::optional<LevelProfile> Profile(const std::string& level) const;
		const std::map<std::string, LevelProfile>& All() const { return profiles_; }
		void Set(const std::string& level, const LevelProfile& p) { profiles_[level] = p; }
		// Seconds until `value` reaches `limit` going down at the trend's rate (none when it isn't going down)
		static std::optional<double> TimeToLimit(const Trend& trend, double value, double limit);
	private:
		std::map<std::string, LevelProfile> profiles_;
	};
}
