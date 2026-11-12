// A C# script: System, System.Linq, System.Collections.Generic and Mirage are already imported. The self-test step
// "Mirage C# mods" checks that it compiles in the game and ticks.
public sealed class HelloScript : Mod
{
    public override void OnLoad() => new HeightModule(this);
}

public sealed class HeightModule : Module
{
    private readonly Setting _ticks, _height;

    public HeightModule(Mod mod) : base(mod, "Script (C#)", "Mods", "A C# script: counts ticks, shows the player's height",
                                        ModuleFlags.EnabledByDefault)
    {
        _ticks = SliderInt("Ticks", 0, 0, int.MaxValue / 2);
        _height = Slider("Height", 0f, -1e6f, 1e6f, "%.0f");
    }

    protected override void OnTick()
    {
        _ticks.Int = _ticks.Int + 1;
        var pawn = GameObject.Player;
        if (pawn.IsValid) _height.Float = pawn.GetFloat("Location.Z");
    }
}
