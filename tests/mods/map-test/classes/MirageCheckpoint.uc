// A checkpoint a mod places in a map that has none (step 26): the game's TdCheckpoint, spawnable. Restart Checkpoint
// asks the checkpoint manager for the active checkpoint, which a level's Kismet sets as the player reaches one; this
// one makes itself the active checkpoint when it spawns, unless the map already has one.
class MirageCheckpoint extends TdCheckpoint
    placeable;

simulated event PostBeginPlay()
{
    local DataStoreClient Client;
    local UIDataStore_TdGameData GameData;

    super.PostBeginPlay();
    Client = class'Engine.UIInteraction'.static.GetDataStoreClient();
    if (Client == None)
        return;
    GameData = UIDataStore_TdGameData(Client.FindDataStore('TdGameData'));
    if (GameData == None || GameData.CheckpointManager == None)
        return;
    if (GameData.CheckpointManager.GetActiveMap() ~= WorldInfo.GetMapName() && GameData.CheckpointManager.GetActiveCheckpoint() != "")
        return;
    GameData.CheckpointManager.SetActiveCheckpoint(CheckpointName);
    `log("active checkpoint" @ CheckpointName @ "in" @ WorldInfo.GetMapName());
}

defaultproperties
{
    CheckpointName="Mirage_Room"
    DefaultCheckpoint=True
    bStatic=False       // A NavigationPoint is a level's own actor; this one is spawned
    bNoDelete=False
}
