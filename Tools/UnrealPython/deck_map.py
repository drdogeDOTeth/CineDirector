# Compact deck maps. Traces start just above each deck so overhead beams and
# rigging are not mistaken for floor, and the Templar is ignored so he does not
# trace onto himself.
import unreal

ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = ues.get_editor_world()
TQ = unreal.TraceTypeQuery.ECC_VISIBILITY

IGNORE = [a for a in aes.get_all_level_actors()
          if "templar" in str(a.get_actor_label()).lower()]
unreal.log("ignoring %d actor(s)" % len(IGNORE))


def floor_at(x, y, z_start, depth=400.0):
    hit = unreal.SystemLibrary.line_trace_single(
        world, unreal.Vector(x, y, z_start), unreal.Vector(x, y, z_start - depth),
        TQ, False, IGNORE, unreal.DrawDebugTrace.NONE, True,
        unreal.LinearColor.RED, unreal.LinearColor.GREEN, 0.0)
    if not hit:
        return None
    return hit.to_tuple()[4].z


def deck_map(tag, xs, ys, z_expect, tol=12.0):
    z_start = z_expect + 90.0
    unreal.log("--- %s  deck z=%.1f  (. = walkable, o = obstacle, ' ' = void) ---"
               % (tag, z_expect))
    unreal.log("      X: %s" % " ".join("%5.0f" % x for x in xs))
    for y in ys:
        cells = []
        for x in xs:
            z = floor_at(x, y, z_start)
            if z is None:
                cells.append("    _")
            elif abs(z - z_expect) <= tol:
                cells.append("    .")
            else:
                cells.append("%5.0f" % (z - z_expect))
        unreal.log("Y=%6.0f %s" % (y, " ".join(cells)))


UPPER_X = [-13020, -12980, -12940, -12900, -12860, -12820]
UPPER_Y = [-800, -700, -600, -500, -400, -300, -200, -100, 0]
deck_map("UPPER", UPPER_X, UPPER_Y, 1981.6)

LOWER_X = [-12920, -12880, -12840, -12800, -12760, -12720]
LOWER_Y = [780, 880, 980, 1080, 1180, 1280, 1380, 1480]
deck_map("LOWER", LOWER_X, LOWER_Y, 1687.3)
