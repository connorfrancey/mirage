#pragma once
// Detect changed mod folders on Windows. The loader uses the same file fingerprint to wait for writes to settle.

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mirage::mods::watch
{
	struct Changes
	{
		bool scanAll = false;
		std::set<std::wstring> folders;
	};

	// Called by the loader, which serializes startup and teardown.
	void Start(const std::vector<std::filesystem::path>& roots);
	void Stop();
	Changes TakeChanges();

	std::uintmax_t Fingerprint(const std::filesystem::path& folder);
}
