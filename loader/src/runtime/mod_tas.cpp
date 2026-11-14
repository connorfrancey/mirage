// The mod API's TAS engine service (MirageApi tas_*): the runtime's TAS engine (loader/src/tas) driven by its UI, a first-party
// mod. Options and status cross as JSON, the replay in its .merp format.
#include "runtime/mod_tas.h"

#if MIRAGE_FEATURE_TAS
#include "tas/tas.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace mirage::mods::tasapi
{
	namespace
	{
		uint32_t CopyOut(const std::string& s, char* buffer, uint32_t capacity)
		{
			if (buffer && capacity) {
				const std::size_t n = std::min<std::size_t>(s.size(), capacity - 1);
				std::memcpy(buffer, s.data(), n);
				buffer[n] = '\0';
			}
			return static_cast<uint32_t>(s.size());
		}

#if MIRAGE_FEATURE_TAS
		using nlohmann::json;

		tas::Options OptionsFrom(const json& j)
		{
			tas::Options o;
			o.recordHz = j.value("recordHz", o.recordHz);
			o.realtimeCap = j.value("realtimeCap", o.realtimeCap);
			o.speed = j.value("speed", o.speed);
			o.onDesync = static_cast<tas::OnDesync>(j.value("onDesync", static_cast<int>(o.onDesync)));
			o.correctMax = j.value("correctMax", o.correctMax);
			o.matchRecording = j.value("matchRecording", o.matchRecording);
			o.gameplayMods = j.value("gameplayMods", o.gameplayMods);
			o.blockStreaming = j.value("blockStreaming", o.blockStreaming);
			o.seekWithoutRendering = j.value("seekWithoutRendering", o.seekWithoutRendering);
			o.atEnd = static_cast<tas::AtEnd>(j.value("atEnd", static_cast<int>(o.atEnd)));
			o.onDeath = static_cast<tas::OnDeath>(j.value("onDeath", static_cast<int>(o.onDeath)));
			o.rewindSeconds = j.value("rewindSeconds", o.rewindSeconds);
			o.keyPause = j.value("keyPause", 0);
			o.keyAdvance = j.value("keyAdvance", 0);
			o.keyTakeOver = j.value("keyTakeOver", 0);
			o.keyStop = j.value("keyStop", 0);
			o.keyRewind = j.value("keyRewind", 0);
			o.keyRestart = j.value("keyRestart", 0);
			o.keyRecord = j.value("keyRecord", 0);
			o.keyPlay = j.value("keyPlay", 0);
			o.keyBookmark = j.value("keyBookmark", 0);
			o.keyFrameBack = j.value("keyFrameBack", 0);
			o.keyPrevBookmark = j.value("keyPrevBookmark", 0);
			o.keyNextBookmark = j.value("keyNextBookmark", 0);
			o.autoSplits = j.value("autoSplits", o.autoSplits);
			o.snapshots = j.value("snapshots", o.snapshots);
			o.keyframeInterval = j.value("keyframeInterval", o.keyframeInterval);
			o.snapshotBudgetMB = j.value("snapshotBudgetMB", o.snapshotBudgetMB);
			o.recordStart = static_cast<tas::StartKind>(j.value("recordStart", static_cast<int>(o.recordStart)));
			o.recordCommand = j.value("recordCommand", std::string());
			o.recordMap = j.value("recordMap", std::string());
			o.recordCheckpoint = j.value("recordCheckpoint", std::string());
			return o;
		}

		json ToJson(const tas::Status& s)
		{
			return {{"available", s.available}, {"mode", static_cast<int>(s.mode)}, {"armed", s.armed}, {"paused", s.paused},
					{"seeking", s.seeking}, {"frame", s.frame}, {"frames", s.frames}, {"time", s.time},
					{"desyncFrame", s.desyncFrame}, {"desyncInfo", s.desyncInfo}, {"corrections", s.corrections},
					{"syncChecked", s.syncChecked}, {"verified", s.verified}, {"message", s.message},
					{"snapshots", s.snapshots}, {"snapshotsOff", s.snapshotsOff}, {"keyframes", s.keyframes},
					{"snapshotBytes", s.snapshotBytes}, {"captureMs", s.captureMs}, {"restoreMs", s.restoreMs},
					{"snapshotBeginMs", s.snapshotBeginMs}, {"snapped", s.snapped}, {"snappedPages", s.snappedPages},
					{"player", {s.player.x, s.player.y, s.player.z, s.player.vx, s.player.vy, s.player.vz, s.player.state,
								s.player.present}}};
		}
#endif
	}

	int32_t MIRAGE_CALL Available()
	{
#if MIRAGE_FEATURE_TAS
		return tas::Available() ? 1 : 0;
#else
		return 0;
#endif
	}

	uint32_t MIRAGE_CALL Status(char* buffer, uint32_t capacity)
	{
#if MIRAGE_FEATURE_TAS
		return CopyOut(ToJson(tas::GetStatus()).dump(), buffer, capacity);
#else
		return CopyOut("{}", buffer, capacity);
#endif
	}

	void MIRAGE_CALL SetOptions(const char* text)
	{
#if MIRAGE_FEATURE_TAS
		if (!text) return;
		const auto j = nlohmann::json::parse(text, nullptr, false);
		if (j.is_object()) tas::SetOptions(OptionsFrom(j));
#else
		(void)text;
#endif
	}

	int32_t MIRAGE_CALL Command(int32_t command, int64_t arg)
	{
#if MIRAGE_FEATURE_TAS
		switch (command) {
			case MIRAGE_TAS_PLAY: tas::Play(); return 1;
			case MIRAGE_TAS_RESTART: tas::Restart(); return 1;
			case MIRAGE_TAS_STOP: tas::Stop(); return 1;
			case MIRAGE_TAS_TOGGLE_PAUSE: tas::TogglePause(); return 1;
			case MIRAGE_TAS_ADVANCE: tas::Advance(static_cast<int>(arg)); return 1;
			case MIRAGE_TAS_TAKE_OVER: tas::TakeOver(); return 1;
			case MIRAGE_TAS_GOTO: tas::Goto(static_cast<std::size_t>(std::max<int64_t>(arg, 0))); return 1;
			case MIRAGE_TAS_FRAME_BACK: tas::FrameBack(static_cast<int>(arg)); return 1;
			case MIRAGE_TAS_GOTO_BOOKMARK: tas::GotoBookmark(static_cast<int>(arg)); return 1;
			case MIRAGE_TAS_REWIND: tas::Rewind(); return 1;
			case MIRAGE_TAS_KEY_CAMERA: tas::KeyCameraFromView(static_cast<std::uint32_t>(arg)); return 1;
			case MIRAGE_TAS_CLEAR: tas::Clear(); return 1;
			case MIRAGE_TAS_RESET_SYNC: tas::ResetSync(); return 1;
			case MIRAGE_TAS_UNSAVED: return tas::Unsaved() ? 1 : 0;
			default: return 0;
		}
#else
		(void)command;
		(void)arg;
		return 0;
#endif
	}

	void MIRAGE_CALL Record(int32_t start, const char* command, const char* map, const char* checkpoint)
	{
#if MIRAGE_FEATURE_TAS
		tas::Record(static_cast<tas::StartKind>(start), command ? command : "", map ? map : "", checkpoint ? checkpoint : "");
#else
		(void)start, (void)command, (void)map, (void)checkpoint;
#endif
	}

	uint64_t MIRAGE_CALL ReplayRevision()
	{
#if MIRAGE_FEATURE_TAS
		return tas::ReplayRevision();
#else
		return 0;
#endif
	}

	uint32_t MIRAGE_CALL ReplayGet(char* data, uint32_t capacity)
	{
#if MIRAGE_FEATURE_TAS
		const std::string bytes = tas::ReplayBytes();
		if (data && capacity >= bytes.size()) std::memcpy(data, bytes.data(), bytes.size());
		return static_cast<uint32_t>(bytes.size());
#else
		(void)data, (void)capacity;
		return 0;
#endif
	}

	int32_t MIRAGE_CALL ReplaySet(const char* data, uint32_t size)
	{
#if MIRAGE_FEATURE_TAS
		if (!data) return 0;
		std::string error;
		return tas::SetReplayBytes(std::string(data, size), &error) ? 1 : 0;
#else
		(void)data, (void)size;
		return 0;
#endif
	}

	int32_t MIRAGE_CALL File(int32_t save, const char* path, char* error, uint32_t capacity)
	{
#if MIRAGE_FEATURE_TAS
		if (!path) return 0;
		std::string e;
		const std::string p = path;
		const std::filesystem::path fp(std::u8string(p.begin(), p.end()));
		const bool ok = save ? tas::SaveFile(fp, &e) : tas::LoadFile(fp, &e);
		CopyOut(e, error, capacity);
		return ok ? 1 : 0;
#else
		(void)save, (void)path;
		CopyOut("no TAS engine in this build", error, capacity);
		return 0;
#endif
	}

	uint32_t MIRAGE_CALL Folder(int32_t autosaves, char* buffer, uint32_t capacity)
	{
#if MIRAGE_FEATURE_TAS
		const auto u = (autosaves ? tas::AutosaveFolder() : tas::Folder()).u8string();
		return CopyOut(std::string(u.begin(), u.end()), buffer, capacity);
#else
		(void)autosaves;
		return CopyOut("", buffer, capacity);
#endif
	}

	uint32_t MIRAGE_CALL Preflight(char* buffer, uint32_t capacity)
	{
#if MIRAGE_FEATURE_TAS
		nlohmann::json list = nlohmann::json::array();
		for (const tas::Issue& i : tas::Preflight())
			list.push_back({{"level", static_cast<int>(i.level)}, {"text", i.text}, {"fixed", i.fixed}});
		return CopyOut(list.dump(), buffer, capacity);
#else
		return CopyOut("[]", buffer, capacity);
#endif
	}
}
