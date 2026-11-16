namespace Mirage
{
    /// <summary>A TdPawn's custom animation slots (the game's CustomNodeType; api.h MIRAGE_ANIM_*)</summary>
    public enum AnimSlot
    {
        Canned = 0,
        CannedUpperBody = 1,
        FullBody = 2,           // What the game's moves play their animations on
        FullBodyDir = 3,
        UpperBody = 4,
        LowerBody = 5,
        Camera = 6,
        Weapon = 7,
        Face = 8,
    }

    /// <summary>What an animation slot plays: its sequence ("" none), seconds into it, its length and play rate</summary>
    public sealed record AnimState(bool Playing, bool Looping, string Sequence, float Position, float Length, float Rate);
}
