// Self-test mod (step 12): MirageBouncePadCs, the self-test's bounce pad described from C# (Mod.DefineClass): the same
// variables and defaults, its Touch event and a new function written in C#, run from inside the class's bytecode.
// Defined on the first tick with the module on.
public sealed class ClassCs : Mod
{
    public override void OnLoad() => new PadModule(this);
}

public sealed class PadModule : Module
{
    private readonly Mod _mod;
    private readonly Setting _touches;
    private bool _defined;

    public PadModule(Mod mod) : base(mod, "Class (C#)", "Mods", "Self-test: a bounce pad class defined from C#")
    {
        _mod = mod;
        _touches = SliderInt("Touches", 0, 0, int.MaxValue / 2);
    }

    protected override void OnTick()
    {
        if (_defined) return;
        _defined = true;
        _mod.DefineClass(new ClassSpec
        {
            Name = "MirageBouncePadCs",
            Extends = "Trigger",
            Placeable = true,
            Vars = { "var() float Strength", "int Bounces" },
            Defaults = { "Strength=1500.0", "bNoDelete=False" },
            Functions =
            {
                ["Touch"] = call =>
                {
                    var other = call.Get("Other").Object;
                    if (!other.IsValid || !other.IsA("Pawn")) return;
                    var self = call.Object;
                    self.SetInt("Bounces", self.GetInt("Bounces") + 1);
                    other.SetFloat("Velocity.Z", self.GetFloat("Strength"));
                    other.SetInt("Physics", 2);     // PHYS_Falling
                    _touches.Int = _touches.Int + 1;
                },
                ["function int TimesBounced()"] = call =>
                {
                    call.Set("ReturnValue", Value.From(call.Object.GetInt("Bounces")));
                    call.Skip();
                },
            },
        });
    }
}
