#include "runtime/crash_reporter.h"

#include "build_info.h"
#include "paths.h"
#include "runtime/mem/manager.h"
#include "runtime/mods.h"

#include "core/hooking.h"
#include <spdlog/spdlog.h>

#include <DbgHelp.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <mutex>

namespace mirage::crash
{
	namespace
	{
		namespace fs = std::filesystem;

		using MiniDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
										  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
		using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);

		MiniDumpFn oMiniDump = nullptr;
		SetFilterFn oSetFilter = nullptr;
		void* g_miniDumpTarget = nullptr;
		void* g_setFilterTarget = nullptr;
		LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter = nullptr;	// What the game set (called in Game mode)
		std::atomic<int> g_mode{static_cast<int>(Mode::Mirage)};
		std::atomic<bool> g_openReport{true};
		std::atomic<bool> g_installed{false};
		std::atomic<LONG> g_handling{0};
		bool TakeHandling() { LONG expected = 0; return g_handling.compare_exchange_strong(expected, 1); }						// One fatal crash at a time

		// ---- What the reporter thread works from: fixed storage, filled by the crashing thread ----------------------
		wchar_t g_folder[MAX_PATH];
		HMODULE g_self = nullptr;
		struct Job
		{
			EXCEPTION_RECORD record;
			CONTEXT context;
			EXCEPTION_POINTERS pointers;
			DWORD thread;
			const char* title;
			bool fatal;					// A crash that ends the game ("crash-" files: shown at the next start)
			bool dump;					// A minidump too
			wchar_t reportPath[MAX_PATH];
			bool ok;
		};
		Job g_job;
		HANDLE g_jobReady = nullptr, g_jobDone = nullptr, g_reporter = nullptr;
		char g_text[256 * 1024];			// The report, built here
		std::size_t g_len = 0;

		void Put(const char* fmt, ...)
		{
			if (g_len >= sizeof g_text - 1) return;
			va_list args;
			va_start(args, fmt);
			const int n = std::vsnprintf(g_text + g_len, sizeof g_text - g_len, fmt, args);
			va_end(args);
			if (n > 0) g_len = std::min(sizeof g_text - 1, g_len + static_cast<std::size_t>(n));
		}

		const char* CodeName(DWORD code)
		{
			switch (code) {
				case EXCEPTION_ACCESS_VIOLATION: return "access violation";
				case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
				case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
				case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
				case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
				case EXCEPTION_IN_PAGE_ERROR: return "in-page error (a mapped file or the page file couldn't be read)";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float division by zero";
				case EXCEPTION_BREAKPOINT: return "breakpoint";
				case 0xE06D7363: return "C++ exception not caught";
				case 0xC0000409: return "stack buffer overrun (/GS) or fail-fast";
				case 0xC0000374: return "heap corruption";
				default: return "exception";
			}
		}

