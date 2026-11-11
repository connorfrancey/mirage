// The mod API's game input (the engine's input monitor) and files in a mod's own folder.
#include "runtime/mod_io.h"

#include "runtime/mod_internal.h"

#if MEMENU_FEATURE_TAS
#include "tas/input_monitor.h"
#include "tas/tas.h"
#endif

#include <Windows.h>
#include <shellapi.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mirage::mods::io
{
	namespace
	{
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		// `rel` inside the mod's folder, or empty when it would leave it (.., a drive, an absolute path)
		fs::path Inside(MirageMod* mod, const char* rel, bool allowEmpty = false)
		{
			if (!mod || !rel) return {};
			const std::string r = rel;
			if (r.empty() && !allowEmpty) return {};
			const fs::path p = FromUtf8(r);
			if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return {};
			for (const auto& part : p)
				if (part == "..") return {};
			return FromUtf8(mod->dir) / p;
		}

		uint32_t CopyOut(const std::string& s, char* buffer, uint32_t capacity)
		{
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(s.size(), capacity - 1);
				std::memcpy(buffer, s.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(s.size());
		}
	}

	// ---- Input ----------------------------------------------------------------------------------------------------------
	int32_t MIRAGE_CALL InputKeyState(const char* key, MirageKeyState* out)
	{
		if (!key || !out || out->size < sizeof(MirageKeyState)) return 0;
		*out = MirageKeyState{sizeof(MirageKeyState), 0, 0, -1.0, -1.0};
#if MEMENU_FEATURE_TAS
		if (!tas::input::Installed()) return 0;
		const auto snap = tas::input::Get(0.05);
		if (const auto it = snap.keys.find(Lower(key)); it != snap.keys.end()) {
			out->held = it->second.held ? 1 : 0;
			out->presses = it->second.presses;
			out->down = it->second.down;
			out->up = it->second.up;
		}
		return 1;
#else
		return 0;
#endif
	}

	void MIRAGE_CALL InputMouse(float window, float* x, float* y)
	{
		float mx = 0.f, my = 0.f;
#if MEMENU_FEATURE_TAS
		if (tas::input::Installed()) {
			const auto snap = tas::input::Get(window > 0.f ? window : 0.05);
			mx = snap.mouseX;
			my = snap.mouseY;
		}
#else
		(void)window;
#endif
		if (x) *x = mx;
		if (y) *y = my;
	}

	int32_t MIRAGE_CALL InputKps()
	{
#if MEMENU_FEATURE_TAS
		if (tas::input::Installed()) return tas::input::Get(0.05).kps;
#endif
		return 0;
	}

	double MIRAGE_CALL InputNow()
	{
#if MEMENU_FEATURE_TAS
		if (tas::input::Installed()) return tas::input::Now();
#endif
		LARGE_INTEGER f, c;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&c);
		return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
	}

	void MIRAGE_CALL InputResetCounts()
	{
#if MEMENU_FEATURE_TAS
		tas::input::ResetCounts();
#endif
	}

	int32_t MIRAGE_CALL TasSession()
	{
#if MEMENU_FEATURE_TAS
		switch (tas::GetStatus().mode) {
			case tas::Mode::Recording: return 1;
			case tas::Mode::Playing: return 2;
			default: return 0;
		}
#else
		return 0;
#endif
	}

	// ---- Files ----------------------------------------------------------------------------------------------------------
	int32_t MIRAGE_CALL FileRead(MirageMod* mod, const char* path, char* buffer, uint32_t capacity)
	{
		const fs::path p = Inside(mod, path);
		if (p.empty()) return -1;
		std::ifstream in(p, std::ios::binary);
		if (!in) return -1;
		std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (buffer && capacity) std::memcpy(buffer, data.data(), std::min<std::size_t>(data.size(), capacity));
		return static_cast<int32_t>(std::min<std::size_t>(data.size(), INT32_MAX));
	}

	int32_t MIRAGE_CALL FileWrite(MirageMod* mod, const char* path, const char* data, uint32_t size)
	{
		const fs::path p = Inside(mod, path);
		if (p.empty() || (!data && size)) return 0;
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
		fs::path tmp = p;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out || !out.write(data, size)) return 0;
		}
		fs::rename(tmp, p, ec);
		if (ec) {
			spdlog::warn("[mod {}] can't write {}: {}", mod->id, ToUtf8(p), ec.message());
			return 0;
		}
		return 1;
	}

	uint32_t MIRAGE_CALL FileList(MirageMod* mod, const char* folder, const char* extension, char* buffer, uint32_t capacity)
	{
		std::vector<std::string> names;
		const fs::path dir = Inside(mod, folder ? folder : "", true);
		std::error_code ec;
		if (!dir.empty())
			for (const auto& e : fs::directory_iterator(dir, ec)) {
				if (!e.is_regular_file(ec)) continue;
				if (extension && *extension && Lower(ToUtf8(e.path().extension())) != Lower(extension)) continue;
				names.push_back(ToUtf8(e.path().filename()));
			}
		std::sort(names.begin(), names.end());
		std::string joined;
		for (const std::string& n : names) joined += n + "\n";
		return CopyOut(joined, buffer, capacity);
	}

	uint64_t MIRAGE_CALL FileStamp(MirageMod* mod, const char* path)
	{
		const fs::path p = Inside(mod, path);
		std::error_code ec;
		if (p.empty() || !fs::exists(p, ec)) return 0;
		const auto t = fs::last_write_time(p, ec);
		return ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
	}

	void MIRAGE_CALL FileOpenFolder(MirageMod* mod, const char* folder)
	{
		const fs::path dir = Inside(mod, folder ? folder : "", true);
		if (dir.empty()) return;
		std::error_code ec;
		fs::create_directories(dir, ec);
		ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
}
