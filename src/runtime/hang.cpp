#include "runtime/hang.h"

#include "core/events.h"
#include "core/notify.h"
#include "failsafe.h"
#include "runtime/crash_reporter.h"
#include "runtime/mem/heap.h"

#if MEMENU_FEATURE_TAS
#include "tas/tas.h"
#endif

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace mirage::hang
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The real clock: on the game thread during a TAS session the usual ones run on the replay's (runtime/mem/heap.h)
		std::int64_t Now() { return static_cast<std::int64_t>(mem::RealTimeMs()); }

		struct Watched
		{
			const char* name;
			std::atomic<std::int64_t> lastBeat{0};		// ms, 0 = never
			std::atomic<DWORD> threadId{0};
			HANDLE thread = nullptr;					// Opened by the watchdog
			bool hung = false;
			std::int64_t hungSince = 0;
		};
		Watched g_game{"game thread"}, g_render{"render thread"};

		std::mutex g_lock;								// Settings, status
		Settings g_settings;
		Status g_status;
		std::atomic<bool> g_loading{false};
		std::thread g_watchdog;
		std::mutex g_wakeLock;
		std::condition_variable g_wake;
		bool g_stopping = false;

		// The stuck thread's sample: fixed storage (filled while it is suspended: nothing may allocate then)
		CONTEXT g_context;
		std::uint8_t g_stack[32 * 1024];
		std::size_t g_stackBytes = 0;

		std::size_t CopyStack(std::uintptr_t from)
		{
			// Page by page while readable (the stack ends somewhere above esp)
			std::size_t copied = 0;
			while (copied < sizeof g_stack) {
				MEMORY_BASIC_INFORMATION mbi{};
				const std::uintptr_t at = from + copied;
				if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
					(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
					break;
				const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				const std::size_t n = std::min<std::size_t>(sizeof g_stack - copied, end - at);
				std::memcpy(g_stack + copied, reinterpret_cast<const void*>(at), n);
				copied += n;
			}
			return copied;
		}

		// Suspends, reads, resumes. Returns false when it could not (then nothing was read).
		bool Sample(HANDLE thread)
		{
			if (!thread || SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
			g_context = {};
			g_context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			const bool ok = GetThreadContext(thread, &g_context) != 0;
			g_stackBytes = ok ? CopyStack(g_context.Esp) : 0;
			ResumeThread(thread);
			return ok;
		}

		bool InModule(std::uintptr_t address, char* out, std::size_t size)
		{
			HMODULE m = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
									reinterpret_cast<LPCWSTR>(address), &m) || !m)
				return false;
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(m, path, MAX_PATH);
			const wchar_t* name = std::wcsrchr(path, L'\\');
			std::snprintf(out, size, "%ls+0x%X", name ? name + 1 : path, static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(m)));
			return true;
		}

		// Where the sample says the thread is: the instruction, then code addresses on its stack
		std::string Describe()
		{
			std::string out;
			char w[MAX_PATH + 32];
			out += InModule(g_context.Eip, w, sizeof w) ? w : "(not in a module)";
			int found = 0;
			for (std::size_t off = 0; off + 4 <= g_stackBytes && found < 12; off += 4) {
				std::uintptr_t v;
				std::memcpy(&v, g_stack + off, 4);
				if (v < 0x10000) continue;
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
					!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
					continue;
				if (!InModule(v, w, sizeof w)) continue;
				out += " < ";
				out += w;
				++found;
			}
			return out;
		}

		// The game's window is minimized (found through the render thread's top-level windows)
		bool Minimized()
		{
			const DWORD tid = g_render.threadId.load();
			if (!tid) return false;
			bool iconic = false;
			EnumThreadWindows(tid, [](HWND h, LPARAM p) -> BOOL {
				if (IsWindowVisible(h) || IsIconic(h)) {
					if (IsIconic(h)) *reinterpret_cast<bool*>(p) = true;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&iconic));
			return iconic;
		}

		bool Suppressed(const Watched& w)
		{
			if (IsDebuggerPresent() || g_loading.load() || Minimized()) return true;
#if MEMENU_FEATURE_TAS
			if (tas::Available() && tas::GetStatus().mode != tas::Mode::Idle) return true;	// The TAS freezes threads itself
#endif
			if (&w == &g_render) {
				// No frames while minimized or in the background is normal
				HWND fg = GetForegroundWindow();
				DWORD pid = 0;
				if (fg) GetWindowThreadProcessId(fg, &pid);
				if (pid != GetCurrentProcessId() || IsIconic(fg)) return true;
			}
			return false;
		}

		void Check(Watched& w, double threshold, bool sample, std::int64_t now)
		{
			const std::int64_t last = w.lastBeat.load();
			if (!last) return;							// Not started yet
			const double since = static_cast<double>(now - last) / 1000.0;
			if (w.hung) {
				if (since < threshold) {				// Progress again
					const double lasted = static_cast<double>(now - w.hungSince) / 1000.0;
					w.hung = false;
					spdlog::warn("Hang: the {} is running again after {:.1f} s", w.name, lasted);
					events::Bus().Publish(events::HangRecovered{w.name, lasted});
					std::lock_guard<std::mutex> l(g_lock);
					g_status.hungNow = g_game.hung || g_render.hung;
				}
				return;
			}
			if (since < threshold) return;
			if (since > 3600.0) {						// Not a hang: a clock that jumped. Start over, report nothing
				w.lastBeat = now;
				return;
			}
			if (Suppressed(w)) {
				w.lastBeat = now;						// A normal pause: the clock starts again after it
				return;
			}
			w.hung = true;
			w.hungSince = last;
			if (!w.thread && w.threadId.load())
				w.thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, w.threadId.load());
			std::string where = "(not sampled)";
			std::wstring report;
			if (sample && Sample(w.thread)) {
				where = Describe();
				// A report like a crash's, from the sample (the process goes on)
				EXCEPTION_RECORD rec{};
				rec.ExceptionCode = 0x80000000 | 0x48414E47;	// "HANG"
				rec.ExceptionAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_context.Eip));
				CONTEXT ctx = g_context;
				EXCEPTION_POINTERS ep{&rec, &ctx};
				report = crash::WriteReport(&ep, w.name == g_game.name ? "the game thread stopped responding" : "the render thread stopped responding",
										   false);		// No minidump: a hang may pass, and tens of MB each time would add up
			}
			spdlog::error("Hang: the {} hasn't made progress for {:.1f} s; it is at {}{}", w.name, since, where,
						  report.empty() ? std::string() : ". Report: " + std::string(report.begin(), report.end()));
			notify::Push(std::string("The game isn't responding (") + w.name + "). " + failsafe::kName + " quits it.");
			events::Bus().Publish(events::HangDetected{w.name, since, where});
			std::lock_guard<std::mutex> l(g_lock);
			++g_status.hangs;
			g_status.hungNow = true;
			char buf[96];
			std::snprintf(buf, sizeof buf, "%s, %.1f s: ", w.name, since);
			g_status.last = buf + where;
		}

		void Watchdog()
		{
			SetThreadDescription(GetCurrentThread(), L"memenu hang detector");
			for (;;) {
				Settings s;
				{
					std::lock_guard<std::mutex> l(g_lock);
					s = g_settings;
				}
				const std::int64_t now = Now();
				if (s.enabled) {
					Check(g_game, s.gameSeconds, s.sampleStack, now);
					Check(g_render, s.renderSeconds, s.sampleStack, now);
				}
				{
					std::lock_guard<std::mutex> l(g_lock);
					g_status.watching = s.enabled;
					const auto since = [now](const Watched& w) { const auto b = w.lastBeat.load(); return b ? (now - b) / 1000.0 : 0.0; };
					g_status.sinceGameBeat = since(g_game);
					g_status.sinceRenderBeat = since(g_render);
				}
				std::unique_lock<std::mutex> wl(g_wakeLock);
				if (g_wake.wait_for(wl, std::chrono::milliseconds(250), [] { return g_stopping; })) return;
			}
		}

		void Beat(Watched& w)
		{
			w.lastBeat.store(Now(), std::memory_order_relaxed);
			if (!w.threadId.load(std::memory_order_relaxed)) w.threadId = GetCurrentThreadId();
		}
	}

	void BeatGame() { Beat(g_game); }
	void BeatRender() { Beat(g_render); }
	void SetLoading(bool loading)
	{
		g_loading = loading;
		if (!loading) {		// The clocks start again after a load
			const auto now = Now();
			if (g_game.lastBeat.load()) g_game.lastBeat = now;
			if (g_render.lastBeat.load()) g_render.lastBeat = now;
		}
	}

	void Configure(const Settings& settings)
	{
		std::lock_guard<std::mutex> l(g_lock);
		g_settings = settings;
	}

	Settings Configured()
	{
		std::lock_guard<std::mutex> l(g_lock);
		return g_settings;
	}

	Status GetStatus()
	{
		std::lock_guard<std::mutex> l(g_lock);
		return g_status;
	}

	void Start()
	{
		if (g_watchdog.joinable()) return;
		{
			std::lock_guard<std::mutex> l(g_wakeLock);
			g_stopping = false;
		}
		g_watchdog = std::thread(Watchdog);
		spdlog::info("Hang detector: watching the game and render threads");
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> l(g_wakeLock);
			g_stopping = true;
		}
		g_wake.notify_all();
		if (g_watchdog.joinable()) g_watchdog.join();
		for (Watched* w : {&g_game, &g_render})
			if (w->thread) {
				CloseHandle(w->thread);
				w->thread = nullptr;
			}
	}
}
