#pragma once
// The menu's event bus: typed events, published by one part of the menu and handled by others that don't know about
// each other (the memory manager, the hang detector, level loads, mods' reloads, ...).
//
//   struct LevelLoaded { std::string map; };                         // any copyable struct is an event
//   auto sub = events::Bus().Subscribe<LevelLoaded>([](const LevelLoaded& e) { ... });          // until `sub` goes
//   auto sub2 = events::Bus().Subscribe<LevelLoaded>(fn, {.priority = 10, .where = events::Where::RenderThread});
//   events::Bus().Publish(LevelLoaded{"edge_p"});
//
// Delivery: Immediate handlers run inside Publish, on the publishing thread, highest priority first. GameThread,
// RenderThread and Worker handlers get a copy of the event queued to that thread (through the dispatchers dllmain
// sets; without one, they run immediately). A handler that throws is logged and skipped: one bad handler never stops
// the others or the publisher. Subscribing and unsubscribing are safe from any thread, during a publish too (a handler
// removed mid-publish is not called once removal returned).
//
// Statistics per event type (published, handlers, time in handlers, failures) for the diagnostics.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace events
{
	enum class Where : std::uint8_t { Immediate, GameThread, RenderThread, Worker };

	struct Options
	{
		int priority = 0;				// Higher first (among Immediate handlers; queued ones in that order too)
		Where where = Where::Immediate;
		std::string owner;				// Who subscribed ("Memory manager", a mod's id): for the diagnostics
	};

	using Dispatch = std::function<void(std::function<void()>)>;
	// dllmain: how to get work onto the game thread, the render thread and a background worker (core/tasks.h)
	void SetDispatcher(Where where, Dispatch dispatch);

	namespace detail
	{
		struct Handler
		{
			std::uint64_t id;
			Options options;
			std::function<void(const void*)> fn;
			std::shared_ptr<std::atomic<bool>> alive;
		};
		struct Channel
		{
			std::string name;
			std::shared_ptr<const std::vector<Handler>> handlers = std::make_shared<std::vector<Handler>>();
			std::atomic<std::uint64_t> published{0}, failures{0}, nanoseconds{0};
		};
		void Run(Channel& channel, const Handler& h, const void* event);
		void Queue(Where where, std::function<void()> fn);
	}

	// Unsubscribes when destroyed (or Reset). Movable.
	class Subscription
	{
	public:
		Subscription() = default;
		Subscription(std::function<void()> remove) : remove_(std::move(remove)) {}
		Subscription(Subscription&& o) noexcept : remove_(std::move(o.remove_)) { o.remove_ = nullptr; }
		Subscription& operator=(Subscription&& o) noexcept
		{
			if (this != &o) {
				Reset();
				remove_ = std::move(o.remove_);
				o.remove_ = nullptr;
			}
			return *this;
		}
		~Subscription() { Reset(); }
		void Reset()
		{
			if (remove_) remove_();
			remove_ = nullptr;
		}
		bool Active() const { return static_cast<bool>(remove_); }
	private:
		std::function<void()> remove_;
	};

	struct ChannelStats
	{
		std::string name;
		std::size_t handlers = 0;
		std::uint64_t published = 0, failures = 0;
		double milliseconds = 0.0;		// In handlers, in total
		std::vector<std::string> owners;
	};

	class EventBus
	{
	public:
		template <class E>
		Subscription Subscribe(std::function<void(const E&)> fn, Options options = {})
		{
			detail::Channel& ch = ChannelFor<E>();
			auto alive = std::make_shared<std::atomic<bool>>(true);
			const std::uint64_t id = nextId_.fetch_add(1);
			{
				std::lock_guard<std::mutex> l(lock_);
				auto next = std::make_shared<std::vector<detail::Handler>>(*ch.handlers);
				detail::Handler h{id, std::move(options), [fn = std::move(fn)](const void* e) { fn(*static_cast<const E*>(e)); }, alive};
				const auto at = std::find_if(next->begin(), next->end(), [&](const detail::Handler& x) { return x.options.priority < h.options.priority; });
				next->insert(at, std::move(h));
				ch.handlers = std::move(next);
			}
			return Subscription([this, &ch, id, alive] {
				alive->store(false);
				std::lock_guard<std::mutex> l(lock_);
				auto next = std::make_shared<std::vector<detail::Handler>>(*ch.handlers);
				next->erase(std::remove_if(next->begin(), next->end(), [id](const detail::Handler& x) { return x.id == id; }), next->end());
				ch.handlers = std::move(next);
			});
		}

		template <class E>
		void Publish(const E& event)
		{
			detail::Channel& ch = ChannelFor<E>();
			ch.published.fetch_add(1, std::memory_order_relaxed);
			std::shared_ptr<const std::vector<detail::Handler>> handlers;
			{
				std::lock_guard<std::mutex> l(lock_);
				handlers = ch.handlers;
			}
			std::shared_ptr<const E> copy;		// Made once, for the queued deliveries
			for (const detail::Handler& h : *handlers) {
				if (h.options.where == Where::Immediate) {
					detail::Run(ch, h, &event);
					continue;
				}
				if (!copy) copy = std::make_shared<const E>(event);
				detail::Queue(h.options.where, [&ch, h, copy] { detail::Run(ch, h, copy.get()); });
			}
		}

		template <class E>
		std::size_t Handlers()
		{
			detail::Channel& ch = ChannelFor<E>();
			std::lock_guard<std::mutex> l(lock_);
			return ch.handlers->size();
		}

		std::vector<ChannelStats> Snapshot();

		// The channel's display name (defaults to the C++ type name)
		template <class E>
		void Name(std::string name)
		{
			detail::Channel& ch = ChannelFor<E>();
			std::lock_guard<std::mutex> l(lock_);
			ch.name = std::move(name);
		}

	private:
		template <class E>
		detail::Channel& ChannelFor()
		{
			std::lock_guard<std::mutex> l(lock_);
			auto& slot = channels_[std::type_index(typeid(E))];
			if (!slot) {
				slot = std::make_unique<detail::Channel>();
				slot->name = typeid(E).name();
			}
			return *slot;
		}

		std::mutex lock_;
		std::unordered_map<std::type_index, std::unique_ptr<detail::Channel>> channels_;
		std::atomic<std::uint64_t> nextId_{1};
	};

	EventBus& Bus();		// The menu's (never destroyed: subscriptions may be released during unload)

	// ---- The menu's own events ------------------------------------------------------------------------------------------
	struct LevelLoad { bool finished = false; std::string map; };			// Game thread
	struct MemoryPressure { int level = 0; int previous = 0; };			// Game thread (mirage::mem::Level values)
	struct HangDetected { std::string thread; double seconds = 0.0; std::string where; };	// The hang detector's thread
	struct HangRecovered { std::string thread; double seconds = 0.0; };	// The hang detector's thread
	struct ModReloaded { std::string id; bool ok = true; };				// Game thread
}
