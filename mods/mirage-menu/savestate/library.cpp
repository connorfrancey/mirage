#include "library.h"

#include "mirage/sdk.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace {

using json = glz::generic_i64;

std::string Serialize(const json& value)
{
    std::string text;
    if (glz::write_json(value, text)) return {};
    return text;
}

struct Stored {
    save_library::Entry info;
    json state;                 // MirageApi::player_state_capture, parsed
};

std::mutex g_mutex;
std::string g_folder = "savestates";    // In the mod's folder
std::string g_map;                      // Lower case
bool g_read = false;                    // g_entries holds g_map's file
std::vector<Stored> g_entries;
int g_selected = -1;
std::string g_current;                  // The Save State (Save State / Load State keys): the last saved or loaded one

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string FileFor(const std::string& map)
{
    std::string name;
    for (char c : map) name += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    return g_folder + "/" + name + ".json";
}

std::string Now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// The list's numbers come from the stored state itself
void Describe(Stored& s)
{
    const json& st = s.state;
    auto vector = [&st](const char* name) -> const json::array_t* {
        if (!st.contains(name)) return nullptr;
        const auto* arr = st[name].get_if<json::array_t>();
        return arr && arr->size() == 3 && std::ranges::all_of(*arr, &json::is_number) ? arr : nullptr;
    };
    if (const auto* location = vector("location")) {
        s.info.x = (*location)[0].as<float>();
        s.info.y = (*location)[1].as<float>();
        s.info.z = (*location)[2].as<float>();
    }
    if (const auto* velocity = vector("velocity"))
        s.info.speed = std::hypot((*velocity)[0].as<float>(), (*velocity)[1].as<float>());
}

void ReadLocked()
{
    g_entries.clear();
    g_selected = -1;
    g_read = true;
    if (g_map.empty()) return;

    const std::string path = FileFor(g_map);
    std::string text;
    if (!mirage::ReadFile(path, text)) return;
    json j;
    if (glz::read_json(j, text) || !j.is_object() || !j.contains("states") || !j["states"].is_array()) {
        mirage::Warn(path + " is not a save state file, ignored");
        return;
    }
    for (const json& e : j["states"].get_array()) {
        if (!e.is_object() || !e.contains("state") || !e["state"].is_object()) continue;
        Stored s;
        s.info.name = "State";
        if (e.contains("name"))
            if (const auto* name = e["name"].get_if<std::string>()) s.info.name = *name;
        if (e.contains("created"))
            if (const auto* created = e["created"].get_if<std::string>()) s.info.created = *created;
        s.state = e["state"];
        Describe(s);
        g_entries.push_back(std::move(s));
    }
    if (!g_entries.empty()) g_selected = 0;
    mirage::Info(std::to_string(g_entries.size()) + " save states in " + path);
}

void WriteLocked()
{
    if (g_map.empty()) return;
    json::array_t states;
    for (const Stored& s : g_entries)
        states.emplace_back(json::object_t{{"name", s.info.name}, {"created", s.info.created}, {"state", s.state}});
    const std::string text = Serialize(json::object_t{{"map", g_map}, {"states", std::move(states)}});
    // The API writes a temporary file and swaps it in: a crash mid-write never destroys the previous list
    if (text.empty() || !mirage::WriteFile(FileFor(g_map), text)) mirage::Error("can't write " + FileFor(g_map));
}

// Switches the list to `map` (lower-cased here); empty keeps the current one
void FollowLocked(const std::string& map)
{
    const std::string m = Lower(map);
    if (!m.empty() && m != g_map) {
        g_map = m;
        g_read = false;
    }
    if (!g_read) ReadLocked();
}

bool Valid(int index) { return index >= 0 && index < static_cast<int>(g_entries.size()); }

bool Fail(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
    return false;
}

// Game thread: the player's state now, as data
bool Capture(json& state, std::string* error)
{
    if (!mirage::sdk::Pawn()) return Fail(error, "no player");
    const MirageApi& api = mirage::Api();
    const uint32_t n = api.player_state_capture(nullptr, 0);
    if (n == 0) return Fail(error, "failed (see the log)");
    std::string text(n + 1, '\0');
    api.player_state_capture(text.data(), n + 1);
    text.resize(n);
    return (!glz::read_json(state, text) && state.is_object()) || Fail(error, "failed (see the log)");
}

} // namespace

// ---- The Save State --------------------------------------------------------------------------------------------------------
bool save_library::SaveCurrent(std::string* error)
{
    json state;
    if (!Capture(state, error)) return false;
    std::lock_guard lock(g_mutex);
    g_current = Serialize(state);
    return true;
}

bool save_library::LoadCurrent(std::string* error)
{
    std::string state;
    {
        std::lock_guard lock(g_mutex);
        state = g_current;
    }
    if (state.empty()) return Fail(error, "nothing saved yet");
    return mirage::Api().player_state_apply(state.c_str()) != 0 || Fail(error, "not right now");
}

