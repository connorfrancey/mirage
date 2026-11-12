// A C# Mirage mod: a module that follows the player, a module that fails on purpose (the runtime switches it off and
// logs where, the game goes on), and an event. The self-test step "Mirage C# mods" checks each of them.
using Mirage;

namespace HelloCs
{
    public sealed class HelloMod : Mod
    {
        public override void OnLoad()
        {
            var hello = new HelloModule(this);
            new ThrowModule(this);
            On(GameEvent.Tick, e => hello.Events.Int = hello.Events.Int + 1);
            Log.Info($"Hello from C#! {Id} {Version} in {Folder}");
        }

        public override void OnUnload() => Log.Info("Bye from C#");
    }

    public sealed class HelloModule : Module
    {
        public readonly Setting Ticks, Events, PawnZ, Speed;

        public HelloModule(Mod mod) : base(mod, "Hello (C#)", "Mods", "A C# mod: counts ticks, reads the player", ModuleFlags.EnabledByDefault)
        {
            Ticks = SliderInt("Ticks", 0, 0, int.MaxValue / 2);
            Events = SliderInt("Tick events", 0, 0, int.MaxValue / 2);
            PawnZ = Slider("Pawn Z", 0f, -1e6f, 1e6f, "%.1f");
            Speed = Slider("Speed", 0f, 0f, 1e5f, "%.0f u/s");
        }

        protected override void OnTick()
        {
            Ticks.Int = Ticks.Int + 1;
            var pawn = GameObject.Player;
            if (!pawn.IsValid) return;
            PawnZ.Float = pawn.GetFloat("Location.Z");
            float vx = pawn.GetFloat("Velocity.X"), vy = pawn.GetFloat("Velocity.Y");
            Speed.Float = System.MathF.Sqrt(vx * vx + vy * vy);
        }

        protected override void OnRender()
        {
            var (w, _) = Draw.Screen;
            Draw.Text(w - 220, 40, 0xFF80FFFF, $"C#: {Speed.Float:0} u/s");
        }
    }

    // Throws when switched on: the runtime logs it with the mod's id and switches the module off again
    public sealed class ThrowModule : Module
    {
        public ThrowModule(Mod mod) : base(mod, "Throw (C#)", "Mods", "Fails on purpose (self-test)") { }

        protected override void OnTick() => throw new System.InvalidOperationException("thrown on purpose by the self-test");
    }
}
