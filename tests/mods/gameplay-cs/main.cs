// Self-test mod (steps 7 to 9): the gameplay API from C#. Its test runs on the game thread in one call, so it checks
// what doesn't need time to pass: each piece is made and reports itself, a move of the mod's own starts at once (its
// start callback runs before StartMove returns) and goes again with RemoveMove (the player back in the game's moves), an
// animation the player has plays and stops, and the revert takes everything away.
public sealed class GameplayCs : Mod
{
    private int _started, _actions;

    public override void OnLoad()
    {
        On(GameEvent.Action, e => _actions++);      // The API is there; the key isn't pressed by the test
        Test("gameplay from C#", Run);
        Test("vector math (Vec3, Mathf)", VectorMath);
    }

    // Vec3 and Mathf (managed/Mirage/Mathf.cs): null when they hold
    private static string? VectorMath()
    {
        var v = new Vec3(1, 2, 3);
        if (v + new Vec3(1) != new Vec3(2, 3, 4)) return "+";
        if (v - v != Vec3.Zero || -v != new Vec3(-1, -2, -3)) return "-";
        if (v * 2 != 2 * v || v * 2 != new Vec3(2, 4, 6) || v * v != new Vec3(1, 4, 9) || v / 2 != new Vec3(0.5f, 1, 1.5f)) return "* /";
        var w = new Vec3(3, 4, 0);
        if (w.Length != 5 || w.LengthSquared != 25 || w.Normalized != new Vec3(0.6f, 0.8f, 0) || Vec3.Zero.Normalized != Vec3.Zero) return "length";
        if (w.Dot(Vec3.One) != 7 || Vec3.Forward.Cross(Vec3.Right) != Vec3.Up || w.Distance(Vec3.Zero) != 5) return "dot, cross, distance";
        if (w.Lerp(Vec3.Zero, 0.5f) != new Vec3(1.5f, 2, 0)) return "lerp";
        var (x, y, z) = v;
        if (x != 1 || y != 2 || z != 3 || v.ToString() != "(1, 2, 3)") return "deconstruct, ToString: " + v;
        var f = Vec3.FromRotation(0, Mathf.DegreesToUnreal(90));
        if (!Mathf.Approximately(f.Y, 1) || MathF.Abs(f.X) > 1e-6f) return "FromRotation: " + f;
        if (Mathf.Clamp(5, 0, 1) != 1 || Mathf.InverseLerp(10, 20, 15) != 0.5f || Mathf.UnrealToDegrees(16384) != 90) return "Mathf";
        return null;
    }

    private string? Run()
    {
        var player = GameObject.Player;
        if (!player.IsValid) return "no player";
        var at = Body(player)?.Location ?? new Vec3(0, 0, 0);
        try
        {
            // A trigger and a mover (a KActor's mesh, the Prologue's cardboard boxes)
            var trigger = Trigger(new Vec3(at.X, at.Y, at.Z + 400), 80, 80);
            if (!trigger.IsValid || TriggerCount(trigger) < 0) return "the trigger wasn't made";
            string? mesh = null;
            foreach (var k in Actors("KActor"))
            {
                var m = k.Get("StaticMeshComponent.StaticMesh").Object;
                if (m.IsValid) { mesh = m.Path; break; }
            }
            if (mesh is null) return "no KActor with a mesh in the level";
            var mover = Mover(mesh, new Vec3(at.X, at.Y, at.Z + 600), new Vec3(2, 2, 0.5f));
            if (!mover.IsValid) return "the mover wasn't made";
            if (!MoveTo(mover, new Vec3(at.X, at.Y, at.Z + 700), 1f) || !MoverMoving(mover)) return "the mover wasn't sent";

            // A swing pole and a zipline, out of the player's way
            var pole = SwingPole(new Vec3(at.X, at.Y, at.Z + 900), 0);
            var line = Zipline(new Vec3(at.X, at.Y, at.Z + 1200), new Vec3(at.X + 1000, at.Y, at.Z + 1000));
            if (!pole.IsValid || !line.IsValid) return "the swing pole or the zipline wasn't made";

            // A move of its own: started, the player in it, removed
            var move = Move("CsTestMove", onStart: () => _started++);
            if (move == 0) return "the move wasn't registered";
            if (!StartMove(move) || _started != 1) return $"the move didn't start (start callbacks: {_started})";
            if (player.Get("MovementState").Int != move) return $"the player isn't in the move ({player.Get("MovementState").Int})";
            RemoveMove(move);
            if (player.Get("MovementState").Int == move) return "the player is still in the move after RemoveMove";

            // Animation: listed, played, stopped
            var sets = AnimSetsJson(player);
            if (!sets.Contains("SpringBoardLeftLeg")) return "the player's AnimSets don't list SpringBoardLeftLeg";
            if (!AnimPlay(player, "SpringBoardLeftLeg", blendIn: 0f)) return "AnimPlay failed";
            var s = AnimGet(player);
            if (s is null || !s.Playing || s.Sequence != "SpringBoardLeftLeg") return $"the slot plays '{s?.Sequence}', playing {s?.Playing}";
            if (!AnimStop(player, blendOut: 0f)) return "AnimStop failed";
            Log.Info($"gameplay from C#: trigger {trigger.Name}, mover {mover.Name}, {pole.Name}, {line.Name}, move {move}, " +
                     $"animation {s.Sequence} ({s.Length:0.00} s); AnimSets JSON {sets.Length} characters");
        }
        finally
        {
            Revert();
        }
        return null;
    }
}
