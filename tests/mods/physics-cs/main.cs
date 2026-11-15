// Self-test mod (step 6): the physics API from C#. Its test runs on the game thread in one call, so it checks what
// doesn't need time to pass: a prop spawned from a KActor's mesh (the Prologue has cardboard boxes) is simulated, an
// impulse is taken, an overlap and a downward trace with actors find it, a trace onto the floor reports its physical
// material, a joint holds it to the world, and the revert takes the prop and the joint away.
public sealed class PhysicsCs : Mod
{
    public override void OnLoad() => Test("physics from C#", Run);

    private string? Run()
    {
        var kactors = Actors("KActor");
        string? mesh = null;
        foreach (var k in kactors)
        {
            var m = k.Get("StaticMeshComponent.StaticMesh").Object;
            if (m.IsValid) { mesh = m.Path; break; }
        }
        if (mesh is null) return "no KActor with a mesh in the level";

        var player = GameObject.Player;
        var at = Body(player)?.Location ?? new Vec3(0, 0, 0);
        var spot = new Vec3(at.X, at.Y, at.Z + 250);
        var prop = SpawnProp(mesh, spot);
        if (!prop.IsValid) return $"SpawnProp({mesh}) failed";
        try
        {
            var body = Body(prop);
            if (body is null || !body.Value.Simulated) return "the prop isn't simulated";
            if (!Impulse(prop, new Vec3(0, 0, 100), velocityChange: true)) return "the impulse wasn't taken";

            // The overlap sees the actors as of this tick (the new prop from the next): around a box already there
            var box = kactors[0];
            var boxAt = Body(box)?.Location ?? new Vec3(0, 0, 0);
            var near = false;
            foreach (var a in Overlap(boxAt, 60)) near |= a.Equals(box);
            if (!near) return $"the overlap around {box.Name} didn't find it";

            var hit = Trace(new Vec3(spot.X, spot.Y, spot.Z + 60), new Vec3(spot.X, spot.Y, spot.Z - 60), actors: true);
            if (hit is null || !hit.Value.Actor.Equals(prop)) return $"a trace through the prop hit {hit?.Actor.Name ?? "nothing"}";

            var floor = Trace(new Vec3(at.X, at.Y, at.Z), new Vec3(at.X, at.Y, at.Z - 1000), material: true);
            if (floor is null) return "nothing under the player";

            var joint = Joint(prop, new Vec3(spot.X, spot.Y, spot.Z + 100));
            if (!joint.IsValid) return "the joint wasn't made";
            Log.Info($"physics from C#: prop {prop.Name} of {mesh}; under the player: {floor.Value.Actor.Name}, physical material " +
                $"{(floor.Value.PhysMaterial.IsValid ? floor.Value.PhysMaterial.Name : "none")}; joint {joint.Name}");
        }
        finally
        {
            Revert();
        }
        return null;
    }
}
