# Fall line via least-squares on the walkable surface rather than band centroids.
# A 12% band averages over several treads at each end, which shortens the run and
# reported the stairs as 52.8deg when they are nothing like that steep.
import unreal, math

TARGETS = ["SM_Stairs_1", "SM_Stairs_3", "SM_Stairs_00", "SM_Stairs_2"]

aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
by_label = {str(a.get_actor_label()): a for a in aes.get_all_level_actors()}


def world_points(actor):
    comp = actor.get_components_by_class(unreal.StaticMeshComponent)[0]
    mesh, xf = comp.static_mesh, comp.get_world_transform()
    pts, sec = [], 0
    while True:
        try:
            verts, _i, _n, _uv, _t = \
                unreal.ProceduralMeshLibrary.get_section_from_static_mesh(mesh, 0, sec)
        except Exception:
            break
        if not verts:
            break
        pts.extend(xf.transform_location(v) for v in verts)
        sec += 1
    return pts


def analyse(pts):
    zs = [p.z for p in pts]
    zmin, zmax = min(zs), max(zs)

    # Heading: tight 4-unit bands at each end, so each centroid sits on a single
    # tread instead of smearing across many.
    lo = [p for p in pts if p.z <= zmin + 4.0]
    hi = [p for p in pts if p.z >= zmax - 4.0]

    def cen(ps):
        n = float(len(ps))
        return (sum(p.x for p in ps) / n, sum(p.y for p in ps) / n)

    tx, ty = cen(hi)
    bx, by = cen(lo)
    heading = math.atan2(by - ty, bx - tx)
    ux, uy = math.cos(heading), math.sin(heading)

    # Project onto the fall line and least-squares fit z against distance.
    s = [(p.x - tx) * ux + (p.y - ty) * uy for p in pts]
    n = float(len(pts))
    ms, mz = sum(s) / n, sum(zs) / n
    num = sum((si - ms) * (zi - mz) for si, zi in zip(s, zs))
    den = sum((si - ms) ** 2 for si in s) or 1e-9
    slope = num / den                       # dz per unit along the fall line

    s_top = (zmax - mz) / slope + ms
    s_bot = (zmin - mz) / slope + ms
    run = abs(s_bot - s_top)
    return {
        "zmin": zmin, "zmax": zmax, "rise": zmax - zmin, "run": run,
        "pitch": math.degrees(math.atan2(zmax - zmin, run)),
        "heading": math.degrees(heading),
        "top": (tx + ux * s_top, ty + uy * s_top, zmax),
        "bot": (tx + ux * s_bot, ty + uy * s_bot, zmin),
    }


for name in TARGETS:
    a = by_label.get(name)
    if not a:
        unreal.log_warning("%s missing" % name)
        continue
    m = analyse(world_points(a))
    unreal.log("%-13s rise=%6.1f run=%6.1f pitch=%4.1f  heading=%6.1f" % (
        name, m["rise"], m["run"], m["pitch"], m["heading"]))
    unreal.log("              TOP=(%.1f, %.1f, %.1f)  BOT=(%.1f, %.1f, %.1f)" % (
        m["top"][0], m["top"][1], m["top"][2],
        m["bot"][0], m["bot"][1], m["bot"][2]))
