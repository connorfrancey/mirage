// An enemy of the game's own kinds where this actor is placed (step 28): the pawn and controller an AI template names
// ("TdGame.AITemplate_PatrolCop" and its kin, all in TdGame.u), set up the way the game's Kismet spawner does it
// (SeqAct_TdActorFactory, native: found in MirrorsEdge.exe with Ghidra). The pawn starts hidden and in stasis, then one
// step a tick: the pawn's SetupTemplate until it is done (mesh, animations, weapons), its animation speeds, the
// controller's SetupTemplate, then the team, the controller's Initialize, and the pawn shown. The bot goes with this
// actor: a checkpoint restart or the mod's unload destroys it.
class MirageBot extends Actor
    placeable;

var() string AIClass;           // An AITemplate class by path
var() bool bGiveDefaultInventory;
var TdBotPawn Bot;
var TdAIController BotController;
var AITemplate BotTemplate;
var AITeam Team;
var string Problem;             // Why there is no bot ("" while all is well)
var int Step;

function bool SpawnBot()
{
    local class<AITemplate> TemplateClass;
    local class<TdBotPawn> PawnClass;
    local class<TdAIController> ControllerClass;

    TemplateClass = class<AITemplate>(DynamicLoadObject(AIClass, class'Class'));
    if (TemplateClass == None)
        return Fail("no AI template" @ AIClass);
    BotTemplate = new(self) TemplateClass;
    PawnClass = class<TdBotPawn>(DynamicLoadObject(BotTemplate.PawnClass, class'Class'));
    ControllerClass = class<TdAIController>(DynamicLoadObject(BotTemplate.ControllerClass, class'Class'));
    if (PawnClass == None || ControllerClass == None)
        return Fail("the template's classes didn't load:" @ BotTemplate.PawnClass @ BotTemplate.ControllerClass);
    Bot = Spawn(PawnClass,,, Location, Rotation);
    if (Bot == None)
        return Fail("the pawn couldn't spawn here (inside geometry?)");
    Bot.bInitializing = true;
    Bot.SetHidden(true);
    Bot.bStasis = true;
    BotController = Spawn(ControllerClass);
    if (BotController == None)
        return Fail("the controller couldn't spawn");
    BotController.Possess(Bot, false);
    return true;
}

function bool Fail(string Why)
{
    Problem = Why;
    `warn("MirageBot" @ Name $ ":" @ Why);
    return false;
}

event Destroyed()
{
    if (BotController != None)
        BotController.Destroy();
    if (Bot != None)
        Bot.Destroy();
    if (Team != None)
        Team.Destroy();
}

// One setup step a tick, as the game's spawner does: 0 spawn, 1 the pawn's template, 2 the controller's, 3 done
event Tick(float DeltaTime)
{
    switch (Step)
    {
        case 0:
            Step = SpawnBot() ? 1 : -1;
            break;
        case 1:
            if (Bot.SetupTemplate(BotTemplate, bGiveDefaultInventory) >= 1)
            {
                Bot.GroundSpeed = Bot.AnimationRunSpeed;
                Bot.SpeedMaxBaseVelocity = Bot.AnimationWalkSpeed;
                Bot.SpeedMinBaseVelocity = 1.0;
                Bot.UpdateAnimationBaseSpeed();
                Step = 2;
            }
            break;
        case 2:
            if (BotController.SetupTemplate(BotTemplate) >= 1)
            {
                Team = Spawn(class'AITeam');
                Team.AddMember(BotController);
                BotController.Initialize();
                Bot.bStasis = false;
                Bot.SetHidden(false);
                Bot.bInitializing = false;
                Step = 3;
            }
            break;
    }
    if (Step < 0 || Step == 3)
        Disable('Tick');
}

defaultproperties
{
    AIClass="TdGame.AITemplate_PatrolCop"
    bGiveDefaultInventory=True
    bHidden=True
    bNoDelete=False
}
