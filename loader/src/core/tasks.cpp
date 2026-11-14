#include "core/tasks.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <deque>
#include <thread>

namespace tasks
{
	namespace
	{
		struct Job
		{
			std::shared_ptr<detail::State> state;
			Options options;
			std::function<void()> body;
		};

		struct Pool
		{
			std::mutex lock;
			std::condition_variable wake;
			std::deque<Job> queues[3];
			std::vector<std::thread> workers;
			bool stopping = false;
			bool running = false;
			int busy = 0;
			std::vector<detail::State*> active;		// Running now (Stop asks them to cancel)
			std::uint64_t completed = 0, failed = 0, cancelled = 0;
			double totalMs = 0.0, longestMs = 0.0;
			std::string longest;
		};

		Pool& P()
		{
			static Pool* p = new Pool();		// Never destroyed: Stop() joins the workers before the DLL goes
			return *p;
		}

		std::mutex g_dispatchLock;
		Dispatch g_dispatch[4];
		thread_local detail::State* t_current = nullptr;

		void Execute(Job& job)
		{
			detail::State& s = *job.state;
			if (s.cancel.load()) {
				detail::Finish(s, Status::Cancelled);
				return;
			}
			s.status = Status::Running;
			t_current = &s;
			const auto t0 = std::chrono::steady_clock::now();
			Status outcome = Status::Done;
			std::string error;
			try {
				job.body();
				if (s.cancel.load()) outcome = Status::Cancelled;
			} catch (const std::exception& e) {
				outcome = Status::Failed;
				error = e.what();
			} catch (...) {
				outcome = Status::Failed;
				error = "unknown exception";
			}
			t_current = nullptr;
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (outcome == Status::Failed)
				spdlog::warn("Tasks: {} failed: {}", job.options.name.empty() ? "a task" : job.options.name, error);
			{
				Pool& p = P();
				std::lock_guard<std::mutex> l(p.lock);
				(outcome == Status::Done ? p.completed : outcome == Status::Failed ? p.failed : p.cancelled)++;
				p.totalMs += ms;
				if (ms > p.longestMs) {
					p.longestMs = ms;
					p.longest = job.options.name;
				}
			}
			detail::Finish(s, outcome, std::move(error));
		}

		void Worker(int index)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			const std::wstring name = L"mirage task worker " + std::to_wstring(index);
			SetThreadDescription(GetCurrentThread(), name.c_str());
			Pool& p = P();
			for (;;) {
				Job job;
				{
					std::unique_lock<std::mutex> l(p.lock);
					p.wake.wait(l, [&] { return p.stopping || !p.queues[0].empty() || !p.queues[1].empty() || !p.queues[2].empty(); });
					if (p.stopping) return;
					for (auto& q : p.queues)
						if (!q.empty()) {
							job = std::move(q.front());
							q.pop_front();
							break;
						}
					++p.busy;
					p.active.push_back(job.state.get());
				}
				Execute(job);
				std::lock_guard<std::mutex> l(p.lock);
				--p.busy;
				p.active.erase(std::find(p.active.begin(), p.active.end(), job.state.get()));
			}
		}
	}

	void SetDispatcher(On where, Dispatch dispatch)
	{
		std::lock_guard<std::mutex> l(g_dispatchLock);
		g_dispatch[static_cast<int>(where)] = std::move(dispatch);
	}

	void Start(int workers)
	{
		Pool& p = P();
		std::lock_guard<std::mutex> l(p.lock);
		if (p.running) return;
		if (workers <= 0) workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 3);
		p.stopping = false;
		p.running = true;
		for (int i = 0; i < workers; ++i) p.workers.emplace_back(Worker, i);
		SetDispatcher(On::Worker, [](std::function<void()> fn) { Run(std::move(fn), {Priority::Normal, "continuation"}); });
	}

	void Stop()
	{
		Pool& p = P();
		std::vector<Job> dropped;
		{
			std::lock_guard<std::mutex> l(p.lock);
			if (!p.running) return;
			p.stopping = true;
			for (detail::State* s : p.active) s->cancel = true;		// Running tasks see Cancelled() and may stop early
			for (auto& q : p.queues) {
				for (Job& j : q) dropped.push_back(std::move(j));
				q.clear();
			}
		}
		p.wake.notify_all();
		for (std::thread& t : p.workers)
			if (t.joinable()) t.join();
		SetDispatcher(On::Worker, nullptr);
		std::lock_guard<std::mutex> l(p.lock);
		p.workers.clear();
		p.running = false;
		p.cancelled += dropped.size();
		for (Job& j : dropped) {
			j.state->cancel = true;
			j.state->status = Status::Cancelled;
			j.state->done.notify_all();
		}
	}

	bool Running()
	{
		Pool& p = P();
		std::lock_guard<std::mutex> l(p.lock);
		return p.running;
	}

	bool Cancelled() { return t_current && t_current->cancel.load(); }

	Stats Snapshot()
	{
		Pool& p = P();
		std::lock_guard<std::mutex> l(p.lock);
		Stats s;
		s.workers = static_cast<int>(p.workers.size());
		s.busy = p.busy;
		for (int i = 0; i < 3; ++i) s.queued[i] = p.queues[i].size();
		s.completed = p.completed;
		s.failed = p.failed;
		s.cancelled = p.cancelled;
		const std::uint64_t n = p.completed + p.failed + p.cancelled;
		s.averageMs = n ? p.totalMs / static_cast<double>(n) : 0.0;
		s.longestMs = p.longestMs;
		s.longest = p.longest;
		return s;
	}

	namespace detail
	{
		void Submit(std::shared_ptr<State> state, Options options, std::function<void()> body)
		{
			Pool& p = P();
			{
				std::lock_guard<std::mutex> l(p.lock);
				if (p.running && !p.stopping) {
					p.queues[static_cast<int>(options.priority)].push_back({std::move(state), std::move(options), std::move(body)});
					p.wake.notify_one();
					return;
				}
			}
			// No workers (not started, or stopping): inline, so callers never wait for a pool that isn't there
			Job job{std::move(state), std::move(options), std::move(body)};
			Execute(job);
		}

		void Dispatch(On where, std::function<void()> fn)
		{
			if (where == On::Inline) {
				fn();
				return;
			}
			tasks::Dispatch d;
			{
				std::lock_guard<std::mutex> l(g_dispatchLock);
				d = g_dispatch[static_cast<int>(where)];
			}
			if (d) d(std::move(fn));
			else fn();
		}

		void Finish(State& s, Status status, std::string error)
		{
			std::vector<std::function<void()>> continuations;
			{
				std::lock_guard<std::mutex> l(s.lock);
				s.error = std::move(error);
				s.status = status;
				continuations.swap(s.continuations);
			}
			s.done.notify_all();
			for (auto& c : continuations) c();
		}
	}
}
