#pragma once
// core/notify.h's Push, for the TAS mod: the API's notification
#include "mirage/mirage.hpp"

#include <string>

namespace notify {
inline void Push(const std::string& text) { mirage::Api().notify(mirage::Mod(), text.c_str()); }
}
