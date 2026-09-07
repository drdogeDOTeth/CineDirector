# Check the authored foot path against the collision surface. The character's
# feet follow path(t) by construction, so tracing the floor under path(t) says
# whether he walks on the deck, floats, or sinks into it.
import unreal, math

SEGMENTS = [
    ("walk-in",  [(-12820.0, -470.0), (-12827.0, -5.0)], 1981.6, 1981.6),
    ("stairs-1", [(-12827.0, -5.0), (-12720.0, 249.0)], 1981.6, 1789.4),
    ("landing",  [(-12720.0, 249.0), (-12781.0, 470.0)], 1789.4, 1789.4),
    ("stairs-3", [(-12781.0, 470.0), (-12794.0, 756.0)], 1789.4, 1689.6),
    ("walk-out", [(-12794.0, 756.0), (-12930.0, 900.0), (-12950.0, 1380.0)],
     1689.6, 1687.4),
]

ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = ues.get_editor_world()
TQ = unreal.TraceTypeQuery.ECC_VISIBILITY
IGNORE = [a for a in aes.get_all_level_actors()
          if "templar" in str(a.get_actor_label()).lower()]


def floor_at(x, y, z_ref):
    hit = unreal.SystemLibrary.line_trace_single(
        world, unreal.Vector(x, y, z_ref + 80.0), unreal.Vector(x, y, z_ref - 200.0),
        TQ, False, IGNORE, unreal.DrawDebugTrace.NONE, True,
        unreal.LinearColor.RED, unreal.LinearColor.GREEN, 0.0)
    if not hit:
        return None, "none"
    t = hit.to_tuple()
    a = t[9]
    return t[4].z, (str(a.get_actor_label()) if a else "?")


def legs(path):
    return [math.hypot(path[i + 1][0] - path[i][0], path[i + 1][1] - path[i][1])
            for i in range(len(path) - 1)]


def at(path, ls, s):
    for i, ln in enumerate(ls):
        if s <= ln or i == len(ls) - 1:
            f = 0.0 if ln == 0 else s / ln
            return (path[i][0] + (path[i + 1][0] - path[i][0]) * f,
                    path[i][1] + (path[i + 1][1] - path[i][1]) * f)
        s -= ln
    return path[-1]


worst = 0.0
for name, path, z0, z1 in SEGMENTS:
    ls = legs(path)
    total = sum(ls)
    unreal.log("--- %s ---" % name)
    for i in range(11):
        f = i / 10.0
        x, y = at(path, ls, f * total)
        pz = z0 + (z1 - z0) * f
        fz, lbl = floor_at(x, y, pz)
        if fz is None:
            unreal.log("  f=%.1f (%9.1f,%8.1f) path_z=%7.1f  NO FLOOR" % (f, x, y, pz))
            continue
        d = pz - fz
        worst = max(worst, abs(d))
        flag = "  <-- %s" % ("FLOAT" if d > 25 else "SUNK") if abs(d) > 25 else ""
        unreal.log("  f=%.1f (%9.1f,%8.1f) path_z=%7.1f floor=%7.1f  d=%+6.1f  %-18s%s"
                   % (f, x, y, pz, fz, d, lbl[:18], flag))

unreal.log("worst deviation: %.1f" % worst)
