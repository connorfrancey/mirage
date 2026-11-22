#include "runtime/mod_watch.h"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace mirage::mods::watch
{
	namespace
	{
		namespace fs = std::filesystem;

		struct Watch
		{
			HANDLE dir = INVALID_HANDLE_VALUE;
			OVERLAPPED request{};
			alignas(DWORD) std::byte buffer[32 * 1024];
			bool pending = false;

			~Watch()
			{
				if (pending) {
					CancelIoEx(dir, &request);
					DWORD ignored = 0;
					GetOverlappedResult(dir, &request, &ignored, TRUE);
				}
				if (request.hEvent) CloseHandle(request.hEvent);
				if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
			}

			bool Arm()
			{
				constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
					FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
				ResetEvent(request.hEvent);
				pending = ReadDirectoryChangesW(dir, buffer, sizeof buffer, TRUE, filter, nullptr, &request, nullptr) != 0;
				return pending;
			}
		};

		std::thread g_thread;
		HANDLE g_stop = nullptr;
		std::mutex g_lock;
		Changes g_changes{true, {}};
		bool g_poll = true;

		bool Ignored(std::wstring_view path)
		{
			const auto slash = path.find(L'\\');
			if (slash == std::wstring_view::npos)
				return path == L"disabled.toml" || path == L"crashes.toml" || path == L"starting.toml";
			const auto rest = path.substr(slash + 1);
			const auto top = rest.substr(0, rest.find(L'\\'));
			return rest == L"config.toml" || top == L".mirage" || top == L"obj" || top == L"bin";
		}

		void ReadChanges(const Watch& watch, std::size_t bytes)
		{
			std::lock_guard lock(g_lock);
			constexpr auto header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
			for (std::size_t at = 0; at < bytes;) {
				const auto remaining = bytes - at;
				if (remaining < header) break;
				const auto* entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watch.buffer + at);
				const auto length = entry->FileNameLength;
				if (!length || length > remaining - header || length % sizeof(wchar_t)) break;
				const std::wstring_view path(entry->FileName, length / sizeof(wchar_t));
				if (!Ignored(path)) g_changes.folders.emplace(path.substr(0, path.find(L'\\')));
				const auto next = entry->NextEntryOffset;
				if (!next) return;
				if (next < header + length || next >= remaining || next % alignof(DWORD)) break;
				at += next;
			}
			// A lost or incomplete notification batch requires a full check.
			g_changes.scanAll = true;
		}

		void Run(std::vector<std::unique_ptr<Watch>> watches, HANDLE stop)
		{
			std::vector<HANDLE> events{stop};
			for (const auto& watch : watches) events.push_back(watch->request.hEvent);
			for (;;) {
				const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
				if (result == WAIT_OBJECT_0) return;
				const auto index = result - WAIT_OBJECT_0 - 1;
				if (index >= watches.size()) break;
				Watch& watch = *watches[index];
				DWORD bytes = 0;
				const bool received = GetOverlappedResult(watch.dir, &watch.request, &bytes, FALSE) != 0;
				if (!received && GetLastError() == ERROR_IO_INCOMPLETE) break;
				watch.pending = false;
				ReadChanges(watch, received ? bytes : 0);
				if (!watch.Arm()) break;
			}
			std::lock_guard lock(g_lock);
			g_poll = true;
		}
	}

	void Start(const std::vector<fs::path>& roots)
	{
		if (g_thread.joinable() || roots.empty() || roots.size() >= MAXIMUM_WAIT_OBJECTS) return;
		std::vector<std::unique_ptr<Watch>> watches;
		for (const auto& root : roots) {
			auto watch = std::make_unique<Watch>();
			watch->dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
			if (watch->dir == INVALID_HANDLE_VALUE) return;
			watch->request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!watch->request.hEvent || !watch->Arm()) return;
			watches.push_back(std::move(watch));
		}

		g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!g_stop) return;
		{
			std::lock_guard lock(g_lock);
			g_poll = false;
		}
		try {
			g_thread = std::thread(Run, std::move(watches), g_stop);
		} catch (const std::system_error&) {
			Stop();
		}
	}

	void Stop()
	{
		if (g_thread.joinable()) {
			SetEvent(g_stop);
			g_thread.join();
		}
		if (g_stop) CloseHandle(g_stop);
		g_stop = nullptr;
		std::lock_guard lock(g_lock);
		g_poll = true;
		g_changes = {true, {}};
	}

	Changes TakeChanges()
	{
		std::lock_guard lock(g_lock);
		Changes result{g_poll || g_changes.scanAll, {}};
		result.folders.swap(g_changes.folders);
		g_changes.scanAll = false;
		return result;
	}

	std::uintmax_t Fingerprint(const fs::path& folder)
	{
		std::uintmax_t hash = 1469598103934665603ull;
		const auto mix = [&](std::uintmax_t value) { hash = (hash ^ value) * 1099511628211ull; };
		std::error_code ec;
		for (auto entry = fs::recursive_directory_iterator(folder, ec); !ec && entry != fs::recursive_directory_iterator();
			 entry.increment(ec)) {
			const auto& path = entry->path();
			const auto name = path.filename();
			if (entry->is_directory(ec)) {
				// Layers, routes and Kismet already reload their own root folders.
				if (name == ".mirage" || (path.parent_path() == folder && (name == "levels" || name == "routes" || name == "kismet")))
					entry.disable_recursion_pending();
			} else if (name != "config.toml" && entry->is_regular_file(ec)) {
				const auto written = entry->last_write_time(ec);
				if (!ec) {
					const auto size = entry->file_size(ec);
					if (!ec) {
						mix(std::hash<std::wstring>{}(path.native()));
						mix(static_cast<std::uintmax_t>(written.time_since_epoch().count()));
						mix(size);
					}
				}
			}
			// An editor can replace a file between enumeration and a metadata query.
			ec.clear();
		}
		return hash;
	}
}
