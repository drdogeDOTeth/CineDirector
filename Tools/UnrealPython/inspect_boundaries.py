# Compare each anim-section boundary against the transform keys that straddle it.
# Keys come back as display-rate FrameTime (frame + subframe); sections are in
# ticks. Convert keys to ticks so the two are comparable.
import unreal

SEQ = "/Game/TemplarDoge"
ACTOR = "void_4003_Templar"

seq = unreal.EditorAssetLibrary.load_asset(SEQ)
tickres = seq.get_tick_resolution()
disp = seq.get_display_rate()
trate = float(tickres.numerator) / tickres.denominator
drate = float(disp.numerator) / disp.denominator
PER = trate / drate           # ticks per display frame
unreal.log("tick=%.0f/s  display=%.0f/s  ticks per frame=%.0f" % (trate, drate, PER))

binding = next(b for b in seq.get_bindings()
               if str(b.get_display_name()) == ACTOR)

bounds = []
for tr in binding.get_tracks():
    if isinstance(tr, unreal.MovieSceneSkeletalAnimationTrack):
        for s in tr.get_sections():
            bounds.append((s.get_start_frame(), s.get_end_frame()))
bounds.sort()
unreal.log("anim sections (ticks): %s" % bounds)

for tr in binding.get_tracks():
    if not isinstance(tr, unreal.MovieScene3DTransformTrack):
        continue
    sec = tr.get_sections()[0]
    ch = sec.get_all_channels()[0]          # locX is enough to see the jump
    ks = []
    for k in ch.get_keys():
        ft = k.get_time()
        ticks = (ft.frame_number.value + ft.sub_frame) * PER
        ks.append((ticks, k.get_value()))
    ks.sort()

    for start, end in bounds[:-1]:
        b = end
        near = [(t, v) for t, v in ks if abs(t - b) <= 2.2 * 0.15 * trate]
        unreal.log("--- boundary at tick %d (%.4fs) ---" % (b, b / trate))
        prev = None
        for t, v in near:
            mark = ""
            if prev is not None:
                gap_frames = (t - prev[0]) / PER
                mark = "  gap=%.4f frames  dX=%+.1f" % (gap_frames, v - prev[1])
            side = "A" if t < b else "B"
            unreal.log("   %s tick=%10.1f (%.4fs) X=%9.1f%s" % (side, t, t / trate, v, mark))
            prev = (t, v)
