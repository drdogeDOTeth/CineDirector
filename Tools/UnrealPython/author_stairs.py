# Author the full descent: upper deck -> flight 1 -> mid landing -> flight 2 ->
# lower deck. Every height comes from a line trace, not from mesh bounds (the
# stair mesh's zmax is a handrail, which is what put him 60 units in the air).
#
# The clips have root motion disabled and carry travel in the hips, so keying the
# actor along the same vector moved him twice. Here the actor keys are the path
# MINUS the clip's own travel projected onto its direction of motion:
#
#     actor(t) = path(t) - R(yaw) * u * dot(hips_local(t), u)
#
# so the character lands on path(t) exactly while keeping the clip's bob and sway.
import unreal, math

SEQ = "/Game/TemplarDoge"
ACTOR = "void_4003_Templar"
A_WALK = "/Game/CineDirector/Retarget/Anims/walk_cautious_Templar"
A_STAIR = "/Game/CineDirector/Retarget/Anims/climb_stairs_down_Templar"

KEY_STEP = 0.15          # seconds between transform keys
TURN = 0.30              # seconds to blend a heading change at a segment start

L = unreal.log


def W(m):
    unreal.log_warning(m)


# --- route, from the trace pass -------------------------------------------
# xy waypoints, plus the surface height at each end of the segment
SEGMENTS = [
    dict(name="walk-in",  clip="walk",
         path=[(-12820.0, -470.0), (-12827.0, -5.0)],
         z0=1981.6, z1=1981.6),
    dict(name="stairs-1", clip="stair",
         path=[(-12827.0, -5.0), (-12720.0, 249.0)],
         z0=1981.6, z1=1789.4),
    dict(name="landing",  clip="walk",
         path=[(-12720.0, 249.0), (-12781.0, 470.0)],
         z0=1789.4, z1=1789.4),
    dict(name="stairs-3", clip="stair",
         path=[(-12781.0, 470.0), (-12794.0, 756.0)],
         z0=1789.4, z1=1689.6),
    dict(name="walk-out", clip="walk",
         path=[(-12794.0, 756.0), (-12930.0, 900.0), (-12950.0, 1380.0)],
         z0=1689.6, z1=1687.4),
]

EAL = unreal.EditorAssetLibrary
AL = unreal.AnimationLibrary

seq = EAL.load_asset(SEQ)
clips = {"walk": EAL.load_asset(A_WALK), "stair": EAL.load_asset(A_STAIR)}
for k, v in clips.items():
    if not v:
        raise SystemExit("missing clip: %s" % k)

tick = seq.get_tick_resolution()
rate = float(tick.numerator) / tick.denominator


def F(sec):
    return unreal.FrameNumber(int(round(sec * rate)))


# --- sample each clip's hips track once ------------------------------------
SAMPLES = 120
hips = {}
for tag, a in clips.items():
    dur = a.get_play_length()
    pts = []
    for i in range(SAMPLES + 1):
        t = dur * i / float(SAMPLES)
        p = AL.get_bone_pose_for_time(a, "Hips", min(t, dur - 1e-3), False).translation
        pts.append((t, p))
    base = pts[0][1]
    rel = [(t, unreal.Vector(p.x - base.x, p.y - base.y, p.z - base.z)) for t, p in pts]
    total = rel[-1][1]
    n = math.sqrt(total.x ** 2 + total.y ** 2 + total.z ** 2)
    u = unreal.Vector(total.x / n, total.y / n, total.z / n)
    hips[tag] = dict(dur=dur, rel=rel, u=u,
                     local_heading=math.degrees(math.atan2(total.y, total.x)),
                     span=n)
    L("%-6s dur=%.2fs travel=(%.1f, %.1f, %.1f) |v|=%.1f local heading=%.1f" % (
        tag, dur, total.x, total.y, total.z, n, hips[tag]["local_heading"]))


