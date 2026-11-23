#include "runtime/mem/profiler.h"

#include "runtime/mem/manager.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <iterator>
#include <new>

namespace mirage::mem
{
	namespace
	{
		constexpr std::size_t kMaxImages = 512;

		template <std::size_t N>
		void CopyLabel(std::array<char, N>& destination, const std::string& source)
		{
			const auto size = std::min(source.size(), N - 1);
			std::copy_n(source.data(), size, destination.data());
			destination[size] = '\0';
		}

		void AttributeImages(MemoryProfileSnapshot& snapshot)
		{
			const HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (handle == INVALID_HANDLE_VALUE) return;
			struct SnapshotHandle
			{
				HANDLE handle;
				~SnapshotHandle() { CloseHandle(handle); }
			} owner{handle};

			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			for (BOOL found = Module32FirstW(handle, &entry); ; found = Module32NextW(handle, &entry)) {
				if (!found) {
					snapshot.imagesComplete = GetLastError() == ERROR_NO_MORE_FILES;
					break;
				}
				if (snapshot.images.size() == kMaxImages) break;
				char name[4 * std::size(entry.szModule)]{};
				WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
				snapshot.images.push_back({reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), name});
			}
			std::ranges::sort(snapshot.images, {}, &ProfileImage::base);
			snapshot.regionImages.resize(snapshot.regions.size());
			for (std::size_t i = 0; i < snapshot.regions.size(); ++i) {
				const auto& region = snapshot.regions[i];
				if (region.type != MEM_IMAGE) continue;
				const auto it = std::ranges::lower_bound(snapshot.images, region.allocationBase, {}, &ProfileImage::base);
				if (it == snapshot.images.end() || it->base != region.allocationBase) continue;
				snapshot.regionImages[i] = static_cast<std::uint16_t>(it - snapshot.images.begin() + 1);
				if (region.state == MEM_COMMIT) it->committed += region.Size();
				else if (region.state == MEM_RESERVE) it->reserved += region.Size();
			}
		}
	}

	MemoryProfiler& Profiler()
	{
		static MemoryProfiler profiler;
		return profiler;
	}

	void MemoryProfiler::SetInterval(int seconds)
	{
		std::shared_ptr<const MemoryProfileSnapshot> old;
		std::lock_guard lock(lock_);
		if (interval_ == seconds) return;
		if (!seconds || !interval_) {
			++session_;
			old = latest_.exchange(nullptr);
			captureFailed_ = false;
		}
		interval_ = seconds;
		next_ = 0;
	}

	std::shared_ptr<MemoryProfileSnapshot> MemoryProfiler::BeginSample(double now)
	{
		std::uint64_t session;
		{
			std::lock_guard lock(lock_);
			if (!interval_ || now < next_) return {};
			next_ = now + interval_;
			session = session_;
		}
		try {
			auto snapshot = std::make_shared<MemoryProfileSnapshot>();
			snapshot->session = session;
			snapshot->capturedAtMs = RealTimeMs();
			snapshot->seconds = now;
			snapshot->regions.reserve(kMaxProfileRegions);
			return snapshot;
		} catch (const std::bad_alloc&) {
			SkipSample(session);
			return {};
		}
	}

	void MemoryProfiler::CopyDiagnostics(MemoryProfileSnapshot& snapshot, const Status& status)
	{
		snapshot.luaCount = std::min(status.modLua.size(), snapshot.modLua.size());
		for (std::size_t i = 0; i < snapshot.luaCount; ++i) {
			const auto& [id, bytes] = status.modLua[i];
			CopyLabel(snapshot.modLua[i].id, id);
			snapshot.modLua[i].bytes = bytes;
		}
		snapshot.moreLuaMods = snapshot.luaCount != status.modLua.size();
		snapshot.findingsCount = std::min(status.recent.size(), snapshot.findings.size());
		for (std::size_t i = 0; i < snapshot.findingsCount; ++i) {
			const auto& finding = status.recent[status.recent.size() - 1 - i];
			CopyLabel(snapshot.findings[i].subject, finding.subject);
			CopyLabel(snapshot.findings[i].text, finding.text);
		}
		snapshot.pressure = status.level;
		CopyLabel(snapshot.pressureReason, status.reason);
		snapshot.process = status.process;
		snapshot.allocationFailures = status.heap.failures;
		snapshot.lastFailedBytes = status.heap.lastFailedBytes;
	}

	void MemoryProfiler::Publish(std::shared_ptr<MemoryProfileSnapshot> snapshot)
	{
		if (trendSession_ != snapshot->session) {
			privateTrend_.Clear();
			trendSession_ = snapshot->session;
		}
		if (snapshot->process.countersOk) {
			privateTrend_.Add(snapshot->seconds, static_cast<double>(snapshot->process.privateBytes));
			if (privateTrend_.Count() >= 6 && privateTrend_.Span() >= 15.0 && privateTrend_.Rise() >= (16u << 20)
				&& privateTrend_.R2() >= 0.85)
				snapshot->privateGrowthBytesPerSecond = std::max(0.0, privateTrend_.Slope());
		}
		AttributeImages(*snapshot);
		const auto previous = Latest();
		if (previous && previous->session == snapshot->session) {
			snapshot->history = previous->history;
			snapshot->historyCount = previous->historyCount;
			snapshot->historyNext = previous->historyNext;
			snapshot->peakCommitted = previous->peakCommitted;
			snapshot->peakReserved = previous->peakReserved;
		}
		const auto& space = snapshot->address;
		if (space.complete) {
			snapshot->peakCommitted = std::max(snapshot->peakCommitted, space.committed);
			snapshot->peakReserved = std::max(snapshot->peakReserved, space.reserved);
			snapshot->history[snapshot->historyNext] = {snapshot->seconds, space.committed, space.reserved,
				space.free.totalFree, space.free.largestFree};
			snapshot->historyNext = (snapshot->historyNext + 1) % snapshot->history.size();
			snapshot->historyCount = std::min(snapshot->historyCount + 1, snapshot->history.size());
		}
		std::lock_guard lock(lock_);
		if (session_ == snapshot->session && interval_) {
			latest_.store(std::move(snapshot));
			captureFailed_ = false;
		}
	}

	void MemoryProfiler::SkipSample(std::uint64_t session)
	{
		std::lock_guard lock(lock_);
		if (session_ == session && interval_) captureFailed_ = true;
	}
}
