#pragma once
/** @file
 * @ingroup mirage_core
 * @brief Worker queue and continuations for game, render, and worker threads.
 */
// Background tasks: work that must not run on the game or render thread (reading and hashing files, building data,
// compressing) on a small pool of low-priority workers, with results handed back where they are needed.
//
//   auto t = tasks::Run([path] { return ReadFile(path); }, {.name = "Read " + path.string()});
//   t.Then([](const Bytes& b) { Use(b); }, tasks::On::GameThread);    // continue on the game thread when done
//   if (t.Ready()) ...;  t.Wait(0.5);  t.Cancel();
//
// Conservative: few workers (cores - 1, at most 3; below normal priority so the game's threads win), three priorities
// (High before Normal before Low, FIFO within each), a task that throws is Failed with its message (logged), never a
// crash of the worker. Cancel() is a request: a queued task is dropped, a running one sees Cancelled() through its token
// (tasks::Current()) and may stop early. Stop() at unload cancels what is queued and joins the workers (never left
// running past the DLL). Without Start() (tests, tools) Run() runs the task inline.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace tasks
{
	enum class Priority : std::uint8_t { High, Normal, Low };
	enum class Status : std::uint8_t { Queued, Running, Done, Failed, Cancelled };
	enum class On : std::uint8_t { Worker, GameThread, RenderThread, Inline };	// Where a continuation runs

	struct Options
	{
		Priority priority = Priority::Normal;
		std::string name;
	};

	using Dispatch = std::function<void(std::function<void()>)>;
	void SetDispatcher(On where, Dispatch dispatch);	// dllmain: the game and render threads' queues

	void Start(int workers = 0);		// 0: cores - 1, between 1 and 3
	void Stop();						// Cancels what is queued, waits for the running ones, joins the workers
	bool Running();

	// A running task's cancellation flag (on a worker, inside the task; false elsewhere)
	bool Cancelled();

	struct Stats
	{
		int workers = 0, busy = 0;
		std::size_t queued[3] = {};
		std::uint64_t completed = 0, failed = 0, cancelled = 0;
		double averageMs = 0.0, longestMs = 0.0;
		std::string longest;			// The name of the longest task so far
	};
	Stats Snapshot();

	namespace detail
	{
		struct State
		{
			std::mutex lock;
			std::condition_variable done;
			std::atomic<Status> status{Status::Queued};
			std::atomic<bool> cancel{false};
			std::string error;
			std::vector<std::function<void()>> continuations;	// Run once it's finished (any outcome)
		};
		void Submit(std::shared_ptr<State> state, Options options, std::function<void()> body);
		void Dispatch(On where, std::function<void()> fn);
		void Finish(State& s, Status status, std::string error = {});
	}

	/// @ingroup mirage_core
	/// @brief Shared handle to a worker result and its completion state.
	template <class T>
	class Task
	{
	public:
		Task() = default;
		Task(std::shared_ptr<detail::State> s, std::shared_ptr<std::optional<T>> r) : state_(std::move(s)), result_(std::move(r)) {}

		Status GetStatus() const { return state_ ? state_->status.load() : Status::Cancelled; }
		bool Ready() const { const Status s = GetStatus(); return s == Status::Done || s == Status::Failed || s == Status::Cancelled; }
		bool Ok() const { return GetStatus() == Status::Done; }
		void Cancel() { if (state_) state_->cancel = true; }
		std::string Error() const
		{
			if (!state_) return "no task";
			std::lock_guard<std::mutex> l(state_->lock);
			return state_->error;
		}
		// Waits up to `seconds` (< 0: as long as it takes). True when it finished.
		bool Wait(double seconds = -1.0) const
		{
			if (!state_) return true;
			std::unique_lock<std::mutex> l(state_->lock);
			const auto finished = [&] { const Status s = state_->status.load(); return s == Status::Done || s == Status::Failed || s == Status::Cancelled; };
			if (seconds < 0.0) state_->done.wait(l, finished);
			else state_->done.wait_for(l, std::chrono::duration<double>(seconds), finished);
			return finished();
		}
		/// @return A borrowed pointer to the completed result, or null. Keep a task handle alive while using it.
		const T* Result() const { return Ok() && result_ && result_->has_value() ? &**result_ : nullptr; }

		// Runs `fn(result)` where asked once it is Done (not when it failed or was cancelled; then `otherwise(error)`)
		void Then(std::function<void(const T&)> fn, On where = On::Worker, std::function<void(const std::string&)> otherwise = {})
		{
			if (!state_) return;
			auto s = state_;
			auto r = result_;
			auto run = [s, r, fn = std::move(fn), otherwise = std::move(otherwise), where] {
				detail::Dispatch(where, [s, r, fn, otherwise] {
					if (s->status.load() == Status::Done && r->has_value()) fn(**r);
					else if (otherwise) otherwise(s->status.load() == Status::Cancelled ? std::string("cancelled") : s->error);
				});
			};
			std::unique_lock<std::mutex> l(s->lock);
			const Status st = s->status.load();
			if (st == Status::Done || st == Status::Failed || st == Status::Cancelled) {
				l.unlock();
				run();
			} else {
				s->continuations.push_back(std::move(run));
			}
		}

	private:
		std::shared_ptr<detail::State> state_;
		std::shared_ptr<std::optional<T>> result_;
	};

	/// @brief Submit work to a worker thread.
	/// @param fn Job moved into the queue. Worker code must avoid direct engine object access.
	/// @param options Scheduling settings.
	/// @return Task handle. A void job has a boolean result on success.
	template <class F>
	auto Run(F&& fn, Options options = {})
	{
		using R0 = std::invoke_result_t<F>;
		using R = std::conditional_t<std::is_void_v<R0>, bool, R0>;
		auto state = std::make_shared<detail::State>();
		auto result = std::make_shared<std::optional<R>>();
		detail::Submit(state, std::move(options), [state, result, fn = std::forward<F>(fn)]() mutable {
			if constexpr (std::is_void_v<R0>) {
				fn();
				*result = true;
			} else {
				*result = fn();
			}
		});
		return Task<R>(state, result);
	}
}
