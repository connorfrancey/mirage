// A trigger that launches whoever touches it. Compiled in the game by the mod loader (UnrealLib's compiler): the engine
// loads it like any of its own classes.
class MirageBouncePad extends Trigger
    placeable;

var() float Strength;   // The vertical speed it gives (the player's jump is about 600)
var int Bounces;

event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
{
    local Pawn P;

    P = Pawn(Other);
    if (P == None)
        return;
    Bounces++;
    P.Velocity.Z = Strength;
    P.SetPhysics(PHYS_Falling);
    `log("bounce" @ Bounces);
}

defaultproperties
{
    Strength=1500.0
    bNoDelete=False     // A Trigger is a level's own actor; this one is spawned
}
