// The engine's path builder run in the game (see navigation.h).
#include "runtime/navigation.h"

#include "game/game.h"
#include "runtime/hang.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace mirage::nav
{
	namespace
	{
		constexpr std::uintptr_t kImageBase = 0x00400000;
		constexpr std::uintptr_t kScoutExec = 0x00ec1100;	// AScout::Exec (DEFINEPATHS, BUILDCOVER, ADDLONGREACHSPECS, ...)
		constexpr std::size_t kExecSlot = 0x44c / sizeof(void*);
		constexpr std::uintptr_t kGWarn = 0x020186f8;		// FFeedbackContext* GWarn: the builder reports its progress there
		constexpr std::size_t kStatusSlot = 0x1c / sizeof(void*);	// StatusUpdatef(Numerator, Denominator, Fmt, ...): cdecl

		// The builder is editor code: it runs with GIsGame off, as in the editor. In the game, ULevel::AddToNavList
		// (0x00e89990) splices a level's list into the world's as soon as its first point is added, and keeps each new
		// point's old link; the builder then splices every level again (UWorld::AddLevelNavList, 0x00b82c80), which links a
		// level's last point back to its first: the world's list becomes a loop and walking it never ends (found stuck in
		// "Navigation Points on Bases"). With GIsGame off the lists are built the editor's way; after it, they are joined
		// again the game's way (AddLevelNavList with GIsGame on also sets each point up for the game, as a level load does).
		constexpr std::uintptr_t kGIsGame = 0x02027fd8;			// UBOOL GIsGame (SetPlayInEditorWorld, 0x007bbec0, sets it)
		constexpr std::uintptr_t kGWorld = 0x01ffbca4;			// UWorld* GWorld
		constexpr std::uintptr_t kAddLevelNavList = 0x00b82c80;	// UWorld::AddLevelNavList(ULevel*, UBOOL): thiscall, ret 8

		template <class T>
		T& At(std::uintptr_t address)
		{
			return *reinterpret_cast<T*>(address - kImageBase + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
		}

		// Every level's list unlinked from the others (its last point and cover link pointing nowhere), the world's emptied,
		// then each level added again the game's way
		void RelinkNavLists()
		{
			auto* world = At<std::uint8_t*>(kGWorld);
			Classes::AWorldInfo* info = game::World();
			if (!world || !info) return;
			auto** levels = *reinterpret_cast<std::uint8_t***>(world + 0x44);	// UWorld::Levels (TArray<ULevel*>)
			const int count = *reinterpret_cast<int*>(world + 0x48);
			for (int i = 0; i < count; ++i) {
				std::uint8_t* level = levels ? levels[i] : nullptr;
				if (!level) continue;
				if (auto* last = *reinterpret_cast<Classes::ANavigationPoint**>(level + 0x1e0))	// ULevel::NavListEnd
					last->nextNavigationPoint = nullptr;
				if (auto* last = *reinterpret_cast<Classes::ACoverLink**>(level + 0x1e8))			// ULevel::CoverListEnd
					last->NextCoverLink = nullptr;
			}
			info->NavigationPointList = nullptr;
			info->CoverList = nullptr;
			using AddFn = void(__thiscall*)(void*, void*, int);
			const auto add = reinterpret_cast<AddFn>(&At<std::uint8_t>(kAddLevelNavList));
			for (int i = 0; i < count; ++i)
				if (levels && levels[i]) add(world, levels[i], 0);
		}

		// While paths are built: the builder's last status ("BuildPathsDefining 12/366"), for the progress lines
		std::mutex g_progressLock;
		std::wstring g_progress;
		using StatusFn = void(__cdecl*)(void*, int, int, const wchar_t*, ...);
		StatusFn g_status = nullptr;

		void __cdecl Status(void* self, int numerator, int denominator, const wchar_t* fmt, ...)
		{
			wchar_t text[256] = L"";
			if (fmt) {
				va_list args;
				va_start(args, fmt);
				_vsnwprintf_s(text, _TRUNCATE, fmt, args);
				va_end(args);
			}
			{
				std::lock_guard lock(g_progressLock);
				g_progress = std::wstring(text) + L" " + std::to_wstring(numerator) + L"/" + std::to_wstring(denominator);
			}
			if (g_status) g_status(self, numerator, denominator, L"%s", text);
		}

		// GWarn's vtable swapped for a copy whose StatusUpdatef is Status, for as long as it lives
		struct StatusTap
		{
			void** object = nullptr;
			void** original = nullptr;
			void* copy[64]{};
			StatusTap()
			{
				object = At<void**>(kGWarn);
				if (!object) return;
				original = *reinterpret_cast<void***>(object);
				std::memcpy(copy, original, sizeof copy);
				g_status = reinterpret_cast<StatusFn>(original[kStatusSlot]);
				copy[kStatusSlot] = reinterpret_cast<void*>(&Status);
				*reinterpret_cast<void***>(object) = copy;
			}
			~StatusTap()
			{
				if (object) *reinterpret_cast<void***>(object) = original;
			}
		};
	}

	Stats Count()
	{
		Stats s;
		Classes::AWorldInfo* world = game::World();
		if (!world) return s;
		for (auto* n = world->NavigationPointList; n && s.points < 100000; n = n->nextNavigationPoint) {
			++s.points;
			s.reachSpecs += n->PathList.Num();
		}
		return s;
	}

	bool BuildPaths(std::string* error)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = why;
			spdlog::warn("Navigation: paths not built: {}", why);
			return false;
		};
		Classes::AWorldInfo* world = game::World();
		if (!world) return fail("no level");
		Classes::FVector at{};
		if (auto* pawn = game::Pawn()) at = pawn->Location;
		auto* scout = world->Spawn(Classes::ATdScout::StaticClass(), nullptr, Classes::FName{}, at, Classes::FRotator{}, nullptr, true);
		if (!scout) return fail("the Scout (TdGame.TdScout) didn't spawn");

		// The Exec this was found for, where it was found (the exe isn't relocated, but check rather than call a stranger)
		void** vtable = *reinterpret_cast<void***>(scout);
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto exec = reinterpret_cast<std::uintptr_t>(vtable[kExecSlot]);
		if (exec != base + (kScoutExec - kImageBase)) {
			scout->Destroy();
			return fail("the Scout's Exec isn't at the address found (another build of the game?)");
		}

		const Stats before = Count();
		spdlog::info("Navigation: defining paths ({} navigation points; the game thread is busy until it's done)", before.points);
		const auto t0 = std::chrono::steady_clock::now();
		using ExecFn = void(__thiscall*)(void*, const wchar_t*);
		hang::SetLoading(true);		// One long call, like a level load: not a hang
		// Every 3 s while it runs: the builder's last status and where the game thread is (it reports per stage, not per
		// point), so a slow pass shows as progress and a stuck one shows where
		std::atomic<bool> done{false};
		std::thread watcher([&done, t0] {
			while (!done.load()) {
				for (int i = 0; i < 30 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (done.load()) break;
				std::wstring stage;
				{
					std::lock_guard lock(g_progressLock);
					stage = g_progress;
				}
				const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				spdlog::info("Navigation: still defining paths after {:.0f} s: {}; the game thread at {}", s,
							 std::string(stage.begin(), stage.end()), hang::WhereIsGameThread());
			}
		});
		{
			StatusTap tap;
			int& isGame = At<int>(kGIsGame);
			const int wasGame = isGame;
			isGame = 0;
			reinterpret_cast<ExecFn>(vtable[kExecSlot])(scout, L"DEFINEPATHS REVIEWPATHS=0 SHOWMAPCHECK=0");
			isGame = wasGame;
			RelinkNavLists();
		}
		done = true;
		watcher.join();
		hang::SetLoading(false);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		const Stats after = Count();
		scout->Destroy();
		spdlog::info("Navigation: paths defined in {:.0f} ms: {} navigation points, reach specs {} -> {}", ms, after.points,
					 before.reachSpecs, after.reachSpecs);
		return true;
	}
}
