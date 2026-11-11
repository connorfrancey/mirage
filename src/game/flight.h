#pragma once

// Flight and No Clip are a mod (mods/memenu-trainer/flight.lua). While one of them flies the player, the mod shares
// flight = "1": what the menu's own code (Save State, Kickglitch Helper) checks here. Any thread.
namespace flight
{
    bool Active();
}
