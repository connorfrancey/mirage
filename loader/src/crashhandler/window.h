#pragma once
#include <string>

#include "crashhandler/analysis.h"

// The crash window (like Geode's BetterCrashlogs): when the game crashes, the crashed thread waits here while the menu
// draws only this window over the frozen game: what happened and where, the likely causes, both call stacks, the
// registers and stack memory annotated, the code at the crash, the mods and the log. The player copies the report,
// opens it or its folder, restarts the game or closes it; continues past a breakpoint; or ends the crashed thread
// when it isn't the game's or the render thread.
//
// Drawn from the crashed thread when it owns the game's window (it pumps the window's messages, so the window takes
// clicks) through hooks::DrawWhileStalled, or by the render thread as usual when the game is still running (a crash on
// another thread). When no frame is drawn within a few seconds, Run gives up and the crash goes the old way.
namespace crashhandler::window
{
	enum class Action { None, Close, Restart, Continue, EndThread };

	struct Options
	{
		bool canContinue = false;		// The crash can be resumed (a breakpoint, from the unhandled-exception filter)
		bool canEndThread = false;		// Not the game or the render thread: it can end alone
		std::wstring report;			// The report's file
		std::wstring folder;			// The reports' folder
	};

	// Blocks the calling (crashed) thread with the window until the player chooses. None: it couldn't be shown.
	Action Run(const Analysis& analysis, const Options& options, double noFrameSeconds = 4.0);

	bool Showing();					// Run is waiting for a choice
	unsigned FramesDrawn();			// Frames the live window drew since Run began
	void Choose(Action action);		// Answers Run as if clicked (the self-test)

	// The same window over the running game (no actions but the files): the self-test, and "the last crash"
	void Preview(const Analysis& analysis, const std::wstring& report = {});
	bool PreviewOpen();
	void ClosePreview();
	void DrawPreview();				// Render thread, every frame (the Crash Reporter module)
	unsigned PreviewFrames();
}
