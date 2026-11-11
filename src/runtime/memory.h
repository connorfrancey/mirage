#pragma once
#include <cstdint>

// What the menu's own code has allocated with new (this DLL replaces operator new / delete with counting versions; the
// game's allocations are never counted). Precise where the process's memory isn't: the game allocates and frees all
// the time, so a leak in the menu doesn't show in the process's numbers.
namespace mirage::memory
{
	std::int64_t LiveBytes();		// Allocated with new and not deleted yet, now (any thread)
	std::int64_t TotalAllocations();	// Calls to new so far

	// What this thread allocated minus what it freed while the tally lived: the net memory a piece of work leaves
	// behind, untouched by other threads and by this thread's work outside it (a leak check of one operation)
	class Tally
	{
	public:
		Tally();
		~Tally();
		Tally(const Tally&) = delete;
		Tally& operator=(const Tally&) = delete;
		std::int64_t Bytes() const;
	private:
		std::int64_t start_;
	};
}
