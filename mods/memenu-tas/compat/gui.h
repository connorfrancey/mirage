#pragma once
// gui.h's `visible` (the menu is open), for the TAS mod: MirageApi::menu_open
#include "mirage/mirage.hpp"

namespace gui {
struct Visible {
    operator bool() const { return mirage::Api().menu_open() != 0; }
};
inline constexpr Visible visible{};
}
