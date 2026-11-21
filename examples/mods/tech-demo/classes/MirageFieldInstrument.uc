// An instrument that receives a pulse from Lua and returns the accumulated reading.
class MirageFieldInstrument extends Actor
    placeable;

var int PulseCount;

function int RecordPulse()
{
    PulseCount++;
    return PulseCount;
}

defaultproperties
{
    bHidden=True
    bCollideActors=False
    bBlockActors=False
    bNoDelete=False
}
