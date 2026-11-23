// The menu's columns: their order and width. A module names its tab in its constructor; a tab that is not listed here is
// created automatically and goes last.
#include "core/module.h"

namespace {
[[maybe_unused]] const modules::TabRegistrar kPlayer {"Player",  0};
[[maybe_unused]] const modules::TabRegistrar kTrainer{"Trainer", 1};
[[maybe_unused]] const modules::TabRegistrar kTas    {"TAS",     2};
[[maybe_unused]] const modules::TabRegistrar kWorld  {"World",   3};
[[maybe_unused]] const modules::TabRegistrar kVisual {"Visual",  4};
[[maybe_unused]] const modules::TabRegistrar kDebug  {"Debug",   5};
[[maybe_unused]] const modules::TabRegistrar kLevels {"Levels",  6};
[[maybe_unused]] const modules::TabRegistrar kMisc   {"Misc",    7};
[[maybe_unused]] const modules::TabRegistrar kMods   {"Mods",    8};
}
