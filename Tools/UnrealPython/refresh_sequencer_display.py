import unreal
lib=unreal.LevelSequenceEditorBlueprintLibrary
seq=lib.get_current_level_sequence()
assert seq
lib.refresh_current_level_sequence()
sections=[s for b in seq.get_bindings() for t in b.get_tracks() for s in t.get_sections() if isinstance(s,unreal.MovieSceneSkeletalAnimationSection)]
lib.select_sections(sections)
print('REFRESHED',seq.get_path_name(),len(sections))
