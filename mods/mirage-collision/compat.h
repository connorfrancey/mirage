#pragma once
// What the collision view's world readers (debug_world, static_world) used from the menu, on the public API and SDK:
// they moved into this mod as they were. game:: (the player, the world, object helpers, the camera, events, game-thread
// work), levels:: (the level list and loads, for baking), paths::Dir() (this mod's folder), mlog:: (the mod's log) and
// debugrender:: (the runtime's 3D renderer, MirageApi::world_draw_*).
#include "render/math.h"
#include "render/mesh.h"

#include "mirage/sdk.hpp"

#include <fmt/format.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---- The mod's log, in spdlog's style ------------------------------------------------------------------------------------
namespace mlog {
    template <class... A>
    void info(fmt::format_string<A...> f, A&&... a) { mirage::Info(fmt::format(f, std::forward<A>(a)...)); }
    template <class... A>
    void warn(fmt::format_string<A...> f, A&&... a) { mirage::Warn(fmt::format(f, std::forward<A>(a)...)); }
    template <class... A>
    void debug(fmt::format_string<A...> f, A&&... a) { mirage::Log(MIRAGE_LOG_DEBUG, fmt::format(f, std::forward<A>(a)...)); }
}

namespace paths {
    inline std::filesystem::path Dir()
    {
        const std::string d = mirage::Dir();
        return std::filesystem::path(std::u8string(d.begin(), d.end()));
    }
}

namespace game {
    namespace layout {
        constexpr int kFieldSuper = 0x3C;
        constexpr int kFieldNext = 0x40;
        constexpr int kStructChildren = 0x4C;
        constexpr int kStructPropertySize = 0x50;

        template <class T = void>
        T* PtrAt(const void* object, int offset)
        {
            std::uint32_t v;
            std::memcpy(&v, static_cast<const std::uint8_t*>(object) + offset, sizeof v);
            return reinterpret_cast<T*>(static_cast<std::uintptr_t>(v));
        }
    }

    inline Classes::UStruct* SuperOf(const Classes::UStruct* s) { return layout::PtrAt<Classes::UStruct>(s, layout::kFieldSuper); }
    inline Classes::UField* ChildrenOf(const Classes::UStruct* s) { return layout::PtrAt<Classes::UField>(s, layout::kStructChildren); }
    inline Classes::UField* NextOf(const Classes::UField* f) { return layout::PtrAt<Classes::UField>(f, layout::kFieldNext); }
    inline std::uint32_t PropertySizeOf(const Classes::UStruct* s)
    {
        std::uint32_t v;
        std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(s) + layout::kStructPropertySize, sizeof v);
        return v;
    }
    // `object` is an instance of `cls` or of a class derived from it
    inline bool IsA(const Classes::UObject* object, const Classes::UClass* cls)
    {
        if (!object || !cls) return false;
        int depth = 0;
        for (const Classes::UStruct* c = object->Class; c && depth < 64; c = SuperOf(c), ++depth)
            if (c == cls) return true;
        return false;
    }
    // The object at `index` of the engine's object list is still alive (never dereferences `object`)
    inline bool AliveAt(const Classes::UObject* object, std::size_t index)
    {
        if (!mirage::sdk::Ready()) return false;
        const auto& objects = Classes::UObject::GetGlobalObjects();
        return object && index < objects.Num() && objects.GetByIndex(index) == object;
    }
    inline std::size_t IndexOf(const Classes::UObject* object) { return static_cast<std::size_t>(object->ObjectInternalInteger); }

    inline Classes::ATdPlayerPawn* Pawn() { return mirage::sdk::Pawn(); }
    inline Classes::ATdPlayerController* Controller() { return mirage::sdk::Controller(); }
    inline Classes::AWorldInfo* World() { return mirage::sdk::World(); }
    inline std::string MapName() { return mirage::sdk::MapName(); }

    // Level loads, seen through the API's event (Hooks() subscribes once, from the module)
    struct EventState {
        std::atomic<bool> loading{false};
        std::mutex lock;
        std::vector<void (*)(bool)> levelLoad;
        std::vector<void (*)()> restored;
        std::vector<std::function<void()>> queue;
    };
    inline EventState& Events()
    {
        static EventState e;
        return e;
    }
    inline bool IsLoading() { return Events().loading.load(); }
    inline bool Ready() { return mirage::sdk::Ready() && World() != nullptr; }
    inline void OnLevelLoad(void (*cb)(bool))
    {
        std::lock_guard lock(Events().lock);
        Events().levelLoad.push_back(cb);
    }
    inline void OnStateRestored(void (*cb)())
    {
        std::lock_guard lock(Events().lock);
        Events().restored.push_back(cb);
    }
    // Game-thread work: run in the next tick event
    inline void RunOnGameThread(std::function<void()> fn)
    {
        std::lock_guard lock(Events().lock);
        Events().queue.push_back(std::move(fn));
    }
    // The event subscriptions (once, from the module's constructor)
    inline void Hooks()
    {
        mirage::On(MIRAGE_EVENT_LEVEL_LOAD, [](const MirageEvent& e) {
            const bool done = e.value != 0;
            Events().loading = !done;
            std::vector<void (*)(bool)> cbs;
            {
                std::lock_guard lock(Events().lock);
                cbs = Events().levelLoad;
            }
            for (auto cb : cbs) cb(done);
        });
        mirage::On(MIRAGE_EVENT_STATE_RESTORED, [](const MirageEvent&) {
            std::vector<void (*)()> cbs;
            {
                std::lock_guard lock(Events().lock);
                cbs = Events().restored;
            }
            for (auto cb : cbs) cb();
        });
        mirage::On(MIRAGE_EVENT_TICK, [](const MirageEvent&) {
            std::vector<std::function<void()>> queue;
            {
                std::lock_guard lock(Events().lock);
                queue.swap(Events().queue);
            }
            for (auto& fn : queue) fn();
        });
    }

    inline rmath::Camera GetCamera()
    {
        MirageCamera c{sizeof(MirageCamera)};
        rmath::Camera out;
        out.valid = mirage::Api().camera_get(&c) != 0;
        out.pos = {c.x, c.y, c.z};
        out.pitch = c.pitch;
        out.yaw = c.yaw;
        out.roll = c.roll;
        out.fov = c.fov;
        return out;
    }
    inline float EngineNear() { return 0.f; }
    inline void SetCameraTracking(bool) {}      // The runtime follows the camera while the drawing is on
}

