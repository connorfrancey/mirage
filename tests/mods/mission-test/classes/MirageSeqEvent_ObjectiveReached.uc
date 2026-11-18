// The Kismet event a MirageObjective fires when the player reaches it, and again when a restart from its checkpoint
// spawns it: the level's sequences outlive a restart and its spawned actors don't, so the wiring redoes what it did.
class MirageSeqEvent_ObjectiveReached extends SequenceEvent;

defaultproperties
{
    ObjName="Objective Reached"
    ObjCategory="Mirage"
    bPlayerOnly=False
    MaxTriggerCount=0       // Once per spawn of the objective, which a restart repeats
    ReTriggerDelay=0.0
}
