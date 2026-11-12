#pragma once
// game/levels.h's Maps (the game's level-select data), for the TAS mod: read with the public SDK on the game thread
// (objects_of_class walks the object table, which the game thread changes) and kept as a snapshot the UI reads
#include "mirage/sdk.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace levels {
struct MapView {
    std::string label;                      // "Prologue (edge_p)"
    std::vector<std::string> checkpoints;   // Checkpoint labels
    std::string file;
    std::vector<std::string> checkpointNames;
};

inline std::string Str(const Classes::FString& s) { return s.IsValid() ? s.ToString() : std::string(); }

// Game thread only: read from the game's UIDataProvider_TdMaps objects (a short list)
inline std::vector<MapView> ReadMaps()
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

namespace detail {
inline std::mutex& Lock() { static std::mutex m; return m; }
inline std::vector<MapView>& Snapshot() { static std::vector<MapView> v; return v; }
}

// Call once (the module's constructor): refreshes the snapshot on the game thread, every second and after a level load
inline void Start()
{
    const auto refresh = [] {
        std::vector<MapView> maps = ReadMaps();
        std::lock_guard lock(detail::Lock());
        detail::Snapshot() = std::move(maps);
    };
    mirage::On(MIRAGE_EVENT_TICK, [refresh](const MirageEvent&) {
        static auto next = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        next = now + std::chrono::seconds(1);
        refresh();
    });
    mirage::On(MIRAGE_EVENT_LEVEL_LOAD, [refresh](const MirageEvent& e) { if (e.value == 1) refresh(); });
}

// Any thread: the last snapshot
inline std::vector<MapView> Maps()
{
    std::lock_guard lock(detail::Lock());
    return detail::Snapshot();
}
}
