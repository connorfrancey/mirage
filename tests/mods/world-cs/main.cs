// Self-test mod (step 14): world access from C#, like tests/mods/world-access does from Lua. While its module is on it
// counts actors coming and going and checkpoint restarts, keeps the actor the self-test spawns (Tag "WorldGravityZ"),
// asks the world about it (Mod.Actors by tag, around the player, by class), and keeps its handle. When the probe goes,
// the event's object must equal the kept one, and the handle must no longer resolve. What it saw is in its settings
// (the self-test reads them).
public sealed class WorldCs : Mod
{
    public override void OnLoad() => new WorldModule(this);
}

public sealed class WorldModule : Module
{
    private readonly Mod _mod;
    private readonly Setting _added, _removed, _restarts, _probeClassOk, _queryTag, _queryNear, _queryClass, _queryMissing,
        _probeRemoved, _handleAfter, _queried;
    private readonly System.Collections.Generic.List<Subscription> _subs = new();
    private GameObject _probe;
    private Mirage.Native.MirageHandle _probeHandle;
    private bool _haveProbe, _queryPending;

    public WorldModule(Mod mod) : base(mod, "World access (C#)", "Mods", "Self-test: world access from C# (step 14)")
    {
        _mod = mod;
        _added = SliderInt("Added", 0, 0, int.MaxValue / 2);
        _removed = SliderInt("Removed", 0, 0, int.MaxValue / 2);
        _restarts = SliderInt("Restarts", 0, 0, int.MaxValue / 2);
        _probeClassOk = SliderInt("Probe class ok", 0, 0, 1);
        _queryTag = SliderInt("Query tag", -1, -1, 100000);          // How many actors the tag found: the probe alone
        _queryNear = SliderInt("Query near", 0, 0, 1);               // Around the player: the player and the probe, nearest first
        _queryClass = SliderInt("Query class", -1, -1, 100000);      // PlayerStarts in the level
        _queryMissing = SliderInt("Query missing", -1, -1, 100000);  // A class that doesn't exist: none
        _probeRemoved = SliderInt("Probe removed", 0, 0, 1);         // actor_removed came equal to the kept probe
        _handleAfter = SliderInt("Handle after", -1, -1, 1);         // The kept handle resolves (1) or not (0) after removal
        _queried = SliderInt("Queried", 0, 0, 1);                    // Set last, after every query
    }

    protected override void OnEnable()
    {
        _subs.Add(_mod.On(GameEvent.ActorAdded, e =>
        {
            _added.Int = _added.Int + 1;
            if (e.Object.Get("Tag").Text != "WorldGravityZ") return;
            _probe = e.Object;
            _probeHandle = e.Object.HandleOf();
            _haveProbe = true;
            _probeClassOk.Int = e.Text == "DynamicSMActor_Spawnable" ? 1 : 0;
            _queryPending = true;       // The queries on the next tick, with the probe in the actor lists
        }));
        _subs.Add(_mod.On(GameEvent.ActorRemoved, e =>
        {
            _removed.Int = _removed.Int + 1;
            if (!_haveProbe || !e.Object.Equals(_probe)) return;
            _probeRemoved.Int = 1;
            _handleAfter.Int = GameObject.Resolve(_probeHandle).IsValid ? 1 : 0;
        }));
        _subs.Add(_mod.On(GameEvent.Restart, _ => _restarts.Int = _restarts.Int + 1));
    }

    protected override void OnDisable()
    {
        foreach (var s in _subs) s.Dispose();
        _subs.Clear();
        _haveProbe = false;
    }

    protected override void OnTick()
    {
        if (!_queryPending) return;
        _queryPending = false;
        var byTag = _mod.Actors(tag: "WorldGravityZ");
        _queryTag.Int = byTag.Length == 1 && byTag[0].Equals(_probe) ? 1 : byTag.Length + 100;

        var player = GameObject.Player;
        var near = _mod.Actors(near: player, radius: 1000f);
        bool hasPlayer = near.Length > 0 && near[0].Equals(player);     // Distance 0: first
        bool hasProbe = System.Array.Exists(near, a => a.Equals(_probe));
        _queryNear.Int = hasPlayer && hasProbe ? 1 : 0;

        _queryClass.Int = _mod.Actors(className: "Engine.PlayerStart").Length;
        _queryMissing.Int = _mod.Actors(className: "Engine.NoSuchClassAnywhere").Length;
        _queried.Int = 1;
    }
}
