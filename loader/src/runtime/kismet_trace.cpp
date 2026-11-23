#include "runtime/kismet_trace.h"

#include "game/game.h"
#include "game/uobject.h"
#include "runtime/kismet.h"
#include "sdk/SDK.hpp"

#include "util/glz_compat.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace mirage::kismet_trace
{
	namespace
	{
		struct Watched
		{
			Classes::USequenceOp* op;
			std::size_t slot;
			int count;					// Its count the tick before
			bool event;					// An event counts in TriggerCount (the engine doesn't count it in ActivateCount)
		};
		struct Hit
		{
			std::uint64_t tick;
			Classes::USequenceOp* op;
			std::size_t slot;
			int times;
			std::string path, cls;		// Filled when read (names only for what a mod asks for)
		};
		struct ModTrace
		{
			bool on = false;
			std::deque<Hit> hits;
		};
		constexpr std::size_t kKeep = 4096;
		constexpr std::uint64_t kRelistTicks = 300;		// About five seconds: a streamed sublevel's new sequences

		std::map<std::string, ModTrace> g_mods;
		std::vector<Watched> g_ops;
		bool g_stale = true;
		std::uint64_t g_tick = 0, g_listedAt = 0, g_activations = 0;
		double g_lastUs = 0.0;

		int CountOf(Classes::USequenceOp* op, bool event)
		{
			return event ? static_cast<Classes::USequenceEvent*>(op)->TriggerCount : op->ActivateCount;
		}

		bool AnyOn()
		{
			for (const auto& [id, m] : g_mods)
				if (m.on) return true;
			return false;
		}

		// The ops as they are now: their counts are the baseline (an op seen for the first time reports nothing)
		void List()
		{
			std::map<Classes::USequenceOp*, int> before;
			for (const Watched& w : g_ops)
				if (game::AliveAt(w.op, w.slot)) before[w.op] = w.count;
			g_ops.clear();
			for (Classes::USequenceObject* o : kismet::LevelObjects()) {
				if (!o->IsA(Classes::USequenceOp::StaticClass())) continue;
				auto* op = static_cast<Classes::USequenceOp*>(o);
				const bool event = op->IsA(Classes::USequenceEvent::StaticClass());
				const auto it = before.find(op);
				g_ops.push_back({op, game::IndexOf(op), it != before.end() ? it->second : CountOf(op, event), event});
			}
			g_stale = false;
			g_listedAt = g_tick;
		}
	}

	void Invalidate() { g_stale = true; }

	void Tick()
	{
		++g_tick;
		if (!AnyOn()) {
			g_lastUs = 0.0;
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		if (g_stale || g_tick - g_listedAt >= kRelistTicks) List();
		for (Watched& w : g_ops) {
			if (!game::AliveAt(w.op, w.slot)) continue;		// Gone (its level unloaded): skipped until the next list
			const int now = CountOf(w.op, w.event);
			if (now == w.count) continue;
			const int times = now > w.count ? now - w.count : now;		// Lower: reset (a restart): count from zero
			w.count = now;
			g_activations += static_cast<std::uint64_t>(times);
			for (auto& [id, m] : g_mods) {
				if (!m.on) continue;
				m.hits.push_back({g_tick, w.op, w.slot, times, {}, {}});
				if (m.hits.size() > kKeep) m.hits.pop_front();
			}
		}
		g_lastUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	}

	void Enable(const std::string& modId, bool on)
	{
		ModTrace& m = g_mods[modId];
		if (on && !m.on) g_stale = true;	// Counts from now
		m.on = on;
		if (!on) m.hits.clear();
	}

	namespace detail
	{
		struct TraceEntry
		{
			std::uint64_t tick;
			std::string op, cls;
			int times;
			struct glaze
			{
				using T = TraceEntry;
				static constexpr auto value = glz::object("tick", &T::tick, "op", &T::op, "class", &T::cls, "times", &T::times);
			};
		};
	}

	std::uint32_t Read(const std::string& modId, char* buffer, std::uint32_t capacity)
	{
		ModTrace& m = g_mods[modId];
		std::vector<detail::TraceEntry> out;
		for (Hit& h : m.hits) {
			if (h.path.empty() && game::AliveAt(h.op, h.slot)) {
				h.path = h.op->GetPathName();
				h.cls = h.op->Class ? h.op->Class->GetName() : std::string();
			}
			out.push_back({h.tick, h.path, h.cls, h.times});
		}
		const std::string text = jc::dump(out);
		if (buffer && capacity) {
			const std::size_t n = text.size() < capacity - 1 ? text.size() : capacity - 1;
			std::memcpy(buffer, text.data(), n);
			buffer[n] = '\0';
			if (text.size() < capacity) m.hits.clear();
		}
		return static_cast<std::uint32_t>(text.size());
	}

	void Forget(const std::string& modId) { g_mods.erase(modId); }

	Stats Get()
	{
		Stats s;
		s.on = AnyOn();
		s.ops = static_cast<int>(g_ops.size());
		s.lastTickUs = g_lastUs;
		s.activations = g_activations;
		return s;
	}
}
