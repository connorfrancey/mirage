// The TAS engine's interface (tas.h) over MirageApi tas_*: the TAS / Macro mod drives the runtime's engine with it. The
// replay is kept here as a copy, fetched again in the .merp format when the engine's revision changes (while a session
// records or plays, at most a few times a second), and sent back when the UI changed it.
#include "tas.h"

#include "mirage/mirage.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>

namespace tas {
namespace {

using json = nlohmann::json;

const MirageApi& Api() { return mirage::Api(); }

std::string Text(uint32_t (MIRAGE_CALL* fn)(char*, uint32_t))
{
    const uint32_t n = fn(nullptr, 0);
    std::string s(n + 1, '\0');
    fn(s.data(), n + 1);
    s.resize(n);
    return s;
}

std::filesystem::path PathOf(const std::string& u8) { return std::filesystem::path(std::u8string(u8.begin(), u8.end())); }
std::string U8(const std::filesystem::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- The replay copy ------------------------------------------------------------------------------------------------------
std::mutex g_lock;
Replay g_replay;
std::uint64_t g_revision = ~0ull;
std::chrono::steady_clock::time_point g_fetched{};
Mode g_mode = Mode::Idle;               // From the last status

// A fingerprint of what the UI can change: compared before and after it ran
std::uint64_t Fingerprint(const Replay& r)
{
    std::uint64_t h = r.InputHash() ^ (r.SyncHash() * 31);
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const CameraKey& k : r.camera) {
        std::uint64_t v = k.frame;
        std::uint32_t y, p;
        std::memcpy(&y, &k.yaw, 4);
        std::memcpy(&p, &k.pitch, 4);
        mix(v);
        mix(y);
        mix(p);
        mix(static_cast<std::uint64_t>(k.ease));
    }
    for (const Marker& m : r.markers) {
        mix(m.frame);
        mix(std::hash<std::string>{}(m.name));
    }
    for (const std::string* s : {&r.command, &r.map, &r.checkpoint, &r.author, &r.description})
        mix(std::hash<std::string>{}(*s));
    mix(static_cast<std::uint64_t>(r.start));
    mix(r.syncValidUntil);
    std::uint32_t hz;
    std::memcpy(&hz, &r.fixedHz, 4);
    mix(hz);
    return h;
}

void RefreshLocked()
{
    const std::uint64_t revision = Api().tas_replay_revision();
    if (revision == g_revision) return;
    // A session changes it every frame: fetched a few times a second then (the UI shows it, nothing edits it)
    const auto now = std::chrono::steady_clock::now();
    if (g_mode != Mode::Idle && g_revision != ~0ull && now - g_fetched < std::chrono::milliseconds(250)) return;
    const uint32_t n = Api().tas_replay_get(nullptr, 0);
    std::string data(n, '\0');
    if (n && Api().tas_replay_get(data.data(), n) == n) {
        Replay r;
        std::string error;
        if (r.Deserialize(data, &error)) g_replay = std::move(r);
    } else if (n == 0) {
        g_replay = Replay{};
    }
    g_revision = revision;
    g_fetched = now;
}

} // namespace

std::vector<Issue> Preflight()
{
    std::vector<Issue> out;
    const json j = json::parse(Text(Api().tas_preflight), nullptr, false);
    if (!j.is_array()) return out;
    for (const json& i : j) {
        Issue issue;
        issue.level = static_cast<Issue::Level>(i.value("level", 0));
        issue.text = i.value("text", std::string());
        issue.fixed = i.value("fixed", false);
        out.push_back(std::move(issue));
    }
    return out;
}

void ResetSync() { Api().tas_command(MIRAGE_TAS_RESET_SYNC, 0); }
bool Available() { return Api().tas_available() != 0; }

void SetOptions(const Options& o)
{
    const json j = {
        {"recordHz", o.recordHz}, {"realtimeCap", o.realtimeCap}, {"speed", o.speed}, {"onDesync", static_cast<int>(o.onDesync)},
        {"correctMax", o.correctMax}, {"matchRecording", o.matchRecording}, {"gameplayMods", o.gameplayMods},
        {"blockStreaming", o.blockStreaming}, {"seekWithoutRendering", o.seekWithoutRendering}, {"atEnd", static_cast<int>(o.atEnd)},
        {"onDeath", static_cast<int>(o.onDeath)}, {"rewindSeconds", o.rewindSeconds}, {"keyPause", o.keyPause},
        {"keyAdvance", o.keyAdvance}, {"keyTakeOver", o.keyTakeOver}, {"keyStop", o.keyStop}, {"keyRewind", o.keyRewind},
        {"keyRestart", o.keyRestart}, {"keyRecord", o.keyRecord}, {"keyPlay", o.keyPlay}, {"keyBookmark", o.keyBookmark},
        {"keyFrameBack", o.keyFrameBack}, {"keyPrevBookmark", o.keyPrevBookmark}, {"keyNextBookmark", o.keyNextBookmark},
        {"autoSplits", o.autoSplits}, {"snapshots", o.snapshots}, {"keyframeInterval", o.keyframeInterval},
        {"snapshotBudgetMB", o.snapshotBudgetMB}, {"recordStart", static_cast<int>(o.recordStart)},
        {"recordCommand", o.recordCommand}, {"recordMap", o.recordMap}, {"recordCheckpoint", o.recordCheckpoint},
    };
    Api().tas_set_options(j.dump().c_str());
}

Status GetStatus()
{
    Status s;
    const json j = json::parse(Text(Api().tas_status), nullptr, false);
    if (!j.is_object()) return s;
    s.available = j.value("available", false);
    s.mode = static_cast<Mode>(j.value("mode", 0));
    s.armed = j.value("armed", false);
    s.paused = j.value("paused", false);
    s.seeking = j.value("seeking", false);
    s.frame = j.value("frame", std::size_t{0});
    s.frames = j.value("frames", std::size_t{0});
    s.time = j.value("time", 0.0);
    s.desyncFrame = j.value("desyncFrame", -1LL);
    s.desyncInfo = j.value("desyncInfo", std::string());
    s.corrections = j.value("corrections", std::size_t{0});
    s.syncChecked = j.value("syncChecked", std::size_t{0});
    s.verified = j.value("verified", false);
    s.message = j.value("message", std::string());
    s.snapshots = j.value("snapshots", false);
    s.snapshotsOff = j.value("snapshotsOff", std::string());
    s.keyframes = j.value("keyframes", std::size_t{0});
    s.snapshotBytes = j.value("snapshotBytes", std::uint64_t{0});
    s.captureMs = j.value("captureMs", 0.0);
    s.restoreMs = j.value("restoreMs", 0.0);
    s.snapshotBeginMs = j.value("snapshotBeginMs", 0.0);
    s.snapped = j.value("snapped", std::size_t{0});
    s.snappedPages = j.value("snappedPages", std::size_t{0});
    if (const auto it = j.find("player"); it != j.end() && it->is_array() && it->size() == 8) {
        const json& p = *it;
        s.player.x = p[0].get<float>();
        s.player.y = p[1].get<float>();
        s.player.z = p[2].get<float>();
        s.player.vx = p[3].get<float>();
        s.player.vy = p[4].get<float>();
        s.player.vz = p[5].get<float>();
        s.player.state = p[6].get<std::uint8_t>();
        s.player.present = p[7].get<std::uint8_t>();
    }
    {
        std::lock_guard lock(g_lock);
        g_mode = s.mode;
    }
    return s;
}

void Record(StartKind start, const std::string& command, const std::string& map, const std::string& checkpoint)
{
    Api().tas_record(static_cast<int32_t>(start), command.c_str(), map.c_str(), checkpoint.c_str());
}
void Play() { Api().tas_command(MIRAGE_TAS_PLAY, 0); }
void Restart() { Api().tas_command(MIRAGE_TAS_RESTART, 0); }
void Stop() { Api().tas_command(MIRAGE_TAS_STOP, 0); }
void TogglePause() { Api().tas_command(MIRAGE_TAS_TOGGLE_PAUSE, 0); }
void Advance(int frames) { Api().tas_command(MIRAGE_TAS_ADVANCE, frames); }
void TakeOver() { Api().tas_command(MIRAGE_TAS_TAKE_OVER, 0); }
void Goto(std::size_t frame) { Api().tas_command(MIRAGE_TAS_GOTO, static_cast<int64_t>(frame)); }
void FrameBack(int frames) { Api().tas_command(MIRAGE_TAS_FRAME_BACK, frames); }
void GotoBookmark(int step) { Api().tas_command(MIRAGE_TAS_GOTO_BOOKMARK, step); }
void Rewind() { Api().tas_command(MIRAGE_TAS_REWIND, 0); }
void KeyCameraFromView(std::uint32_t frame) { Api().tas_command(MIRAGE_TAS_KEY_CAMERA, frame); }
void Clear() { Api().tas_command(MIRAGE_TAS_CLEAR, 0); }
bool Unsaved() { return Api().tas_command(MIRAGE_TAS_UNSAVED, 0) != 0; }

void WithReplay(const std::function<void(Replay&)>& fn)
{
    std::lock_guard lock(g_lock);
    RefreshLocked();
    const std::uint64_t before = Fingerprint(g_replay);
    fn(g_replay);
    if (Fingerprint(g_replay) == before) return;
    const std::string data = g_replay.Serialize();
    if (Api().tas_replay_set(data.data(), static_cast<uint32_t>(data.size())))
        g_revision = Api().tas_replay_revision();     // Ours now: not fetched back
}

bool LoadFile(const std::filesystem::path& path, std::string* error)
{
    char e[512]{};
    const bool ok = Api().tas_file(0, U8(path).c_str(), e, sizeof e) != 0;
    if (!ok && error) *error = e;
    return ok;
}

bool SaveFile(const std::filesystem::path& path, std::string* error)
{
    char e[512]{};
    const bool ok = Api().tas_file(1, U8(path).c_str(), e, sizeof e) != 0;
    if (!ok && error) *error = e;
    return ok;
}

std::filesystem::path Folder()
{
    char p[1024]{};
    Api().tas_folder(0, p, sizeof p);
    return PathOf(p);
}

std::filesystem::path AutosaveFolder()
{
    char p[1024]{};
    Api().tas_folder(1, p, sizeof p);
    return PathOf(p);
}

} // namespace tas
