#pragma once
// The menu's module and controls (core/module.h's Module, ctl::*) as the TAS / Macro mod uses them, on the public API:
// each control is a setting the mod makes (MirageApi setting_*), drawn by the panel in the order declared, with the
// panel's own parts (ctl::Custom) between them, and shown only while VisibleIf says so. The menu's ImGui is used for
// the custom parts (mirage::UseImGui).
#include "mirage/imgui.hpp"
#include "mirage/mirage.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace ctl {
class Setting;
}

class Module : public mirage::Module {
public:
    Module(const std::string& name, const std::string& tab, const std::string& description, int order, uint32_t flags = 0)
        : mirage::Module(name, tab, description, order, 0, flags), name_(name)
    {}

    const std::string& Name() const { return name_; }

    // The panel: every control in order (settings through the runtime, custom parts here), as the menu drew them
    void OnPanel() final;

    struct Part {
        ctl::Setting* setting = nullptr;
        std::function<void(float)> custom;
        std::function<bool()> visibleIf;
    };
    std::vector<Part>& Parts() { return parts_; }

private:
    std::string name_;
    std::vector<Part> parts_;
};

namespace ctl {

class Setting {
public:
    Setting(Module* owner, MirageSetting* handle) : owner_(owner), handle_(handle)
    {
        index_ = owner->Parts().size();
        owner->Parts().push_back({this, {}, {}});
        if (handle_) mirage::Api().setting_set_visible(handle_, 0);     // Drawn by the panel, in order
    }
    virtual ~Setting() = default;
    void VisibleIf(std::function<bool()> f) { owner_->Parts()[index_].visibleIf = std::move(f); }
    MirageSetting* Handle() const { return handle_; }

protected:
    int GetInt() const { return handle_ ? mirage::Api().setting_get_int(handle_) : 0; }
    float GetFloat() const { return handle_ ? mirage::Api().setting_get_float(handle_) : 0.f; }
    void SetInt(int v) { if (handle_) mirage::Api().setting_set_int(handle_, v); }
    void SetFloat(float v) { if (handle_) mirage::Api().setting_set_float(handle_, v); }

    Module* owner_;
    MirageSetting* handle_;
    std::size_t index_ = 0;
};

// A section's switch: its controls are shown while it is on
class Section : public Setting {
public:
    Section(Module* m, const std::string& name, bool open)
        : Setting(m, mirage::Api().setting_checkbox(m->Handle(), name.c_str(), open ? 1 : 0, ""))
    {}
    bool Get() const { return GetInt() != 0; }
    operator bool() const { return Get(); }
    void Set(bool v) { SetInt(v ? 1 : 0); }
};

class Checkbox : public Setting {
public:
    Checkbox(Module* m, const std::string& name, bool value, const std::string& tip = {})
        : Setting(m, mirage::Api().setting_checkbox(m->Handle(), name.c_str(), value ? 1 : 0, tip.c_str()))
    {}
    bool Get() const { return GetInt() != 0; }
    operator bool() const { return Get(); }
    void Set(bool v) { SetInt(v ? 1 : 0); }
};

class Slider : public Setting {
public:
    Slider(Module* m, const std::string& name, float value, float min, float max, const std::string& format = "%.2f",
           float /*step*/ = 0.f, const std::string& tip = {})
        : Setting(m, mirage::Api().setting_slider(m->Handle(), name.c_str(), value, min, max, format.c_str(), tip.c_str()))
    {}
    float Get() const { return GetFloat(); }
    operator float() const { return Get(); }
    void Set(float v) { SetFloat(v); }
};

class Combo : public Setting {
public:
    Combo(Module* m, const std::string& name, std::initializer_list<const char*> items, int index = 0, const std::string& tip = {})
        : Setting(m, Make(m, name, items, index, tip))
    {}
    int Get() const { return GetInt(); }
    void Set(int v) { SetInt(v); }

private:
    static MirageSetting* Make(Module* m, const std::string& name, std::initializer_list<const char*> items, int index,
                               const std::string& tip)
    {
        std::string list;
        for (const char* i : items) {
            list += i;
            list.push_back('\0');
        }
        list.push_back('\0');
        return mirage::Api().setting_choice(m->Handle(), name.c_str(), list.c_str(), index, tip.c_str());
    }
};

class KeyHold : public Setting {
public:
    KeyHold(Module* m, const std::string& name, int vk, const std::string& tip = {})
        : Setting(m, mirage::Api().setting_key(m->Handle(), name.c_str(), vk, tip.c_str()))
    {}
    int Get() const { return GetInt(); }
    operator int() const { return Get(); }
    bool Down() const { return handle_ && mirage::Api().key_down(handle_) != 0; }
};

// A part of the panel the module draws itself (with ImGui): a Setting with no runtime setting behind it, so it can sit
// in a section's list of controls like the others
class Custom : public Setting {
public:
    Custom(Module* m, std::function<void(float)> draw) : Setting(m, nullptr)
    {
        owner_->Parts()[index_].custom = std::move(draw);
    }
};

} // namespace ctl

inline void Module::OnPanel()
{
    if (!mirage::UseImGui()) return;
    const float width = ImGui::GetContentRegionAvail().x;
    for (const Part& p : parts_) {
        if (p.visibleIf && !p.visibleIf()) continue;
        if (p.custom) {
            ImGui::PushID(&p);
            p.custom(width);
            ImGui::PopID();
        } else if (p.setting && p.setting->Handle()) {
            mirage::Api().setting_draw(p.setting->Handle(), width);
        }
    }
}

// Modules of the whole menu, as the TAS module looks at them (MirageApi::module_list)
namespace modules {
struct Tab {
    std::string name;
};
class ModuleRef {
public:
    ModuleRef(std::string name, bool enabled, bool action) : name_(std::move(name)), enabled_(enabled), action_(action) {}
    const std::string& Name() const { return name_; }
    bool Enabled() const { return enabled_; }
    bool IsAction() const { return action_; }
    void SetEnabled(bool on) { mirage::Api().module_set_state(name_.c_str(), on ? 1 : 0); }

private:
    std::string name_;
    bool enabled_, action_;
};
template <class F>
void ForEach(F&& f)
{
    const uint32_t n = mirage::Api().module_list(nullptr, 0);
    std::string text(n + 1, '\0');
    mirage::Api().module_list(text.data(), n + 1);
    text.resize(n);
    const auto list = nlohmann::json::parse(text, nullptr, false);
    if (!list.is_array()) return;
    for (const auto& m : list) {
        Tab tab{m.value("tab", std::string())};
        ModuleRef ref(m.value("name", std::string()), m.value("enabled", false), m.value("action", false));
        f(tab, ref);
    }
}
inline void SetReservedKeys(const std::vector<int>& keys)
{
    std::vector<int32_t> k(keys.begin(), keys.end());
    mirage::Api().reserved_keys(mirage::Mod(), k.data(), static_cast<int32_t>(k.size()));
}
} // namespace modules

#define MEM_MODULE(T) MIRAGE_MODULE(T)