		// "Mirage.dll+0x1234" into `out`; true when the address is in a module
		bool Where(std::uintptr_t address, char* out, std::size_t size, HMODULE* module = nullptr)
		{
			HMODULE m = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								   reinterpret_cast<LPCWSTR>(address), &m) && m) {
				wchar_t path[MAX_PATH] = {};
				GetModuleFileNameW(m, path, MAX_PATH);
				const wchar_t* name = std::wcsrchr(path, L'\\');
				std::snprintf(out, size, "%ls+0x%X", name ? name + 1 : path, static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(m)));
				if (module) *module = m;
				return true;
			}
			std::snprintf(out, size, "0x%08X", static_cast<unsigned>(address));
			if (module) *module = nullptr;
			return false;
		}

		bool Readable(std::uintptr_t address, std::size_t bytes)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi)) return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
			return address + bytes <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		bool Executable(std::uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
			return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		// The essentials: what, where, registers, stack. Fixed buffers and Win32 calls only.
		void WriteEssentials(const Job& job)
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			Put("Mirage crash report (%s)\n", job.title ? job.title : "crash");
			Put("Time: %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			Put("Menu: %s\nGame: %ls (pid %lu, thread %lu)\n\n", build_info::kVersion, exe, GetCurrentProcessId(), job.thread);

			const EXCEPTION_RECORD& r = job.record;
			char where[MAX_PATH + 32];
			HMODULE faulting = nullptr;
			Where(reinterpret_cast<std::uintptr_t>(r.ExceptionAddress), where, sizeof where, &faulting);
			Put("What: %s (0x%08lX)\nWhere: %s\n", CodeName(r.ExceptionCode), r.ExceptionCode, where);
			if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
				const char* kind = r.ExceptionInformation[0] == 0 ? "reading" : r.ExceptionInformation[0] == 1 ? "writing" : "executing";
				Put("Access: %s 0x%08X\n", kind, static_cast<unsigned>(r.ExceptionInformation[1]));
			}
			// Whose code it was
			const char* who = "the game";
			if (faulting == g_self) who = "the menu (Mirage.dll)";
			else if (faulting) {
				wchar_t path[MAX_PATH] = {};
				GetModuleFileNameW(faulting, path, MAX_PATH);
				if (std::wcsstr(path, L"\\mirage\\mods\\") || std::wcsstr(path, L"\\.mirage\\")) who = "a mod (its DLL, see Where)";
				else if (!std::wcsstr(path, L"\\Binaries\\")) who = "a module outside the game (a driver, an overlay, ...)";
			}
			Put("Whose code: %s\n\n", who);

			const CONTEXT& c = job.context;
			Put("Registers:\n  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX\n  esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX\n\n",
				c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.Eip);

			// The call stack through frame pointers (complete for the menu's debug builds, partial for optimized code)
			Put("Call stack (frame pointers):\n");
			Put("  #0  %s\n", where);
			std::uintptr_t ebp = c.Ebp;
			for (int i = 1; i < 64 && ebp && Readable(ebp, 8); ++i) {
				const auto* frame = reinterpret_cast<const std::uintptr_t*>(ebp);
				const std::uintptr_t ret = frame[1], next = frame[0];
				if (!ret) break;
				char w[MAX_PATH + 32];
				Where(ret, w, sizeof w);
				Put("  #%-2d %s\n", i, w);
				if (next <= ebp) break;		// Stacks grow down: a frame further up is at a higher address
				ebp = next;
			}
			// Return addresses found on the stack (for code without frame pointers: some are stale, read with care)
			Put("\nCode addresses on the stack (from esp, may include stale ones):\n");
			int found = 0;
			for (std::uintptr_t p = c.Esp; found < 40 && p < c.Esp + 16384 && Readable(p, 4); p += 4) {
				const std::uintptr_t v = *reinterpret_cast<const std::uintptr_t*>(p);
				if (v < 0x10000 || !Executable(v)) continue;
				char w[MAX_PATH + 32];
				if (!Where(v, w, sizeof w)) continue;
				Put("  [esp+0x%04X] %s\n", static_cast<unsigned>(p - c.Esp), w);
				++found;
			}
		}

		// Loaded modules (takes the loader lock: after the essentials, inside the time limit)
		void WriteModules()
		{
			HMODULE mods[512];
			DWORD needed = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
			const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 512);
			Put("\nModules (%lu):\n", n);
			for (DWORD i = 0; i < n; ++i) {
				wchar_t path[MAX_PATH] = {};
				GetModuleFileNameW(mods[i], path, MAX_PATH);
				MODULEINFO mi{};
				K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
				Put("  %08X %7lu KB  %ls\n", static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(mods[i])), mi.SizeOfImage / 1024, path);
			}
		}

		// The parts that need the heap and the menu's state: each guarded, skipped if it fails
		void WriteMirageParts()
		{
			try {
				Put("\nMods loaded:\n");
				for (const mods::Info& m : mods::List())
					if (m.loaded) Put("  %s %s (%s)\n", m.id.c_str(), m.version.c_str(), m.kind.c_str());
			} catch (...) {
				Put("  (could not be listed)\n");
			}
			try {
				const mem::Status st = mem::GetStatus();
				Put("\nMemory: pressure %s; private %.0f MB; address space %.0f MB free, largest block %.0f MB; menu heap %.1f MB\n",
					mem::LevelName(st.level), st.process.privateBytes / 1048576.0, st.address.free.totalFree / 1048576.0,
					st.address.free.largestFree / 1048576.0, st.heap.live / 1048576.0);
			} catch (...) {
				Put("\nMemory: (could not be read)\n");
			}
		}

		// SEH needs a function without C++ objects to unwind
		bool Guarded(void (*fn)())
		{
			__try {
				fn();
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		const Job* g_current = nullptr;
		void EssentialsOfCurrent() { WriteEssentials(*g_current); }

		bool OpenFile(const wchar_t* path, HANDLE* out)
		{
			*out = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			return *out != INVALID_HANDLE_VALUE;
		}

		// Runs on the reporter thread
		void Produce(Job& job)
		{
			g_len = 0;
			g_current = &job;
			CreateDirectoryW(g_folder, nullptr);
			SYSTEMTIME t;
			GetLocalTime(&t);
			wchar_t base[MAX_PATH];
			std::swprintf(base, MAX_PATH, L"%s\\%s-%04d%02d%02d-%02d%02d%02d", g_folder, job.fatal ? L"crash" : L"report", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
						  t.wSecond);
			std::swprintf(job.reportPath, MAX_PATH, L"%s.txt", base);

			if (!Guarded(&EssentialsOfCurrent)) Put("\n(the report stopped early: reading the crash failed)\n");
			// Written now, so the essentials are on disk whatever happens next
			HANDLE f;
			if (OpenFile(job.reportPath, &f)) {
				DWORD written = 0;
				WriteFile(f, g_text, static_cast<DWORD>(g_len), &written, nullptr);
				const std::size_t essentials = g_len;
				Guarded(&WriteModules);
				Guarded(&WriteMirageParts);
				Put("\nThe menu's log: %ls\\..\\logs\\mirage.log\n", g_folder);
				WriteFile(f, g_text + essentials, static_cast<DWORD>(g_len - essentials), &written, nullptr);
				FlushFileBuffers(f);
				CloseHandle(f);
				job.ok = true;
			}
			// The minidump, from this thread (dbghelp's advice: not from the one that crashed)
			wchar_t dmp[MAX_PATH];
			std::swprintf(dmp, MAX_PATH, L"%s.dmp", base);
			if (job.dump && oMiniDump && OpenFile(dmp, &f)) {
				MINIDUMP_EXCEPTION_INFORMATION mei{job.thread, &job.pointers, FALSE};
				oMiniDump(GetCurrentProcess(), GetCurrentProcessId(), f,
						  static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo),
						  &mei, nullptr, nullptr);
				CloseHandle(f);
			}
		}

		DWORD WINAPI ReporterThread(LPVOID)
		{
			for (;;) {
				if (WaitForSingleObject(g_jobReady, INFINITE) != WAIT_OBJECT_0) return 0;
				if (!g_installed.load()) return 0;
				Produce(g_job);
				SetEvent(g_jobDone);
			}
		}

		// Hands the crash to the reporter thread and waits (at most 20 s: a stuck report never keeps a dead game up)
		bool Handoff(const EXCEPTION_POINTERS* ep, const char* title, bool fatal, bool dump = true)
		{
			if (!g_reporter || !ep || !ep->ExceptionRecord) return false;
			g_job.record = *ep->ExceptionRecord;
			if (ep->ContextRecord) g_job.context = *ep->ContextRecord;
			g_job.pointers = {&g_job.record, &g_job.context};
			g_job.thread = GetCurrentThreadId();
			g_job.title = title;
			g_job.fatal = fatal;
			g_job.dump = dump;
			g_job.reportPath[0] = 0;
			g_job.ok = false;
			ResetEvent(g_jobDone);
			SetEvent(g_jobReady);
			return WaitForSingleObject(g_jobDone, 20000) == WAIT_OBJECT_0 && g_job.ok;
		}

		[[noreturn]] void Die(const EXCEPTION_POINTERS* ep)
		{
			const bool written = Handoff(ep, "the game crashed", true);
			// Best effort: the log too (another thread could hold its lock: not waited for long)
			HANDLE flush = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { spdlog::default_logger_raw()->flush(); return 0; }, nullptr, 0, nullptr);
			if (flush) {
				WaitForSingleObject(flush, 1000);
				CloseHandle(flush);
			}
			if (written && g_openReport.load()) {
				// Notepad shows it once the fullscreen game is gone
				wchar_t cmd[MAX_PATH + 32];
				std::swprintf(cmd, MAX_PATH + 32, L"notepad.exe \"%s\"", g_job.reportPath);
				STARTUPINFOW si{sizeof si};
				PROCESS_INFORMATION pi{};
				if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
					CloseHandle(pi.hThread);
					CloseHandle(pi.hProcess);
				}
			}
			TerminateProcess(GetCurrentProcess(), ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 1);
			for (;;) Sleep(1000);
		}

		// ---- The hooks ---------------------------------------------------------------------------------------------
		BOOL WINAPI HookedMiniDump(HANDLE process, DWORD pid, HANDLE file, MINIDUMP_TYPE type, PMINIDUMP_EXCEPTION_INFORMATION info,
								   PMINIDUMP_USER_STREAM_INFORMATION user, PMINIDUMP_CALLBACK_INFORMATION callback)
		{
			// The game's crash handler (an exception record, in this process, not our own dump): the crash is fatal
			if (g_mode.load() == static_cast<int>(Mode::Mirage) && info && info->ExceptionPointers && !info->ClientPointers &&
				pid == GetCurrentProcessId() && GetCurrentThreadId() != GetThreadId(g_reporter) &&
				TakeHandling()) {
				Die(info->ExceptionPointers);
			}
			return oMiniDump(process, pid, file, type, info, user, callback);
		}

		LONG WINAPI OurFilter(EXCEPTION_POINTERS* ep)
		{
			if (g_mode.load() == static_cast<int>(Mode::Game) || !TakeHandling())
				return g_gameFilter ? g_gameFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
			Die(ep);
		}

		LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookedSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
		{
			// Ours stays the process's filter; the game's is kept for "the game's own" mode
			if (filter == &OurFilter) return oSetFilter(filter);
			LPTOP_LEVEL_EXCEPTION_FILTER previous = g_gameFilter;
			g_gameFilter = filter;
			return previous;
		}

		std::filesystem::path FolderPath() { return paths::Dir() / "crashlogs"; }
		std::filesystem::path SeenMarker() { return FolderPath() / "last-shown.txt"; }
	}

	void Install()
	{
		if (g_installed.exchange(true)) return;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   reinterpret_cast<LPCWSTR>(&Install), &g_self);
		const std::wstring folder = FolderPath().wstring();
		wcsncpy_s(g_folder, folder.c_str(), _TRUNCATE);
		g_jobReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g_jobDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_reporter = CreateThread(nullptr, 256 * 1024, ReporterThread, nullptr, 0, nullptr);
		if (g_reporter) SetThreadDescription(g_reporter, L"mirage crash reporter");

		// dbghelp is in the game's imports: loaded already
		if (HMODULE dbghelp = GetModuleHandleW(L"dbghelp.dll")) {
			g_miniDumpTarget = reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
			if (!g_miniDumpTarget || !hooking::Install(g_miniDumpTarget, &HookedMiniDump, &oMiniDump, "MiniDumpWriteDump"))
				g_miniDumpTarget = nullptr;
		}
		g_setFilterTarget = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter"));
		if (g_setFilterTarget && hooking::Install(g_setFilterTarget, &HookedSetFilter, &oSetFilter, "SetUnhandledExceptionFilter")) {
			g_gameFilter = oSetFilter(&OurFilter);
		} else {
			g_setFilterTarget = nullptr;
			g_gameFilter = SetUnhandledExceptionFilter(&OurFilter);
		}
		spdlog::info("Crash reporter: installed ({} the game's crash handler, unhandled-exception filter{}), reports in {}",
					 g_miniDumpTarget ? "in front of" : "NOT in front of (dbghelp missing)", g_setFilterTarget ? " kept on top" : "",
					 FolderPath().string());
	}

	void Uninstall()
	{
		if (!g_installed.exchange(false)) return;
		if (g_setFilterTarget) {
			oSetFilter(g_gameFilter);
			hooking::Remove(g_setFilterTarget);
		} else {
			SetUnhandledExceptionFilter(g_gameFilter);
		}
		if (g_miniDumpTarget) {
			hooking::Remove(g_miniDumpTarget);
		}
		if (g_reporter) {
			SetEvent(g_jobReady);		// Wakes it: it sees g_installed false and ends
			WaitForSingleObject(g_reporter, 2000);
			CloseHandle(g_reporter);
			g_reporter = nullptr;
		}
	}

	void SetMode(Mode mode) { g_mode = static_cast<int>(mode); }
	Mode GetMode() { return static_cast<Mode>(g_mode.load()); }
	void SetOpenReport(bool open) { g_openReport = open; }

	std::wstring WriteReport(const EXCEPTION_POINTERS* ep, const char* title, bool dump)
	{
		static std::mutex lock;
		std::lock_guard<std::mutex> l(lock);
		if (!Handoff(ep, title, false, dump)) return {};
		return g_job.reportPath;
	}

	std::filesystem::path Folder() { return FolderPath(); }

	std::string ReadReport(const fs::path& path, std::size_t maxBytes)
	{
		std::ifstream in(path, std::ios::binary);
		std::string text(maxBytes, '\0');
		in.read(text.data(), static_cast<std::streamsize>(maxBytes));
		text.resize(static_cast<std::size_t>(in.gcount()));
		return text;
	}

	std::vector<Report> List(std::size_t max)
	{
		std::vector<Report> out;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(FolderPath(), ec)) {
			if (e.path().extension() != ".txt" || e.path().filename().string().rfind("crash-", 0) != 0) continue;
			Report r;
			r.path = e.path();
			r.time = e.last_write_time(ec);
			// The summary: the lines from "What:" to "Whose code:"
			const std::string text = ReadReport(r.path, 4096);
			for (const char* key : {"What: ", "Where: ", "Access: ", "Whose code: "}) {
				const auto at = text.find(key);
				if (at == std::string::npos) continue;
				r.summary += text.substr(at, text.find('\n', at) - at) + "\n";
			}
			out.push_back(std::move(r));
		}
		std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) { return a.time > b.time; });
		if (out.size() > max) out.resize(max);
		return out;
	}

	bool TakeUnseen(Report& out)
	{
		const auto all = List(1);
		if (all.empty()) return false;
		const std::string name = all[0].path.filename().string();
		std::string seen;
		{
			std::ifstream in(SeenMarker());
			std::getline(in, seen);
		}
		if (seen == name) return false;
		std::ofstream(SeenMarker(), std::ios::trunc) << name;
		out = all[0];
		return true;
	}
}
