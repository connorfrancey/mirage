#pragma once
// The crash reporter (like Geode's): when the game is about to die, Mirage handles it instead of the game's own crash
// dialog. It writes a crash report (what happened, where, the registers, the stack, the suspected mod, the mods loaded,
// memory, the modules in the process) and a minidump to mirage\crashlogs\, opens the report once the game is gone, and
// at the next start the menu shows "the game crashed last time" with the report.
//
// How it takes over, without reverse engineering the game:
//   - UE3's own crash handler writes a minidump (dbghelp!MiniDumpWriteDump) before its dialog: that call is hooked, so
//     the moment the game decides a crash is fatal is ours, with its exception record.
//   - An unhandled exception filter for crashes outside the game's guard (other threads); SetUnhandledExceptionFilter
//     is hooked so the game can't replace it (its filter is kept and called in "the game's own" mode).
// Crashes inside mods' guarded calls are not fatal and never come here (the runtime stops the mod instead).
//
// Safe in a dying process: the report is written by a thread made at startup and parked (no thread creation, no heap
// for the essential part: fixed buffers and Win32 calls), with a time limit; what needs more (the mods, memory, the
// log) is added after, each part guarded, and skipped if it fails. Then the process ends.
#include <Windows.h>

#include "crashhandler/analysis.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mirage::crash
{
	enum class Mode { Mirage = 0, Game = 1 };	// Who handles a fatal crash

	void Install();				// After hooks::Install (the hooks are up); before the game can crash
	void Uninstall();
	void SetMode(Mode mode);
	Mode GetMode();
	void SetOpenReport(bool open);				// Open the report in Notepad once the game is gone (no crash window)
	void SetWindow(bool show);					// The crash window for a fatal crash (crashhandler/window.h)
	void SetFatalDump(bool dump);				// A fatal crash writes a minidump (on unless the self-test turns it off)
	// Nobody is there to answer (the self-test runs unattended): a fatal crash writes its report and ends the game,
	// without the crash window waiting for a click or Notepad opening.
	void SetUnattended(bool unattended);
	// The analysis of the last report written (crashhandler/analysis.h). False if none.
	bool LastAnalysis(crashhandler::Analysis& out);

	// Writes a report for `ep` now (the self-test, a hang report). Returns the report's path ("" if it failed). Does not
	// end the process. `dump`: a minidump next to it too (tens of MB; a fatal crash always gets one), written by the
	// helper process mirage_crashdump.exe (loader/crashdump/): dbghelp suspends every thread of the process it dumps, and
	// from inside the game it deadlocked when one of them held a lock it needed. Without the helper, only a fatal crash
	// is dumped (from inside).
	std::wstring WriteReport(const EXCEPTION_POINTERS* ep, const char* title, bool dump = true);

	std::filesystem::path Folder();				// mirage\crashlogs

	struct Report
	{
		std::filesystem::path path;
		std::string summary;					// The first lines (what, where, suspected)
		std::filesystem::file_time_type time;
	};
	std::vector<Report> List(std::size_t max = 20);	// Newest first
	// The newest crash report from a session before this one that hasn't been shown yet (and marks it shown)
	bool TakeUnseen(Report& out);
	std::string ReadReport(const std::filesystem::path& path, std::size_t maxBytes = 64 * 1024);
}
