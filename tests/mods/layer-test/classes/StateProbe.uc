// The self-test's probe for states in a mod's class (step "Mirage class states"): an auto state is entered at spawn
// (BeginState counts it) and its code runs, across a latent Sleep.
class StateProbe extends Actor
    placeable;

var int Began, Ran, Ticks;
var name AtStart, Now;     // The state at PostBeginPlay, and at the last tick

event PostBeginPlay()
{
    AtStart = GetStateName();
}

event Tick(float DeltaTime)
{
    Ticks++;
    Now = GetStateName();
}

auto state Probing
{
    event BeginState(name PreviousStateName)
    {
        Began++;
    }
Begin:
    Ran++;
    Sleep(0.0);
    Ran++;
}

defaultproperties
{
    bHidden=True
    bNoDelete=False
}
