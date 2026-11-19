#pragma once
// Sound from mods (the plan's step 30, its first part): a mod's own WAV files played on the Windows audio device through
// XAudio2 (part of Windows: no library to ship), mixed with the game's sound by the system. The engine's audio isn't
// used: a mod's sound would otherwise have to become a SoundNodeWave package first. The game's volume settings don't
// reach these sounds; a mod gives each its volume.
//
// Files are PCM WAVs, 8 or 16 bit, mono or stereo, any rate: read once and kept while a sound of theirs may play.
// Any thread.
#include <filesystem>
#include <string>

namespace mirage::audio
{
	// `file` played at `volume` (0 to 1, more is louder than the file), looping until stopped when `loop`. A handle
	// (above 0), or 0 and `error` when it can't be read or the audio device isn't there. `owner` is the mod's id.
	int Play(const std::string& owner, const std::filesystem::path& file, float volume, bool loop, std::string* error);
	// Stops it (nothing if it ended or isn't `owner`'s)
	void Stop(const std::string& owner, int handle);
	bool Playing(const std::string& owner, int handle);
	// Every sound of `owner` stopped: the mod unloads
	void StopAll(const std::string& owner);
	// Sounds playing now (every mod)
	int Count();
}
