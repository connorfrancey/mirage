#pragma once
#include <Windows.h>

#include "crashhandler/analysis.h"

// Fills an Analysis for a crash (crashhandler/analysis.h), on the crash reporter's thread while the crashed thread
// waits. Each part reads the game's memory under its own SEH guard: a part that faults is left out, the rest stay.
namespace crashhandler
{
	Analysis Collect(const EXCEPTION_RECORD& record, const CONTEXT& context, DWORD thread, const char* title, bool fatal);
}
