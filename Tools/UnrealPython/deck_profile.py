# Longitudinal deck profile. Traces straight down along the ship's length to
# find each walkable surface. Collision works here because this runs in the live
# editor -- the headless commandlet had no physics scene and returned 0/225 hits.
import unreal

X = -12850.0
Y0, Y1, STEP = -1000.0, 2600.0, 50.0
Z_HI, Z_LO = 2600.0, 1300.0

ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world = ues.get_editor_world()

hits = 0
prev_z = None
unreal.log("--- deck profile at X=%.0f ---" % X)
y = Y0
while y <= Y1:
    start = unreal.Vector(X, y, Z_HI)
    end = unreal.Vector(X, y, Z_LO)
    hit = unreal.SystemLibrary.line_trace_single(
        world, start, end,
        unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
        False, [], unreal.DrawDebugTrace.NONE, True,
        unreal.LinearColor.RED, unreal.LinearColor.GREEN, 0.0)
    if hit:
        z = hit.to_tuple()[4].z
        actor = hit.to_tuple()[9]
        label = str(actor.get_actor_label()) if actor else "?"
        flag = ""
        if prev_z is not None and abs(z - prev_z) > 20.0:
            flag = "   <-- STEP %+.1f" % (z - prev_z)
        unreal.log("  Y=%7.1f  Z=%7.1f  %-22s%s" % (y, z, label[:22], flag))
        prev_z = z
        hits += 1
    else:
        unreal.log("  Y=%7.1f  ---- no floor" % y)
        prev_z = None
    y += STEP

unreal.log("hits: %d" % hits)
