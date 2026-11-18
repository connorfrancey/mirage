// A door for the objective test: a cylinder that blocks the player until MirageSeqAct_OpenDoor opens it. It has no
// mesh (a level's meshes are its own; this one works in any map), only its collision. A Trigger, for its cylinder.
class MirageDoor extends Trigger
    placeable;

simulated event PostBeginPlay()
{
    super.PostBeginPlay();
    CylinderComponent.SetCylinderSize(128.0, 160.0);
    CylinderComponent.SetActorCollision(true, true);
    SetCollision(true, true);
    bBlockActors = true;
}

defaultproperties
{
    bNoDelete=False     // A Trigger is a level's own actor; this one is spawned
}
