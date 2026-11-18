#pragma once
// The hang detector: a watchdog thread that notices when the game thread (engine ticks) or the render thread (frames)
// stops making progress, says where it is stuck, and tells the player how to get out (the failsafe key).
//
// The threads report progress with Beat*(). When one hasn't for `threshold` seconds, outside the times a long pause is
// normal (a level loading, a TAS session freezing threads, a debugger attached, the game minimized or in the
// background for the render thread), the detector:
//   - takes one sample of the stuck thread: suspended for microseconds to read its registers and copy its stack into
//     a fixed buffer (nothing is allocated or logged while it is suspended: it may hold the heap's lock), then resumed
//   - logs where it is (module+offset of the instruction and the return addresses on its stack), writes a hang report
//     to mirage\crashlogs\report-*.miragecrashlog, publishes events::HangDetected and shows a notification with the failsafe key
//   - when progress resumes: logs how long it lasted and publishes events::HangRecovered
// It never kills, resumes or changes the game: it only watches and reports.
#include <cstdint>
#include <string>

namespace mirage::hang
{
	void Start();						// After the game layer is up; joined in Stop
	void Stop();

	void BeatGame();					// Game thread, every engine tick
	void BeatRender();					// Render thread, every frame
	void SetLoading(bool loading);		// A level is loading: long ticks are normal

	struct Settings
	{
		bool enabled = true;
		double gameSeconds = 10.0;		// No engine tick for this long: the game thread is hung
		double renderSeconds = 10.0;	// No frame for this long (in the foreground): the render thread is hung
		bool sampleStack = true;		// Suspend the stuck thread for a moment to read where it is
	};
	void Configure(const Settings& settings);
	Settings Configured();

	struct Status
	{
		bool watching = false;
		double sinceGameBeat = 0.0, sinceRenderBeat = 0.0;	// Seconds
		std::uint64_t hangs = 0;
		bool hungNow = false;
		std::string last;				// The last hang: which thread, how long, where
	};
	Status GetStatus();

	// Where the game thread is now (its instruction, then code addresses on its stack: "MirrorsEdge.exe+0x... < ..."),
	// "" if it can't be read. For long calls that are allowed to run (SetLoading): what they are doing. Not the game thread.
	std::string WhereIsGameThread();
}
