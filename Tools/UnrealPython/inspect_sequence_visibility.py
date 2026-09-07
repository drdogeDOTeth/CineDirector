import unreal
seq=unreal.LevelSequenceEditorBlueprintLibrary.get_current_level_sequence()
print('RATE',seq.get_display_rate(),seq.get_tick_resolution())
for b in seq.get_bindings():
    for t in b.get_tracks():
        print('TRACK',t.get_name(), 'SECTIONS', len(t.get_sections()))
        for s in t.get_sections():
            if isinstance(s,unreal.MovieSceneSkeletalAnimationSection):
                print('SECTION',s.get_name(),'ROW',s.get_row_index(), 'RANGE',s.get_start_frame(),s.get_end_frame())
                print('PARAMS',s.get_editor_property('params'))
print('SEQAPI', [n for n in dir(unreal.LevelSequenceEditorBlueprintLibrary) if any(x in n for x in ['section','time','refresh'])])
print('TRACKAPI',[n for n in dir(unreal.MovieSceneSkeletalAnimationTrack) if any(x in n for x in ['eval','row','display'])])
