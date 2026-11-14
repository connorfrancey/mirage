#include "core/events.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>

namespace events
{
	namespace
	{
		std::mutex g_dispatchLock;
		Dispatch g_dispatch[4];
	}

	void SetDispatcher(Where where, Dispatch dispatch)
	{
		std::lock_guard<std::mutex> l(g_dispatchLock);
		g_dispatch[static_cast<int>(where)] = std::move(dispatch);
	}

	namespace detail
	{
		void Run(Channel& channel, const Handler& h, const void* event)
		{
			if (!h.alive->load()) return;		// Unsubscribed meanwhile
			const auto t0 = std::chrono::steady_clock::now();
			try {
				h.fn(event);
			} catch (const std::exception& e) {
				channel.failures.fetch_add(1, std::memory_order_relaxed);
				spdlog::warn("Events: a handler of {} ({}) failed: {}", channel.name, h.options.owner.empty() ? "?" : h.options.owner, e.what());
			} catch (...) {
				channel.failures.fetch_add(1, std::memory_order_relaxed);
				spdlog::warn("Events: a handler of {} ({}) failed", channel.name, h.options.owner.empty() ? "?" : h.options.owner);
			}
			const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
			channel.nanoseconds.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
		}

		void Queue(Where where, std::function<void()> fn)
		{
			Dispatch d;
			{
				std::lock_guard<std::mutex> l(g_dispatchLock);
				d = g_dispatch[static_cast<int>(where)];
			}
			if (d) d(std::move(fn));
			else fn();		// No dispatcher (tests, a build without the game): delivered now
		}
	}

	std::vector<ChannelStats> EventBus::Snapshot()
	{
		std::lock_guard<std::mutex> l(lock_);
		std::vector<ChannelStats> out;
		for (const auto& [type, ch] : channels_) {
			ChannelStats s;
			s.name = ch->name;
			s.handlers = ch->handlers->size();
			s.published = ch->published.load(std::memory_order_relaxed);
			s.failures = ch->failures.load(std::memory_order_relaxed);
			s.milliseconds = static_cast<double>(ch->nanoseconds.load(std::memory_order_relaxed)) / 1e6;
			for (const detail::Handler& h : *ch->handlers) s.owners.push_back(h.options.owner);
			out.push_back(std::move(s));
		}
		std::sort(out.begin(), out.end(), [](const ChannelStats& a, const ChannelStats& b) { return a.name < b.name; });
		return out;
	}

	EventBus& Bus()
	{
		static EventBus* bus = [] {
			auto* b = new EventBus();
			b->Name<LevelLoad>("Level load");
			b->Name<MemoryPressure>("Memory pressure");
			b->Name<HangDetected>("Hang detected");
			b->Name<HangRecovered>("Hang recovered");
			b->Name<ModReloaded>("Mod reloaded");
			return b;
		}();
		return *bus;
	}
}