def hips_proj(tag, t):
    """Distance travelled along the clip's own direction of motion at clip time t."""
    h = hips[tag]
    rel = h["rel"]
    t = max(0.0, min(t, h["dur"]))
    i = int(t / h["dur"] * SAMPLES)
    i = min(i, SAMPLES - 1)
    t0, p0 = rel[i]
    t1, p1 = rel[i + 1]
    f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
    p = unreal.Vector(p0.x + (p1.x - p0.x) * f,
                      p0.y + (p1.y - p0.y) * f,
                      p0.z + (p1.z - p0.z) * f)
    u = h["u"]
    return p.x * u.x + p.y * u.y + p.z * u.z


# --- geometry helpers ------------------------------------------------------
def leg_lengths(path):
    out = []
    for i in range(len(path) - 1):
        dx = path[i + 1][0] - path[i][0]
        dy = path[i + 1][1] - path[i][1]
        out.append(math.hypot(dx, dy))
    return out


def path_at(path, legs, s):
    """xy and tangent heading at arc length s along the polyline."""
    total = sum(legs)
    s = max(0.0, min(s, total))
    for i, ln in enumerate(legs):
        if s <= ln or i == len(legs) - 1:
            f = 0.0 if ln == 0 else s / ln
            x = path[i][0] + (path[i + 1][0] - path[i][0]) * f
            y = path[i][1] + (path[i + 1][1] - path[i][1]) * f
            head = math.degrees(math.atan2(path[i + 1][1] - path[i][1],
                                           path[i + 1][0] - path[i][0]))
            return x, y, head
        s -= ln
    return path[-1][0], path[-1][1], 0.0


# --- lay the segments out in time -----------------------------------------
plan, cursor = [], 0.0
for seg in SEGMENTS:
    tag = seg["clip"]
    h = hips[tag]
    legs = leg_lengths(seg["path"])
    horiz = sum(legs)
    drop = seg["z0"] - seg["z1"]
    need = math.sqrt(horiz ** 2 + drop ** 2)

    if tag == "stair":
        # Full clip; the flight is slightly longer than the clip's travel.
        dur = h["dur"]
    else:
        # Trim the walk to the distance actually available on the deck.
        dur = min(h["dur"], need / (h["span"] / h["dur"]))

    used = hips_proj(tag, dur)
    slide = (need / used - 1.0) * 100.0 if used else 0.0
    plan.append(dict(seg=seg, tag=tag, legs=legs, horiz=horiz,
                     t0=cursor, t1=cursor + dur, dur=dur, used=used, need=need))
    L("%-9s %5.2f -> %5.2f  path=%6.1f  clip covers %6.1f  foot slip %+.0f%%" % (
        seg["name"], cursor, cursor + dur, need, used, slide))
    cursor += dur

TOTAL = cursor
L("total %.2fs" % TOTAL)

# --- rebuild the tracks ----------------------------------------------------
binding = None
for b in seq.get_bindings():
    if str(b.get_display_name()) == ACTOR:
        binding = b
        break
if not binding:
    raise SystemExit("binding %s not found" % ACTOR)

for t in binding.get_tracks():
    if isinstance(t, (unreal.MovieSceneSkeletalAnimationTrack,
                      unreal.MovieScene3DTransformTrack)):
        binding.remove_track(t)

at = binding.add_track(unreal.MovieSceneSkeletalAnimationTrack)
for p in plan:
    sec = at.add_section()
    sec.set_range(F(p["t0"]).value, F(p["t1"]).value)
    sec.set_editor_property(
        "params", unreal.MovieSceneSkeletalAnimationParams(animation=clips[p["tag"]]))
    # Any cross-fade here would blend the outgoing clip's hips (far ahead) with
    # the incoming clip's hips (at the origin) and drag the character backwards
    # through the whole travel offset -- the boundary must be a hard cut.
    # Verified: sections created this way carry an empty `easing` struct, so
    # there is no cross-fade to switch off.

