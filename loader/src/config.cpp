#include "config.h"
#include "core/module.h"
#include "core/setting.h"
#include "paths.h"
#include "theme.h"
#include "util/glz_compat.h"

#include <Windows.h>

#include <fstream>
#include <iterator>
#include <string>
#include <spdlog/spdlog.h>

using jc::json;    // Keeps keys in insertion order, so the file stays readable

static std::filesystem::path ConfigPath() { return paths::Dir() / "Mirage.json"; }

// Keyed by "Tab/Module" so equal module names in different tabs don't collide.
static std::string ModuleKey(const modules::Tab& t, const Module& m) { return t.name + "/" + m.Name(); }

static int ReadKey(const json& obj, const char* name, int fallback)
{
    const json& v = jc::member(obj, name);
    return v.is_int64() ? v.as<int>() : fallback;
}

namespace {
    bool g_autoSave = true;
    json g_fileModules = jc::new_object();    // The modules section as loaded: what mods not loaded now keep, and what
                                            // a module added later (a mod's, at its load phase) is given
    std::string g_written;          // What the file holds (as far as this session knows): autosave skips equal configs
    unsigned long long g_lastCheck = 0;

    // The whole config as text. Render thread (or with the menu not drawing): the controls change there.
    std::string Build()
    {
        json mods = jc::new_object();
        modules::ForEach([&](modules::Tab& t, Module& m)
        {
            json settings = jc::new_object();
            json keys = jc::new_object();
            for (const ctl::Setting* s : m.Settings())
            {
                if (s->Persistent()) s->Save(settings[s->Name()]);
                if (s->key || s->keyAlt) keys[s->Name()] = {{"key", s->key}, {"keyAlt", s->keyAlt}};
            }

            json mt = jc::new_object();
            if (!m.IsAction()) mt["enabled"] = m.Enabled();
            mt["key"] = m.key;                           // Windows virtual-key code, 0 = unbound
            if (!settings.empty()) mt["settings"] = std::move(settings);
            if (!keys.empty()) mt["keys"] = std::move(keys);
            mods[ModuleKey(t, m)] = std::move(mt);
        });
        // Modules the file had that aren't here this time (a mod left out, or not loaded yet): kept as they were
        for (const auto& [k, v] : g_fileModules.get_object())
            if (!mods.contains(k)) mods[k] = v;

        json root = jc::new_object();
        root["theme"]   = theme::Current();
        root["modules"] = std::move(mods);
        return jc::dump(root, true) + '\n';
    }

    // A temporary file swapped in: a crash or the game closing mid-write never leaves half a config.
    bool Write(const std::string& text)
    {
        auto tmp = ConfigPath();
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                spdlog::error("Cannot write config to {}", tmp.string());
                return false;
            }
            out << text;
            if (!out.good()) return false;
        }
        if (!MoveFileExW(tmp.c_str(), ConfigPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            spdlog::error("Cannot replace {} (error {})", ConfigPath().string(), GetLastError());
            return false;
        }
        g_written = text;
        return true;
    }
}

bool config::Load()
{
    std::ifstream in(ConfigPath(), std::ios::binary);
    if (!in) {
        spdlog::info("No config at {}, using defaults", ConfigPath().string());
        return false;
    }
    try {
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        std::string why;
        const json root = jc::parse(text, why);
        if (!why.empty()) {
            spdlog::error("Config error: {}", why);
            return false;
        }

        if (const json& t = jc::member(root, "theme"); t.is_string())
            theme::Apply(t.get<std::string>());

        const json& mods = jc::member(root, "modules");
        if (!mods.is_object()) return true;

        g_fileModules = mods;
        modules::ForEach([&](modules::Tab&, Module& m) { ApplySaved(m); });

        spdlog::info("Config loaded");
        g_written = Build();    // As loaded: autosave writes only what changes after this
        return true;
    }
    catch (const std::exception& e)
    {
        spdlog::error("Config error: {}", e.what());
        return false;
    }
}

bool config::ApplySaved(Module& m)
{
    const json& mi = jc::member(g_fileModules, m.Tab() + "/" + m.Name());
    if (!mi.is_object()) return false;

    m.key = ReadKey(mi, "key", m.key);
    if (const json& sj = jc::member(mi, "settings"); sj.is_object())
    {
        for (ctl::Setting* s : m.Settings())
        {
            if (!jc::contains(sj, s->Name())) continue;
            if (s->Persistent()) s->Load(sj[s->Name()]);   // Wrong types are ignored, values clamped by the control
        }
    }
    if (const json& kj = jc::member(mi, "keys"); kj.is_object())
    {
        for (ctl::Setting* s : m.Settings())
        {
            const json& k = jc::member(kj, s->Name());
            if (!k.is_object()) continue;
            s->key = ReadKey(k, "key", s->key);
            s->keyAlt = ReadKey(k, "keyAlt", s->keyAlt);
        }
    }
    // After the settings, so OnEnable sees them
    if (const json& e = jc::member(mi, "enabled"); e.is_boolean()) m.SetEnabled(e.get<bool>());
    return true;
}

bool config::Save()
{
    if (!g_autoSave) return false;      // Self-test: the player's config is never written
    const bool ok = Write(Build());
    if (ok) spdlog::info("Config saved");
    return ok;
}

void config::AutoSave()
{
    if (!g_autoSave) return;
    const unsigned long long now = GetTickCount64();
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    std::string text = Build();
    if (text == g_written) return;
    if (Write(text)) spdlog::debug("Config autosaved");
}

void config::SetAutoSave(bool on) { g_autoSave = on; }
