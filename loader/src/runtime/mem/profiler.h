#pragma once

#include "runtime/mem/process.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mirage::mem
{
	struct Status;
	inline constexpr std::size_t kProfileHistory = 240;

	struct ProfilePoint
	{
		double seconds = 0;
		std::uint64_t committed = 0, reserved = 0, free = 0, largestFree = 0;
	};

	struct ProfileImage
	{
		std::uintptr_t base;
		std::string name;
		std::uint64_t committed = 0, reserved = 0;
	};

	struct ProfileLuaHeap
	{
		std::array<char, 128> id{};
		std::size_t bytes = 0;
	};

	struct ProfileFinding
	{
		std::array<char, 80> subject{};
		std::array<char, 512> text{};
	};

	struct MemoryProfileSnapshot
	{
		std::uint64_t session = 0;
		std::uint64_t capturedAtMs = 0;
		double seconds = 0;
		ProcessSample process;
		AddressSpace address;
		std::vector<ProcessRegion> regions;
		std::vector<ProfileImage> images;
		std::vector<std::uint16_t> regionImages; // One-based indices. Zero means unattributed.
		bool imagesComplete = false;
		std::array<ProfileLuaHeap, 64> modLua;
		std::size_t luaCount = 0;
		bool moreLuaMods = false;
		std::array<ProfileFinding, 12> findings;
		std::size_t findingsCount = 0;
		Level pressure = Level::Normal;
		std::array<char, 160> pressureReason{};
		std::uint64_t allocationFailures = 0, lastFailedBytes = 0;
		double privateGrowthBytesPerSecond = 0.0;
		std::uint64_t peakCommitted = 0, peakReserved = 0;
		std::array<ProfilePoint, kProfileHistory> history{};
		std::size_t historyCount = 0, historyNext = 0;

		const ProfilePoint& Point(std::size_t i) const
		{
			return history[(historyNext + history.size() - historyCount + i) % history.size()];
		}
	};

	class MemoryProfiler
	{
	public:
		void SetInterval(int seconds); // Zero disables capture. The monitor sets the maximum cadence.
		std::shared_ptr<const MemoryProfileSnapshot> Latest() const { return latest_.load(); }
		bool CaptureFailed() const { return captureFailed_.load(); }

		// Only the existing memory monitor calls these methods.
		std::shared_ptr<MemoryProfileSnapshot> BeginSample(double now);
		void CopyDiagnostics(MemoryProfileSnapshot& snapshot, const Status& status);
		void Publish(std::shared_ptr<MemoryProfileSnapshot> snapshot);
		void SkipSample(std::uint64_t session);

	private:
		std::mutex lock_; // Configuration and publication only. Scans and allocations happen outside this lock.
		int interval_ = 0;
		double next_ = 0;
		std::uint64_t session_ = 0;
		std::atomic<bool> captureFailed_{false};
		std::atomic<std::shared_ptr<const MemoryProfileSnapshot>> latest_;
		Trend privateTrend_{24}; // Monitor thread only.
		std::uint64_t trendSession_ = 0;
	};

	MemoryProfiler& Profiler();
}
