// {{name}}: a C# script mod for Mirage. System, System.Linq, System.Collections.Generic and Mirage are imported
// already. The API: docs/api-reference.md (managed/Mirage mirrors api.h).
public sealed class {{class}}Mod : Mod
{
    public override void OnLoad()
    {
        var module = new {{class}}Module(this);
        // A test the game runs with `mirage mod test`: null when it passed, else why it failed
        Test("ticks while on", () => module.Ticks > 0 ? null : "no tick was counted with the module on");
    }
}

public sealed class {{class}}Module : Module
{
    private readonly Setting _ticks, _height;
    public int Ticks => _ticks.Int;

    public {{class}}Module(Mod mod) : base(mod, "{{name}}", "Mods", "{{description}}")
    {
        _ticks = SliderInt("Ticks", 0, 0, int.MaxValue / 2);
        _height = Slider("Height", 0f, -1e6f, 1e6f, "%.0f");
    }

    // The game thread, every tick while the module is on
    protected override void OnTick()
    {
        _ticks.Int = _ticks.Int + 1;
        var pawn = GameObject.Player;
        if (pawn.IsValid) _height.Float = pawn.GetFloat("Location.Z");
    }

    // Undo here whatever OnEnable and OnTick changed in the game
    protected override void OnDisable() { }
}
