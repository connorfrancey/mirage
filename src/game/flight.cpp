#include "game/flight.h"

#include "runtime/mod_game.h"

namespace flight
{
    bool Active()
    {
        char value[4]{};
        mirage::mods::game_api::ShareGet("memenu-trainer", "flight", value, sizeof value);
        return value[0] == '1';
    }
}
