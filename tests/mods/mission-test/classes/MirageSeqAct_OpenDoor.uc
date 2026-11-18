// A mod's Kismet action: opens every MirageDoor of the level, taking its collision away and raising it by Lift. The
// door is found by class name (each .uc is its own package, so the class itself can't be named here).
class MirageSeqAct_OpenDoor extends SequenceAction;

var() float Lift;

event Activated()
{
    local Actor A;

    foreach GetWorldInfo().AllActors(class'Engine.Actor', A)
    {
        if (A.IsA('MirageDoor') && A.bCollideActors)
        {
            A.SetCollision(false, false);
            A.SetLocation(A.Location + vect(0, 0, 1) * Lift);
            `log("opened" @ A.Name);
        }
    }
}

defaultproperties
{
    Lift=256.0
    ObjName="Open Mirage Door"
    ObjCategory="Mirage"
    VariableLinks.Empty
}
