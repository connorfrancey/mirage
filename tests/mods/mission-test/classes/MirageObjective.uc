// An objective a mod places in a level (step 26): a point to reach, which is also a checkpoint. While it is open the
// game's own HUD message (the Supers message) shows Goal; when the player touches it, it becomes the active checkpoint
// (as the levels' Kismet does it, SeqAct_TdCheckpoint -> SetNewCheckpoint, not saved to the profile), shows Reached, and
// fires the level's MirageSeqEvent_ObjectiveReached events. A restart from its checkpoint spawns it again: it finds
// itself the active checkpoint, counts as reached and fires the events again, so what they did (a door opened) is
// redone in the restarted level.
class MirageObjective extends TdCheckpoint
    placeable;

var() string Goal;
var() string Reached;
var() float MessageSeconds;
var bool bReached;
var int Fired;                          // Events activated (for the self-test)
var SeqAct_TdSupersMessage Message;     // Kept while its scene opens (its delegates are on it)

simulated event PostBeginPlay()
{
    local TdCheckpointManager Manager;

    super.PostBeginPlay();
    SetCollisionSize(96.0, 128.0);
    CylinderComponent.SetActorCollision(true, false);
    SetCollision(true, false);
    Manager = CheckpointManager();
    if (Manager != None && Manager.GetActiveMap() ~= WorldInfo.GetMapName() && Manager.GetActiveCheckpoint() == CheckpointName)
    {
        bReached = true;
        FireReached(None);
    }
    else
        ShowMessage(Goal);
}

event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
{
    local TdPlayerPawn P;
    local TdPlayerController PC;
    local TdCheckpointManager Manager;

    P = TdPlayerPawn(Other);
    if (bReached || P == None)
        return;
    PC = TdPlayerController(P.Controller);
    if (PC == None || !ShouldDoCheckpoint(PC))
        return;
    bReached = true;
    Manager = CheckpointManager();
    if (Manager != None)
        Manager.SetNewCheckpoint(self, PC, true);
    ShowMessage(Reached);
    FireReached(P);
}

function TdCheckpointManager CheckpointManager()
{
    local DataStoreClient Client;
    local UIDataStore_TdGameData GameData;

    Client = class'Engine.UIInteraction'.static.GetDataStoreClient();
    if (Client == None)
        return None;
    GameData = UIDataStore_TdGameData(Client.FindDataStore('TdGameData'));
    return GameData != None ? GameData.CheckpointManager : None;
}

// The game's on-screen message, as the levels' Kismet shows it (SeqAct_TdSupersMessage)
function ShowMessage(string Text)
{
    if (Len(Text) == 0)
        return;
    Message = new class'TdGame.SeqAct_TdSupersMessage';
    Message.SupersMessage = Text;
    Message.Duration = MessageSeconds;
    Message.Activated();
    `log("objective" @ CheckpointName @ "message:" @ Text);
}

function FireReached(Pawn Instigator)
{
    local array<SequenceObject> Events;
    local Sequence GameSequence;
    local int i;

    GameSequence = WorldInfo.GetGameSequence();
    if (GameSequence == None)
        return;
    GameSequence.FindSeqObjectsByClass(class'Engine.SequenceEvent', true, Events);
    for (i = 0; i < Events.Length; i++)
        if (Events[i].IsA('MirageSeqEvent_ObjectiveReached') && SequenceEvent(Events[i]).CheckActivate(self, Instigator))
            Fired++;
    `log("objective" @ CheckpointName @ "reached, events fired:" @ Fired);
}

defaultproperties
{
    CheckpointName="Mirage_Objective"
    CheckpointWeight=1000       // Over the level's own checkpoints passed so far (SetNewCheckpoint skips lighter ones)
    Goal="Reach the marker"
    Reached="Objective reached: the door is open"
    MessageSeconds=4.0
    bStatic=False               // A NavigationPoint is a level's own actor; this one is spawned
    bNoDelete=False
}