// The level list and loads (for baking): the game's level-select data and its StartGame, as the Levels mod does
namespace levels {
    enum class Method { Game, Console };
    struct MapView {
        std::string label, file, gameMode;
        std::vector<std::string> checkpoints, checkpointNames;
    };
    inline std::string Str(const Classes::FString& s) { return s.IsValid() ? s.ToString() : std::string(); }
    inline std::vector<MapView> Maps()
    {
        std::vector<MapView> maps;
        if (!mirage::sdk::Ready()) return maps;
        MirageObject* found[256];
        const int32_t n = mirage::Api().objects_of_class("UIDataProvider_TdMaps", 0, found, 256);
        for (int32_t i = 0; i < n && i < 256; ++i) {
            auto* p = reinterpret_cast<Classes::UUIDataProvider_TdMaps*>(found[i]);
            MapView m;
            m.file = Str(p->Filename);
            if (m.file.empty()) continue;
            const std::string title = Str(p->MapName);
            m.label = title.empty() ? m.file : title + " (" + m.file + ")";
            m.gameMode = Str(p->GameMode);
            for (std::size_t c = 0; c < p->Checkpoints.Num(); ++c) {
                const auto& cp = p->Checkpoints[c];
                const std::string name = Str(cp.CheckpointName);
                if (name.empty()) continue;
                const std::string friendly = Str(cp.CheckpointFriendlyName);
                m.checkpoints.push_back(friendly.empty() ? name : friendly + " (" + name + ")");
                m.checkpointNames.push_back(name);
            }
            maps.push_back(std::move(m));
        }
        return maps;
    }
    // Game thread: the same StartGame the game's level select makes (checkpoint 0: the level's start)
    inline void Load(int map, int checkpoint, Method, bool saveProgress)
    {
        const auto maps = Maps();
        if (map < 0 || map >= static_cast<int>(maps.size())) return;
        const MapView& m = maps[static_cast<std::size_t>(map)];
        const std::string cp = checkpoint > 0 && checkpoint - 1 < static_cast<int>(m.checkpointNames.size())
                                   ? m.checkpointNames[static_cast<std::size_t>(checkpoint - 1)] : std::string();
        MirageObject* data = nullptr;
        auto* world = mirage::sdk::World();
        if (world && world->Game && mirage::sdk::IsA(world->Game, "TdGameInfo"))
            data = reinterpret_cast<MirageObject*>(static_cast<Classes::ATdGameInfo*>(world->Game)->TdGameData);
        if (!data) mirage::Api().objects_of_class("UIDataStore_TdGameData", 0, &data, 1);
        if (!data) return;
        MirageCall* call = mirage::Api().call_begin(data, "StartGame");
        if (!call) return;
        const auto text = [&](const char* name, const std::string& v) {
            MirageValue x{};
            x.type = MIRAGE_VALUE_TEXT;
            std::snprintf(x.text, sizeof x.text, "%s", v.c_str());
            mirage::Api().call_set(call, name, &x);
        };
        const auto flag = [&](const char* name, bool v) {
            MirageValue x{};
            x.type = MIRAGE_VALUE_BOOL;
            x.i = v ? 1 : 0;
            mirage::Api().call_set(call, name, &x);
        };
        text("LevelName", m.file);
        text("CheckpointName", cp);
        text("GameMode", m.gameMode);
        text("URL", "");
        flag("bShouldSaveCheckpointProgress", saveProgress);
        flag("bAllowSPLevelAchievements", false);
        mirage::Api().call_invoke(call);
        mirage::Api().call_end(call);
    }
}

