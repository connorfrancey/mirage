# Writes cube brushes for the editor's BRUSH LOAD, in the format and vertex order its own cube builder exports
# (ed/builder.t3d): one polygon per face, normal outward, Flags=3584.
import os
import sys

W = os.path.dirname(os.path.abspath(__file__))


def f(v):
    return f"{v:+013.6f}"


def vec(x, y, z):
    return f"{f(x)},{f(y)},{f(z)}"


def cube(lo, hi):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    # Faces as the builder writes them: (normal, texU, texV, 4 vertices)
    faces = [
        ((-1, 0, 0), (0, 1, 0), (0, 0, -1), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1), [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)]),
        ((1, 0, 0), (0, -1, 0), (0, 0, -1), [(x1, y1, z0), (x1, y1, z1), (x1, y0, z1), (x1, y0, z0)]),
        ((0, -1, 0), (-1, 0, 0), (0, 0, -1), [(x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (x0, y0, z0)]),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0), [(x0, y1, z1), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1)]),
        ((0, 0, -1), (1, 0, 0), (0, -1, 0), [(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)]),
    ]
    out = ["Begin PolyList"]
    for n, u, v, verts in faces:
        out += ["   Begin Polygon Flags=3584",
                "      Origin   " + vec(*verts[0]),
                "      Normal   " + vec(*n),
                "      TextureU " + vec(*u),
                "      TextureV " + vec(*v)]
        out += ["      Vertex   " + vec(*p) for p in verts]
        out += ["   End Polygon"]
    out += ["End PolyList"]
    return "\n".join(out) + "\n"


os.makedirs(W, exist_ok=True)
# A room 2048 x 2048, 800 high, its floor 96 units below the origin (where ACTOR ADD puts the player start and light),
# with 64-unit walls around it
with open(os.path.join(W, "outer.t3d"), "w", newline="\r\n") as fh:
    fh.write(cube((-1088, -1088, -160), (1088, 1088, 768)))
with open(os.path.join(W, "inner.t3d"), "w", newline="\r\n") as fh:
    fh.write(cube((-1024, -1024, -96), (1024, 1024, 704)))
# The check, when the editor's own cube was exported next to this (BRUSH EXPORT FILE=".../builder.t3d")
if os.path.exists(os.path.join(W, "builder.t3d")):
    ref = open(os.path.join(W, "builder.t3d")).read().replace("-00000.", "+00000.").split()
    print("matches the editor's own cube:", cube((-128, -128, -128), (128, 128, 128)).replace("-00000.", "+00000.").split() == ref)