xf = binding.add_track(unreal.MovieScene3DTransformTrack)
xs = xf.add_section()
xs.set_range(F(0.0).value, F(TOTAL).value)
ch = xs.get_all_channels()
TICKS = unreal.MovieSceneTimeUnit.TICK_RESOLUTION
LIN = unreal.MovieSceneKeyInterpolation.LINEAR

TICK = 1.0 / rate

keys = []
for idx, p in enumerate(plan):
    seg, tag = p["seg"], p["tag"]
    h = hips[tag]
    end_proj = p["used"] or 1.0

    # Sample times paired with the clip time to evaluate at. The closing sample
    # carries the segment's FULL travel but sits one tick before the boundary:
    # the next section restarts its clip with the hips back at the origin, so the
    # actor has to jump forward by the whole travel offset at exactly that tick.
    # Emitting it at t1 instead let the next segment's key overwrite it, and the
    # jump degraded into a 0.15s, ~460-unit slide -- the teleport at frame 163.
    samples = []
    t = p["t0"]
    while t < p["t1"] - 1e-9:
        samples.append((t, t - p["t0"]))
        t += KEY_STEP
    last_t = p["t1"] if idx == len(plan) - 1 else p["t1"] - TICK
    samples.append((last_t, p["dur"]))

    for kt, ct_raw in samples:
        ct = min(ct_raw, h["dur"])
        frac = max(0.0, min(1.0, hips_proj(tag, ct) / end_proj))
        s = frac * p["horiz"]
        x, y, head = path_at(seg["path"], p["legs"], s)
        z = seg["z0"] + (seg["z1"] - seg["z0"]) * frac

        yaw = head - h["local_heading"]
        # Undo the clip's own travel so the character sits on the path.
        proj = hips_proj(tag, ct)
        u = h["u"]
        c, sn = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        lx, ly, lz = u.x * proj, u.y * proj, u.z * proj
        keys.append((kt,
                     x - (lx * c - ly * sn),
                     y - (lx * sn + ly * c),
                     z - lz,
                     yaw))

# Snap the heading change into a short blend at each segment start so the turn
# happens while the clip's travel is still near zero (where yaw barely matters).
for i, p in enumerate(plan[1:], start=1):
    b = p["t0"]
    for j, (t, x, y, z, yw) in enumerate(keys):
        if b - 1e-6 <= t < b + TURN:
            prev_yaw = None
            for tt, _x, _y, _z, yy in keys:
                if tt < b - 1e-6:
                    prev_yaw = yy
            if prev_yaw is not None:
                f = (t - b) / TURN
                d = ((yw - prev_yaw + 180.0) % 360.0) - 180.0
                keys[j] = (t, x, y, z, prev_yaw + d * f)

for t, x, y, z, yaw in keys:
    ch[0].add_key(F(t), float(x), 0.0, TICKS, LIN)
    ch[1].add_key(F(t), float(y), 0.0, TICKS, LIN)
    ch[2].add_key(F(t), float(z), 0.0, TICKS, LIN)
    ch[5].add_key(F(t), float(yaw), 0.0, TICKS, LIN)
L("keyed %d transform samples" % len(keys))

seq.set_playback_start_seconds(0.0)
seq.set_playback_end_seconds(TOTAL)

# Park the actor at the route start so he reads correctly outside the sequence.
aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for a in aes.get_all_level_actors():
    if str(a.get_actor_label()) == ACTOR:
        k = keys[0]
        a.set_actor_location_and_rotation(
            unreal.Vector(k[1], k[2], k[3]), unreal.Rotator(0.0, 0.0, k[4]), False, True)
        L("parked actor at (%.1f, %.1f, %.1f) yaw %.1f" % (k[1], k[2], k[3], k[4]))

EAL.save_loaded_asset(seq, False)
L("saved %s" % SEQ)