// The runtime's 3D renderer, with the interface the readers were written against
namespace debugrender {
    enum class Depth { Scene, Overlay };
    enum class Path { None, InScene, Overlay };

    struct Style {
        float opacity = 0.55f;
        Depth depth = Depth::Scene;
        float fovOffset = 0.f;
        float aspect = 0.f;
        float nearPlane = 10.f;
        float staticOpacity = 0.25f;
    };
    struct Scene {
        rmesh::MeshData mesh;
        std::vector<rmesh::Label> labels;
        std::size_t shapes = 0;
        double buildMs = 0.0;
        std::uint64_t version = 0;
    };
    struct Stats {
        bool drawn = false;
        Path path = Path::None;
        std::size_t triangles = 0, lines = 0, shapes = 0, labels = 0;
        std::size_t staticTriangles = 0, staticLines = 0;
        double buildMs = 0.0;
        bool overlayFallback = false;
    };

    namespace detail {
        inline std::mutex& Lock()
        {
            static std::mutex m;
            return m;
        }
        inline std::shared_ptr<const Scene>& Current()
        {
            static std::shared_ptr<const Scene> s;
            return s;
        }
        static_assert(sizeof(rmesh::Vertex) == sizeof(MirageWorldVertex), "the same vertex layout");
        inline void Send(int layer, const rmesh::MeshData* m, const std::vector<rmesh::Label>* labels, std::size_t shapes, double ms)
        {
            if (!m) {
                mirage::Api().world_draw_set(layer, nullptr);
                return;
            }
            std::vector<MirageWorldLabel> l;
            if (labels)
                for (const auto& x : *labels) l.push_back({x.pos.x, x.pos.y, x.pos.z, x.color, x.text.c_str()});
            MirageWorldMesh w{sizeof(MirageWorldMesh)};
            w.triangles = reinterpret_cast<const MirageWorldVertex*>(m->tris.data());
            w.triangle_vertices = static_cast<uint32_t>(m->tris.size());
            w.lines = reinterpret_cast<const MirageWorldVertex*>(m->lines.data());
            w.line_vertices = static_cast<uint32_t>(m->lines.size());
            w.labels = l.empty() ? nullptr : l.data();
            w.label_count = static_cast<uint32_t>(l.size());
            w.shapes = static_cast<uint32_t>(shapes);
            w.build_ms = static_cast<float>(ms);
            mirage::Api().world_draw_set(layer, &w);
        }
    }

    inline void SetActive(bool on) { mirage::Api().world_draw_active(mirage::Mod(), on ? 1 : 0); }
    inline void SetStyle(const Style& s)
    {
        MirageWorldStyle w{sizeof(MirageWorldStyle), s.opacity, s.depth == Depth::Overlay ? 1 : 0, s.fovOffset, s.aspect,
                           s.nearPlane, s.staticOpacity};
        mirage::Api().world_draw_style(&w);
    }
    inline void SetCamera(const rmath::Camera&) {}      // The runtime follows the camera itself
    inline void SetEngineNear(float) {}
    inline void SetScene(std::shared_ptr<Scene> scene)
    {
        {
            std::lock_guard lock(detail::Lock());
            detail::Current() = scene;
        }
        if (scene) detail::Send(MIRAGE_WORLD_SCENE, &scene->mesh, &scene->labels, scene->shapes, scene->buildMs);
        else detail::Send(MIRAGE_WORLD_SCENE, nullptr, nullptr, 0, 0.0);
    }
    inline void SetDynamic(rmesh::MeshData mesh) { detail::Send(MIRAGE_WORLD_MOVING, &mesh, nullptr, 0, 0.0); }
    inline void SetStatic(std::shared_ptr<const rmesh::MeshData> mesh)
    {
        detail::Send(MIRAGE_WORLD_STATIC, mesh.get(), nullptr, 0, 0.0);
    }
    inline std::shared_ptr<const Scene> CurrentScene()
    {
        std::lock_guard lock(detail::Lock());
        return detail::Current();
    }
    inline rmath::Camera CurrentCamera() { return game::GetCamera(); }
    inline bool WorldToScreen(rmath::Vec3 p, float& x, float& y) { return mirage::Api().world_to_screen(p.x, p.y, p.z, &x, &y) != 0; }
    inline Stats GetStats()
    {
        MirageWorldStats w{sizeof(MirageWorldStats)};
        mirage::Api().world_draw_stats(&w);
        Stats s;
        s.drawn = w.drawn != 0;
        s.path = !w.drawn ? Path::None : w.in_scene ? Path::InScene : Path::Overlay;
        s.overlayFallback = w.overlay_fallback != 0;
        s.triangles = w.triangles;
        s.lines = w.lines;
        s.shapes = w.shapes;
        s.labels = w.labels;
        s.staticTriangles = w.static_triangles;
        s.staticLines = w.static_lines;
        s.buildMs = w.build_ms;
        return s;
    }
}