bool save_library::HasCurrent()
{
    std::lock_guard lock(g_mutex);
    return !g_current.empty();
}

void save_library::ClearCurrent()
{
    std::lock_guard lock(g_mutex);
    g_current.clear();
}

// ---- The library -----------------------------------------------------------------------------------------------------------
std::vector<save_library::Entry> save_library::List(const std::string& map)
{
    std::lock_guard lock(g_mutex);
    FollowLocked(map);
    std::vector<Entry> out;
    out.reserve(g_entries.size());
    for (const Stored& s : g_entries) out.push_back(s.info);
    return out;
}

std::string save_library::Map()
{
    std::lock_guard lock(g_mutex);
    return g_map;
}

int save_library::Selected()
{
    std::lock_guard lock(g_mutex);
    return g_selected;
}

void save_library::Select(int index)
{
    std::lock_guard lock(g_mutex);
    if (Valid(index)) g_selected = index;
}

void save_library::SelectNext(int step)
{
    std::lock_guard lock(g_mutex);
    const int n = static_cast<int>(g_entries.size());
    if (n == 0) return;
    g_selected = g_selected < 0 ? 0 : ((g_selected + step) % n + n) % n;
}

bool save_library::SaveNew(std::string* error)
{
    json state;
    if (!Capture(state, error)) return false;

    std::lock_guard lock(g_mutex);
    FollowLocked(mirage::sdk::MapName());
    if (g_map.empty()) return Fail(error, "not in a level");
    Stored s;
    // "State 3": one more than the highest number used so far, so deleting one doesn't produce a duplicate name
    int highest = 0;
    for (const Stored& e : g_entries)
        if (e.info.name.rfind("State ", 0) == 0) highest = std::max(highest, std::atoi(e.info.name.c_str() + 6));
    s.info.name = "State " + std::to_string(highest + 1);
    s.info.created = Now();
    s.state = std::move(state);
    Describe(s);
    g_current = Serialize(s.state);     // It is the Save State too
    g_entries.push_back(std::move(s));
    g_selected = static_cast<int>(g_entries.size()) - 1;
    WriteLocked();
    mirage::Info("saved \"" + g_entries.back().info.name + "\" in " + g_map);
    return true;
}

bool save_library::Overwrite(int index, std::string* error)
{
    json state;
    {
        std::lock_guard lock(g_mutex);
        FollowLocked(mirage::sdk::MapName());
        if (!Valid(index)) return Fail(error, "no state selected");
    }
    if (!Capture(state, error)) return false;

    std::lock_guard lock(g_mutex);
    if (!Valid(index)) return Fail(error, "no state selected");
    Stored& s = g_entries[static_cast<std::size_t>(index)];
    s.state = std::move(state);
    s.info.created = Now();
    Describe(s);
    g_current = Serialize(s.state);
    g_selected = index;
    WriteLocked();
    return true;
}

bool save_library::Load(int index, std::string* error)
{
    std::string state;
    {
        std::lock_guard lock(g_mutex);
        FollowLocked(mirage::sdk::MapName());
        if (!Valid(index)) return Fail(error, "no state selected");
        state = Serialize(g_entries[static_cast<std::size_t>(index)].state);
        g_selected = index;
    }
    auto* pawn = mirage::sdk::Pawn();
    if (!pawn || pawn->Health <= 0) return Fail(error, "no player");
    if (!mirage::Api().player_state_apply(state.c_str())) return Fail(error, "not right now (see the log)");
    std::lock_guard lock(g_mutex);
    g_current = std::move(state);       // Loading one makes it the Save State: the Load State key comes back here
    return true;
}

void save_library::Rename(int index, const std::string& name)
{
    std::lock_guard lock(g_mutex);
    if (!Valid(index) || name.empty() || g_entries[static_cast<std::size_t>(index)].info.name == name) return;
    g_entries[static_cast<std::size_t>(index)].info.name = name;
    WriteLocked();
}

void save_library::Remove(int index)
{
    std::lock_guard lock(g_mutex);
    if (!Valid(index)) return;
    g_entries.erase(g_entries.begin() + index);
    const int n = static_cast<int>(g_entries.size());
    if (g_selected >= n) g_selected = n - 1;
    WriteLocked();
}

void save_library::MoveBy(int index, int step)
{
    std::lock_guard lock(g_mutex);
    const int to = index + step;
    if (!Valid(index) || !Valid(to)) return;
    std::swap(g_entries[static_cast<std::size_t>(index)], g_entries[static_cast<std::size_t>(to)]);
    if (g_selected == index) g_selected = to;
    else if (g_selected == to) g_selected = index;
    WriteLocked();
}

void save_library::SetFolder(const std::string& folder)
{
    std::lock_guard lock(g_mutex);
    const std::string f = folder.empty() ? std::string("savestates") : folder;
    if (f == g_folder) return;
    g_folder = f;
    g_read = false;     // Read again from the new place
}

std::string save_library::Folder()
{
    std::lock_guard lock(g_mutex);
    return g_folder;
}
