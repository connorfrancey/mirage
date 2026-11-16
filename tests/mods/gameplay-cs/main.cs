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
